/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_TASK_H
#define _LINUX_SCHED_TASK_H

/*
 * Interface between the scheduler and various task lifetime (fork()/exit())
 * functionality:
 */

#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/uaccess.h>

struct task_struct;
struct rusage;
union thread_union;
struct css_set;

/* All the bits taken by the old clone syscall. */
#define CLONE_LEGACY_FLAGS 0xffffffffULL

struct kernel_clone_args {
	u64 flags;
	int __user *pidfd;
	int __user *child_tid;
	int __user *parent_tid;
	const char *name;
	int exit_signal;
	u32 kthread:1;
	u32 io_thread:1;
	u32 user_worker:1;
	u32 no_files:1;
	unsigned long stack;
	unsigned long stack_size;
	unsigned long tls;
	pid_t *set_tid;
	/* Number of elements in *set_tid */
	size_t set_tid_size;
	int cgroup;
	int idle;
	int (*fn)(void *);
	void *fn_arg;
	struct cgroup *cgrp;
	struct css_set *cset;
	unsigned int kill_seq;
};

/*
 * This serializes "schedule()" and also protects
 * the run-queue from deletions/modifications (but
 * _adding_ to the beginning of the run-queue has
 * a separate lock).
 */
extern rwlock_t tasklist_lock;
extern spinlock_t mmlist_lock;

extern union thread_union init_thread_union;
extern struct task_struct init_task;

extern int lockdep_tasklist_lock_is_held(void);

extern asmlinkage void schedule_tail(struct task_struct *prev);
extern void init_idle(struct task_struct *idle, int cpu);

extern int sched_fork(unsigned long clone_flags, struct task_struct *p);
extern int sched_cgroup_fork(struct task_struct *p, struct kernel_clone_args *kargs);
extern void sched_cancel_fork(struct task_struct *p);
extern void sched_post_fork(struct task_struct *p);
extern void sched_dead(struct task_struct *p);

void __noreturn do_task_dead(void);
void __noreturn make_task_dead(int signr);

extern void mm_cache_init(void);
extern void proc_caches_init(void);

extern void fork_init(void);

extern void release_task(struct task_struct * p);

extern int copy_thread(struct task_struct *, const struct kernel_clone_args *);

extern void flush_thread(void);

#ifdef CONFIG_HAVE_EXIT_THREAD
extern void exit_thread(struct task_struct *tsk);
#else
static inline void exit_thread(struct task_struct *tsk)
{
}
#endif
extern __noreturn void do_group_exit(int);

extern void exit_files(struct task_struct *);
extern void exit_itimers(struct task_struct *);

extern pid_t kernel_clone(struct kernel_clone_args *kargs);
struct task_struct *copy_process(struct pid *pid, int trace, int node,
				 struct kernel_clone_args *args);
struct task_struct *create_io_thread(int (*fn)(void *), void *arg, int node);
struct task_struct *fork_idle(int);
extern pid_t kernel_thread(int (*fn)(void *), void *arg, const char *name,
			    unsigned long flags);
extern pid_t user_mode_thread(int (*fn)(void *), void *arg, unsigned long flags);
extern long kernel_wait4(pid_t, int __user *, int, struct rusage *);
int kernel_wait(pid_t pid, int *stat);

extern void free_task(struct task_struct *tsk);

/* sched_exec is called by processes performing an exec */
extern void sched_exec(void);

static inline struct task_struct *get_task_struct(struct task_struct *t)
{
	refcount_inc(&t->usage);
	return t;
}

static inline struct task_struct *tryget_task_struct(struct task_struct *t)
{
	return refcount_inc_not_zero(&t->usage) ? t : NULL;
}

extern void __put_task_struct(struct task_struct *t);
extern void __put_task_struct_rcu_cb(struct rcu_head *rhp);

static inline void put_task_struct(struct task_struct *t)
{
	if (!refcount_dec_and_test(&t->usage))
		return;

	/*
	 * Under PREEMPT_RT, we can't call __put_task_struct
	 * in atomic context because it will indirectly
	 * acquire sleeping locks. The same is true if the
	 * current process has a mutex enqueued (blocked on
	 * a PI chain).
	 *
	 * In !RT, it is always safe to call __put_task_struct().
	 * Though, in order to simplify the code, resort to the
	 * deferred call too.
	 *
	 * call_rcu() will schedule __put_task_struct_rcu_cb()
	 * to be called in process context.
	 *
	 * __put_task_struct() is called when
	 * refcount_dec_and_test(&t->usage) succeeds.
	 *
	 * This means that it can't "conflict" with
	 * put_task_struct_rcu_user() which abuses ->rcu the same
	 * way; rcu_users has a reference so task->usage can't be
	 * zero after rcu_users 1 -> 0 transition.
	 *
	 * delayed_free_task() also uses ->rcu, but it is only called
	 * when it fails to fork a process. Therefore, there is no
	 * way it can conflict with __put_task_struct().
	 */
	call_rcu(&t->rcu, __put_task_struct_rcu_cb);
}

DEFINE_FREE(put_task, struct task_struct *, if (_T) put_task_struct(_T))

static inline void put_task_struct_many(struct task_struct *t, int nr)
{
	if (refcount_sub_and_test(nr, &t->usage))
		__put_task_struct(t);
}

void put_task_struct_rcu_user(struct task_struct *task);

/* Free all architecture-specific resources held by a thread. */
void release_thread(struct task_struct *dead_task);

#ifdef CONFIG_ARCH_WANTS_DYNAMIC_TASK_STRUCT
extern int arch_task_struct_size __read_mostly;
#else
# define arch_task_struct_size (sizeof(struct task_struct))
#endif

#ifndef CONFIG_HAVE_ARCH_THREAD_STRUCT_WHITELIST
/*
 * If an architecture has not declared a thread_struct whitelist we
 * must assume something there may need to be copied to userspace.
 */
static inline void arch_thread_struct_whitelist(unsigned long *offset,
						unsigned long *size)
{
	*offset = 0;
	/* Handle dynamically sized thread_struct. */
	*size = arch_task_struct_size - offsetof(struct task_struct, thread);
}
#endif

#ifdef CONFIG_VMAP_STACK
static inline struct vm_struct *task_stack_vm_area(const struct task_struct *t)
{
	return t->stack_vm_area;
}
#else
static inline struct vm_struct *task_stack_vm_area(const struct task_struct *t)
{
	return NULL;
}
#endif

/*
 * Protects ->fs, ->files, ->mm, ->group_info, ->comm, keyring
 * subscriptions and synchronises with wait4().  Also used in procfs.  Also
 * pins the final release of task.io_context.  Also protects ->cpuset and
 * ->cgroup.subsys[]. And ->vfork_done. And ->sysvshm.shm_clist.
 *
 * Nests both inside and outside of read_lock(&tasklist_lock).
 * It must not be nested with write_lock_irq(&tasklist_lock),
 * neither inside nor outside.
 */
static inline void task_lock(struct task_struct *p)
{
	spin_lock(&p->alloc_lock);
}

static inline void task_unlock(struct task_struct *p)
{
	spin_unlock(&p->alloc_lock);
}

DEFINE_GUARD(task_lock, struct task_struct *, task_lock(_T), task_unlock(_T))

#endif /* _LINUX_SCHED_TASK_H */
