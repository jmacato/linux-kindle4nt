// SPDX-License-Identifier: GPL-2.0-only
/*
 *  SMB1 (CIFS) version specific operations
 *
 *  Copyright (c) 2012, Jeff Layton <jlayton@redhat.com>
 */

#include <linux/pagemap.h>
#include <linux/vfs.h>
#include <uapi/linux/magic.h>
#include "cifsglob.h"
#include "cifsproto.h"
#include "cifs_debug.h"
#include "cifspdu.h"
#include "cifs_unicode.h"
#include "fs_context.h"
#include "nterr.h"
#include "smberr.h"
#include "reparse.h"

/*
 * An NT cancel request header looks just like the original request except:
 *
 * The Command is SMB_COM_NT_CANCEL
 * The WordCount is zeroed out
 * The ByteCount is zeroed out
 *
 * This function mangles an existing request buffer into a
 * SMB_COM_NT_CANCEL request and then sends it.
 */
static int
send_nt_cancel(struct TCP_Server_Info *server, struct smb_rqst *rqst,
	       struct mid_q_entry *mid)
{
	int rc = 0;
	struct smb_hdr *in_buf = (struct smb_hdr *)rqst->rq_iov[0].iov_base;

	/* -4 for RFC1001 length and +2 for BCC field */
	in_buf->smb_buf_length = cpu_to_be32(sizeof(struct smb_hdr) - 4  + 2);
	in_buf->Command = SMB_COM_NT_CANCEL;
	in_buf->WordCount = 0;
	put_bcc(0, in_buf);

	cifs_server_lock(server);
	rc = cifs_sign_smb(in_buf, server, &mid->sequence_number);
	if (rc) {
		cifs_server_unlock(server);
		return rc;
	}

	/*
	 * The response to this call was already factored into the sequence
	 * number when the call went out, so we must adjust it back downward
	 * after signing here.
	 */
	--server->sequence_number;
	rc = smb_send(server, in_buf, be32_to_cpu(in_buf->smb_buf_length));
	if (rc < 0)
		server->sequence_number--;

	cifs_server_unlock(server);

	cifs_dbg(FYI, "issued NT_CANCEL for mid %u, rc = %d\n",
		 get_mid(in_buf), rc);

	return rc;
}

static bool
cifs_compare_fids(struct cifsFileInfo *ob1, struct cifsFileInfo *ob2)
{
	return ob1->fid.netfid == ob2->fid.netfid;
}

static unsigned int
cifs_read_data_offset(char *buf)
{
	READ_RSP *rsp = (READ_RSP *)buf;
	return le16_to_cpu(rsp->DataOffset);
}

static unsigned int
cifs_read_data_length(char *buf, bool in_remaining)
{
	READ_RSP *rsp = (READ_RSP *)buf;
	/* It's a bug reading remaining data for SMB1 packets */
	WARN_ON(in_remaining);
	return (le16_to_cpu(rsp->DataLengthHigh) << 16) +
	       le16_to_cpu(rsp->DataLength);
}

static struct mid_q_entry *
cifs_find_mid(struct TCP_Server_Info *server, char *buffer)
{
	struct smb_hdr *buf = (struct smb_hdr *)buffer;
	struct mid_q_entry *mid;

	spin_lock(&server->mid_lock);
	list_for_each_entry(mid, &server->pending_mid_q, qhead) {
		if (compare_mid(mid->mid, buf) &&
		    mid->mid_state == MID_REQUEST_SUBMITTED &&
		    le16_to_cpu(mid->command) == buf->Command) {
			kref_get(&mid->refcount);
			spin_unlock(&server->mid_lock);
			return mid;
		}
	}
	spin_unlock(&server->mid_lock);
	return NULL;
}

static void
cifs_add_credits(struct TCP_Server_Info *server,
		 struct cifs_credits *credits, const int optype)
{
	spin_lock(&server->req_lock);
	server->credits += credits->value;
	server->in_flight--;
	spin_unlock(&server->req_lock);
	wake_up(&server->request_q);
}

static void
cifs_set_credits(struct TCP_Server_Info *server, const int val)
{
	spin_lock(&server->req_lock);
	server->credits = val;
	server->oplocks = val > 1 ? enable_oplocks : false;
	spin_unlock(&server->req_lock);
}

static int *
cifs_get_credits_field(struct TCP_Server_Info *server, const int optype)
{
	return &server->credits;
}

static unsigned int
cifs_get_credits(struct mid_q_entry *mid)
{
	return 1;
}

/*
 * Find a free multiplex id (SMB mid). Otherwise there could be
 * mid collisions which might cause problems, demultiplexing the
 * wrong response to this request. Multiplex ids could collide if
 * one of a series requests takes much longer than the others, or
 * if a very large number of long lived requests (byte range
 * locks or FindNotify requests) are pending. No more than
 * 64K-1 requests can be outstanding at one time. If no
 * mids are available, return zero. A future optimization
 * could make the combination of mids and uid the key we use
 * to demultiplex on (rather than mid alone).
 * In addition to the above check, the cifs demultiplex
 * code already used the command code as a secondary
 * check of the frame and if signing is negotiated the
 * response would be discarded if the mid were the same
 * but the signature was wrong. Since the mid is not put in the
 * pending queue until later (when it is about to be dispatched)
 * we do have to limit the number of outstanding requests
 * to somewhat less than 64K-1 although it is hard to imagine
 * so many threads being in the vfs at one time.
 */
static __u64
cifs_get_next_mid(struct TCP_Server_Info *server)
{
	__u64 mid = 0;
	__u16 last_mid, cur_mid;
	bool collision, reconnect = false;

	spin_lock(&server->mid_lock);

	/* mid is 16 bit only for CIFS/SMB */
	cur_mid = (__u16)((server->CurrentMid) & 0xffff);
	/* we do not want to loop forever */
	last_mid = cur_mid;
	cur_mid++;
	/* avoid 0xFFFF MID */
	if (cur_mid == 0xffff)
		cur_mid++;

	/*
	 * This nested loop looks more expensive than it is.
	 * In practice the list of pending requests is short,
	 * fewer than 50, and the mids are likely to be unique
	 * on the first pass through the loop unless some request
	 * takes longer than the 64 thousand requests before it
	 * (and it would also have to have been a request that
	 * did not time out).
	 */
	while (cur_mid != last_mid) {
		struct mid_q_entry *mid_entry;
		unsigned int num_mids;

		collision = false;
		if (cur_mid == 0)
			cur_mid++;

		num_mids = 0;
		list_for_each_entry(mid_entry, &server->pending_mid_q, qhead) {
			++num_mids;
			if (mid_entry->mid == cur_mid &&
			    mid_entry->mid_state == MID_REQUEST_SUBMITTED) {
				/* This mid is in use, try a different one */
				collision = true;
				break;
			}
		}

		/*
		 * if we have more than 32k mids in the list, then something
		 * is very wrong. Possibly a local user is trying to DoS the
		 * box by issuing long-running calls and SIGKILL'ing them. If
		 * we get to 2^16 mids then we're in big trouble as this
		 * function could loop forever.
		 *
		 * Go ahead and assign out the mid in this situation, but force
		 * an eventual reconnect to clean out the pending_mid_q.
		 */
		if (num_mids > 32768)
			reconnect = true;

		if (!collision) {
			mid = (__u64)cur_mid;
			server->CurrentMid = mid;
			break;
		}
		cur_mid++;
	}
	spin_unlock(&server->mid_lock);

	if (reconnect) {
		cifs_signal_cifsd_for_reconnect(server, false);
	}

	return mid;
}

/*
	return codes:
		0	not a transact2, or all data present
		>0	transact2 with that much data missing
		-EINVAL	invalid transact2
 */
static int
check2ndT2(char *buf)
{
	struct smb_hdr *pSMB = (struct smb_hdr *)buf;
	struct smb_t2_rsp *pSMBt;
	int remaining;
	__u16 total_data_size, data_in_this_rsp;

	if (pSMB->Command != SMB_COM_TRANSACTION2)
		return 0;

	/* check for plausible wct, bcc and t2 data and parm sizes */
	/* check for parm and data offset going beyond end of smb */
	if (pSMB->WordCount != 10) { /* coalesce_t2 depends on this */
		cifs_dbg(FYI, "Invalid transact2 word count\n");
		return -EINVAL;
	}

	pSMBt = (struct smb_t2_rsp *)pSMB;

	total_data_size = get_unaligned_le16(&pSMBt->t2_rsp.TotalDataCount);
	data_in_this_rsp = get_unaligned_le16(&pSMBt->t2_rsp.DataCount);

	if (total_data_size == data_in_this_rsp)
		return 0;
	else if (total_data_size < data_in_this_rsp) {
		cifs_dbg(FYI, "total data %d smaller than data in frame %d\n",
			 total_data_size, data_in_this_rsp);
		return -EINVAL;
	}

	remaining = total_data_size - data_in_this_rsp;

	cifs_dbg(FYI, "missing %d bytes from transact2, check next response\n",
		 remaining);
	if (total_data_size > CIFSMaxBufSize) {
		cifs_dbg(VFS, "TotalDataSize %d is over maximum buffer %d\n",
			 total_data_size, CIFSMaxBufSize);
		return -EINVAL;
	}
	return remaining;
}

static int
coalesce_t2(char *second_buf, struct smb_hdr *target_hdr)
{
	struct smb_t2_rsp *pSMBs = (struct smb_t2_rsp *)second_buf;
	struct smb_t2_rsp *pSMBt  = (struct smb_t2_rsp *)target_hdr;
	char *data_area_of_tgt;
	char *data_area_of_src;
	int remaining;
	unsigned int byte_count, total_in_tgt;
	__u16 tgt_total_cnt, src_total_cnt, total_in_src;

	src_total_cnt = get_unaligned_le16(&pSMBs->t2_rsp.TotalDataCount);
	tgt_total_cnt = get_unaligned_le16(&pSMBt->t2_rsp.TotalDataCount);

	if (tgt_total_cnt != src_total_cnt)
		cifs_dbg(FYI, "total data count of primary and secondary t2 differ source=%hu target=%hu\n",
			 src_total_cnt, tgt_total_cnt);

	total_in_tgt = get_unaligned_le16(&pSMBt->t2_rsp.DataCount);

	remaining = tgt_total_cnt - total_in_tgt;

	if (remaining < 0) {
		cifs_dbg(FYI, "Server sent too much data. tgt_total_cnt=%hu total_in_tgt=%u\n",
			 tgt_total_cnt, total_in_tgt);
		return -EPROTO;
	}

	if (remaining == 0) {
		/* nothing to do, ignore */
		cifs_dbg(FYI, "no more data remains\n");
		return 0;
	}

	total_in_src = get_unaligned_le16(&pSMBs->t2_rsp.DataCount);
	if (remaining < total_in_src)
		cifs_dbg(FYI, "transact2 2nd response contains too much data\n");

	/* find end of first SMB data area */
	data_area_of_tgt = (char *)&pSMBt->hdr.Protocol +
				get_unaligned_le16(&pSMBt->t2_rsp.DataOffset);

	/* validate target area */
	data_area_of_src = (char *)&pSMBs->hdr.Protocol +
				get_unaligned_le16(&pSMBs->t2_rsp.DataOffset);

	data_area_of_tgt += total_in_tgt;

	total_in_tgt += total_in_src;
	/* is the result too big for the field? */
	if (total_in_tgt > USHRT_MAX) {
		cifs_dbg(FYI, "coalesced DataCount too large (%u)\n",
			 total_in_tgt);
		return -EPROTO;
	}
	put_unaligned_le16(total_in_tgt, &pSMBt->t2_rsp.DataCount);

	/* fix up the BCC */
	byte_count = get_bcc(target_hdr);
	byte_count += total_in_src;
	/* is the result too big for the field? */
	if (byte_count > USHRT_MAX) {
		cifs_dbg(FYI, "coalesced BCC too large (%u)\n", byte_count);
		return -EPROTO;
	}
	put_bcc(byte_count, target_hdr);

	byte_count = be32_to_cpu(target_hdr->smb_buf_length);
	byte_count += total_in_src;
	/* don't allow buffer to overflow */
	if (byte_count > CIFSMaxBufSize + MAX_CIFS_HDR_SIZE - 4) {
		cifs_dbg(FYI, "coalesced BCC exceeds buffer size (%u)\n",
			 byte_count);
		return -ENOBUFS;
	}
	target_hdr->smb_buf_length = cpu_to_be32(byte_count);

	/* copy second buffer into end of first buffer */
	memcpy(data_area_of_tgt, data_area_of_src, total_in_src);

	if (remaining != total_in_src) {
		/* more responses to go */
		cifs_dbg(FYI, "waiting for more secondary responses\n");
		return 1;
	}

	/* we are done */
	cifs_dbg(FYI, "found the last secondary response\n");
	return 0;
}

static void
cifs_downgrade_oplock(struct TCP_Server_Info *server,
		      struct cifsInodeInfo *cinode, __u32 oplock,
		      __u16 epoch, bool *purge_cache)
{
	cifs_set_oplock_level(cinode, oplock);
}

static bool
cifs_check_trans2(struct mid_q_entry *mid, struct TCP_Server_Info *server,
		  char *buf, int malformed)
{
	if (malformed)
		return false;
	if (check2ndT2(buf) <= 0)
		return false;
	mid->multiRsp = true;
	if (mid->resp_buf) {
		/* merge response - fix up 1st*/
		malformed = coalesce_t2(buf, mid->resp_buf);
		if (malformed > 0)
			return true;
		/* All parts received or packet is malformed. */
		mid->multiEnd = true;
		dequeue_mid(mid, malformed);
		return true;
	}
	if (!server->large_buf) {
		/*FIXME: switch to already allocated largebuf?*/
		cifs_dbg(VFS, "1st trans2 resp needs bigbuf\n");
	} else {
		/* Have first buffer */
		mid->resp_buf = buf;
		mid->large_buf = true;
		server->bigbuf = NULL;
	}
	return true;
}

static bool
cifs_need_neg(struct TCP_Server_Info *server)
{
	return server->maxBuf == 0;
}

static int
cifs_negotiate(const unsigned int xid,
	       struct cifs_ses *ses,
	       struct TCP_Server_Info *server)
{
	int rc;
	rc = CIFSSMBNegotiate(xid, ses, server);
	return rc;
}

static unsigned int
smb1_negotiate_wsize(struct cifs_tcon *tcon, struct smb3_fs_context *ctx)
{
	__u64 unix_cap = le64_to_cpu(tcon->fsUnixInfo.Capability);
	struct TCP_Server_Info *server = tcon->ses->server;
	unsigned int wsize;

	/* start with specified wsize, or default */
	if (ctx->got_wsize)
		wsize = ctx->vol_wsize;
	else if (tcon->unix_ext && (unix_cap & CIFS_UNIX_LARGE_WRITE_CAP))
		wsize = CIFS_DEFAULT_IOSIZE;
	else
		wsize = CIFS_DEFAULT_NON_POSIX_WSIZE;

	/* can server support 24-bit write sizes? (via UNIX extensions) */
	if (!tcon->unix_ext || !(unix_cap & CIFS_UNIX_LARGE_WRITE_CAP))
		wsize = min_t(unsigned int, wsize, CIFS_MAX_RFC1002_WSIZE);

	/*
	 * no CAP_LARGE_WRITE_X or is signing enabled without CAP_UNIX set?
	 * Limit it to max buffer offered by the server, minus the size of the
	 * WRITEX header, not including the 4 byte RFC1001 length.
	 */
	if (!(server->capabilities & CAP_LARGE_WRITE_X) ||
	    (!(server->capabilities & CAP_UNIX) && server->sign))
		wsize = min_t(unsigned int, wsize,
				server->maxBuf - sizeof(WRITE_REQ) + 4);

	/* hard limit of CIFS_MAX_WSIZE */
	wsize = min_t(unsigned int, wsize, CIFS_MAX_WSIZE);

	return wsize;
}

static unsigned int
smb1_negotiate_rsize(struct cifs_tcon *tcon, struct smb3_fs_context *ctx)
{
	__u64 unix_cap = le64_to_cpu(tcon->fsUnixInfo.Capability);
	struct TCP_Server_Info *server = tcon->ses->server;
	unsigned int rsize, defsize;

	/*
	 * Set default value...
	 *
	 * HACK alert! Ancient servers have very small buffers. Even though
	 * MS-CIFS indicates that servers are only limited by the client's
	 * bufsize for reads, testing against win98se shows that it throws
	 * INVALID_PARAMETER errors if you try to request too large a read.
	 * OS/2 just sends back short reads.
	 *
	 * If the server doesn't advertise CAP_LARGE_READ_X, then assume that
	 * it can't handle a read request larger than its MaxBufferSize either.
	 */
	if (tcon->unix_ext && (unix_cap & CIFS_UNIX_LARGE_READ_CAP))
		defsize = CIFS_DEFAULT_IOSIZE;
	else if (server->capabilities & CAP_LARGE_READ_X)
		defsize = CIFS_DEFAULT_NON_POSIX_RSIZE;
	else
		defsize = server->maxBuf - sizeof(READ_RSP);

	rsize = ctx->got_rsize ? ctx->vol_rsize : defsize;

	/*
	 * no CAP_LARGE_READ_X? Then MS-CIFS states that we must limit this to
	 * the client's MaxBufferSize.
	 */
	if (!(server->capabilities & CAP_LARGE_READ_X))
		rsize = min_t(unsigned int, CIFSMaxBufSize, rsize);

	/* hard limit of CIFS_MAX_RSIZE */
	rsize = min_t(unsigned int, rsize, CIFS_MAX_RSIZE);

	return rsize;
}

static void
cifs_qfs_tcon(const unsigned int xid, struct cifs_tcon *tcon,
	      struct cifs_sb_info *cifs_sb)
{
	CIFSSMBQFSDeviceInfo(xid, tcon);
	CIFSSMBQFSAttributeInfo(xid, tcon);
}

static int
cifs_is_path_accessible(const unsigned int xid, struct cifs_tcon *tcon,
			struct cifs_sb_info *cifs_sb, const char *full_path)
{
	int rc;
	FILE_ALL_INFO *file_info;

	file_info = kmalloc(sizeof(FILE_ALL_INFO), GFP_KERNEL);
	if (file_info == NULL)
		return -ENOMEM;

	rc = CIFSSMBQPathInfo(xid, tcon, full_path, file_info,
			      0 /* not legacy */, cifs_sb->local_nls,
			      cifs_remap(cifs_sb));

	if (rc == -EOPNOTSUPP || rc == -EINVAL)
		rc = SMBQueryInformation(xid, tcon, full_path, file_info,
				cifs_sb->local_nls, cifs_remap(cifs_sb));
	kfree(file_info);
	return rc;
}

static int cifs_query_path_info(const unsigned int xid,
				struct cifs_tcon *tcon,
				struct cifs_sb_info *cifs_sb,
				const char *full_path,
				struct cifs_open_info_data *data)
{
	int rc = -EOPNOTSUPP;
	FILE_ALL_INFO fi = {};
	struct cifs_search_info search_info = {};
	bool non_unicode_wildcard = false;

	data->reparse_point = false;
	data->adjust_tz = false;

	/*
	 * First try CIFSSMBQPathInfo() function which returns more info
	 * (NumberOfLinks) than CIFSFindFirst() fallback function.
	 * Some servers like Win9x do not support SMB_QUERY_FILE_ALL_INFO over
	 * TRANS2_QUERY_PATH_INFORMATION, but supports it with filehandle over
	 * TRANS2_QUERY_FILE_INFORMATION (function CIFSSMBQFileInfo(). But SMB
	 * Open command on non-NT servers works only for files, does not work
	 * for directories. And moreover Win9x SMB server returns bogus data in
	 * SMB_QUERY_FILE_ALL_INFO Attributes field. So for non-NT servers,
	 * do not even use CIFSSMBQPathInfo() or CIFSSMBQFileInfo() function.
	 */
	if (tcon->ses->capabilities & CAP_NT_SMBS)
		rc = CIFSSMBQPathInfo(xid, tcon, full_path, &fi, 0 /* not legacy */,
				      cifs_sb->local_nls, cifs_remap(cifs_sb));

	/*
	 * Non-UNICODE variant of fallback functions below expands wildcards,
	 * so they cannot be used for querying paths with wildcard characters.
	 */
	if (rc && !(tcon->ses->capabilities & CAP_UNICODE) && strpbrk(full_path, "*?\"><"))
		non_unicode_wildcard = true;

	/*
	 * Then fallback to CIFSFindFirst() which works also with non-NT servers
	 * but does not does not provide NumberOfLinks.
	 */
	if ((rc == -EOPNOTSUPP || rc == -EINVAL) &&
	    !non_unicode_wildcard) {
		if (!(tcon->ses->capabilities & tcon->ses->server->vals->cap_nt_find))
			search_info.info_level = SMB_FIND_FILE_INFO_STANDARD;
		else
			search_info.info_level = SMB_FIND_FILE_FULL_DIRECTORY_INFO;
		rc = CIFSFindFirst(xid, tcon, full_path, cifs_sb, NULL,
				   CIFS_SEARCH_CLOSE_ALWAYS | CIFS_SEARCH_CLOSE_AT_END,
				   &search_info, false);
		if (rc == 0) {
			if (!(tcon->ses->capabilities & tcon->ses->server->vals->cap_nt_find)) {
				FIND_FILE_STANDARD_INFO *di;
				int offset = tcon->ses->server->timeAdj;

				di = (FIND_FILE_STANDARD_INFO *)search_info.srch_entries_start;
				fi.CreationTime = cpu_to_le64(cifs_UnixTimeToNT(cnvrtDosUnixTm(
						di->CreationDate, di->CreationTime, offset)));
				fi.LastAccessTime = cpu_to_le64(cifs_UnixTimeToNT(cnvrtDosUnixTm(
						di->LastAccessDate, di->LastAccessTime, offset)));
				fi.LastWriteTime = cpu_to_le64(cifs_UnixTimeToNT(cnvrtDosUnixTm(
						di->LastWriteDate, di->LastWriteTime, offset)));
				fi.ChangeTime = fi.LastWriteTime;
				fi.Attributes = cpu_to_le32(le16_to_cpu(di->Attributes));
				fi.AllocationSize = cpu_to_le64(le32_to_cpu(di->AllocationSize));
				fi.EndOfFile = cpu_to_le64(le32_to_cpu(di->DataSize));
			} else {
				FILE_FULL_DIRECTORY_INFO *di;

				di = (FILE_FULL_DIRECTORY_INFO *)search_info.srch_entries_start;
				fi.CreationTime = di->CreationTime;
				fi.LastAccessTime = di->LastAccessTime;
				fi.LastWriteTime = di->LastWriteTime;
				fi.ChangeTime = di->ChangeTime;
				fi.Attributes = di->ExtFileAttributes;
				fi.AllocationSize = di->AllocationSize;
				fi.EndOfFile = di->EndOfFile;
				fi.EASize = di->EaSize;
			}
			fi.NumberOfLinks = cpu_to_le32(1);
			fi.DeletePending = 0;
			fi.Directory = !!(le32_to_cpu(fi.Attributes) & ATTR_DIRECTORY);
			cifs_buf_release(search_info.ntwrk_buf_start);
		} else if (!full_path[0]) {
			/*
			 * CIFSFindFirst() does not work on root path if the
			 * root path was exported on the server from the top
			 * level path (drive letter).
			 */
			rc = -EOPNOTSUPP;
		}
	}

	/*
	 * If everything failed then fallback to the legacy SMB command
	 * SMB_COM_QUERY_INFORMATION which works with all servers, but
	 * provide just few information.
	 */
	if ((rc == -EOPNOTSUPP || rc == -EINVAL) && !non_unicode_wildcard) {
		rc = SMBQueryInformation(xid, tcon, full_path, &fi, cifs_sb->local_nls,
					 cifs_remap(cifs_sb));
		data->adjust_tz = true;
	} else if ((rc == -EOPNOTSUPP || rc == -EINVAL) && non_unicode_wildcard) {
		/* Path with non-UNICODE wildcard character cannot exist. */
		rc = -ENOENT;
	}

	if (!rc) {
		move_cifs_info_to_smb2(&data->fi, &fi);
		data->reparse_point = le32_to_cpu(fi.Attributes) & ATTR_REPARSE;
	}

#ifdef CONFIG_CIFS_XATTR
	/*
	 * For WSL CHR and BLK reparse points it is required to fetch
	 * EA $LXDEV which contains major and minor device numbers.
	 */
	if (!rc && data->reparse_point) {
		struct smb2_file_full_ea_info *ea;

		ea = (struct smb2_file_full_ea_info *)data->wsl.eas;
		rc = CIFSSMBQAllEAs(xid, tcon, full_path, SMB2_WSL_XATTR_DEV,
				    &ea->ea_data[SMB2_WSL_XATTR_NAME_LEN + 1],
				    SMB2_WSL_XATTR_DEV_SIZE, cifs_sb);
		if (rc == SMB2_WSL_XATTR_DEV_SIZE) {
			ea->next_entry_offset = cpu_to_le32(0);
			ea->flags = 0;
			ea->ea_name_length = SMB2_WSL_XATTR_NAME_LEN;
			ea->ea_value_length = cpu_to_le16(SMB2_WSL_XATTR_DEV_SIZE);
			memcpy(&ea->ea_data[0], SMB2_WSL_XATTR_DEV, SMB2_WSL_XATTR_NAME_LEN + 1);
			data->wsl.eas_len = sizeof(*ea) + SMB2_WSL_XATTR_NAME_LEN + 1 +
					    SMB2_WSL_XATTR_DEV_SIZE;
			rc = 0;
		} else if (rc >= 0) {
			/* It is an error if EA $LXDEV has wrong size. */
			rc = -EINVAL;
		} else {
			/*
			 * In all other cases ignore error if fetching
			 * of EA $LXDEV failed. It is needed only for
			 * WSL CHR and BLK reparse points and wsl_to_fattr()
			 * handle the case when EA is missing.
			 */
			rc = 0;
		}
	}
#endif

	return rc;
}

static int cifs_get_srv_inum(const unsigned int xid, struct cifs_tcon *tcon,
			     struct cifs_sb_info *cifs_sb, const char *full_path,
			     u64 *uniqueid, struct cifs_open_info_data *unused)
{
	/*
	 * We can not use the IndexNumber field by default from Windows or
	 * Samba (in ALL_INFO buf) but we can request it explicitly. The SNIA
	 * CIFS spec claims that this value is unique within the scope of a
	 * share, and the windows docs hint that it's actually unique
	 * per-machine.
	 *
	 * There may be higher info levels that work but are there Windows
	 * server or network appliances for which IndexNumber field is not
	 * guaranteed unique?
	 *
	 * CIFSGetSrvInodeNumber() uses SMB_QUERY_FILE_INTERNAL_INFO
	 * which is SMB PASSTHROUGH level therefore check for capability.
	 * Note that this function can be called with tcon == NULL.
	 */
	if (tcon && !(tcon->ses->capabilities & CAP_INFOLEVEL_PASSTHRU))
		return -EOPNOTSUPP;
	return CIFSGetSrvInodeNumber(xid, tcon, full_path, uniqueid,
				     cifs_sb->local_nls,
				     cifs_remap(cifs_sb));
}

static int cifs_query_file_info(const unsigned int xid, struct cifs_tcon *tcon,
				struct cifsFileInfo *cfile, struct cifs_open_info_data *data)
{
	int rc;
	FILE_ALL_INFO fi = {};

	/*
	 * CIFSSMBQFileInfo() for non-NT servers returns bogus data in
	 * Attributes fields. So do not use this command for non-NT servers.
	 */
	if (!(tcon->ses->capabilities & CAP_NT_SMBS))
		return -EOPNOTSUPP;

	if (cfile->symlink_target) {
		data->symlink_target = kstrdup(cfile->symlink_target, GFP_KERNEL);
		if (!data->symlink_target)
			return -ENOMEM;
	}

	rc = CIFSSMBQFileInfo(xid, tcon, cfile->fid.netfid, &fi);
	if (!rc)
		move_cifs_info_to_smb2(&data->fi, &fi);
	return rc;
}

static void
cifs_clear_stats(struct cifs_tcon *tcon)
{
	atomic_set(&tcon->stats.cifs_stats.num_writes, 0);
	atomic_set(&tcon->stats.cifs_stats.num_reads, 0);
	atomic_set(&tcon->stats.cifs_stats.num_flushes, 0);
	atomic_set(&tcon->stats.cifs_stats.num_oplock_brks, 0);
	atomic_set(&tcon->stats.cifs_stats.num_opens, 0);
	atomic_set(&tcon->stats.cifs_stats.num_posixopens, 0);
	atomic_set(&tcon->stats.cifs_stats.num_posixmkdirs, 0);
	atomic_set(&tcon->stats.cifs_stats.num_closes, 0);
	atomic_set(&tcon->stats.cifs_stats.num_deletes, 0);
	atomic_set(&tcon->stats.cifs_stats.num_mkdirs, 0);
	atomic_set(&tcon->stats.cifs_stats.num_rmdirs, 0);
	atomic_set(&tcon->stats.cifs_stats.num_renames, 0);
	atomic_set(&tcon->stats.cifs_stats.num_t2renames, 0);
	atomic_set(&tcon->stats.cifs_stats.num_ffirst, 0);
	atomic_set(&tcon->stats.cifs_stats.num_fnext, 0);
	atomic_set(&tcon->stats.cifs_stats.num_fclose, 0);
	atomic_set(&tcon->stats.cifs_stats.num_hardlinks, 0);
	atomic_set(&tcon->stats.cifs_stats.num_symlinks, 0);
	atomic_set(&tcon->stats.cifs_stats.num_locks, 0);
	atomic_set(&tcon->stats.cifs_stats.num_acl_get, 0);
	atomic_set(&tcon->stats.cifs_stats.num_acl_set, 0);
}

static void
cifs_print_stats(struct seq_file *m, struct cifs_tcon *tcon)
{
	seq_printf(m, " Oplocks breaks: %d",
		   atomic_read(&tcon->stats.cifs_stats.num_oplock_brks));
	seq_printf(m, "\nReads:  %d Bytes: %llu",
		   atomic_read(&tcon->stats.cifs_stats.num_reads),
		   (long long)(tcon->bytes_read));
	seq_printf(m, "\nWrites: %d Bytes: %llu",
		   atomic_read(&tcon->stats.cifs_stats.num_writes),
		   (long long)(tcon->bytes_written));
	seq_printf(m, "\nFlushes: %d",
		   atomic_read(&tcon->stats.cifs_stats.num_flushes));
	seq_printf(m, "\nLocks: %d HardLinks: %d Symlinks: %d",
		   atomic_read(&tcon->stats.cifs_stats.num_locks),
		   atomic_read(&tcon->stats.cifs_stats.num_hardlinks),
		   atomic_read(&tcon->stats.cifs_stats.num_symlinks));
	seq_printf(m, "\nOpens: %d Closes: %d Deletes: %d",
		   atomic_read(&tcon->stats.cifs_stats.num_opens),
		   atomic_read(&tcon->stats.cifs_stats.num_closes),
		   atomic_read(&tcon->stats.cifs_stats.num_deletes));
	seq_printf(m, "\nPosix Opens: %d Posix Mkdirs: %d",
		   atomic_read(&tcon->stats.cifs_stats.num_posixopens),
		   atomic_read(&tcon->stats.cifs_stats.num_posixmkdirs));
	seq_printf(m, "\nMkdirs: %d Rmdirs: %d",
		   atomic_read(&tcon->stats.cifs_stats.num_mkdirs),
		   atomic_read(&tcon->stats.cifs_stats.num_rmdirs));
	seq_printf(m, "\nRenames: %d T2 Renames %d",
		   atomic_read(&tcon->stats.cifs_stats.num_renames),
		   atomic_read(&tcon->stats.cifs_stats.num_t2renames));
	seq_printf(m, "\nFindFirst: %d FNext %d FClose %d",
		   atomic_read(&tcon->stats.cifs_stats.num_ffirst),
		   atomic_read(&tcon->stats.cifs_stats.num_fnext),
		   atomic_read(&tcon->stats.cifs_stats.num_fclose));
}

static void
cifs_mkdir_setinfo(struct inode *inode, const char *full_path,
		   struct cifs_sb_info *cifs_sb, struct cifs_tcon *tcon,
		   const unsigned int xid)
{
	FILE_BASIC_INFO info;
	struct cifsInodeInfo *cifsInode;
	u32 dosattrs;
	int rc;

	memset(&info, 0, sizeof(info));
	cifsInode = CIFS_I(inode);
	dosattrs = cifsInode->cifsAttrs|ATTR_READONLY;
	info.Attributes = cpu_to_le32(dosattrs);
	rc = CIFSSMBSetPathInfo(xid, tcon, full_path, &info, cifs_sb->local_nls,
				cifs_sb);
	if (rc == 0)
		cifsInode->cifsAttrs = dosattrs;
}

static int cifs_open_file(const unsigned int xid, struct cifs_open_parms *oparms, __u32 *oplock,
			  void *buf)
{
	struct cifs_open_info_data *data = buf;
	FILE_ALL_INFO fi = {};
	int rc;

	if (!(oparms->tcon->ses->capabilities & CAP_NT_SMBS))
		rc = SMBLegacyOpen(xid, oparms->tcon, oparms->path,
				   oparms->disposition,
				   oparms->desired_access,
				   oparms->create_options,
				   &oparms->fid->netfid, oplock, &fi,
				   oparms->cifs_sb->local_nls,
				   cifs_remap(oparms->cifs_sb));
	else
		rc = CIFS_open(xid, oparms, oplock, &fi);

	if (!rc && data)
		move_cifs_info_to_smb2(&data->fi, &fi);

	return rc;
}

static void
cifs_set_fid(struct cifsFileInfo *cfile, struct cifs_fid *fid, __u32 oplock)
{
	struct cifsInodeInfo *cinode = CIFS_I(d_inode(cfile->dentry));
	cfile->fid.netfid = fid->netfid;
	cifs_set_oplock_level(cinode, oplock);
	cinode->can_cache_brlcks = CIFS_CACHE_WRITE(cinode);
}

static int
cifs_close_file(const unsigned int xid, struct cifs_tcon *tcon,
		struct cifs_fid *fid)
{
	return CIFSSMBClose(xid, tcon, fid->netfid);
}

static int
cifs_flush_file(const unsigned int xid, struct cifs_tcon *tcon,
		struct cifs_fid *fid)
{
	return CIFSSMBFlush(xid, tcon, fid->netfid);
}

static int
cifs_sync_read(const unsigned int xid, struct cifs_fid *pfid,
	       struct cifs_io_parms *parms, unsigned int *bytes_read,
	       char **buf, int *buf_type)
{
	parms->netfid = pfid->netfid;
	return CIFSSMBRead(xid, parms, bytes_read, buf, buf_type);
}

static int
cifs_sync_write(const unsigned int xid, struct cifs_fid *pfid,
		struct cifs_io_parms *parms, unsigned int *written,
		struct kvec *iov, unsigned long nr_segs)
{

	parms->netfid = pfid->netfid;
	return CIFSSMBWrite2(xid, parms, written, iov, nr_segs);
}

static int
smb_set_file_info(struct inode *inode, const char *full_path,
		  FILE_BASIC_INFO *buf, const unsigned int xid)
{
	int oplock = 0;
	int rc;
	__u32 netpid;
	struct cifs_fid fid;
	struct cifs_open_parms oparms;
	struct cifsFileInfo *open_file;
	FILE_BASIC_INFO new_buf;
	struct cifs_open_info_data query_data;
	__le64 write_time = buf->LastWriteTime;
	struct cifsInodeInfo *cinode = CIFS_I(inode);
	struct cifs_sb_info *cifs_sb = CIFS_SB(inode->i_sb);
	struct tcon_link *tlink = NULL;
	struct cifs_tcon *tcon;

	/* if the file is already open for write, just use that fileid */
	open_file = find_writable_file(cinode, FIND_WR_FSUID_ONLY);

	if (open_file) {
		fid.netfid = open_file->fid.netfid;
		netpid = open_file->pid;
		tcon = tlink_tcon(open_file->tlink);
	} else {
		tlink = cifs_sb_tlink(cifs_sb);
		if (IS_ERR(tlink)) {
			rc = PTR_ERR(tlink);
			tlink = NULL;
			goto out;
		}
		tcon = tlink_tcon(tlink);
	}

	/*
	 * Non-NT servers interprets zero time value in SMB_SET_FILE_BASIC_INFO
	 * over TRANS2_SET_FILE_INFORMATION as a valid time value. NT servers
	 * interprets zero time value as do not change existing value on server.
	 * API of ->set_file_info() callback expects that zero time value has
	 * the NT meaning - do not change. Therefore if server is non-NT and
	 * some time values in "buf" are zero, then fetch missing time values.
	 */
	if (!(tcon->ses->capabilities & CAP_NT_SMBS) &&
	    (!buf->CreationTime || !buf->LastAccessTime ||
	     !buf->LastWriteTime || !buf->ChangeTime)) {
		rc = cifs_query_path_info(xid, tcon, cifs_sb, full_path, &query_data);
		if (rc) {
			if (open_file) {
				cifsFileInfo_put(open_file);
				open_file = NULL;
			}
			goto out;
		}
		/*
		 * Original write_time from buf->LastWriteTime is preserved
		 * as SMBSetInformation() interprets zero as do not change.
		 */
		new_buf = *buf;
		buf = &new_buf;
		if (!buf->CreationTime)
			buf->CreationTime = query_data.fi.CreationTime;
		if (!buf->LastAccessTime)
			buf->LastAccessTime = query_data.fi.LastAccessTime;
		if (!buf->LastWriteTime)
			buf->LastWriteTime = query_data.fi.LastWriteTime;
		if (!buf->ChangeTime)
			buf->ChangeTime = query_data.fi.ChangeTime;
	}

	if (open_file)
		goto set_via_filehandle;

	rc = CIFSSMBSetPathInfo(xid, tcon, full_path, buf, cifs_sb->local_nls,
				cifs_sb);
	if (rc == 0) {
		cinode->cifsAttrs = le32_to_cpu(buf->Attributes);
		goto out;
	} else if (rc != -EOPNOTSUPP && rc != -EINVAL) {
		goto out;
	}

	oparms = (struct cifs_open_parms) {
		.tcon = tcon,
		.cifs_sb = cifs_sb,
		.desired_access = SYNCHRONIZE | FILE_WRITE_ATTRIBUTES,
		.create_options = cifs_create_options(cifs_sb, CREATE_NOT_DIR),
		.disposition = FILE_OPEN,
		.path = full_path,
		.fid = &fid,
	};

	if (S_ISDIR(inode->i_mode) && !(tcon->ses->capabilities & CAP_NT_SMBS)) {
		/* Opening directory path is not possible on non-NT servers. */
		rc = -EOPNOTSUPP;
	} else {
		/*
		 * Use cifs_open_file() instead of CIFS_open() as the
		 * cifs_open_file() selects the correct function which
		 * works also on non-NT servers.
		 */
		rc = cifs_open_file(xid, &oparms, &oplock, NULL);
		/*
		 * Opening path for writing on non-NT servers is not
		 * possible when the read-only attribute is already set.
		 * Non-NT server in this case returns -EACCES. For those
		 * servers the only possible way how to clear the read-only
		 * bit is via SMB_COM_SETATTR command.
		 */
		if (rc == -EACCES &&
		    (cinode->cifsAttrs & ATTR_READONLY) &&
		     le32_to_cpu(buf->Attributes) != 0 && /* 0 = do not change attrs */
		     !(le32_to_cpu(buf->Attributes) & ATTR_READONLY) &&
		     !(tcon->ses->capabilities & CAP_NT_SMBS))
			rc = -EOPNOTSUPP;
	}

	/* Fallback to SMB_COM_SETATTR command when absolutelty needed. */
	if (rc == -EOPNOTSUPP) {
		cifs_dbg(FYI, "calling SetInformation since SetPathInfo for attrs/times not supported by this server\n");
		rc = SMBSetInformation(xid, tcon, full_path,
				       buf->Attributes != 0 ? buf->Attributes : cpu_to_le32(cinode->cifsAttrs),
				       write_time,
				       cifs_sb->local_nls, cifs_sb);
		if (rc == 0)
			cinode->cifsAttrs = le32_to_cpu(buf->Attributes);
		else
			rc = -EACCES;
		goto out;
	}

	if (rc != 0) {
		if (rc == -EIO)
			rc = -EINVAL;
		goto out;
	}

	netpid = current->tgid;
	cifs_dbg(FYI, "calling SetFileInfo since SetPathInfo for attrs/times not supported by this server\n");

set_via_filehandle:
	rc = CIFSSMBSetFileInfo(xid, tcon, buf, fid.netfid, netpid);
	if (!rc)
		cinode->cifsAttrs = le32_to_cpu(buf->Attributes);

	if (open_file == NULL)
		CIFSSMBClose(xid, tcon, fid.netfid);
	else
		cifsFileInfo_put(open_file);

	/*
	 * Setting the read-only bit is not honered on non-NT servers when done
	 * via open-semantics. So for setting it, use SMB_COM_SETATTR command.
	 * This command works only after the file is closed, so use it only when
	 * operation was called without the filehandle.
	 */
	if (open_file == NULL &&
	    !(tcon->ses->capabilities & CAP_NT_SMBS) &&
	    le32_to_cpu(buf->Attributes) & ATTR_READONLY) {
		SMBSetInformation(xid, tcon, full_path,
				  buf->Attributes,
				  0 /* do not change write time */,
				  cifs_sb->local_nls, cifs_sb);
	}
out:
	if (tlink != NULL)
		cifs_put_tlink(tlink);
	return rc;
}

static int
cifs_set_compression(const unsigned int xid, struct cifs_tcon *tcon,
		   struct cifsFileInfo *cfile)
{
	return CIFSSMB_set_compression(xid, tcon, cfile->fid.netfid);
}

static int
cifs_query_dir_first(const unsigned int xid, struct cifs_tcon *tcon,
		     const char *path, struct cifs_sb_info *cifs_sb,
		     struct cifs_fid *fid, __u16 search_flags,
		     struct cifs_search_info *srch_inf)
{
	int rc;

	rc = CIFSFindFirst(xid, tcon, path, cifs_sb,
			   &fid->netfid, search_flags, srch_inf, true);
	if (rc)
		cifs_dbg(FYI, "find first failed=%d\n", rc);
	return rc;
}

static int
cifs_query_dir_next(const unsigned int xid, struct cifs_tcon *tcon,
		    struct cifs_fid *fid, __u16 search_flags,
		    struct cifs_search_info *srch_inf)
{
	return CIFSFindNext(xid, tcon, fid->netfid, search_flags, srch_inf);
}

static int
cifs_close_dir(const unsigned int xid, struct cifs_tcon *tcon,
	       struct cifs_fid *fid)
{
	return CIFSFindClose(xid, tcon, fid->netfid);
}

static int
cifs_oplock_response(struct cifs_tcon *tcon, __u64 persistent_fid,
		__u64 volatile_fid, __u16 net_fid, struct cifsInodeInfo *cinode)
{
	return CIFSSMBLock(0, tcon, net_fid, current->tgid, 0, 0, 0, 0,
			   LOCKING_ANDX_OPLOCK_RELEASE, false, CIFS_CACHE_READ(cinode) ? 1 : 0);
}

static int
cifs_queryfs(const unsigned int xid, struct cifs_tcon *tcon,
	     const char *path, struct cifs_sb_info *cifs_sb, struct kstatfs *buf)
{
	int rc = -EOPNOTSUPP;

	buf->f_type = CIFS_SUPER_MAGIC;

	/*
	 * We could add a second check for a QFS Unix capability bit
	 */
	if ((tcon->ses->capabilities & CAP_UNIX) &&
	    (CIFS_POSIX_EXTENSIONS & le64_to_cpu(tcon->fsUnixInfo.Capability)))
		rc = CIFSSMBQFSPosixInfo(xid, tcon, buf);

	/*
	 * Only need to call the old QFSInfo if failed on newer one,
	 * e.g. by OS/2.
	 **/
	if (rc && (tcon->ses->capabilities & CAP_NT_SMBS))
		rc = CIFSSMBQFSInfo(xid, tcon, buf);

	/*
	 * Some old Windows servers also do not support level 103, retry with
	 * older level one if old server failed the previous call or we
	 * bypassed it because we detected that this was an older LANMAN sess
	 */
	if (rc)
		rc = SMBOldQFSInfo(xid, tcon, buf);
	return rc;
}

static int
cifs_mand_lock(const unsigned int xid, struct cifsFileInfo *cfile, __u64 offset,
	       __u64 length, __u32 type, int lock, int unlock, bool wait)
{
	return CIFSSMBLock(xid, tlink_tcon(cfile->tlink), cfile->fid.netfid,
			   current->tgid, length, offset, unlock, lock,
			   (__u8)type, wait, 0);
}

static int
cifs_unix_dfs_readlink(const unsigned int xid, struct cifs_tcon *tcon,
		       const unsigned char *searchName, char **symlinkinfo,
		       const struct nls_table *nls_codepage)
{
#ifdef CONFIG_CIFS_DFS_UPCALL
	int rc;
	struct dfs_info3_param referral = {0};

	rc = get_dfs_path(xid, tcon->ses, searchName, nls_codepage, &referral,
			  0);

	if (!rc) {
		*symlinkinfo = kstrdup(referral.node_name, GFP_KERNEL);
		free_dfs_info_param(&referral);
		if (!*symlinkinfo)
			rc = -ENOMEM;
	}
	return rc;
#else /* No DFS support */
	return -EREMOTE;
#endif
}

static int cifs_query_symlink(const unsigned int xid,
			      struct cifs_tcon *tcon,
			      struct cifs_sb_info *cifs_sb,
			      const char *full_path,
			      char **target_path)
{
	int rc;

	cifs_tcon_dbg(FYI, "%s: path=%s\n", __func__, full_path);

	if (!cap_unix(tcon->ses))
		return -EOPNOTSUPP;

	rc = CIFSSMBUnixQuerySymLink(xid, tcon, full_path, target_path,
				     cifs_sb->local_nls, cifs_remap(cifs_sb));
	if (rc == -EREMOTE)
		rc = cifs_unix_dfs_readlink(xid, tcon, full_path,
					    target_path, cifs_sb->local_nls);
	return rc;
}

static struct reparse_data_buffer *cifs_get_reparse_point_buffer(const struct kvec *rsp_iov,
								 u32 *plen)
{
	TRANSACT_IOCTL_RSP *io = rsp_iov->iov_base;
	*plen = le16_to_cpu(io->ByteCount);
	return (struct reparse_data_buffer *)((__u8 *)&io->hdr.Protocol +
					      le32_to_cpu(io->DataOffset));
}

static bool
cifs_is_read_op(__u32 oplock)
{
	return oplock == OPLOCK_READ;
}

static unsigned int
cifs_wp_retry_size(struct inode *inode)
{
	return CIFS_SB(inode->i_sb)->ctx->wsize;
}

static bool
cifs_dir_needs_close(struct cifsFileInfo *cfile)
{
	return !cfile->srch_inf.endOfSearch && !cfile->invalidHandle;
}

static bool
cifs_can_echo(struct TCP_Server_Info *server)
{
	if (server->tcpStatus == CifsGood)
		return true;

	return false;
}

static int
cifs_make_node(unsigned int xid, struct inode *inode,
	       struct dentry *dentry, struct cifs_tcon *tcon,
	       const char *full_path, umode_t mode, dev_t dev)
{
	struct cifs_sb_info *cifs_sb = CIFS_SB(inode->i_sb);
	struct inode *newinode = NULL;
	int rc;

	if (tcon->unix_ext) {
		/*
		 * SMB1 Unix Extensions: requires server support but
		 * works with all special files
		 */
		struct cifs_unix_set_info_args args = {
			.mode	= mode & ~current_umask(),
			.ctime	= NO_CHANGE_64,
			.atime	= NO_CHANGE_64,
			.mtime	= NO_CHANGE_64,
			.device	= dev,
		};
		if (cifs_sb->mnt_cifs_flags & CIFS_MOUNT_SET_UID) {
			args.uid = current_fsuid();
			args.gid = current_fsgid();
		} else {
			args.uid = INVALID_UID; /* no change */
			args.gid = INVALID_GID; /* no change */
		}
		rc = CIFSSMBUnixSetPathInfo(xid, tcon, full_path, &args,
					    cifs_sb->local_nls,
					    cifs_remap(cifs_sb));
		if (rc)
			return rc;

		rc = cifs_get_inode_info_unix(&newinode, full_path,
					      inode->i_sb, xid);

		if (rc == 0)
			d_instantiate(dentry, newinode);
		return rc;
	} else if (cifs_sb->mnt_cifs_flags & CIFS_MOUNT_UNX_EMUL) {
		/*
		 * Check if mounted with mount parm 'sfu' mount parm.
		 * SFU emulation should work with all servers
		 * and was used by default in earlier versions of Windows.
		 */
		return cifs_sfu_make_node(xid, inode, dentry, tcon,
					  full_path, mode, dev);
	} else if (le32_to_cpu(tcon->fsAttrInfo.Attributes) & FILE_SUPPORTS_REPARSE_POINTS) {
		/*
		 * mknod via reparse points requires server support for
		 * storing reparse points, which is available since
		 * Windows 2000, but was not widely used until release
		 * of Windows Server 2012 by the Windows NFS server.
		 */
		return mknod_reparse(xid, inode, dentry, tcon,
				     full_path, mode, dev);
	} else {
		return -EOPNOTSUPP;
	}
}

static bool
cifs_is_network_name_deleted(char *buf, struct TCP_Server_Info *server)
{
	struct smb_hdr *shdr = (struct smb_hdr *)buf;
	struct TCP_Server_Info *pserver;
	struct cifs_ses *ses;
	struct cifs_tcon *tcon;

	if (shdr->Flags2 & SMBFLG2_ERR_STATUS) {
		if (shdr->Status.CifsError != cpu_to_le32(NT_STATUS_NETWORK_NAME_DELETED))
			return false;
	} else {
		if (shdr->Status.DosError.ErrorClass != ERRSRV ||
		    shdr->Status.DosError.Error != cpu_to_le16(ERRinvtid))
			return false;
	}

	/* If server is a channel, select the primary channel */
	pserver = SERVER_IS_CHAN(server) ? server->primary_server : server;

	spin_lock(&cifs_tcp_ses_lock);
	list_for_each_entry(ses, &pserver->smb_ses_list, smb_ses_list) {
		if (cifs_ses_exiting(ses))
			continue;
		list_for_each_entry(tcon, &ses->tcon_list, tcon_list) {
			if (tcon->tid == shdr->Tid) {
				spin_lock(&tcon->tc_lock);
				tcon->need_reconnect = true;
				spin_unlock(&tcon->tc_lock);
				spin_unlock(&cifs_tcp_ses_lock);
				pr_warn_once("Server share %s deleted.\n",
					     tcon->tree_name);
				return true;
			}
		}
	}
	spin_unlock(&cifs_tcp_ses_lock);

	return false;
}

struct smb_version_operations smb1_operations = {
	.send_cancel = send_nt_cancel,
	.compare_fids = cifs_compare_fids,
	.setup_request = cifs_setup_request,
	.setup_async_request = cifs_setup_async_request,
	.check_receive = cifs_check_receive,
	.add_credits = cifs_add_credits,
	.set_credits = cifs_set_credits,
	.get_credits_field = cifs_get_credits_field,
	.get_credits = cifs_get_credits,
	.wait_mtu_credits = cifs_wait_mtu_credits,
	.get_next_mid = cifs_get_next_mid,
	.read_data_offset = cifs_read_data_offset,
	.read_data_length = cifs_read_data_length,
	.map_error = map_smb_to_linux_error,
	.find_mid = cifs_find_mid,
	.check_message = checkSMB,
	.dump_detail = cifs_dump_detail,
	.clear_stats = cifs_clear_stats,
	.print_stats = cifs_print_stats,
	.is_oplock_break = is_valid_oplock_break,
	.downgrade_oplock = cifs_downgrade_oplock,
	.check_trans2 = cifs_check_trans2,
	.need_neg = cifs_need_neg,
	.negotiate = cifs_negotiate,
	.negotiate_wsize = smb1_negotiate_wsize,
	.negotiate_rsize = smb1_negotiate_rsize,
	.sess_setup = CIFS_SessSetup,
	.logoff = CIFSSMBLogoff,
	.tree_connect = CIFSTCon,
	.tree_disconnect = CIFSSMBTDis,
	.get_dfs_refer = CIFSGetDFSRefer,
	.qfs_tcon = cifs_qfs_tcon,
	.is_path_accessible = cifs_is_path_accessible,
	.can_echo = cifs_can_echo,
	.query_path_info = cifs_query_path_info,
	.query_reparse_point = cifs_query_reparse_point,
	.query_file_info = cifs_query_file_info,
	.get_srv_inum = cifs_get_srv_inum,
	.set_path_size = CIFSSMBSetEOF,
	.set_file_size = CIFSSMBSetFileSize,
	.set_file_info = smb_set_file_info,
	.set_compression = cifs_set_compression,
	.echo = CIFSSMBEcho,
	.mkdir = CIFSSMBMkDir,
	.mkdir_setinfo = cifs_mkdir_setinfo,
	.rmdir = CIFSSMBRmDir,
	.unlink = CIFSSMBDelFile,
	.rename_pending_delete = cifs_rename_pending_delete,
	.rename = CIFSSMBRename,
	.create_hardlink = CIFSCreateHardLink,
	.query_symlink = cifs_query_symlink,
	.get_reparse_point_buffer = cifs_get_reparse_point_buffer,
	.create_reparse_inode = cifs_create_reparse_inode,
	.open = cifs_open_file,
	.set_fid = cifs_set_fid,
	.close = cifs_close_file,
	.flush = cifs_flush_file,
	.async_readv = cifs_async_readv,
	.async_writev = cifs_async_writev,
	.sync_read = cifs_sync_read,
	.sync_write = cifs_sync_write,
	.query_dir_first = cifs_query_dir_first,
	.query_dir_next = cifs_query_dir_next,
	.close_dir = cifs_close_dir,
	.calc_smb_size = smbCalcSize,
	.oplock_response = cifs_oplock_response,
	.queryfs = cifs_queryfs,
	.mand_lock = cifs_mand_lock,
	.mand_unlock_range = cifs_unlock_range,
	.push_mand_locks = cifs_push_mandatory_locks,
	.query_mf_symlink = cifs_query_mf_symlink,
	.create_mf_symlink = cifs_create_mf_symlink,
	.is_read_op = cifs_is_read_op,
	.wp_retry_size = cifs_wp_retry_size,
	.dir_needs_close = cifs_dir_needs_close,
	.select_sectype = cifs_select_sectype,
#ifdef CONFIG_CIFS_XATTR
	.query_all_EAs = CIFSSMBQAllEAs,
	.set_EA = CIFSSMBSetEA,
#endif /* CIFS_XATTR */
	.get_acl = get_cifs_acl,
	.get_acl_by_fid = get_cifs_acl_by_fid,
	.set_acl = set_cifs_acl,
	.make_node = cifs_make_node,
	.is_network_name_deleted = cifs_is_network_name_deleted,
};

struct smb_version_values smb1_values = {
	.version_string = SMB1_VERSION_STRING,
	.protocol_id = SMB10_PROT_ID,
	.large_lock_type = LOCKING_ANDX_LARGE_FILES,
	.exclusive_lock_type = 0,
	.shared_lock_type = LOCKING_ANDX_SHARED_LOCK,
	.unlock_lock_type = 0,
	.header_preamble_size = 4,
	.header_size = sizeof(struct smb_hdr),
	.max_header_size = MAX_CIFS_HDR_SIZE,
	.read_rsp_size = sizeof(READ_RSP),
	.lock_cmd = cpu_to_le16(SMB_COM_LOCKING_ANDX),
	.cap_unix = CAP_UNIX,
	.cap_nt_find = CAP_NT_SMBS | CAP_NT_FIND,
	.cap_large_files = CAP_LARGE_FILES,
	.cap_unicode = CAP_UNICODE,
	.signing_enabled = SECMODE_SIGN_ENABLED,
	.signing_required = SECMODE_SIGN_REQUIRED,
};
