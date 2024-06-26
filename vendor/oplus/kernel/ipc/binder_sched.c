// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 Oplus. All rights reserved.
 */


#include <linux/seq_file.h>
#include <drivers/android/binder_internal.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <trace/hooks/binder.h>
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
#include <../kernel/oplus_cpu/sched/sched_assist/sa_common.h>
#endif

unsigned int g_sched_enable = 1;
unsigned int g_sched_debug = 0;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
static inline void binder_set_inherit_ux(struct task_struct *thread_task, struct task_struct *from_task, bool sync)
{
	int from_depth = oplus_get_ux_depth(from_task);
	int from_state = oplus_get_ux_state(from_task);
	int type = get_ux_state_type(thread_task);

	if (type != UX_STATE_NONE && type != UX_STATE_INHERIT)
		return;

	if (from_task && test_set_inherit_ux(from_task)) {
		if (!test_inherit_ux(thread_task, INHERIT_UX_BINDER))
			set_inherit_ux(thread_task, INHERIT_UX_BINDER, from_depth, from_state);
		else
			reset_inherit_ux(thread_task, from_task, INHERIT_UX_BINDER);
	} else if (from_task && test_task_is_rt(from_task)) { /* rt trans can be set as ux if binder thread is cfs class */
		if (!test_inherit_ux(thread_task, INHERIT_UX_BINDER)) {
			int ux_value = SA_TYPE_LIGHT;

			set_inherit_ux(thread_task, INHERIT_UX_BINDER, from_depth, ux_value);
		}
	}
}

static inline void binder_unset_inherit_ux(struct task_struct *thread_task)
{
	if (test_inherit_ux(thread_task, INHERIT_UX_BINDER))
		unset_inherit_ux(thread_task, INHERIT_UX_BINDER);
}
#endif

/* implement vender hook in driver/android/binder.c */
void android_vh_binder_restore_priority_handler(void *unused, struct binder_transaction *t, struct task_struct *task)
{
	if (unlikely(!g_sched_enable))
		return;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	if (t != NULL)
		binder_unset_inherit_ux(task);
#endif
}

void android_vh_binder_wait_for_work_handler(void *unused,
			bool do_proc_work, struct binder_thread *tsk, struct binder_proc *proc)
{
	if (unlikely(!g_sched_enable))
		return;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	if (do_proc_work)
		binder_unset_inherit_ux(tsk->task);
#endif
}

void android_vh_sync_txn_recvd_handler(void *unused, struct task_struct *tsk, struct task_struct *from)
{
	if (unlikely(!g_sched_enable))
		return;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	binder_set_inherit_ux(tsk, from, false);
#endif
}

#ifdef CONFIG_OPLUS_BINDER_PRIO_SKIP
static void android_vh_binder_priority_skip_handler(void *unused, struct task_struct *task, bool *skip)
{
	if (task->prio < MAX_RT_PRIO)
		*skip = true;
}
#endif

static void android_vh_binder_proc_transaction_end_handler(void *unused, struct task_struct *caller_task, struct task_struct *binder_proc_task,
		struct task_struct *binder_th_task, unsigned int code,
		bool pending_async, bool sync)
{
	bool set_ux = sync;
	struct task_struct *grp_leader = NULL;

	if (unlikely(!g_sched_enable))
		return;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_SCHED_ASSIST)
	if (sync && (!strncmp(binder_proc_task->comm, "servicemanager", TASK_COMM_LEN)
				|| !strncmp(binder_proc_task->comm, "hwservicemanage", TASK_COMM_LEN))) {
		binder_set_inherit_ux(binder_proc_task, current, sync);
		if (unlikely(g_sched_debug))
			trace_printk("caller_task(comm=%-12s pid=%d tgid=%d) binder_proc_task(comm=%-12s pid=%d tgid=%d) code=%d sync=%d\n",
				caller_task->comm, caller_task->pid, caller_task->tgid,
				binder_proc_task->comm, binder_proc_task->pid, binder_proc_task->tgid,
				code, sync);
	}

	if (!binder_th_task)
		return;

	grp_leader = binder_th_task->group_leader;
	if (grp_leader) {
		struct oplus_task_struct *ots = get_oplus_task_struct(current);

		if (!IS_ERR_OR_NULL(ots) && (ots->im_flag == IM_FLAG_SURFACEFLINGER) && !sync && test_task_ux(grp_leader))
			set_ux = true;
	}

	if (set_ux)
		binder_set_inherit_ux(binder_th_task, current, sync);

	if (unlikely(g_sched_debug)) {
		trace_printk("caller_task(comm=%-12s pid=%d tgid=%d) binder_proc_task(comm=%-12s pid=%d tgid=%d) binder_th_task(comm=%-12s pid=%d tgid=%d) code=%d sync=%d\n",
			caller_task->comm, caller_task->pid, caller_task->tgid,
			binder_proc_task->comm, binder_proc_task->pid, binder_proc_task->tgid,
			binder_th_task->comm, binder_th_task->pid, binder_th_task->tgid,
			code, sync);
	}
#endif
}

void register_binder_sched_vendor_hooks(void)
{
	register_trace_android_vh_binder_restore_priority(
		android_vh_binder_restore_priority_handler, NULL);
	register_trace_android_vh_binder_wait_for_work(
		android_vh_binder_wait_for_work_handler, NULL);
	register_trace_android_vh_sync_txn_recvd(
		android_vh_sync_txn_recvd_handler, NULL);
#ifdef CONFIG_OPLUS_BINDER_PRIO_SKIP
	register_trace_android_vh_binder_priority_skip(
		android_vh_binder_priority_skip_handler, NULL);
#endif
	register_trace_android_vh_binder_proc_transaction_end(
		android_vh_binder_proc_transaction_end_handler, NULL);
}

int oplus_binder_sched_init(void)
{
	int ret = 0;

	register_binder_sched_vendor_hooks();
	return ret;
}

module_param_named(binder_sched_enable, g_sched_enable, uint, 0660);
module_param_named(binder_sched_debug, g_sched_debug, uint, 0660);
