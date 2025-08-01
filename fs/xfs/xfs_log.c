// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_errortag.h"
#include "xfs_error.h"
#include "xfs_trans.h"
#include "xfs_trans_priv.h"
#include "xfs_log.h"
#include "xfs_log_priv.h"
#include "xfs_trace.h"
#include "xfs_sysfs.h"
#include "xfs_sb.h"
#include "xfs_health.h"
#include "xfs_zone_alloc.h"

struct kmem_cache	*xfs_log_ticket_cache;

/* Local miscellaneous function prototypes */
STATIC struct xlog *
xlog_alloc_log(
	struct xfs_mount	*mp,
	struct xfs_buftarg	*log_target,
	xfs_daddr_t		blk_offset,
	int			num_bblks);
STATIC void
xlog_dealloc_log(
	struct xlog		*log);

/* local state machine functions */
STATIC void xlog_state_done_syncing(
	struct xlog_in_core	*iclog);
STATIC void xlog_state_do_callback(
	struct xlog		*log);
STATIC int
xlog_state_get_iclog_space(
	struct xlog		*log,
	int			len,
	struct xlog_in_core	**iclog,
	struct xlog_ticket	*ticket,
	int			*logoffsetp);
STATIC void
xlog_sync(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	struct xlog_ticket	*ticket);
#if defined(DEBUG)
STATIC void
xlog_verify_iclog(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	int			count);
STATIC void
xlog_verify_tail_lsn(
	struct xlog		*log,
	struct xlog_in_core	*iclog);
#else
#define xlog_verify_iclog(a,b,c)
#define xlog_verify_tail_lsn(a,b)
#endif

STATIC int
xlog_iclogs_empty(
	struct xlog		*log);

static int
xfs_log_cover(struct xfs_mount *);

/*
 * We need to make sure the buffer pointer returned is naturally aligned for the
 * biggest basic data type we put into it. We have already accounted for this
 * padding when sizing the buffer.
 *
 * However, this padding does not get written into the log, and hence we have to
 * track the space used by the log vectors separately to prevent log space hangs
 * due to inaccurate accounting (i.e. a leak) of the used log space through the
 * CIL context ticket.
 *
 * We also add space for the xlog_op_header that describes this region in the
 * log. This prepends the data region we return to the caller to copy their data
 * into, so do all the static initialisation of the ophdr now. Because the ophdr
 * is not 8 byte aligned, we have to be careful to ensure that we align the
 * start of the buffer such that the region we return to the call is 8 byte
 * aligned and packed against the tail of the ophdr.
 */
void *
xlog_prepare_iovec(
	struct xfs_log_vec	*lv,
	struct xfs_log_iovec	**vecp,
	uint			type)
{
	struct xfs_log_iovec	*vec = *vecp;
	struct xlog_op_header	*oph;
	uint32_t		len;
	void			*buf;

	if (vec) {
		ASSERT(vec - lv->lv_iovecp < lv->lv_niovecs);
		vec++;
	} else {
		vec = &lv->lv_iovecp[0];
	}

	len = lv->lv_buf_used + sizeof(struct xlog_op_header);
	if (!IS_ALIGNED(len, sizeof(uint64_t))) {
		lv->lv_buf_used = round_up(len, sizeof(uint64_t)) -
					sizeof(struct xlog_op_header);
	}

	vec->i_type = type;
	vec->i_addr = lv->lv_buf + lv->lv_buf_used;

	oph = vec->i_addr;
	oph->oh_clientid = XFS_TRANSACTION;
	oph->oh_res2 = 0;
	oph->oh_flags = 0;

	buf = vec->i_addr + sizeof(struct xlog_op_header);
	ASSERT(IS_ALIGNED((unsigned long)buf, sizeof(uint64_t)));

	*vecp = vec;
	return buf;
}

static inline void
xlog_grant_sub_space(
	struct xlog_grant_head	*head,
	int64_t			bytes)
{
	atomic64_sub(bytes, &head->grant);
}

static inline void
xlog_grant_add_space(
	struct xlog_grant_head	*head,
	int64_t			bytes)
{
	atomic64_add(bytes, &head->grant);
}

static void
xlog_grant_head_init(
	struct xlog_grant_head	*head)
{
	atomic64_set(&head->grant, 0);
	INIT_LIST_HEAD(&head->waiters);
	spin_lock_init(&head->lock);
}

void
xlog_grant_return_space(
	struct xlog	*log,
	xfs_lsn_t	old_head,
	xfs_lsn_t	new_head)
{
	int64_t		diff = xlog_lsn_sub(log, new_head, old_head);

	xlog_grant_sub_space(&log->l_reserve_head, diff);
	xlog_grant_sub_space(&log->l_write_head, diff);
}

/*
 * Return the space in the log between the tail and the head.  In the case where
 * we have overrun available reservation space, return 0. The memory barrier
 * pairs with the smp_wmb() in xlog_cil_ail_insert() to ensure that grant head
 * vs tail space updates are seen in the correct order and hence avoid
 * transients as space is transferred from the grant heads to the AIL on commit
 * completion.
 */
static uint64_t
xlog_grant_space_left(
	struct xlog		*log,
	struct xlog_grant_head	*head)
{
	int64_t			free_bytes;

	smp_rmb();	/* paired with smp_wmb in xlog_cil_ail_insert() */
	free_bytes = log->l_logsize - READ_ONCE(log->l_tail_space) -
			atomic64_read(&head->grant);
	if (free_bytes > 0)
		return free_bytes;
	return 0;
}

STATIC void
xlog_grant_head_wake_all(
	struct xlog_grant_head	*head)
{
	struct xlog_ticket	*tic;

	spin_lock(&head->lock);
	list_for_each_entry(tic, &head->waiters, t_queue)
		wake_up_process(tic->t_task);
	spin_unlock(&head->lock);
}

static inline int
xlog_ticket_reservation(
	struct xlog		*log,
	struct xlog_grant_head	*head,
	struct xlog_ticket	*tic)
{
	if (head == &log->l_write_head) {
		ASSERT(tic->t_flags & XLOG_TIC_PERM_RESERV);
		return tic->t_unit_res;
	}

	if (tic->t_flags & XLOG_TIC_PERM_RESERV)
		return tic->t_unit_res * tic->t_cnt;

	return tic->t_unit_res;
}

STATIC bool
xlog_grant_head_wake(
	struct xlog		*log,
	struct xlog_grant_head	*head,
	int			*free_bytes)
{
	struct xlog_ticket	*tic;
	int			need_bytes;

	list_for_each_entry(tic, &head->waiters, t_queue) {
		need_bytes = xlog_ticket_reservation(log, head, tic);
		if (*free_bytes < need_bytes)
			return false;

		*free_bytes -= need_bytes;
		trace_xfs_log_grant_wake_up(log, tic);
		wake_up_process(tic->t_task);
	}

	return true;
}

STATIC int
xlog_grant_head_wait(
	struct xlog		*log,
	struct xlog_grant_head	*head,
	struct xlog_ticket	*tic,
	int			need_bytes) __releases(&head->lock)
					    __acquires(&head->lock)
{
	list_add_tail(&tic->t_queue, &head->waiters);

	do {
		if (xlog_is_shutdown(log))
			goto shutdown;

		__set_current_state(TASK_UNINTERRUPTIBLE);
		spin_unlock(&head->lock);

		XFS_STATS_INC(log->l_mp, xs_sleep_logspace);

		/* Push on the AIL to free up all the log space. */
		xfs_ail_push_all(log->l_ailp);

		trace_xfs_log_grant_sleep(log, tic);
		schedule();
		trace_xfs_log_grant_wake(log, tic);

		spin_lock(&head->lock);
		if (xlog_is_shutdown(log))
			goto shutdown;
	} while (xlog_grant_space_left(log, head) < need_bytes);

	list_del_init(&tic->t_queue);
	return 0;
shutdown:
	list_del_init(&tic->t_queue);
	return -EIO;
}

/*
 * Atomically get the log space required for a log ticket.
 *
 * Once a ticket gets put onto head->waiters, it will only return after the
 * needed reservation is satisfied.
 *
 * This function is structured so that it has a lock free fast path. This is
 * necessary because every new transaction reservation will come through this
 * path. Hence any lock will be globally hot if we take it unconditionally on
 * every pass.
 *
 * As tickets are only ever moved on and off head->waiters under head->lock, we
 * only need to take that lock if we are going to add the ticket to the queue
 * and sleep. We can avoid taking the lock if the ticket was never added to
 * head->waiters because the t_queue list head will be empty and we hold the
 * only reference to it so it can safely be checked unlocked.
 */
STATIC int
xlog_grant_head_check(
	struct xlog		*log,
	struct xlog_grant_head	*head,
	struct xlog_ticket	*tic,
	int			*need_bytes)
{
	int			free_bytes;
	int			error = 0;

	ASSERT(!xlog_in_recovery(log));

	/*
	 * If there are other waiters on the queue then give them a chance at
	 * logspace before us.  Wake up the first waiters, if we do not wake
	 * up all the waiters then go to sleep waiting for more free space,
	 * otherwise try to get some space for this transaction.
	 */
	*need_bytes = xlog_ticket_reservation(log, head, tic);
	free_bytes = xlog_grant_space_left(log, head);
	if (!list_empty_careful(&head->waiters)) {
		spin_lock(&head->lock);
		if (!xlog_grant_head_wake(log, head, &free_bytes) ||
		    free_bytes < *need_bytes) {
			error = xlog_grant_head_wait(log, head, tic,
						     *need_bytes);
		}
		spin_unlock(&head->lock);
	} else if (free_bytes < *need_bytes) {
		spin_lock(&head->lock);
		error = xlog_grant_head_wait(log, head, tic, *need_bytes);
		spin_unlock(&head->lock);
	}

	return error;
}

bool
xfs_log_writable(
	struct xfs_mount	*mp)
{
	/*
	 * Do not write to the log on norecovery mounts, if the data or log
	 * devices are read-only, or if the filesystem is shutdown. Read-only
	 * mounts allow internal writes for log recovery and unmount purposes,
	 * so don't restrict that case.
	 */
	if (xfs_has_norecovery(mp))
		return false;
	if (xfs_readonly_buftarg(mp->m_ddev_targp))
		return false;
	if (xfs_readonly_buftarg(mp->m_log->l_targ))
		return false;
	if (xlog_is_shutdown(mp->m_log))
		return false;
	return true;
}

/*
 * Replenish the byte reservation required by moving the grant write head.
 */
int
xfs_log_regrant(
	struct xfs_mount	*mp,
	struct xlog_ticket	*tic)
{
	struct xlog		*log = mp->m_log;
	int			need_bytes;
	int			error = 0;

	if (xlog_is_shutdown(log))
		return -EIO;

	XFS_STATS_INC(mp, xs_try_logspace);

	/*
	 * This is a new transaction on the ticket, so we need to change the
	 * transaction ID so that the next transaction has a different TID in
	 * the log. Just add one to the existing tid so that we can see chains
	 * of rolling transactions in the log easily.
	 */
	tic->t_tid++;
	tic->t_curr_res = tic->t_unit_res;
	if (tic->t_cnt > 0)
		return 0;

	trace_xfs_log_regrant(log, tic);

	error = xlog_grant_head_check(log, &log->l_write_head, tic,
				      &need_bytes);
	if (error)
		goto out_error;

	xlog_grant_add_space(&log->l_write_head, need_bytes);
	trace_xfs_log_regrant_exit(log, tic);
	return 0;

out_error:
	/*
	 * If we are failing, make sure the ticket doesn't have any current
	 * reservations.  We don't want to add this back when the ticket/
	 * transaction gets cancelled.
	 */
	tic->t_curr_res = 0;
	tic->t_cnt = 0;	/* ungrant will give back unit_res * t_cnt. */
	return error;
}

/*
 * Reserve log space and return a ticket corresponding to the reservation.
 *
 * Each reservation is going to reserve extra space for a log record header.
 * When writes happen to the on-disk log, we don't subtract the length of the
 * log record header from any reservation.  By wasting space in each
 * reservation, we prevent over allocation problems.
 */
int
xfs_log_reserve(
	struct xfs_mount	*mp,
	int			unit_bytes,
	int			cnt,
	struct xlog_ticket	**ticp,
	bool			permanent)
{
	struct xlog		*log = mp->m_log;
	struct xlog_ticket	*tic;
	int			need_bytes;
	int			error = 0;

	if (xlog_is_shutdown(log))
		return -EIO;

	XFS_STATS_INC(mp, xs_try_logspace);

	ASSERT(*ticp == NULL);
	tic = xlog_ticket_alloc(log, unit_bytes, cnt, permanent);
	*ticp = tic;
	trace_xfs_log_reserve(log, tic);
	error = xlog_grant_head_check(log, &log->l_reserve_head, tic,
				      &need_bytes);
	if (error)
		goto out_error;

	xlog_grant_add_space(&log->l_reserve_head, need_bytes);
	xlog_grant_add_space(&log->l_write_head, need_bytes);
	trace_xfs_log_reserve_exit(log, tic);
	return 0;

out_error:
	/*
	 * If we are failing, make sure the ticket doesn't have any current
	 * reservations.  We don't want to add this back when the ticket/
	 * transaction gets cancelled.
	 */
	tic->t_curr_res = 0;
	tic->t_cnt = 0;	/* ungrant will give back unit_res * t_cnt. */
	return error;
}

/*
 * Run all the pending iclog callbacks and wake log force waiters and iclog
 * space waiters so they can process the newly set shutdown state. We really
 * don't care what order we process callbacks here because the log is shut down
 * and so state cannot change on disk anymore. However, we cannot wake waiters
 * until the callbacks have been processed because we may be in unmount and
 * we must ensure that all AIL operations the callbacks perform have completed
 * before we tear down the AIL.
 *
 * We avoid processing actively referenced iclogs so that we don't run callbacks
 * while the iclog owner might still be preparing the iclog for IO submssion.
 * These will be caught by xlog_state_iclog_release() and call this function
 * again to process any callbacks that may have been added to that iclog.
 */
static void
xlog_state_shutdown_callbacks(
	struct xlog		*log)
{
	struct xlog_in_core	*iclog;
	LIST_HEAD(cb_list);

	iclog = log->l_iclog;
	do {
		if (atomic_read(&iclog->ic_refcnt)) {
			/* Reference holder will re-run iclog callbacks. */
			continue;
		}
		list_splice_init(&iclog->ic_callbacks, &cb_list);
		spin_unlock(&log->l_icloglock);

		xlog_cil_process_committed(&cb_list);

		spin_lock(&log->l_icloglock);
		wake_up_all(&iclog->ic_write_wait);
		wake_up_all(&iclog->ic_force_wait);
	} while ((iclog = iclog->ic_next) != log->l_iclog);

	wake_up_all(&log->l_flush_wait);
}

/*
 * Flush iclog to disk if this is the last reference to the given iclog and the
 * it is in the WANT_SYNC state.
 *
 * If XLOG_ICL_NEED_FUA is already set on the iclog, we need to ensure that the
 * log tail is updated correctly. NEED_FUA indicates that the iclog will be
 * written to stable storage, and implies that a commit record is contained
 * within the iclog. We need to ensure that the log tail does not move beyond
 * the tail that the first commit record in the iclog ordered against, otherwise
 * correct recovery of that checkpoint becomes dependent on future operations
 * performed on this iclog.
 *
 * Hence if NEED_FUA is set and the current iclog tail lsn is empty, write the
 * current tail into iclog. Once the iclog tail is set, future operations must
 * not modify it, otherwise they potentially violate ordering constraints for
 * the checkpoint commit that wrote the initial tail lsn value. The tail lsn in
 * the iclog will get zeroed on activation of the iclog after sync, so we
 * always capture the tail lsn on the iclog on the first NEED_FUA release
 * regardless of the number of active reference counts on this iclog.
 */
int
xlog_state_release_iclog(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	struct xlog_ticket	*ticket)
{
	bool			last_ref;

	lockdep_assert_held(&log->l_icloglock);

	trace_xlog_iclog_release(iclog, _RET_IP_);
	/*
	 * Grabbing the current log tail needs to be atomic w.r.t. the writing
	 * of the tail LSN into the iclog so we guarantee that the log tail does
	 * not move between the first time we know that the iclog needs to be
	 * made stable and when we eventually submit it.
	 */
	if ((iclog->ic_state == XLOG_STATE_WANT_SYNC ||
	     (iclog->ic_flags & XLOG_ICL_NEED_FUA)) &&
	    !iclog->ic_header.h_tail_lsn) {
		iclog->ic_header.h_tail_lsn =
				cpu_to_be64(atomic64_read(&log->l_tail_lsn));
	}

	last_ref = atomic_dec_and_test(&iclog->ic_refcnt);

	if (xlog_is_shutdown(log)) {
		/*
		 * If there are no more references to this iclog, process the
		 * pending iclog callbacks that were waiting on the release of
		 * this iclog.
		 */
		if (last_ref)
			xlog_state_shutdown_callbacks(log);
		return -EIO;
	}

	if (!last_ref)
		return 0;

	if (iclog->ic_state != XLOG_STATE_WANT_SYNC) {
		ASSERT(iclog->ic_state == XLOG_STATE_ACTIVE);
		return 0;
	}

	iclog->ic_state = XLOG_STATE_SYNCING;
	xlog_verify_tail_lsn(log, iclog);
	trace_xlog_iclog_syncing(iclog, _RET_IP_);

	spin_unlock(&log->l_icloglock);
	xlog_sync(log, iclog, ticket);
	spin_lock(&log->l_icloglock);
	return 0;
}

/*
 * Mount a log filesystem
 *
 * mp		- ubiquitous xfs mount point structure
 * log_target	- buftarg of on-disk log device
 * blk_offset	- Start block # where block size is 512 bytes (BBSIZE)
 * num_bblocks	- Number of BBSIZE blocks in on-disk log
 *
 * Return error or zero.
 */
int
xfs_log_mount(
	xfs_mount_t		*mp,
	struct xfs_buftarg	*log_target,
	xfs_daddr_t		blk_offset,
	int			num_bblks)
{
	struct xlog		*log;
	int			error = 0;
	int			min_logfsbs;

	if (!xfs_has_norecovery(mp)) {
		xfs_notice(mp, "Mounting V%d Filesystem %pU",
			   XFS_SB_VERSION_NUM(&mp->m_sb),
			   &mp->m_sb.sb_uuid);
	} else {
		xfs_notice(mp,
"Mounting V%d filesystem %pU in no-recovery mode. Filesystem will be inconsistent.",
			   XFS_SB_VERSION_NUM(&mp->m_sb),
			   &mp->m_sb.sb_uuid);
		ASSERT(xfs_is_readonly(mp));
	}

	log = xlog_alloc_log(mp, log_target, blk_offset, num_bblks);
	if (IS_ERR(log)) {
		error = PTR_ERR(log);
		goto out;
	}
	mp->m_log = log;

	/*
	 * Now that we have set up the log and it's internal geometry
	 * parameters, we can validate the given log space and drop a critical
	 * message via syslog if the log size is too small. A log that is too
	 * small can lead to unexpected situations in transaction log space
	 * reservation stage. The superblock verifier has already validated all
	 * the other log geometry constraints, so we don't have to check those
	 * here.
	 *
	 * Note: For v4 filesystems, we can't just reject the mount if the
	 * validation fails.  This would mean that people would have to
	 * downgrade their kernel just to remedy the situation as there is no
	 * way to grow the log (short of black magic surgery with xfs_db).
	 *
	 * We can, however, reject mounts for V5 format filesystems, as the
	 * mkfs binary being used to make the filesystem should never create a
	 * filesystem with a log that is too small.
	 */
	min_logfsbs = xfs_log_calc_minimum_size(mp);
	if (mp->m_sb.sb_logblocks < min_logfsbs) {
		xfs_warn(mp,
		"Log size %d blocks too small, minimum size is %d blocks",
			 mp->m_sb.sb_logblocks, min_logfsbs);

		/*
		 * Log check errors are always fatal on v5; or whenever bad
		 * metadata leads to a crash.
		 */
		if (xfs_has_crc(mp)) {
			xfs_crit(mp, "AAIEEE! Log failed size checks. Abort!");
			ASSERT(0);
			error = -EINVAL;
			goto out_free_log;
		}
		xfs_crit(mp, "Log size out of supported range.");
		xfs_crit(mp,
"Continuing onwards, but if log hangs are experienced then please report this message in the bug report.");
	}

	/*
	 * Initialize the AIL now we have a log.
	 */
	error = xfs_trans_ail_init(mp);
	if (error) {
		xfs_warn(mp, "AIL initialisation failed: error %d", error);
		goto out_free_log;
	}
	log->l_ailp = mp->m_ail;

	/*
	 * skip log recovery on a norecovery mount.  pretend it all
	 * just worked.
	 */
	if (!xfs_has_norecovery(mp)) {
		error = xlog_recover(log);
		if (error) {
			xfs_warn(mp, "log mount/recovery failed: error %d",
				error);
			xlog_recover_cancel(log);
			goto out_destroy_ail;
		}
	}

	error = xfs_sysfs_init(&log->l_kobj, &xfs_log_ktype, &mp->m_kobj,
			       "log");
	if (error)
		goto out_destroy_ail;

	/* Normal transactions can now occur */
	clear_bit(XLOG_ACTIVE_RECOVERY, &log->l_opstate);

	/*
	 * Now the log has been fully initialised and we know were our
	 * space grant counters are, we can initialise the permanent ticket
	 * needed for delayed logging to work.
	 */
	xlog_cil_init_post_recovery(log);

	return 0;

out_destroy_ail:
	xfs_trans_ail_destroy(mp);
out_free_log:
	xlog_dealloc_log(log);
out:
	return error;
}

/*
 * Finish the recovery of the file system.  This is separate from the
 * xfs_log_mount() call, because it depends on the code in xfs_mountfs() to read
 * in the root and real-time bitmap inodes between calling xfs_log_mount() and
 * here.
 *
 * If we finish recovery successfully, start the background log work. If we are
 * not doing recovery, then we have a RO filesystem and we don't need to start
 * it.
 */
int
xfs_log_mount_finish(
	struct xfs_mount	*mp)
{
	struct xlog		*log = mp->m_log;
	int			error = 0;

	if (xfs_has_norecovery(mp)) {
		ASSERT(xfs_is_readonly(mp));
		return 0;
	}

	/*
	 * During the second phase of log recovery, we need iget and
	 * iput to behave like they do for an active filesystem.
	 * xfs_fs_drop_inode needs to be able to prevent the deletion
	 * of inodes before we're done replaying log items on those
	 * inodes.  Turn it off immediately after recovery finishes
	 * so that we don't leak the quota inodes if subsequent mount
	 * activities fail.
	 *
	 * We let all inodes involved in redo item processing end up on
	 * the LRU instead of being evicted immediately so that if we do
	 * something to an unlinked inode, the irele won't cause
	 * premature truncation and freeing of the inode, which results
	 * in log recovery failure.  We have to evict the unreferenced
	 * lru inodes after clearing SB_ACTIVE because we don't
	 * otherwise clean up the lru if there's a subsequent failure in
	 * xfs_mountfs, which leads to us leaking the inodes if nothing
	 * else (e.g. quotacheck) references the inodes before the
	 * mount failure occurs.
	 */
	mp->m_super->s_flags |= SB_ACTIVE;
	xfs_log_work_queue(mp);
	if (xlog_recovery_needed(log))
		error = xlog_recover_finish(log);
	mp->m_super->s_flags &= ~SB_ACTIVE;
	evict_inodes(mp->m_super);

	/*
	 * Drain the buffer LRU after log recovery. This is required for v4
	 * filesystems to avoid leaving around buffers with NULL verifier ops,
	 * but we do it unconditionally to make sure we're always in a clean
	 * cache state after mount.
	 *
	 * Don't push in the error case because the AIL may have pending intents
	 * that aren't removed until recovery is cancelled.
	 */
	if (xlog_recovery_needed(log)) {
		if (!error) {
			xfs_log_force(mp, XFS_LOG_SYNC);
			xfs_ail_push_all_sync(mp->m_ail);
		}
		xfs_notice(mp, "Ending recovery (logdev: %s)",
				mp->m_logname ? mp->m_logname : "internal");
	} else {
		xfs_info(mp, "Ending clean mount");
	}
	xfs_buftarg_drain(mp->m_ddev_targp);

	clear_bit(XLOG_RECOVERY_NEEDED, &log->l_opstate);

	/* Make sure the log is dead if we're returning failure. */
	ASSERT(!error || xlog_is_shutdown(log));

	return error;
}

/*
 * The mount has failed. Cancel the recovery if it hasn't completed and destroy
 * the log.
 */
void
xfs_log_mount_cancel(
	struct xfs_mount	*mp)
{
	xlog_recover_cancel(mp->m_log);
	xfs_log_unmount(mp);
}

/*
 * Flush out the iclog to disk ensuring that device caches are flushed and
 * the iclog hits stable storage before any completion waiters are woken.
 */
static inline int
xlog_force_iclog(
	struct xlog_in_core	*iclog)
{
	atomic_inc(&iclog->ic_refcnt);
	iclog->ic_flags |= XLOG_ICL_NEED_FLUSH | XLOG_ICL_NEED_FUA;
	if (iclog->ic_state == XLOG_STATE_ACTIVE)
		xlog_state_switch_iclogs(iclog->ic_log, iclog, 0);
	return xlog_state_release_iclog(iclog->ic_log, iclog, NULL);
}

/*
 * Cycle all the iclogbuf locks to make sure all log IO completion
 * is done before we tear down these buffers.
 */
static void
xlog_wait_iclog_completion(struct xlog *log)
{
	int		i;
	struct xlog_in_core	*iclog = log->l_iclog;

	for (i = 0; i < log->l_iclog_bufs; i++) {
		down(&iclog->ic_sema);
		up(&iclog->ic_sema);
		iclog = iclog->ic_next;
	}
}

/*
 * Wait for the iclog and all prior iclogs to be written disk as required by the
 * log force state machine. Waiting on ic_force_wait ensures iclog completions
 * have been ordered and callbacks run before we are woken here, hence
 * guaranteeing that all the iclogs up to this one are on stable storage.
 */
int
xlog_wait_on_iclog(
	struct xlog_in_core	*iclog)
		__releases(iclog->ic_log->l_icloglock)
{
	struct xlog		*log = iclog->ic_log;

	trace_xlog_iclog_wait_on(iclog, _RET_IP_);
	if (!xlog_is_shutdown(log) &&
	    iclog->ic_state != XLOG_STATE_ACTIVE &&
	    iclog->ic_state != XLOG_STATE_DIRTY) {
		XFS_STATS_INC(log->l_mp, xs_log_force_sleep);
		xlog_wait(&iclog->ic_force_wait, &log->l_icloglock);
	} else {
		spin_unlock(&log->l_icloglock);
	}

	if (xlog_is_shutdown(log))
		return -EIO;
	return 0;
}

/*
 * Write out an unmount record using the ticket provided. We have to account for
 * the data space used in the unmount ticket as this write is not done from a
 * transaction context that has already done the accounting for us.
 */
static int
xlog_write_unmount_record(
	struct xlog		*log,
	struct xlog_ticket	*ticket)
{
	struct  {
		struct xlog_op_header ophdr;
		struct xfs_unmount_log_format ulf;
	} unmount_rec = {
		.ophdr = {
			.oh_clientid = XFS_LOG,
			.oh_tid = cpu_to_be32(ticket->t_tid),
			.oh_flags = XLOG_UNMOUNT_TRANS,
		},
		.ulf = {
			.magic = XLOG_UNMOUNT_TYPE,
		},
	};
	struct xfs_log_iovec reg = {
		.i_addr = &unmount_rec,
		.i_len = sizeof(unmount_rec),
		.i_type = XLOG_REG_TYPE_UNMOUNT,
	};
	struct xfs_log_vec vec = {
		.lv_niovecs = 1,
		.lv_iovecp = &reg,
	};
	LIST_HEAD(lv_chain);
	list_add(&vec.lv_list, &lv_chain);

	BUILD_BUG_ON((sizeof(struct xlog_op_header) +
		      sizeof(struct xfs_unmount_log_format)) !=
							sizeof(unmount_rec));

	/* account for space used by record data */
	ticket->t_curr_res -= sizeof(unmount_rec);

	return xlog_write(log, NULL, &lv_chain, ticket, reg.i_len);
}

/*
 * Mark the filesystem clean by writing an unmount record to the head of the
 * log.
 */
static void
xlog_unmount_write(
	struct xlog		*log)
{
	struct xfs_mount	*mp = log->l_mp;
	struct xlog_in_core	*iclog;
	struct xlog_ticket	*tic = NULL;
	int			error;

	error = xfs_log_reserve(mp, 600, 1, &tic, 0);
	if (error)
		goto out_err;

	error = xlog_write_unmount_record(log, tic);
	/*
	 * At this point, we're umounting anyway, so there's no point in
	 * transitioning log state to shutdown. Just continue...
	 */
out_err:
	if (error)
		xfs_alert(mp, "%s: unmount record failed", __func__);

	spin_lock(&log->l_icloglock);
	iclog = log->l_iclog;
	error = xlog_force_iclog(iclog);
	xlog_wait_on_iclog(iclog);

	if (tic) {
		trace_xfs_log_umount_write(log, tic);
		xfs_log_ticket_ungrant(log, tic);
	}
}

static void
xfs_log_unmount_verify_iclog(
	struct xlog		*log)
{
	struct xlog_in_core	*iclog = log->l_iclog;

	do {
		ASSERT(iclog->ic_state == XLOG_STATE_ACTIVE);
		ASSERT(iclog->ic_offset == 0);
	} while ((iclog = iclog->ic_next) != log->l_iclog);
}

/*
 * Unmount record used to have a string "Unmount filesystem--" in the
 * data section where the "Un" was really a magic number (XLOG_UNMOUNT_TYPE).
 * We just write the magic number now since that particular field isn't
 * currently architecture converted and "Unmount" is a bit foo.
 * As far as I know, there weren't any dependencies on the old behaviour.
 */
static void
xfs_log_unmount_write(
	struct xfs_mount	*mp)
{
	struct xlog		*log = mp->m_log;

	if (!xfs_log_writable(mp))
		return;

	xfs_log_force(mp, XFS_LOG_SYNC);

	if (xlog_is_shutdown(log))
		return;

	/*
	 * If we think the summary counters are bad, avoid writing the unmount
	 * record to force log recovery at next mount, after which the summary
	 * counters will be recalculated.  Refer to xlog_check_unmount_rec for
	 * more details.
	 */
	if (XFS_TEST_ERROR(xfs_fs_has_sickness(mp, XFS_SICK_FS_COUNTERS), mp,
			XFS_ERRTAG_FORCE_SUMMARY_RECALC)) {
		xfs_alert(mp, "%s: will fix summary counters at next mount",
				__func__);
		return;
	}

	xfs_log_unmount_verify_iclog(log);
	xlog_unmount_write(log);
}

/*
 * Empty the log for unmount/freeze.
 *
 * To do this, we first need to shut down the background log work so it is not
 * trying to cover the log as we clean up. We then need to unpin all objects in
 * the log so we can then flush them out. Once they have completed their IO and
 * run the callbacks removing themselves from the AIL, we can cover the log.
 */
int
xfs_log_quiesce(
	struct xfs_mount	*mp)
{
	/*
	 * Clear log incompat features since we're quiescing the log.  Report
	 * failures, though it's not fatal to have a higher log feature
	 * protection level than the log contents actually require.
	 */
	if (xfs_clear_incompat_log_features(mp)) {
		int error;

		error = xfs_sync_sb(mp, false);
		if (error)
			xfs_warn(mp,
	"Failed to clear log incompat features on quiesce");
	}

	cancel_delayed_work_sync(&mp->m_log->l_work);
	xfs_log_force(mp, XFS_LOG_SYNC);

	/*
	 * The superblock buffer is uncached and while xfs_ail_push_all_sync()
	 * will push it, xfs_buftarg_wait() will not wait for it. Further,
	 * xfs_buf_iowait() cannot be used because it was pushed with the
	 * XBF_ASYNC flag set, so we need to use a lock/unlock pair to wait for
	 * the IO to complete.
	 */
	xfs_ail_push_all_sync(mp->m_ail);
	xfs_buftarg_wait(mp->m_ddev_targp);
	xfs_buf_lock(mp->m_sb_bp);
	xfs_buf_unlock(mp->m_sb_bp);

	return xfs_log_cover(mp);
}

void
xfs_log_clean(
	struct xfs_mount	*mp)
{
	xfs_log_quiesce(mp);
	xfs_log_unmount_write(mp);
}

/*
 * Shut down and release the AIL and Log.
 *
 * During unmount, we need to ensure we flush all the dirty metadata objects
 * from the AIL so that the log is empty before we write the unmount record to
 * the log. Once this is done, we can tear down the AIL and the log.
 */
void
xfs_log_unmount(
	struct xfs_mount	*mp)
{
	xfs_log_clean(mp);

	/*
	 * If shutdown has come from iclog IO context, the log
	 * cleaning will have been skipped and so we need to wait
	 * for the iclog to complete shutdown processing before we
	 * tear anything down.
	 */
	xlog_wait_iclog_completion(mp->m_log);

	xfs_buftarg_drain(mp->m_ddev_targp);

	xfs_trans_ail_destroy(mp);

	xfs_sysfs_del(&mp->m_log->l_kobj);

	xlog_dealloc_log(mp->m_log);
}

void
xfs_log_item_init(
	struct xfs_mount	*mp,
	struct xfs_log_item	*item,
	int			type,
	const struct xfs_item_ops *ops)
{
	item->li_log = mp->m_log;
	item->li_ailp = mp->m_ail;
	item->li_type = type;
	item->li_ops = ops;
	item->li_lv = NULL;

	INIT_LIST_HEAD(&item->li_ail);
	INIT_LIST_HEAD(&item->li_cil);
	INIT_LIST_HEAD(&item->li_bio_list);
	INIT_LIST_HEAD(&item->li_trans);
}

/*
 * Wake up processes waiting for log space after we have moved the log tail.
 */
void
xfs_log_space_wake(
	struct xfs_mount	*mp)
{
	struct xlog		*log = mp->m_log;
	int			free_bytes;

	if (xlog_is_shutdown(log))
		return;

	if (!list_empty_careful(&log->l_write_head.waiters)) {
		ASSERT(!xlog_in_recovery(log));

		spin_lock(&log->l_write_head.lock);
		free_bytes = xlog_grant_space_left(log, &log->l_write_head);
		xlog_grant_head_wake(log, &log->l_write_head, &free_bytes);
		spin_unlock(&log->l_write_head.lock);
	}

	if (!list_empty_careful(&log->l_reserve_head.waiters)) {
		ASSERT(!xlog_in_recovery(log));

		spin_lock(&log->l_reserve_head.lock);
		free_bytes = xlog_grant_space_left(log, &log->l_reserve_head);
		xlog_grant_head_wake(log, &log->l_reserve_head, &free_bytes);
		spin_unlock(&log->l_reserve_head.lock);
	}
}

/*
 * Determine if we have a transaction that has gone to disk that needs to be
 * covered. To begin the transition to the idle state firstly the log needs to
 * be idle. That means the CIL, the AIL and the iclogs needs to be empty before
 * we start attempting to cover the log.
 *
 * Only if we are then in a state where covering is needed, the caller is
 * informed that dummy transactions are required to move the log into the idle
 * state.
 *
 * If there are any items in the AIl or CIL, then we do not want to attempt to
 * cover the log as we may be in a situation where there isn't log space
 * available to run a dummy transaction and this can lead to deadlocks when the
 * tail of the log is pinned by an item that is modified in the CIL.  Hence
 * there's no point in running a dummy transaction at this point because we
 * can't start trying to idle the log until both the CIL and AIL are empty.
 */
static bool
xfs_log_need_covered(
	struct xfs_mount	*mp)
{
	struct xlog		*log = mp->m_log;
	bool			needed = false;

	if (!xlog_cil_empty(log))
		return false;

	spin_lock(&log->l_icloglock);
	switch (log->l_covered_state) {
	case XLOG_STATE_COVER_DONE:
	case XLOG_STATE_COVER_DONE2:
	case XLOG_STATE_COVER_IDLE:
		break;
	case XLOG_STATE_COVER_NEED:
	case XLOG_STATE_COVER_NEED2:
		if (xfs_ail_min_lsn(log->l_ailp))
			break;
		if (!xlog_iclogs_empty(log))
			break;

		needed = true;
		if (log->l_covered_state == XLOG_STATE_COVER_NEED)
			log->l_covered_state = XLOG_STATE_COVER_DONE;
		else
			log->l_covered_state = XLOG_STATE_COVER_DONE2;
		break;
	default:
		needed = true;
		break;
	}
	spin_unlock(&log->l_icloglock);
	return needed;
}

/*
 * Explicitly cover the log. This is similar to background log covering but
 * intended for usage in quiesce codepaths. The caller is responsible to ensure
 * the log is idle and suitable for covering. The CIL, iclog buffers and AIL
 * must all be empty.
 */
static int
xfs_log_cover(
	struct xfs_mount	*mp)
{
	int			error = 0;
	bool			need_covered;

	ASSERT((xlog_cil_empty(mp->m_log) && xlog_iclogs_empty(mp->m_log) &&
	        !xfs_ail_min_lsn(mp->m_log->l_ailp)) ||
		xlog_is_shutdown(mp->m_log));

	if (!xfs_log_writable(mp))
		return 0;

	/*
	 * xfs_log_need_covered() is not idempotent because it progresses the
	 * state machine if the log requires covering. Therefore, we must call
	 * this function once and use the result until we've issued an sb sync.
	 * Do so first to make that abundantly clear.
	 *
	 * Fall into the covering sequence if the log needs covering or the
	 * mount has lazy superblock accounting to sync to disk. The sb sync
	 * used for covering accumulates the in-core counters, so covering
	 * handles this for us.
	 */
	need_covered = xfs_log_need_covered(mp);
	if (!need_covered && !xfs_has_lazysbcount(mp))
		return 0;

	/*
	 * To cover the log, commit the superblock twice (at most) in
	 * independent checkpoints. The first serves as a reference for the
	 * tail pointer. The sync transaction and AIL push empties the AIL and
	 * updates the in-core tail to the LSN of the first checkpoint. The
	 * second commit updates the on-disk tail with the in-core LSN,
	 * covering the log. Push the AIL one more time to leave it empty, as
	 * we found it.
	 */
	do {
		error = xfs_sync_sb(mp, true);
		if (error)
			break;
		xfs_ail_push_all_sync(mp->m_ail);
	} while (xfs_log_need_covered(mp));

	return error;
}

static void
xlog_ioend_work(
	struct work_struct	*work)
{
	struct xlog_in_core     *iclog =
		container_of(work, struct xlog_in_core, ic_end_io_work);
	struct xlog		*log = iclog->ic_log;
	int			error;

	error = blk_status_to_errno(iclog->ic_bio.bi_status);
#ifdef DEBUG
	/* treat writes with injected CRC errors as failed */
	if (iclog->ic_fail_crc)
		error = -EIO;
#endif

	/*
	 * Race to shutdown the filesystem if we see an error.
	 */
	if (XFS_TEST_ERROR(error, log->l_mp, XFS_ERRTAG_IODONE_IOERR)) {
		xfs_alert(log->l_mp, "log I/O error %d", error);
		xlog_force_shutdown(log, SHUTDOWN_LOG_IO_ERROR);
	}

	xlog_state_done_syncing(iclog);
	bio_uninit(&iclog->ic_bio);

	/*
	 * Drop the lock to signal that we are done. Nothing references the
	 * iclog after this, so an unmount waiting on this lock can now tear it
	 * down safely. As such, it is unsafe to reference the iclog after the
	 * unlock as we could race with it being freed.
	 */
	up(&iclog->ic_sema);
}

/*
 * Return size of each in-core log record buffer.
 *
 * All machines get 8 x 32kB buffers by default, unless tuned otherwise.
 *
 * If the filesystem blocksize is too large, we may need to choose a
 * larger size since the directory code currently logs entire blocks.
 */
STATIC void
xlog_get_iclog_buffer_size(
	struct xfs_mount	*mp,
	struct xlog		*log)
{
	if (mp->m_logbufs <= 0)
		mp->m_logbufs = XLOG_MAX_ICLOGS;
	if (mp->m_logbsize <= 0)
		mp->m_logbsize = XLOG_BIG_RECORD_BSIZE;

	log->l_iclog_bufs = mp->m_logbufs;
	log->l_iclog_size = mp->m_logbsize;

	/*
	 * # headers = size / 32k - one header holds cycles from 32k of data.
	 */
	log->l_iclog_heads =
		DIV_ROUND_UP(mp->m_logbsize, XLOG_HEADER_CYCLE_SIZE);
	log->l_iclog_hsize = log->l_iclog_heads << BBSHIFT;
}

void
xfs_log_work_queue(
	struct xfs_mount        *mp)
{
	queue_delayed_work(mp->m_sync_workqueue, &mp->m_log->l_work,
				msecs_to_jiffies(xfs_syncd_centisecs * 10));
}

/*
 * Clear the log incompat flags if we have the opportunity.
 *
 * This only happens if we're about to log the second dummy transaction as part
 * of covering the log.
 */
static inline void
xlog_clear_incompat(
	struct xlog		*log)
{
	struct xfs_mount	*mp = log->l_mp;

	if (!xfs_sb_has_incompat_log_feature(&mp->m_sb,
				XFS_SB_FEAT_INCOMPAT_LOG_ALL))
		return;

	if (log->l_covered_state != XLOG_STATE_COVER_DONE2)
		return;

	xfs_clear_incompat_log_features(mp);
}

/*
 * Every sync period we need to unpin all items in the AIL and push them to
 * disk. If there is nothing dirty, then we might need to cover the log to
 * indicate that the filesystem is idle.
 */
static void
xfs_log_worker(
	struct work_struct	*work)
{
	struct xlog		*log = container_of(to_delayed_work(work),
						struct xlog, l_work);
	struct xfs_mount	*mp = log->l_mp;

	/* dgc: errors ignored - not fatal and nowhere to report them */
	if (xfs_fs_writable(mp, SB_FREEZE_WRITE) && xfs_log_need_covered(mp)) {
		/*
		 * Dump a transaction into the log that contains no real change.
		 * This is needed to stamp the current tail LSN into the log
		 * during the covering operation.
		 *
		 * We cannot use an inode here for this - that will push dirty
		 * state back up into the VFS and then periodic inode flushing
		 * will prevent log covering from making progress. Hence we
		 * synchronously log the superblock instead to ensure the
		 * superblock is immediately unpinned and can be written back.
		 */
		xlog_clear_incompat(log);
		xfs_sync_sb(mp, true);
	} else
		xfs_log_force(mp, 0);

	/* start pushing all the metadata that is currently dirty */
	xfs_ail_push_all(mp->m_ail);

	/* queue us up again */
	xfs_log_work_queue(mp);
}

/*
 * This routine initializes some of the log structure for a given mount point.
 * Its primary purpose is to fill in enough, so recovery can occur.  However,
 * some other stuff may be filled in too.
 */
STATIC struct xlog *
xlog_alloc_log(
	struct xfs_mount	*mp,
	struct xfs_buftarg	*log_target,
	xfs_daddr_t		blk_offset,
	int			num_bblks)
{
	struct xlog		*log;
	xlog_rec_header_t	*head;
	xlog_in_core_t		**iclogp;
	xlog_in_core_t		*iclog, *prev_iclog=NULL;
	int			i;
	int			error = -ENOMEM;
	uint			log2_size = 0;

	log = kzalloc(sizeof(struct xlog), GFP_KERNEL | __GFP_RETRY_MAYFAIL);
	if (!log) {
		xfs_warn(mp, "Log allocation failed: No memory!");
		goto out;
	}

	log->l_mp	   = mp;
	log->l_targ	   = log_target;
	log->l_logsize     = BBTOB(num_bblks);
	log->l_logBBstart  = blk_offset;
	log->l_logBBsize   = num_bblks;
	log->l_covered_state = XLOG_STATE_COVER_IDLE;
	set_bit(XLOG_ACTIVE_RECOVERY, &log->l_opstate);
	INIT_DELAYED_WORK(&log->l_work, xfs_log_worker);
	INIT_LIST_HEAD(&log->r_dfops);

	log->l_prev_block  = -1;
	/* log->l_tail_lsn = 0x100000000LL; cycle = 1; current block = 0 */
	xlog_assign_atomic_lsn(&log->l_tail_lsn, 1, 0);
	log->l_curr_cycle  = 1;	    /* 0 is bad since this is initial value */

	if (xfs_has_logv2(mp) && mp->m_sb.sb_logsunit > 1)
		log->l_iclog_roundoff = mp->m_sb.sb_logsunit;
	else
		log->l_iclog_roundoff = BBSIZE;

	xlog_grant_head_init(&log->l_reserve_head);
	xlog_grant_head_init(&log->l_write_head);

	error = -EFSCORRUPTED;
	if (xfs_has_sector(mp)) {
	        log2_size = mp->m_sb.sb_logsectlog;
		if (log2_size < BBSHIFT) {
			xfs_warn(mp, "Log sector size too small (0x%x < 0x%x)",
				log2_size, BBSHIFT);
			goto out_free_log;
		}

	        log2_size -= BBSHIFT;
		if (log2_size > mp->m_sectbb_log) {
			xfs_warn(mp, "Log sector size too large (0x%x > 0x%x)",
				log2_size, mp->m_sectbb_log);
			goto out_free_log;
		}

		/* for larger sector sizes, must have v2 or external log */
		if (log2_size && log->l_logBBstart > 0 &&
			    !xfs_has_logv2(mp)) {
			xfs_warn(mp,
		"log sector size (0x%x) invalid for configuration.",
				log2_size);
			goto out_free_log;
		}
	}
	log->l_sectBBsize = 1 << log2_size;

	xlog_get_iclog_buffer_size(mp, log);

	spin_lock_init(&log->l_icloglock);
	init_waitqueue_head(&log->l_flush_wait);

	iclogp = &log->l_iclog;
	/*
	 * The amount of memory to allocate for the iclog structure is
	 * rather funky due to the way the structure is defined.  It is
	 * done this way so that we can use different sizes for machines
	 * with different amounts of memory.  See the definition of
	 * xlog_in_core_t in xfs_log_priv.h for details.
	 */
	ASSERT(log->l_iclog_size >= 4096);
	for (i = 0; i < log->l_iclog_bufs; i++) {
		size_t bvec_size = howmany(log->l_iclog_size, PAGE_SIZE) *
				sizeof(struct bio_vec);

		iclog = kzalloc(sizeof(*iclog) + bvec_size,
				GFP_KERNEL | __GFP_RETRY_MAYFAIL);
		if (!iclog)
			goto out_free_iclog;

		*iclogp = iclog;
		iclog->ic_prev = prev_iclog;
		prev_iclog = iclog;

		iclog->ic_data = kvzalloc(log->l_iclog_size,
				GFP_KERNEL | __GFP_RETRY_MAYFAIL);
		if (!iclog->ic_data)
			goto out_free_iclog;
		head = &iclog->ic_header;
		memset(head, 0, sizeof(xlog_rec_header_t));
		head->h_magicno = cpu_to_be32(XLOG_HEADER_MAGIC_NUM);
		head->h_version = cpu_to_be32(
			xfs_has_logv2(log->l_mp) ? 2 : 1);
		head->h_size = cpu_to_be32(log->l_iclog_size);
		/* new fields */
		head->h_fmt = cpu_to_be32(XLOG_FMT);
		memcpy(&head->h_fs_uuid, &mp->m_sb.sb_uuid, sizeof(uuid_t));

		iclog->ic_size = log->l_iclog_size - log->l_iclog_hsize;
		iclog->ic_state = XLOG_STATE_ACTIVE;
		iclog->ic_log = log;
		atomic_set(&iclog->ic_refcnt, 0);
		INIT_LIST_HEAD(&iclog->ic_callbacks);
		iclog->ic_datap = (void *)iclog->ic_data + log->l_iclog_hsize;

		init_waitqueue_head(&iclog->ic_force_wait);
		init_waitqueue_head(&iclog->ic_write_wait);
		INIT_WORK(&iclog->ic_end_io_work, xlog_ioend_work);
		sema_init(&iclog->ic_sema, 1);

		iclogp = &iclog->ic_next;
	}
	*iclogp = log->l_iclog;			/* complete ring */
	log->l_iclog->ic_prev = prev_iclog;	/* re-write 1st prev ptr */

	log->l_ioend_workqueue = alloc_workqueue("xfs-log/%s",
			XFS_WQFLAGS(WQ_FREEZABLE | WQ_MEM_RECLAIM |
				    WQ_HIGHPRI),
			0, mp->m_super->s_id);
	if (!log->l_ioend_workqueue)
		goto out_free_iclog;

	error = xlog_cil_init(log);
	if (error)
		goto out_destroy_workqueue;
	return log;

out_destroy_workqueue:
	destroy_workqueue(log->l_ioend_workqueue);
out_free_iclog:
	for (iclog = log->l_iclog; iclog; iclog = prev_iclog) {
		prev_iclog = iclog->ic_next;
		kvfree(iclog->ic_data);
		kfree(iclog);
		if (prev_iclog == log->l_iclog)
			break;
	}
out_free_log:
	kfree(log);
out:
	return ERR_PTR(error);
}	/* xlog_alloc_log */

/*
 * Stamp cycle number in every block
 */
STATIC void
xlog_pack_data(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	int			roundoff)
{
	int			i, j, k;
	int			size = iclog->ic_offset + roundoff;
	__be32			cycle_lsn;
	char			*dp;

	cycle_lsn = CYCLE_LSN_DISK(iclog->ic_header.h_lsn);

	dp = iclog->ic_datap;
	for (i = 0; i < BTOBB(size); i++) {
		if (i >= (XLOG_HEADER_CYCLE_SIZE / BBSIZE))
			break;
		iclog->ic_header.h_cycle_data[i] = *(__be32 *)dp;
		*(__be32 *)dp = cycle_lsn;
		dp += BBSIZE;
	}

	if (xfs_has_logv2(log->l_mp)) {
		xlog_in_core_2_t *xhdr = iclog->ic_data;

		for ( ; i < BTOBB(size); i++) {
			j = i / (XLOG_HEADER_CYCLE_SIZE / BBSIZE);
			k = i % (XLOG_HEADER_CYCLE_SIZE / BBSIZE);
			xhdr[j].hic_xheader.xh_cycle_data[k] = *(__be32 *)dp;
			*(__be32 *)dp = cycle_lsn;
			dp += BBSIZE;
		}

		for (i = 1; i < log->l_iclog_heads; i++)
			xhdr[i].hic_xheader.xh_cycle = cycle_lsn;
	}
}

/*
 * Calculate the checksum for a log buffer.
 *
 * This is a little more complicated than it should be because the various
 * headers and the actual data are non-contiguous.
 */
__le32
xlog_cksum(
	struct xlog		*log,
	struct xlog_rec_header	*rhead,
	char			*dp,
	int			size)
{
	uint32_t		crc;

	/* first generate the crc for the record header ... */
	crc = xfs_start_cksum_update((char *)rhead,
			      sizeof(struct xlog_rec_header),
			      offsetof(struct xlog_rec_header, h_crc));

	/* ... then for additional cycle data for v2 logs ... */
	if (xfs_has_logv2(log->l_mp)) {
		union xlog_in_core2 *xhdr = (union xlog_in_core2 *)rhead;
		int		i;
		int		xheads;

		xheads = DIV_ROUND_UP(size, XLOG_HEADER_CYCLE_SIZE);

		for (i = 1; i < xheads; i++) {
			crc = crc32c(crc, &xhdr[i].hic_xheader,
				     sizeof(struct xlog_rec_ext_header));
		}
	}

	/* ... and finally for the payload */
	crc = crc32c(crc, dp, size);

	return xfs_end_cksum(crc);
}

static void
xlog_bio_end_io(
	struct bio		*bio)
{
	struct xlog_in_core	*iclog = bio->bi_private;

	queue_work(iclog->ic_log->l_ioend_workqueue,
		   &iclog->ic_end_io_work);
}

STATIC void
xlog_write_iclog(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	uint64_t		bno,
	unsigned int		count)
{
	ASSERT(bno < log->l_logBBsize);
	trace_xlog_iclog_write(iclog, _RET_IP_);

	/*
	 * We lock the iclogbufs here so that we can serialise against I/O
	 * completion during unmount.  We might be processing a shutdown
	 * triggered during unmount, and that can occur asynchronously to the
	 * unmount thread, and hence we need to ensure that completes before
	 * tearing down the iclogbufs.  Hence we need to hold the buffer lock
	 * across the log IO to archieve that.
	 */
	down(&iclog->ic_sema);
	if (xlog_is_shutdown(log)) {
		/*
		 * It would seem logical to return EIO here, but we rely on
		 * the log state machine to propagate I/O errors instead of
		 * doing it here.  We kick of the state machine and unlock
		 * the buffer manually, the code needs to be kept in sync
		 * with the I/O completion path.
		 */
		goto sync;
	}

	/*
	 * We use REQ_SYNC | REQ_IDLE here to tell the block layer the are more
	 * IOs coming immediately after this one. This prevents the block layer
	 * writeback throttle from throttling log writes behind background
	 * metadata writeback and causing priority inversions.
	 */
	bio_init(&iclog->ic_bio, log->l_targ->bt_bdev, iclog->ic_bvec,
		 howmany(count, PAGE_SIZE),
		 REQ_OP_WRITE | REQ_META | REQ_SYNC | REQ_IDLE);
	iclog->ic_bio.bi_iter.bi_sector = log->l_logBBstart + bno;
	iclog->ic_bio.bi_end_io = xlog_bio_end_io;
	iclog->ic_bio.bi_private = iclog;

	if (iclog->ic_flags & XLOG_ICL_NEED_FLUSH) {
		iclog->ic_bio.bi_opf |= REQ_PREFLUSH;
		/*
		 * For external log devices, we also need to flush the data
		 * device cache first to ensure all metadata writeback covered
		 * by the LSN in this iclog is on stable storage. This is slow,
		 * but it *must* complete before we issue the external log IO.
		 *
		 * If the flush fails, we cannot conclude that past metadata
		 * writeback from the log succeeded.  Repeating the flush is
		 * not possible, hence we must shut down with log IO error to
		 * avoid shutdown re-entering this path and erroring out again.
		 */
		if (log->l_targ != log->l_mp->m_ddev_targp &&
		    blkdev_issue_flush(log->l_mp->m_ddev_targp->bt_bdev))
			goto shutdown;
	}
	if (iclog->ic_flags & XLOG_ICL_NEED_FUA)
		iclog->ic_bio.bi_opf |= REQ_FUA;

	iclog->ic_flags &= ~(XLOG_ICL_NEED_FLUSH | XLOG_ICL_NEED_FUA);

	if (is_vmalloc_addr(iclog->ic_data)) {
		if (!bio_add_vmalloc(&iclog->ic_bio, iclog->ic_data, count))
			goto shutdown;
	} else {
		bio_add_virt_nofail(&iclog->ic_bio, iclog->ic_data, count);
	}

	/*
	 * If this log buffer would straddle the end of the log we will have
	 * to split it up into two bios, so that we can continue at the start.
	 */
	if (bno + BTOBB(count) > log->l_logBBsize) {
		struct bio *split;

		split = bio_split(&iclog->ic_bio, log->l_logBBsize - bno,
				  GFP_NOIO, &fs_bio_set);
		bio_chain(split, &iclog->ic_bio);
		submit_bio(split);

		/* restart at logical offset zero for the remainder */
		iclog->ic_bio.bi_iter.bi_sector = log->l_logBBstart;
	}

	submit_bio(&iclog->ic_bio);
	return;
shutdown:
	xlog_force_shutdown(log, SHUTDOWN_LOG_IO_ERROR);
sync:
	xlog_state_done_syncing(iclog);
	up(&iclog->ic_sema);
}

/*
 * We need to bump cycle number for the part of the iclog that is
 * written to the start of the log. Watch out for the header magic
 * number case, though.
 */
static void
xlog_split_iclog(
	struct xlog		*log,
	void			*data,
	uint64_t		bno,
	unsigned int		count)
{
	unsigned int		split_offset = BBTOB(log->l_logBBsize - bno);
	unsigned int		i;

	for (i = split_offset; i < count; i += BBSIZE) {
		uint32_t cycle = get_unaligned_be32(data + i);

		if (++cycle == XLOG_HEADER_MAGIC_NUM)
			cycle++;
		put_unaligned_be32(cycle, data + i);
	}
}

static int
xlog_calc_iclog_size(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	uint32_t		*roundoff)
{
	uint32_t		count_init, count;

	/* Add for LR header */
	count_init = log->l_iclog_hsize + iclog->ic_offset;
	count = roundup(count_init, log->l_iclog_roundoff);

	*roundoff = count - count_init;

	ASSERT(count >= count_init);
	ASSERT(*roundoff < log->l_iclog_roundoff);
	return count;
}

/*
 * Flush out the in-core log (iclog) to the on-disk log in an asynchronous
 * fashion.  Previously, we should have moved the current iclog
 * ptr in the log to point to the next available iclog.  This allows further
 * write to continue while this code syncs out an iclog ready to go.
 * Before an in-core log can be written out, the data section must be scanned
 * to save away the 1st word of each BBSIZE block into the header.  We replace
 * it with the current cycle count.  Each BBSIZE block is tagged with the
 * cycle count because there in an implicit assumption that drives will
 * guarantee that entire 512 byte blocks get written at once.  In other words,
 * we can't have part of a 512 byte block written and part not written.  By
 * tagging each block, we will know which blocks are valid when recovering
 * after an unclean shutdown.
 *
 * This routine is single threaded on the iclog.  No other thread can be in
 * this routine with the same iclog.  Changing contents of iclog can there-
 * fore be done without grabbing the state machine lock.  Updating the global
 * log will require grabbing the lock though.
 *
 * The entire log manager uses a logical block numbering scheme.  Only
 * xlog_write_iclog knows about the fact that the log may not start with
 * block zero on a given device.
 */
STATIC void
xlog_sync(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	struct xlog_ticket	*ticket)
{
	unsigned int		count;		/* byte count of bwrite */
	unsigned int		roundoff;       /* roundoff to BB or stripe */
	uint64_t		bno;
	unsigned int		size;

	ASSERT(atomic_read(&iclog->ic_refcnt) == 0);
	trace_xlog_iclog_sync(iclog, _RET_IP_);

	count = xlog_calc_iclog_size(log, iclog, &roundoff);

	/*
	 * If we have a ticket, account for the roundoff via the ticket
	 * reservation to avoid touching the hot grant heads needlessly.
	 * Otherwise, we have to move grant heads directly.
	 */
	if (ticket) {
		ticket->t_curr_res -= roundoff;
	} else {
		xlog_grant_add_space(&log->l_reserve_head, roundoff);
		xlog_grant_add_space(&log->l_write_head, roundoff);
	}

	/* put cycle number in every block */
	xlog_pack_data(log, iclog, roundoff);

	/* real byte length */
	size = iclog->ic_offset;
	if (xfs_has_logv2(log->l_mp))
		size += roundoff;
	iclog->ic_header.h_len = cpu_to_be32(size);

	XFS_STATS_INC(log->l_mp, xs_log_writes);
	XFS_STATS_ADD(log->l_mp, xs_log_blocks, BTOBB(count));

	bno = BLOCK_LSN(be64_to_cpu(iclog->ic_header.h_lsn));

	/* Do we need to split this write into 2 parts? */
	if (bno + BTOBB(count) > log->l_logBBsize)
		xlog_split_iclog(log, &iclog->ic_header, bno, count);

	/* calculcate the checksum */
	iclog->ic_header.h_crc = xlog_cksum(log, &iclog->ic_header,
					    iclog->ic_datap, size);
	/*
	 * Intentionally corrupt the log record CRC based on the error injection
	 * frequency, if defined. This facilitates testing log recovery in the
	 * event of torn writes. Hence, set the IOABORT state to abort the log
	 * write on I/O completion and shutdown the fs. The subsequent mount
	 * detects the bad CRC and attempts to recover.
	 */
#ifdef DEBUG
	if (XFS_TEST_ERROR(false, log->l_mp, XFS_ERRTAG_LOG_BAD_CRC)) {
		iclog->ic_header.h_crc &= cpu_to_le32(0xAAAAAAAA);
		iclog->ic_fail_crc = true;
		xfs_warn(log->l_mp,
	"Intentionally corrupted log record at LSN 0x%llx. Shutdown imminent.",
			 be64_to_cpu(iclog->ic_header.h_lsn));
	}
#endif
	xlog_verify_iclog(log, iclog, count);
	xlog_write_iclog(log, iclog, bno, count);
}

/*
 * Deallocate a log structure
 */
STATIC void
xlog_dealloc_log(
	struct xlog	*log)
{
	xlog_in_core_t	*iclog, *next_iclog;
	int		i;

	/*
	 * Destroy the CIL after waiting for iclog IO completion because an
	 * iclog EIO error will try to shut down the log, which accesses the
	 * CIL to wake up the waiters.
	 */
	xlog_cil_destroy(log);

	iclog = log->l_iclog;
	for (i = 0; i < log->l_iclog_bufs; i++) {
		next_iclog = iclog->ic_next;
		kvfree(iclog->ic_data);
		kfree(iclog);
		iclog = next_iclog;
	}

	log->l_mp->m_log = NULL;
	destroy_workqueue(log->l_ioend_workqueue);
	kfree(log);
}

/*
 * Update counters atomically now that memcpy is done.
 */
static inline void
xlog_state_finish_copy(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	int			record_cnt,
	int			copy_bytes)
{
	lockdep_assert_held(&log->l_icloglock);

	be32_add_cpu(&iclog->ic_header.h_num_logops, record_cnt);
	iclog->ic_offset += copy_bytes;
}

/*
 * print out info relating to regions written which consume
 * the reservation
 */
void
xlog_print_tic_res(
	struct xfs_mount	*mp,
	struct xlog_ticket	*ticket)
{
	xfs_warn(mp, "ticket reservation summary:");
	xfs_warn(mp, "  unit res    = %d bytes", ticket->t_unit_res);
	xfs_warn(mp, "  current res = %d bytes", ticket->t_curr_res);
	xfs_warn(mp, "  original count  = %d", ticket->t_ocnt);
	xfs_warn(mp, "  remaining count = %d", ticket->t_cnt);
}

/*
 * Print a summary of the transaction.
 */
void
xlog_print_trans(
	struct xfs_trans	*tp)
{
	struct xfs_mount	*mp = tp->t_mountp;
	struct xfs_log_item	*lip;

	/* dump core transaction and ticket info */
	xfs_warn(mp, "transaction summary:");
	xfs_warn(mp, "  log res   = %d", tp->t_log_res);
	xfs_warn(mp, "  log count = %d", tp->t_log_count);
	xfs_warn(mp, "  flags     = 0x%x", tp->t_flags);

	xlog_print_tic_res(mp, tp->t_ticket);

	/* dump each log item */
	list_for_each_entry(lip, &tp->t_items, li_trans) {
		struct xfs_log_vec	*lv = lip->li_lv;
		struct xfs_log_iovec	*vec;
		int			i;

		xfs_warn(mp, "log item: ");
		xfs_warn(mp, "  type	= 0x%x", lip->li_type);
		xfs_warn(mp, "  flags	= 0x%lx", lip->li_flags);
		if (!lv)
			continue;
		xfs_warn(mp, "  niovecs	= %d", lv->lv_niovecs);
		xfs_warn(mp, "  alloc_size = %d", lv->lv_alloc_size);
		xfs_warn(mp, "  bytes	= %d", lv->lv_bytes);
		xfs_warn(mp, "  buf used= %d", lv->lv_buf_used);

		/* dump each iovec for the log item */
		vec = lv->lv_iovecp;
		for (i = 0; i < lv->lv_niovecs; i++) {
			int dumplen = min(vec->i_len, 32);

			xfs_warn(mp, "  iovec[%d]", i);
			xfs_warn(mp, "    type	= 0x%x", vec->i_type);
			xfs_warn(mp, "    len	= %d", vec->i_len);
			xfs_warn(mp, "    first %d bytes of iovec[%d]:", dumplen, i);
			xfs_hex_dump(vec->i_addr, dumplen);

			vec++;
		}
	}
}

static inline void
xlog_write_iovec(
	struct xlog_in_core	*iclog,
	uint32_t		*log_offset,
	void			*data,
	uint32_t		write_len,
	int			*bytes_left,
	uint32_t		*record_cnt,
	uint32_t		*data_cnt)
{
	ASSERT(*log_offset < iclog->ic_log->l_iclog_size);
	ASSERT(*log_offset % sizeof(int32_t) == 0);
	ASSERT(write_len % sizeof(int32_t) == 0);

	memcpy(iclog->ic_datap + *log_offset, data, write_len);
	*log_offset += write_len;
	*bytes_left -= write_len;
	(*record_cnt)++;
	*data_cnt += write_len;
}

/*
 * Write log vectors into a single iclog which is guaranteed by the caller
 * to have enough space to write the entire log vector into.
 */
static void
xlog_write_full(
	struct xfs_log_vec	*lv,
	struct xlog_ticket	*ticket,
	struct xlog_in_core	*iclog,
	uint32_t		*log_offset,
	uint32_t		*len,
	uint32_t		*record_cnt,
	uint32_t		*data_cnt)
{
	int			index;

	ASSERT(*log_offset + *len <= iclog->ic_size ||
		iclog->ic_state == XLOG_STATE_WANT_SYNC);

	/*
	 * Ordered log vectors have no regions to write so this
	 * loop will naturally skip them.
	 */
	for (index = 0; index < lv->lv_niovecs; index++) {
		struct xfs_log_iovec	*reg = &lv->lv_iovecp[index];
		struct xlog_op_header	*ophdr = reg->i_addr;

		ophdr->oh_tid = cpu_to_be32(ticket->t_tid);
		xlog_write_iovec(iclog, log_offset, reg->i_addr,
				reg->i_len, len, record_cnt, data_cnt);
	}
}

static int
xlog_write_get_more_iclog_space(
	struct xlog_ticket	*ticket,
	struct xlog_in_core	**iclogp,
	uint32_t		*log_offset,
	uint32_t		len,
	uint32_t		*record_cnt,
	uint32_t		*data_cnt)
{
	struct xlog_in_core	*iclog = *iclogp;
	struct xlog		*log = iclog->ic_log;
	int			error;

	spin_lock(&log->l_icloglock);
	ASSERT(iclog->ic_state == XLOG_STATE_WANT_SYNC);
	xlog_state_finish_copy(log, iclog, *record_cnt, *data_cnt);
	error = xlog_state_release_iclog(log, iclog, ticket);
	spin_unlock(&log->l_icloglock);
	if (error)
		return error;

	error = xlog_state_get_iclog_space(log, len, &iclog, ticket,
					log_offset);
	if (error)
		return error;
	*record_cnt = 0;
	*data_cnt = 0;
	*iclogp = iclog;
	return 0;
}

/*
 * Write log vectors into a single iclog which is smaller than the current chain
 * length. We write until we cannot fit a full record into the remaining space
 * and then stop. We return the log vector that is to be written that cannot
 * wholly fit in the iclog.
 */
static int
xlog_write_partial(
	struct xfs_log_vec	*lv,
	struct xlog_ticket	*ticket,
	struct xlog_in_core	**iclogp,
	uint32_t		*log_offset,
	uint32_t		*len,
	uint32_t		*record_cnt,
	uint32_t		*data_cnt)
{
	struct xlog_in_core	*iclog = *iclogp;
	struct xlog_op_header	*ophdr;
	int			index = 0;
	uint32_t		rlen;
	int			error;

	/* walk the logvec, copying until we run out of space in the iclog */
	for (index = 0; index < lv->lv_niovecs; index++) {
		struct xfs_log_iovec	*reg = &lv->lv_iovecp[index];
		uint32_t		reg_offset = 0;

		/*
		 * The first region of a continuation must have a non-zero
		 * length otherwise log recovery will just skip over it and
		 * start recovering from the next opheader it finds. Because we
		 * mark the next opheader as a continuation, recovery will then
		 * incorrectly add the continuation to the previous region and
		 * that breaks stuff.
		 *
		 * Hence if there isn't space for region data after the
		 * opheader, then we need to start afresh with a new iclog.
		 */
		if (iclog->ic_size - *log_offset <=
					sizeof(struct xlog_op_header)) {
			error = xlog_write_get_more_iclog_space(ticket,
					&iclog, log_offset, *len, record_cnt,
					data_cnt);
			if (error)
				return error;
		}

		ophdr = reg->i_addr;
		rlen = min_t(uint32_t, reg->i_len, iclog->ic_size - *log_offset);

		ophdr->oh_tid = cpu_to_be32(ticket->t_tid);
		ophdr->oh_len = cpu_to_be32(rlen - sizeof(struct xlog_op_header));
		if (rlen != reg->i_len)
			ophdr->oh_flags |= XLOG_CONTINUE_TRANS;

		xlog_write_iovec(iclog, log_offset, reg->i_addr,
				rlen, len, record_cnt, data_cnt);

		/* If we wrote the whole region, move to the next. */
		if (rlen == reg->i_len)
			continue;

		/*
		 * We now have a partially written iovec, but it can span
		 * multiple iclogs so we loop here. First we release the iclog
		 * we currently have, then we get a new iclog and add a new
		 * opheader. Then we continue copying from where we were until
		 * we either complete the iovec or fill the iclog. If we
		 * complete the iovec, then we increment the index and go right
		 * back to the top of the outer loop. if we fill the iclog, we
		 * run the inner loop again.
		 *
		 * This is complicated by the tail of a region using all the
		 * space in an iclog and hence requiring us to release the iclog
		 * and get a new one before returning to the outer loop. We must
		 * always guarantee that we exit this inner loop with at least
		 * space for log transaction opheaders left in the current
		 * iclog, hence we cannot just terminate the loop at the end
		 * of the of the continuation. So we loop while there is no
		 * space left in the current iclog, and check for the end of the
		 * continuation after getting a new iclog.
		 */
		do {
			/*
			 * Ensure we include the continuation opheader in the
			 * space we need in the new iclog by adding that size
			 * to the length we require. This continuation opheader
			 * needs to be accounted to the ticket as the space it
			 * consumes hasn't been accounted to the lv we are
			 * writing.
			 */
			error = xlog_write_get_more_iclog_space(ticket,
					&iclog, log_offset,
					*len + sizeof(struct xlog_op_header),
					record_cnt, data_cnt);
			if (error)
				return error;

			ophdr = iclog->ic_datap + *log_offset;
			ophdr->oh_tid = cpu_to_be32(ticket->t_tid);
			ophdr->oh_clientid = XFS_TRANSACTION;
			ophdr->oh_res2 = 0;
			ophdr->oh_flags = XLOG_WAS_CONT_TRANS;

			ticket->t_curr_res -= sizeof(struct xlog_op_header);
			*log_offset += sizeof(struct xlog_op_header);
			*data_cnt += sizeof(struct xlog_op_header);

			/*
			 * If rlen fits in the iclog, then end the region
			 * continuation. Otherwise we're going around again.
			 */
			reg_offset += rlen;
			rlen = reg->i_len - reg_offset;
			if (rlen <= iclog->ic_size - *log_offset)
				ophdr->oh_flags |= XLOG_END_TRANS;
			else
				ophdr->oh_flags |= XLOG_CONTINUE_TRANS;

			rlen = min_t(uint32_t, rlen, iclog->ic_size - *log_offset);
			ophdr->oh_len = cpu_to_be32(rlen);

			xlog_write_iovec(iclog, log_offset,
					reg->i_addr + reg_offset,
					rlen, len, record_cnt, data_cnt);

		} while (ophdr->oh_flags & XLOG_CONTINUE_TRANS);
	}

	/*
	 * No more iovecs remain in this logvec so return the next log vec to
	 * the caller so it can go back to fast path copying.
	 */
	*iclogp = iclog;
	return 0;
}

/*
 * Write some region out to in-core log
 *
 * This will be called when writing externally provided regions or when
 * writing out a commit record for a given transaction.
 *
 * General algorithm:
 *	1. Find total length of this write.  This may include adding to the
 *		lengths passed in.
 *	2. Check whether we violate the tickets reservation.
 *	3. While writing to this iclog
 *	    A. Reserve as much space in this iclog as can get
 *	    B. If this is first write, save away start lsn
 *	    C. While writing this region:
 *		1. If first write of transaction, write start record
 *		2. Write log operation header (header per region)
 *		3. Find out if we can fit entire region into this iclog
 *		4. Potentially, verify destination memcpy ptr
 *		5. Memcpy (partial) region
 *		6. If partial copy, release iclog; otherwise, continue
 *			copying more regions into current iclog
 *	4. Mark want sync bit (in simulation mode)
 *	5. Release iclog for potential flush to on-disk log.
 *
 * ERRORS:
 * 1.	Panic if reservation is overrun.  This should never happen since
 *	reservation amounts are generated internal to the filesystem.
 * NOTES:
 * 1. Tickets are single threaded data structures.
 * 2. The XLOG_END_TRANS & XLOG_CONTINUE_TRANS flags are passed down to the
 *	syncing routine.  When a single log_write region needs to span
 *	multiple in-core logs, the XLOG_CONTINUE_TRANS bit should be set
 *	on all log operation writes which don't contain the end of the
 *	region.  The XLOG_END_TRANS bit is used for the in-core log
 *	operation which contains the end of the continued log_write region.
 * 3. When xlog_state_get_iclog_space() grabs the rest of the current iclog,
 *	we don't really know exactly how much space will be used.  As a result,
 *	we don't update ic_offset until the end when we know exactly how many
 *	bytes have been written out.
 */
int
xlog_write(
	struct xlog		*log,
	struct xfs_cil_ctx	*ctx,
	struct list_head	*lv_chain,
	struct xlog_ticket	*ticket,
	uint32_t		len)

{
	struct xlog_in_core	*iclog = NULL;
	struct xfs_log_vec	*lv;
	uint32_t		record_cnt = 0;
	uint32_t		data_cnt = 0;
	int			error = 0;
	int			log_offset;

	if (ticket->t_curr_res < 0) {
		xfs_alert_tag(log->l_mp, XFS_PTAG_LOGRES,
		     "ctx ticket reservation ran out. Need to up reservation");
		xlog_print_tic_res(log->l_mp, ticket);
		xlog_force_shutdown(log, SHUTDOWN_LOG_IO_ERROR);
	}

	error = xlog_state_get_iclog_space(log, len, &iclog, ticket,
					   &log_offset);
	if (error)
		return error;

	ASSERT(log_offset <= iclog->ic_size - 1);

	/*
	 * If we have a context pointer, pass it the first iclog we are
	 * writing to so it can record state needed for iclog write
	 * ordering.
	 */
	if (ctx)
		xlog_cil_set_ctx_write_state(ctx, iclog);

	list_for_each_entry(lv, lv_chain, lv_list) {
		/*
		 * If the entire log vec does not fit in the iclog, punt it to
		 * the partial copy loop which can handle this case.
		 */
		if (lv->lv_niovecs &&
		    lv->lv_bytes > iclog->ic_size - log_offset) {
			error = xlog_write_partial(lv, ticket, &iclog,
					&log_offset, &len, &record_cnt,
					&data_cnt);
			if (error) {
				/*
				 * We have no iclog to release, so just return
				 * the error immediately.
				 */
				return error;
			}
		} else {
			xlog_write_full(lv, ticket, iclog, &log_offset,
					 &len, &record_cnt, &data_cnt);
		}
	}
	ASSERT(len == 0);

	/*
	 * We've already been guaranteed that the last writes will fit inside
	 * the current iclog, and hence it will already have the space used by
	 * those writes accounted to it. Hence we do not need to update the
	 * iclog with the number of bytes written here.
	 */
	spin_lock(&log->l_icloglock);
	xlog_state_finish_copy(log, iclog, record_cnt, 0);
	error = xlog_state_release_iclog(log, iclog, ticket);
	spin_unlock(&log->l_icloglock);

	return error;
}

static void
xlog_state_activate_iclog(
	struct xlog_in_core	*iclog,
	int			*iclogs_changed)
{
	ASSERT(list_empty_careful(&iclog->ic_callbacks));
	trace_xlog_iclog_activate(iclog, _RET_IP_);

	/*
	 * If the number of ops in this iclog indicate it just contains the
	 * dummy transaction, we can change state into IDLE (the second time
	 * around). Otherwise we should change the state into NEED a dummy.
	 * We don't need to cover the dummy.
	 */
	if (*iclogs_changed == 0 &&
	    iclog->ic_header.h_num_logops == cpu_to_be32(XLOG_COVER_OPS)) {
		*iclogs_changed = 1;
	} else {
		/*
		 * We have two dirty iclogs so start over.  This could also be
		 * num of ops indicating this is not the dummy going out.
		 */
		*iclogs_changed = 2;
	}

	iclog->ic_state	= XLOG_STATE_ACTIVE;
	iclog->ic_offset = 0;
	iclog->ic_header.h_num_logops = 0;
	memset(iclog->ic_header.h_cycle_data, 0,
		sizeof(iclog->ic_header.h_cycle_data));
	iclog->ic_header.h_lsn = 0;
	iclog->ic_header.h_tail_lsn = 0;
}

/*
 * Loop through all iclogs and mark all iclogs currently marked DIRTY as
 * ACTIVE after iclog I/O has completed.
 */
static void
xlog_state_activate_iclogs(
	struct xlog		*log,
	int			*iclogs_changed)
{
	struct xlog_in_core	*iclog = log->l_iclog;

	do {
		if (iclog->ic_state == XLOG_STATE_DIRTY)
			xlog_state_activate_iclog(iclog, iclogs_changed);
		/*
		 * The ordering of marking iclogs ACTIVE must be maintained, so
		 * an iclog doesn't become ACTIVE beyond one that is SYNCING.
		 */
		else if (iclog->ic_state != XLOG_STATE_ACTIVE)
			break;
	} while ((iclog = iclog->ic_next) != log->l_iclog);
}

static int
xlog_covered_state(
	int			prev_state,
	int			iclogs_changed)
{
	/*
	 * We go to NEED for any non-covering writes. We go to NEED2 if we just
	 * wrote the first covering record (DONE). We go to IDLE if we just
	 * wrote the second covering record (DONE2) and remain in IDLE until a
	 * non-covering write occurs.
	 */
	switch (prev_state) {
	case XLOG_STATE_COVER_IDLE:
		if (iclogs_changed == 1)
			return XLOG_STATE_COVER_IDLE;
		fallthrough;
	case XLOG_STATE_COVER_NEED:
	case XLOG_STATE_COVER_NEED2:
		break;
	case XLOG_STATE_COVER_DONE:
		if (iclogs_changed == 1)
			return XLOG_STATE_COVER_NEED2;
		break;
	case XLOG_STATE_COVER_DONE2:
		if (iclogs_changed == 1)
			return XLOG_STATE_COVER_IDLE;
		break;
	default:
		ASSERT(0);
	}

	return XLOG_STATE_COVER_NEED;
}

STATIC void
xlog_state_clean_iclog(
	struct xlog		*log,
	struct xlog_in_core	*dirty_iclog)
{
	int			iclogs_changed = 0;

	trace_xlog_iclog_clean(dirty_iclog, _RET_IP_);

	dirty_iclog->ic_state = XLOG_STATE_DIRTY;

	xlog_state_activate_iclogs(log, &iclogs_changed);
	wake_up_all(&dirty_iclog->ic_force_wait);

	if (iclogs_changed) {
		log->l_covered_state = xlog_covered_state(log->l_covered_state,
				iclogs_changed);
	}
}

STATIC xfs_lsn_t
xlog_get_lowest_lsn(
	struct xlog		*log)
{
	struct xlog_in_core	*iclog = log->l_iclog;
	xfs_lsn_t		lowest_lsn = 0, lsn;

	do {
		if (iclog->ic_state == XLOG_STATE_ACTIVE ||
		    iclog->ic_state == XLOG_STATE_DIRTY)
			continue;

		lsn = be64_to_cpu(iclog->ic_header.h_lsn);
		if ((lsn && !lowest_lsn) || XFS_LSN_CMP(lsn, lowest_lsn) < 0)
			lowest_lsn = lsn;
	} while ((iclog = iclog->ic_next) != log->l_iclog);

	return lowest_lsn;
}

/*
 * Return true if we need to stop processing, false to continue to the next
 * iclog. The caller will need to run callbacks if the iclog is returned in the
 * XLOG_STATE_CALLBACK state.
 */
static bool
xlog_state_iodone_process_iclog(
	struct xlog		*log,
	struct xlog_in_core	*iclog)
{
	xfs_lsn_t		lowest_lsn;
	xfs_lsn_t		header_lsn;

	switch (iclog->ic_state) {
	case XLOG_STATE_ACTIVE:
	case XLOG_STATE_DIRTY:
		/*
		 * Skip all iclogs in the ACTIVE & DIRTY states:
		 */
		return false;
	case XLOG_STATE_DONE_SYNC:
		/*
		 * Now that we have an iclog that is in the DONE_SYNC state, do
		 * one more check here to see if we have chased our tail around.
		 * If this is not the lowest lsn iclog, then we will leave it
		 * for another completion to process.
		 */
		header_lsn = be64_to_cpu(iclog->ic_header.h_lsn);
		lowest_lsn = xlog_get_lowest_lsn(log);
		if (lowest_lsn && XFS_LSN_CMP(lowest_lsn, header_lsn) < 0)
			return false;
		/*
		 * If there are no callbacks on this iclog, we can mark it clean
		 * immediately and return. Otherwise we need to run the
		 * callbacks.
		 */
		if (list_empty(&iclog->ic_callbacks)) {
			xlog_state_clean_iclog(log, iclog);
			return false;
		}
		trace_xlog_iclog_callback(iclog, _RET_IP_);
		iclog->ic_state = XLOG_STATE_CALLBACK;
		return false;
	default:
		/*
		 * Can only perform callbacks in order.  Since this iclog is not
		 * in the DONE_SYNC state, we skip the rest and just try to
		 * clean up.
		 */
		return true;
	}
}

/*
 * Loop over all the iclogs, running attached callbacks on them. Return true if
 * we ran any callbacks, indicating that we dropped the icloglock. We don't need
 * to handle transient shutdown state here at all because
 * xlog_state_shutdown_callbacks() will be run to do the necessary shutdown
 * cleanup of the callbacks.
 */
static bool
xlog_state_do_iclog_callbacks(
	struct xlog		*log)
		__releases(&log->l_icloglock)
		__acquires(&log->l_icloglock)
{
	struct xlog_in_core	*first_iclog = log->l_iclog;
	struct xlog_in_core	*iclog = first_iclog;
	bool			ran_callback = false;

	do {
		LIST_HEAD(cb_list);

		if (xlog_state_iodone_process_iclog(log, iclog))
			break;
		if (iclog->ic_state != XLOG_STATE_CALLBACK) {
			iclog = iclog->ic_next;
			continue;
		}
		list_splice_init(&iclog->ic_callbacks, &cb_list);
		spin_unlock(&log->l_icloglock);

		trace_xlog_iclog_callbacks_start(iclog, _RET_IP_);
		xlog_cil_process_committed(&cb_list);
		trace_xlog_iclog_callbacks_done(iclog, _RET_IP_);
		ran_callback = true;

		spin_lock(&log->l_icloglock);
		xlog_state_clean_iclog(log, iclog);
		iclog = iclog->ic_next;
	} while (iclog != first_iclog);

	return ran_callback;
}


/*
 * Loop running iclog completion callbacks until there are no more iclogs in a
 * state that can run callbacks.
 */
STATIC void
xlog_state_do_callback(
	struct xlog		*log)
{
	int			flushcnt = 0;
	int			repeats = 0;

	spin_lock(&log->l_icloglock);
	while (xlog_state_do_iclog_callbacks(log)) {
		if (xlog_is_shutdown(log))
			break;

		if (++repeats > 5000) {
			flushcnt += repeats;
			repeats = 0;
			xfs_warn(log->l_mp,
				"%s: possible infinite loop (%d iterations)",
				__func__, flushcnt);
		}
	}

	if (log->l_iclog->ic_state == XLOG_STATE_ACTIVE)
		wake_up_all(&log->l_flush_wait);

	spin_unlock(&log->l_icloglock);
}


/*
 * Finish transitioning this iclog to the dirty state.
 *
 * Callbacks could take time, so they are done outside the scope of the
 * global state machine log lock.
 */
STATIC void
xlog_state_done_syncing(
	struct xlog_in_core	*iclog)
{
	struct xlog		*log = iclog->ic_log;

	spin_lock(&log->l_icloglock);
	ASSERT(atomic_read(&iclog->ic_refcnt) == 0);
	trace_xlog_iclog_sync_done(iclog, _RET_IP_);

	/*
	 * If we got an error, either on the first buffer, or in the case of
	 * split log writes, on the second, we shut down the file system and
	 * no iclogs should ever be attempted to be written to disk again.
	 */
	if (!xlog_is_shutdown(log)) {
		ASSERT(iclog->ic_state == XLOG_STATE_SYNCING);
		iclog->ic_state = XLOG_STATE_DONE_SYNC;
	}

	/*
	 * Someone could be sleeping prior to writing out the next
	 * iclog buffer, we wake them all, one will get to do the
	 * I/O, the others get to wait for the result.
	 */
	wake_up_all(&iclog->ic_write_wait);
	spin_unlock(&log->l_icloglock);
	xlog_state_do_callback(log);
}

/*
 * If the head of the in-core log ring is not (ACTIVE or DIRTY), then we must
 * sleep.  We wait on the flush queue on the head iclog as that should be
 * the first iclog to complete flushing. Hence if all iclogs are syncing,
 * we will wait here and all new writes will sleep until a sync completes.
 *
 * The in-core logs are used in a circular fashion. They are not used
 * out-of-order even when an iclog past the head is free.
 *
 * return:
 *	* log_offset where xlog_write() can start writing into the in-core
 *		log's data space.
 *	* in-core log pointer to which xlog_write() should write.
 *	* boolean indicating this is a continued write to an in-core log.
 *		If this is the last write, then the in-core log's offset field
 *		needs to be incremented, depending on the amount of data which
 *		is copied.
 */
STATIC int
xlog_state_get_iclog_space(
	struct xlog		*log,
	int			len,
	struct xlog_in_core	**iclogp,
	struct xlog_ticket	*ticket,
	int			*logoffsetp)
{
	int		  log_offset;
	xlog_rec_header_t *head;
	xlog_in_core_t	  *iclog;

restart:
	spin_lock(&log->l_icloglock);
	if (xlog_is_shutdown(log)) {
		spin_unlock(&log->l_icloglock);
		return -EIO;
	}

	iclog = log->l_iclog;
	if (iclog->ic_state != XLOG_STATE_ACTIVE) {
		XFS_STATS_INC(log->l_mp, xs_log_noiclogs);

		/* Wait for log writes to have flushed */
		xlog_wait(&log->l_flush_wait, &log->l_icloglock);
		goto restart;
	}

	head = &iclog->ic_header;

	atomic_inc(&iclog->ic_refcnt);	/* prevents sync */
	log_offset = iclog->ic_offset;

	trace_xlog_iclog_get_space(iclog, _RET_IP_);

	/* On the 1st write to an iclog, figure out lsn.  This works
	 * if iclogs marked XLOG_STATE_WANT_SYNC always write out what they are
	 * committing to.  If the offset is set, that's how many blocks
	 * must be written.
	 */
	if (log_offset == 0) {
		ticket->t_curr_res -= log->l_iclog_hsize;
		head->h_cycle = cpu_to_be32(log->l_curr_cycle);
		head->h_lsn = cpu_to_be64(
			xlog_assign_lsn(log->l_curr_cycle, log->l_curr_block));
		ASSERT(log->l_curr_block >= 0);
	}

	/* If there is enough room to write everything, then do it.  Otherwise,
	 * claim the rest of the region and make sure the XLOG_STATE_WANT_SYNC
	 * bit is on, so this will get flushed out.  Don't update ic_offset
	 * until you know exactly how many bytes get copied.  Therefore, wait
	 * until later to update ic_offset.
	 *
	 * xlog_write() algorithm assumes that at least 2 xlog_op_header_t's
	 * can fit into remaining data section.
	 */
	if (iclog->ic_size - iclog->ic_offset < 2*sizeof(xlog_op_header_t)) {
		int		error = 0;

		xlog_state_switch_iclogs(log, iclog, iclog->ic_size);

		/*
		 * If we are the only one writing to this iclog, sync it to
		 * disk.  We need to do an atomic compare and decrement here to
		 * avoid racing with concurrent atomic_dec_and_lock() calls in
		 * xlog_state_release_iclog() when there is more than one
		 * reference to the iclog.
		 */
		if (!atomic_add_unless(&iclog->ic_refcnt, -1, 1))
			error = xlog_state_release_iclog(log, iclog, ticket);
		spin_unlock(&log->l_icloglock);
		if (error)
			return error;
		goto restart;
	}

	/* Do we have enough room to write the full amount in the remainder
	 * of this iclog?  Or must we continue a write on the next iclog and
	 * mark this iclog as completely taken?  In the case where we switch
	 * iclogs (to mark it taken), this particular iclog will release/sync
	 * to disk in xlog_write().
	 */
	if (len <= iclog->ic_size - iclog->ic_offset)
		iclog->ic_offset += len;
	else
		xlog_state_switch_iclogs(log, iclog, iclog->ic_size);
	*iclogp = iclog;

	ASSERT(iclog->ic_offset <= iclog->ic_size);
	spin_unlock(&log->l_icloglock);

	*logoffsetp = log_offset;
	return 0;
}

/*
 * The first cnt-1 times a ticket goes through here we don't need to move the
 * grant write head because the permanent reservation has reserved cnt times the
 * unit amount.  Release part of current permanent unit reservation and reset
 * current reservation to be one units worth.  Also move grant reservation head
 * forward.
 */
void
xfs_log_ticket_regrant(
	struct xlog		*log,
	struct xlog_ticket	*ticket)
{
	trace_xfs_log_ticket_regrant(log, ticket);

	if (ticket->t_cnt > 0)
		ticket->t_cnt--;

	xlog_grant_sub_space(&log->l_reserve_head, ticket->t_curr_res);
	xlog_grant_sub_space(&log->l_write_head, ticket->t_curr_res);
	ticket->t_curr_res = ticket->t_unit_res;

	trace_xfs_log_ticket_regrant_sub(log, ticket);

	/* just return if we still have some of the pre-reserved space */
	if (!ticket->t_cnt) {
		xlog_grant_add_space(&log->l_reserve_head, ticket->t_unit_res);
		trace_xfs_log_ticket_regrant_exit(log, ticket);
	}

	xfs_log_ticket_put(ticket);
}

/*
 * Give back the space left from a reservation.
 *
 * All the information we need to make a correct determination of space left
 * is present.  For non-permanent reservations, things are quite easy.  The
 * count should have been decremented to zero.  We only need to deal with the
 * space remaining in the current reservation part of the ticket.  If the
 * ticket contains a permanent reservation, there may be left over space which
 * needs to be released.  A count of N means that N-1 refills of the current
 * reservation can be done before we need to ask for more space.  The first
 * one goes to fill up the first current reservation.  Once we run out of
 * space, the count will stay at zero and the only space remaining will be
 * in the current reservation field.
 */
void
xfs_log_ticket_ungrant(
	struct xlog		*log,
	struct xlog_ticket	*ticket)
{
	int			bytes;

	trace_xfs_log_ticket_ungrant(log, ticket);

	if (ticket->t_cnt > 0)
		ticket->t_cnt--;

	trace_xfs_log_ticket_ungrant_sub(log, ticket);

	/*
	 * If this is a permanent reservation ticket, we may be able to free
	 * up more space based on the remaining count.
	 */
	bytes = ticket->t_curr_res;
	if (ticket->t_cnt > 0) {
		ASSERT(ticket->t_flags & XLOG_TIC_PERM_RESERV);
		bytes += ticket->t_unit_res*ticket->t_cnt;
	}

	xlog_grant_sub_space(&log->l_reserve_head, bytes);
	xlog_grant_sub_space(&log->l_write_head, bytes);

	trace_xfs_log_ticket_ungrant_exit(log, ticket);

	xfs_log_space_wake(log->l_mp);
	xfs_log_ticket_put(ticket);
}

/*
 * This routine will mark the current iclog in the ring as WANT_SYNC and move
 * the current iclog pointer to the next iclog in the ring.
 */
void
xlog_state_switch_iclogs(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	int			eventual_size)
{
	ASSERT(iclog->ic_state == XLOG_STATE_ACTIVE);
	assert_spin_locked(&log->l_icloglock);
	trace_xlog_iclog_switch(iclog, _RET_IP_);

	if (!eventual_size)
		eventual_size = iclog->ic_offset;
	iclog->ic_state = XLOG_STATE_WANT_SYNC;
	iclog->ic_header.h_prev_block = cpu_to_be32(log->l_prev_block);
	log->l_prev_block = log->l_curr_block;
	log->l_prev_cycle = log->l_curr_cycle;

	/* roll log?: ic_offset changed later */
	log->l_curr_block += BTOBB(eventual_size)+BTOBB(log->l_iclog_hsize);

	/* Round up to next log-sunit */
	if (log->l_iclog_roundoff > BBSIZE) {
		uint32_t sunit_bb = BTOBB(log->l_iclog_roundoff);
		log->l_curr_block = roundup(log->l_curr_block, sunit_bb);
	}

	if (log->l_curr_block >= log->l_logBBsize) {
		/*
		 * Rewind the current block before the cycle is bumped to make
		 * sure that the combined LSN never transiently moves forward
		 * when the log wraps to the next cycle. This is to support the
		 * unlocked sample of these fields from xlog_valid_lsn(). Most
		 * other cases should acquire l_icloglock.
		 */
		log->l_curr_block -= log->l_logBBsize;
		ASSERT(log->l_curr_block >= 0);
		smp_wmb();
		log->l_curr_cycle++;
		if (log->l_curr_cycle == XLOG_HEADER_MAGIC_NUM)
			log->l_curr_cycle++;
	}
	ASSERT(iclog == log->l_iclog);
	log->l_iclog = iclog->ic_next;
}

/*
 * Force the iclog to disk and check if the iclog has been completed before
 * xlog_force_iclog() returns. This can happen on synchronous (e.g.
 * pmem) or fast async storage because we drop the icloglock to issue the IO.
 * If completion has already occurred, tell the caller so that it can avoid an
 * unnecessary wait on the iclog.
 */
static int
xlog_force_and_check_iclog(
	struct xlog_in_core	*iclog,
	bool			*completed)
{
	xfs_lsn_t		lsn = be64_to_cpu(iclog->ic_header.h_lsn);
	int			error;

	*completed = false;
	error = xlog_force_iclog(iclog);
	if (error)
		return error;

	/*
	 * If the iclog has already been completed and reused the header LSN
	 * will have been rewritten by completion
	 */
	if (be64_to_cpu(iclog->ic_header.h_lsn) != lsn)
		*completed = true;
	return 0;
}

/*
 * Write out all data in the in-core log as of this exact moment in time.
 *
 * Data may be written to the in-core log during this call.  However,
 * we don't guarantee this data will be written out.  A change from past
 * implementation means this routine will *not* write out zero length LRs.
 *
 * Basically, we try and perform an intelligent scan of the in-core logs.
 * If we determine there is no flushable data, we just return.  There is no
 * flushable data if:
 *
 *	1. the current iclog is active and has no data; the previous iclog
 *		is in the active or dirty state.
 *	2. the current iclog is dirty, and the previous iclog is in the
 *		active or dirty state.
 *
 * We may sleep if:
 *
 *	1. the current iclog is not in the active nor dirty state.
 *	2. the current iclog dirty, and the previous iclog is not in the
 *		active nor dirty state.
 *	3. the current iclog is active, and there is another thread writing
 *		to this particular iclog.
 *	4. a) the current iclog is active and has no other writers
 *	   b) when we return from flushing out this iclog, it is still
 *		not in the active nor dirty state.
 */
int
xfs_log_force(
	struct xfs_mount	*mp,
	uint			flags)
{
	struct xlog		*log = mp->m_log;
	struct xlog_in_core	*iclog;

	XFS_STATS_INC(mp, xs_log_force);
	trace_xfs_log_force(mp, 0, _RET_IP_);

	xlog_cil_force(log);

	spin_lock(&log->l_icloglock);
	if (xlog_is_shutdown(log))
		goto out_error;

	iclog = log->l_iclog;
	trace_xlog_iclog_force(iclog, _RET_IP_);

	if (iclog->ic_state == XLOG_STATE_DIRTY ||
	    (iclog->ic_state == XLOG_STATE_ACTIVE &&
	     atomic_read(&iclog->ic_refcnt) == 0 && iclog->ic_offset == 0)) {
		/*
		 * If the head is dirty or (active and empty), then we need to
		 * look at the previous iclog.
		 *
		 * If the previous iclog is active or dirty we are done.  There
		 * is nothing to sync out. Otherwise, we attach ourselves to the
		 * previous iclog and go to sleep.
		 */
		iclog = iclog->ic_prev;
	} else if (iclog->ic_state == XLOG_STATE_ACTIVE) {
		if (atomic_read(&iclog->ic_refcnt) == 0) {
			/* We have exclusive access to this iclog. */
			bool	completed;

			if (xlog_force_and_check_iclog(iclog, &completed))
				goto out_error;

			if (completed)
				goto out_unlock;
		} else {
			/*
			 * Someone else is still writing to this iclog, so we
			 * need to ensure that when they release the iclog it
			 * gets synced immediately as we may be waiting on it.
			 */
			xlog_state_switch_iclogs(log, iclog, 0);
		}
	}

	/*
	 * The iclog we are about to wait on may contain the checkpoint pushed
	 * by the above xlog_cil_force() call, but it may not have been pushed
	 * to disk yet. Like the ACTIVE case above, we need to make sure caches
	 * are flushed when this iclog is written.
	 */
	if (iclog->ic_state == XLOG_STATE_WANT_SYNC)
		iclog->ic_flags |= XLOG_ICL_NEED_FLUSH | XLOG_ICL_NEED_FUA;

	if (flags & XFS_LOG_SYNC)
		return xlog_wait_on_iclog(iclog);
out_unlock:
	spin_unlock(&log->l_icloglock);
	return 0;
out_error:
	spin_unlock(&log->l_icloglock);
	return -EIO;
}

/*
 * Force the log to a specific LSN.
 *
 * If an iclog with that lsn can be found:
 *	If it is in the DIRTY state, just return.
 *	If it is in the ACTIVE state, move the in-core log into the WANT_SYNC
 *		state and go to sleep or return.
 *	If it is in any other state, go to sleep or return.
 *
 * Synchronous forces are implemented with a wait queue.  All callers trying
 * to force a given lsn to disk must wait on the queue attached to the
 * specific in-core log.  When given in-core log finally completes its write
 * to disk, that thread will wake up all threads waiting on the queue.
 */
static int
xlog_force_lsn(
	struct xlog		*log,
	xfs_lsn_t		lsn,
	uint			flags,
	int			*log_flushed,
	bool			already_slept)
{
	struct xlog_in_core	*iclog;
	bool			completed;

	spin_lock(&log->l_icloglock);
	if (xlog_is_shutdown(log))
		goto out_error;

	iclog = log->l_iclog;
	while (be64_to_cpu(iclog->ic_header.h_lsn) != lsn) {
		trace_xlog_iclog_force_lsn(iclog, _RET_IP_);
		iclog = iclog->ic_next;
		if (iclog == log->l_iclog)
			goto out_unlock;
	}

	switch (iclog->ic_state) {
	case XLOG_STATE_ACTIVE:
		/*
		 * We sleep here if we haven't already slept (e.g. this is the
		 * first time we've looked at the correct iclog buf) and the
		 * buffer before us is going to be sync'ed.  The reason for this
		 * is that if we are doing sync transactions here, by waiting
		 * for the previous I/O to complete, we can allow a few more
		 * transactions into this iclog before we close it down.
		 *
		 * Otherwise, we mark the buffer WANT_SYNC, and bump up the
		 * refcnt so we can release the log (which drops the ref count).
		 * The state switch keeps new transaction commits from using
		 * this buffer.  When the current commits finish writing into
		 * the buffer, the refcount will drop to zero and the buffer
		 * will go out then.
		 */
		if (!already_slept &&
		    (iclog->ic_prev->ic_state == XLOG_STATE_WANT_SYNC ||
		     iclog->ic_prev->ic_state == XLOG_STATE_SYNCING)) {
			xlog_wait(&iclog->ic_prev->ic_write_wait,
					&log->l_icloglock);
			return -EAGAIN;
		}
		if (xlog_force_and_check_iclog(iclog, &completed))
			goto out_error;
		if (log_flushed)
			*log_flushed = 1;
		if (completed)
			goto out_unlock;
		break;
	case XLOG_STATE_WANT_SYNC:
		/*
		 * This iclog may contain the checkpoint pushed by the
		 * xlog_cil_force_seq() call, but there are other writers still
		 * accessing it so it hasn't been pushed to disk yet. Like the
		 * ACTIVE case above, we need to make sure caches are flushed
		 * when this iclog is written.
		 */
		iclog->ic_flags |= XLOG_ICL_NEED_FLUSH | XLOG_ICL_NEED_FUA;
		break;
	default:
		/*
		 * The entire checkpoint was written by the CIL force and is on
		 * its way to disk already. It will be stable when it
		 * completes, so we don't need to manipulate caches here at all.
		 * We just need to wait for completion if necessary.
		 */
		break;
	}

	if (flags & XFS_LOG_SYNC)
		return xlog_wait_on_iclog(iclog);
out_unlock:
	spin_unlock(&log->l_icloglock);
	return 0;
out_error:
	spin_unlock(&log->l_icloglock);
	return -EIO;
}

/*
 * Force the log to a specific checkpoint sequence.
 *
 * First force the CIL so that all the required changes have been flushed to the
 * iclogs. If the CIL force completed it will return a commit LSN that indicates
 * the iclog that needs to be flushed to stable storage. If the caller needs
 * a synchronous log force, we will wait on the iclog with the LSN returned by
 * xlog_cil_force_seq() to be completed.
 */
int
xfs_log_force_seq(
	struct xfs_mount	*mp,
	xfs_csn_t		seq,
	uint			flags,
	int			*log_flushed)
{
	struct xlog		*log = mp->m_log;
	xfs_lsn_t		lsn;
	int			ret;
	ASSERT(seq != 0);

	XFS_STATS_INC(mp, xs_log_force);
	trace_xfs_log_force(mp, seq, _RET_IP_);

	lsn = xlog_cil_force_seq(log, seq);
	if (lsn == NULLCOMMITLSN)
		return 0;

	ret = xlog_force_lsn(log, lsn, flags, log_flushed, false);
	if (ret == -EAGAIN) {
		XFS_STATS_INC(mp, xs_log_force_sleep);
		ret = xlog_force_lsn(log, lsn, flags, log_flushed, true);
	}
	return ret;
}

/*
 * Free a used ticket when its refcount falls to zero.
 */
void
xfs_log_ticket_put(
	struct xlog_ticket	*ticket)
{
	ASSERT(atomic_read(&ticket->t_ref) > 0);
	if (atomic_dec_and_test(&ticket->t_ref))
		kmem_cache_free(xfs_log_ticket_cache, ticket);
}

struct xlog_ticket *
xfs_log_ticket_get(
	struct xlog_ticket	*ticket)
{
	ASSERT(atomic_read(&ticket->t_ref) > 0);
	atomic_inc(&ticket->t_ref);
	return ticket;
}

/*
 * Figure out the total log space unit (in bytes) that would be
 * required for a log ticket.
 */
static int
xlog_calc_unit_res(
	struct xlog		*log,
	int			unit_bytes,
	int			*niclogs)
{
	int			iclog_space;
	uint			num_headers;

	/*
	 * Permanent reservations have up to 'cnt'-1 active log operations
	 * in the log.  A unit in this case is the amount of space for one
	 * of these log operations.  Normal reservations have a cnt of 1
	 * and their unit amount is the total amount of space required.
	 *
	 * The following lines of code account for non-transaction data
	 * which occupy space in the on-disk log.
	 *
	 * Normal form of a transaction is:
	 * <oph><trans-hdr><start-oph><reg1-oph><reg1><reg2-oph>...<commit-oph>
	 * and then there are LR hdrs, split-recs and roundoff at end of syncs.
	 *
	 * We need to account for all the leadup data and trailer data
	 * around the transaction data.
	 * And then we need to account for the worst case in terms of using
	 * more space.
	 * The worst case will happen if:
	 * - the placement of the transaction happens to be such that the
	 *   roundoff is at its maximum
	 * - the transaction data is synced before the commit record is synced
	 *   i.e. <transaction-data><roundoff> | <commit-rec><roundoff>
	 *   Therefore the commit record is in its own Log Record.
	 *   This can happen as the commit record is called with its
	 *   own region to xlog_write().
	 *   This then means that in the worst case, roundoff can happen for
	 *   the commit-rec as well.
	 *   The commit-rec is smaller than padding in this scenario and so it is
	 *   not added separately.
	 */

	/* for trans header */
	unit_bytes += sizeof(xlog_op_header_t);
	unit_bytes += sizeof(xfs_trans_header_t);

	/* for start-rec */
	unit_bytes += sizeof(xlog_op_header_t);

	/*
	 * for LR headers - the space for data in an iclog is the size minus
	 * the space used for the headers. If we use the iclog size, then we
	 * undercalculate the number of headers required.
	 *
	 * Furthermore - the addition of op headers for split-recs might
	 * increase the space required enough to require more log and op
	 * headers, so take that into account too.
	 *
	 * IMPORTANT: This reservation makes the assumption that if this
	 * transaction is the first in an iclog and hence has the LR headers
	 * accounted to it, then the remaining space in the iclog is
	 * exclusively for this transaction.  i.e. if the transaction is larger
	 * than the iclog, it will be the only thing in that iclog.
	 * Fundamentally, this means we must pass the entire log vector to
	 * xlog_write to guarantee this.
	 */
	iclog_space = log->l_iclog_size - log->l_iclog_hsize;
	num_headers = howmany(unit_bytes, iclog_space);

	/* for split-recs - ophdrs added when data split over LRs */
	unit_bytes += sizeof(xlog_op_header_t) * num_headers;

	/* add extra header reservations if we overrun */
	while (!num_headers ||
	       howmany(unit_bytes, iclog_space) > num_headers) {
		unit_bytes += sizeof(xlog_op_header_t);
		num_headers++;
	}
	unit_bytes += log->l_iclog_hsize * num_headers;

	/* for commit-rec LR header - note: padding will subsume the ophdr */
	unit_bytes += log->l_iclog_hsize;

	/* roundoff padding for transaction data and one for commit record */
	unit_bytes += 2 * log->l_iclog_roundoff;

	if (niclogs)
		*niclogs = num_headers;
	return unit_bytes;
}

int
xfs_log_calc_unit_res(
	struct xfs_mount	*mp,
	int			unit_bytes)
{
	return xlog_calc_unit_res(mp->m_log, unit_bytes, NULL);
}

/*
 * Allocate and initialise a new log ticket.
 */
struct xlog_ticket *
xlog_ticket_alloc(
	struct xlog		*log,
	int			unit_bytes,
	int			cnt,
	bool			permanent)
{
	struct xlog_ticket	*tic;
	int			unit_res;

	tic = kmem_cache_zalloc(xfs_log_ticket_cache,
			GFP_KERNEL | __GFP_NOFAIL);

	unit_res = xlog_calc_unit_res(log, unit_bytes, &tic->t_iclog_hdrs);

	atomic_set(&tic->t_ref, 1);
	tic->t_task		= current;
	INIT_LIST_HEAD(&tic->t_queue);
	tic->t_unit_res		= unit_res;
	tic->t_curr_res		= unit_res;
	tic->t_cnt		= cnt;
	tic->t_ocnt		= cnt;
	tic->t_tid		= get_random_u32();
	if (permanent)
		tic->t_flags |= XLOG_TIC_PERM_RESERV;

	return tic;
}

#if defined(DEBUG)
static void
xlog_verify_dump_tail(
	struct xlog		*log,
	struct xlog_in_core	*iclog)
{
	xfs_alert(log->l_mp,
"ran out of log space tail 0x%llx/0x%llx, head lsn 0x%llx, head 0x%x/0x%x, prev head 0x%x/0x%x",
			iclog ? be64_to_cpu(iclog->ic_header.h_tail_lsn) : -1,
			atomic64_read(&log->l_tail_lsn),
			log->l_ailp->ail_head_lsn,
			log->l_curr_cycle, log->l_curr_block,
			log->l_prev_cycle, log->l_prev_block);
	xfs_alert(log->l_mp,
"write grant 0x%llx, reserve grant 0x%llx, tail_space 0x%llx, size 0x%x, iclog flags 0x%x",
			atomic64_read(&log->l_write_head.grant),
			atomic64_read(&log->l_reserve_head.grant),
			log->l_tail_space, log->l_logsize,
			iclog ? iclog->ic_flags : -1);
}

/* Check if the new iclog will fit in the log. */
STATIC void
xlog_verify_tail_lsn(
	struct xlog		*log,
	struct xlog_in_core	*iclog)
{
	xfs_lsn_t	tail_lsn = be64_to_cpu(iclog->ic_header.h_tail_lsn);
	int		blocks;

	if (CYCLE_LSN(tail_lsn) == log->l_prev_cycle) {
		blocks = log->l_logBBsize -
				(log->l_prev_block - BLOCK_LSN(tail_lsn));
		if (blocks < BTOBB(iclog->ic_offset) +
					BTOBB(log->l_iclog_hsize)) {
			xfs_emerg(log->l_mp,
					"%s: ran out of log space", __func__);
			xlog_verify_dump_tail(log, iclog);
		}
		return;
	}

	if (CYCLE_LSN(tail_lsn) + 1 != log->l_prev_cycle) {
		xfs_emerg(log->l_mp, "%s: head has wrapped tail.", __func__);
		xlog_verify_dump_tail(log, iclog);
		return;
	}
	if (BLOCK_LSN(tail_lsn) == log->l_prev_block) {
		xfs_emerg(log->l_mp, "%s: tail wrapped", __func__);
		xlog_verify_dump_tail(log, iclog);
		return;
	}

	blocks = BLOCK_LSN(tail_lsn) - log->l_prev_block;
	if (blocks < BTOBB(iclog->ic_offset) + 1) {
		xfs_emerg(log->l_mp, "%s: ran out of iclog space", __func__);
		xlog_verify_dump_tail(log, iclog);
	}
}

/*
 * Perform a number of checks on the iclog before writing to disk.
 *
 * 1. Make sure the iclogs are still circular
 * 2. Make sure we have a good magic number
 * 3. Make sure we don't have magic numbers in the data
 * 4. Check fields of each log operation header for:
 *	A. Valid client identifier
 *	B. tid ptr value falls in valid ptr space (user space code)
 *	C. Length in log record header is correct according to the
 *		individual operation headers within record.
 * 5. When a bwrite will occur within 5 blocks of the front of the physical
 *	log, check the preceding blocks of the physical log to make sure all
 *	the cycle numbers agree with the current cycle number.
 */
STATIC void
xlog_verify_iclog(
	struct xlog		*log,
	struct xlog_in_core	*iclog,
	int			count)
{
	xlog_op_header_t	*ophead;
	xlog_in_core_t		*icptr;
	xlog_in_core_2_t	*xhdr;
	void			*base_ptr, *ptr, *p;
	ptrdiff_t		field_offset;
	uint8_t			clientid;
	int			len, i, j, k, op_len;
	int			idx;

	/* check validity of iclog pointers */
	spin_lock(&log->l_icloglock);
	icptr = log->l_iclog;
	for (i = 0; i < log->l_iclog_bufs; i++, icptr = icptr->ic_next)
		ASSERT(icptr);

	if (icptr != log->l_iclog)
		xfs_emerg(log->l_mp, "%s: corrupt iclog ring", __func__);
	spin_unlock(&log->l_icloglock);

	/* check log magic numbers */
	if (iclog->ic_header.h_magicno != cpu_to_be32(XLOG_HEADER_MAGIC_NUM))
		xfs_emerg(log->l_mp, "%s: invalid magic num", __func__);

	base_ptr = ptr = &iclog->ic_header;
	p = &iclog->ic_header;
	for (ptr += BBSIZE; ptr < base_ptr + count; ptr += BBSIZE) {
		if (*(__be32 *)ptr == cpu_to_be32(XLOG_HEADER_MAGIC_NUM))
			xfs_emerg(log->l_mp, "%s: unexpected magic num",
				__func__);
	}

	/* check fields */
	len = be32_to_cpu(iclog->ic_header.h_num_logops);
	base_ptr = ptr = iclog->ic_datap;
	ophead = ptr;
	xhdr = iclog->ic_data;
	for (i = 0; i < len; i++) {
		ophead = ptr;

		/* clientid is only 1 byte */
		p = &ophead->oh_clientid;
		field_offset = p - base_ptr;
		if (field_offset & 0x1ff) {
			clientid = ophead->oh_clientid;
		} else {
			idx = BTOBBT((void *)&ophead->oh_clientid - iclog->ic_datap);
			if (idx >= (XLOG_HEADER_CYCLE_SIZE / BBSIZE)) {
				j = idx / (XLOG_HEADER_CYCLE_SIZE / BBSIZE);
				k = idx % (XLOG_HEADER_CYCLE_SIZE / BBSIZE);
				clientid = xlog_get_client_id(
					xhdr[j].hic_xheader.xh_cycle_data[k]);
			} else {
				clientid = xlog_get_client_id(
					iclog->ic_header.h_cycle_data[idx]);
			}
		}
		if (clientid != XFS_TRANSACTION && clientid != XFS_LOG) {
			xfs_warn(log->l_mp,
				"%s: op %d invalid clientid %d op "PTR_FMT" offset 0x%lx",
				__func__, i, clientid, ophead,
				(unsigned long)field_offset);
		}

		/* check length */
		p = &ophead->oh_len;
		field_offset = p - base_ptr;
		if (field_offset & 0x1ff) {
			op_len = be32_to_cpu(ophead->oh_len);
		} else {
			idx = BTOBBT((void *)&ophead->oh_len - iclog->ic_datap);
			if (idx >= (XLOG_HEADER_CYCLE_SIZE / BBSIZE)) {
				j = idx / (XLOG_HEADER_CYCLE_SIZE / BBSIZE);
				k = idx % (XLOG_HEADER_CYCLE_SIZE / BBSIZE);
				op_len = be32_to_cpu(xhdr[j].hic_xheader.xh_cycle_data[k]);
			} else {
				op_len = be32_to_cpu(iclog->ic_header.h_cycle_data[idx]);
			}
		}
		ptr += sizeof(xlog_op_header_t) + op_len;
	}
}
#endif

/*
 * Perform a forced shutdown on the log.
 *
 * This can be called from low level log code to trigger a shutdown, or from the
 * high level mount shutdown code when the mount shuts down.
 *
 * Our main objectives here are to make sure that:
 *	a. if the shutdown was not due to a log IO error, flush the logs to
 *	   disk. Anything modified after this is ignored.
 *	b. the log gets atomically marked 'XLOG_IO_ERROR' for all interested
 *	   parties to find out. Nothing new gets queued after this is done.
 *	c. Tasks sleeping on log reservations, pinned objects and
 *	   other resources get woken up.
 *	d. The mount is also marked as shut down so that log triggered shutdowns
 *	   still behave the same as if they called xfs_forced_shutdown().
 *
 * Return true if the shutdown cause was a log IO error and we actually shut the
 * log down.
 */
bool
xlog_force_shutdown(
	struct xlog	*log,
	uint32_t	shutdown_flags)
{
	bool		log_error = (shutdown_flags & SHUTDOWN_LOG_IO_ERROR);

	if (!log)
		return false;

	/*
	 * Ensure that there is only ever one log shutdown being processed.
	 * If we allow the log force below on a second pass after shutting
	 * down the log, we risk deadlocking the CIL push as it may require
	 * locks on objects the current shutdown context holds (e.g. taking
	 * buffer locks to abort buffers on last unpin of buf log items).
	 */
	if (test_and_set_bit(XLOG_SHUTDOWN_STARTED, &log->l_opstate))
		return false;

	/*
	 * Flush all the completed transactions to disk before marking the log
	 * being shut down. We need to do this first as shutting down the log
	 * before the force will prevent the log force from flushing the iclogs
	 * to disk.
	 *
	 * When we are in recovery, there are no transactions to flush, and
	 * we don't want to touch the log because we don't want to perturb the
	 * current head/tail for future recovery attempts. Hence we need to
	 * avoid a log force in this case.
	 *
	 * If we are shutting down due to a log IO error, then we must avoid
	 * trying to write the log as that may just result in more IO errors and
	 * an endless shutdown/force loop.
	 */
	if (!log_error && !xlog_in_recovery(log))
		xfs_log_force(log->l_mp, XFS_LOG_SYNC);

	/*
	 * Atomically set the shutdown state. If the shutdown state is already
	 * set, there someone else is performing the shutdown and so we are done
	 * here. This should never happen because we should only ever get called
	 * once by the first shutdown caller.
	 *
	 * Much of the log state machine transitions assume that shutdown state
	 * cannot change once they hold the log->l_icloglock. Hence we need to
	 * hold that lock here, even though we use the atomic test_and_set_bit()
	 * operation to set the shutdown state.
	 */
	spin_lock(&log->l_icloglock);
	if (test_and_set_bit(XLOG_IO_ERROR, &log->l_opstate)) {
		spin_unlock(&log->l_icloglock);
		ASSERT(0);
		return false;
	}
	spin_unlock(&log->l_icloglock);

	/*
	 * If this log shutdown also sets the mount shutdown state, issue a
	 * shutdown warning message.
	 */
	if (!xfs_set_shutdown(log->l_mp)) {
		xfs_alert_tag(log->l_mp, XFS_PTAG_SHUTDOWN_LOGERROR,
"Filesystem has been shut down due to log error (0x%x).",
				shutdown_flags);
		xfs_alert(log->l_mp,
"Please unmount the filesystem and rectify the problem(s).");
		if (xfs_error_level >= XFS_ERRLEVEL_HIGH)
			xfs_stack_trace();
	}

	/*
	 * We don't want anybody waiting for log reservations after this. That
	 * means we have to wake up everybody queued up on reserveq as well as
	 * writeq.  In addition, we make sure in xlog_{re}grant_log_space that
	 * we don't enqueue anything once the SHUTDOWN flag is set, and this
	 * action is protected by the grant locks.
	 */
	xlog_grant_head_wake_all(&log->l_reserve_head);
	xlog_grant_head_wake_all(&log->l_write_head);

	/*
	 * Wake up everybody waiting on xfs_log_force. Wake the CIL push first
	 * as if the log writes were completed. The abort handling in the log
	 * item committed callback functions will do this again under lock to
	 * avoid races.
	 */
	spin_lock(&log->l_cilp->xc_push_lock);
	wake_up_all(&log->l_cilp->xc_start_wait);
	wake_up_all(&log->l_cilp->xc_commit_wait);
	spin_unlock(&log->l_cilp->xc_push_lock);

	spin_lock(&log->l_icloglock);
	xlog_state_shutdown_callbacks(log);
	spin_unlock(&log->l_icloglock);

	wake_up_var(&log->l_opstate);
	if (IS_ENABLED(CONFIG_XFS_RT) && xfs_has_zoned(log->l_mp))
		xfs_zoned_wake_all(log->l_mp);

	return log_error;
}

STATIC int
xlog_iclogs_empty(
	struct xlog	*log)
{
	xlog_in_core_t	*iclog;

	iclog = log->l_iclog;
	do {
		/* endianness does not matter here, zero is zero in
		 * any language.
		 */
		if (iclog->ic_header.h_num_logops)
			return 0;
		iclog = iclog->ic_next;
	} while (iclog != log->l_iclog);
	return 1;
}

/*
 * Verify that an LSN stamped into a piece of metadata is valid. This is
 * intended for use in read verifiers on v5 superblocks.
 */
bool
xfs_log_check_lsn(
	struct xfs_mount	*mp,
	xfs_lsn_t		lsn)
{
	struct xlog		*log = mp->m_log;
	bool			valid;

	/*
	 * norecovery mode skips mount-time log processing and unconditionally
	 * resets the in-core LSN. We can't validate in this mode, but
	 * modifications are not allowed anyways so just return true.
	 */
	if (xfs_has_norecovery(mp))
		return true;

	/*
	 * Some metadata LSNs are initialized to NULL (e.g., the agfl). This is
	 * handled by recovery and thus safe to ignore here.
	 */
	if (lsn == NULLCOMMITLSN)
		return true;

	valid = xlog_valid_lsn(mp->m_log, lsn);

	/* warn the user about what's gone wrong before verifier failure */
	if (!valid) {
		spin_lock(&log->l_icloglock);
		xfs_warn(mp,
"Corruption warning: Metadata has LSN (%d:%d) ahead of current LSN (%d:%d). "
"Please unmount and run xfs_repair (>= v4.3) to resolve.",
			 CYCLE_LSN(lsn), BLOCK_LSN(lsn),
			 log->l_curr_cycle, log->l_curr_block);
		spin_unlock(&log->l_icloglock);
	}

	return valid;
}
