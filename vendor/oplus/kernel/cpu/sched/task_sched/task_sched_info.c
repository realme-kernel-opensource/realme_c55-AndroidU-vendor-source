// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2021 Oplus. All rights reserved.
 */
#include <linux/string.h>
#include <linux/kernel_stat.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <../fs/proc/internal.h>
#include <linux/uaccess.h>
#include <linux/timekeeping.h>
#include <linux/delay.h>
#include <linux/sched/clock.h>
#include <linux/cpufreq.h>
#include <linux/processor.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/spinlock.h>
#include <trace/events/sched.h>
#include <linux/workqueue.h>
#include <../kernel/sched/sched.h>
#include <linux/sched.h>
#include <trace/hooks/cpufreq.h>
#include <trace/hooks/sched.h>
#include <trace/events/power.h>
#include <trace/events/task.h>
#include "task_sched_info.h"
#include "../sched_assist/sa_common.h"
#include <asm/stacktrace.h>
#include <linux/kallsyms.h>

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_FRAME_BOOST)
#include <../kernel/oplus_cpu/sched/frame_boost/frame_group.h>
#endif

#define MAX_PID_NUM 10
#define BUFFER_SIZE 5760
#define NOTIFY_SIZE 2880
#define DURATION_THRESHOLD 16777215
#define CPU_NUM 8
#define NUM_LEN 21
#define SYSTEM_SERVER_NAME "SS"
#define SURFACE_FLINGER_NAME "SF"
#define MAX_THRESHOLD_NUM 5

#define REGISTER_TRACE_VH(vender_hook, handler) \
({ \
	rc = register_##vender_hook(handler, NULL); \
	if (rc) { \
		sched_err("CTP-3：register_"#vender_hook", ret=%d\n", rc); \
		return rc; \
		} \
})

#define REGISTER_TRACE_RVH		REGISTER_TRACE_VH

int systemserver_pid = -1;
int surfaceflinger_pid = -1;
unsigned int task_sched_info_enable;
static unsigned int arr_read;
static unsigned int arr_write;
u64 time_threshold[MAX_THRESHOLD_NUM] = {4000000, 4000000, 4000000, 4000000, 4000000};
static int target_pids[MAX_PID_NUM] = {-1};
unsigned int target_pids_num;
static u64 data_arr[BUFFER_SIZE];
static u64 datainfo[BUFFER_SIZE];
static int data_size = BUFFER_SIZE;
static unsigned int dt;
static int prev_freq[CPU_NUM] = {-1};
static int prev_freq_min[CPU_NUM] = {-1};
static int prev_freq_max[CPU_NUM] = {-1};

#define MAX_UEVENT_PARAM 4
static struct kobject *sched_kobj;
static struct kset *sched_kset;
static struct work_struct sched_detect_ws;
char *sched_detect_env[MAX_UEVENT_PARAM] = {"SCHEDACTION=uevent", NULL};
static int uevent_id;
static bool sched_info_ctrl = true;


static spinlock_t read_write_lock;
static DEFINE_SPINLOCK(read_write_lock);


bool ctp_send_message;

static u64 write_pid_time;

static int last_read_idx;

static unsigned long d_convert_info;

static void update_task_sched_info(struct task_struct *p, u64 delay, int type, int cpu);

static void sched_action_trig(int cpu)
{
	if (!sched_info_ctrl) {
		sched_err("sched_info_ctrl off\n");
		return;
	}

	if (sched_kobj == NULL) {
		sched_err("kobj NULL\n");
		return;
	}

	if (cpu > 0)
		return;

	sprintf(sched_detect_env[1], "SCHEDNUM=%d", uevent_id);
	sched_detect_env[MAX_UEVENT_PARAM - 2] = NULL;
	sched_detect_env[MAX_UEVENT_PARAM - 1] = NULL;
	schedule_work(&sched_detect_ws);
	uevent_id++;
}

static void sched_detect_work(struct work_struct *work)
{
	kobject_uevent_env(sched_kobj, KOBJ_CHANGE, sched_detect_env);
}

static void sched_action_init(void)
{
	int i = 0;

	sched_kobj = NULL;
	uevent_id = 0;

	sched_kset = kset_create_and_add("task_sched", NULL, kernel_kobj);
	if (!sched_kset) {
		sched_err("error creating sched_kset\n");
		return;
	}

	for (i = 1; i < MAX_UEVENT_PARAM - 2; i++) {
		sched_detect_env[i] = kzalloc(50, GFP_KERNEL);
		if (!sched_detect_env[i]) {
			sched_err("kzalloc sched uevent param failed\n");
			goto sched_action_init_free_memory;
		}
	}

	sched_kobj = kobject_create_and_add("task_sched_info", &sched_kset->kobj);
	if (sched_kobj == NULL) {
		sched_err("sched_kobj init err\n");
		goto sched_action_init_free_memory;
	}

	sched_kobj->kset = sched_kset;

	INIT_WORK(&sched_detect_ws, sched_detect_work);
	return;

sched_action_init_free_memory:
	kset_unregister(sched_kset);
	for (i--; i > 0; i--)
		kfree(sched_detect_env[i]);
	sched_err("Failed!\n");
}

static void get_target_thread_pid(struct task_struct *task)
{
	struct task_struct *grp;

	if (task_uid(task).val == 1000) {
		if (strstr(task->comm, "android.anim")) {
			systemserver_pid = task->tgid;
			return;
		}

		grp = task->group_leader;
		if (strstr(grp->comm, "surfaceflinger")) {
			surfaceflinger_pid = grp->pid;
			return;
		}
	}
}

static void update_wake_tid(struct task_struct *p, struct task_struct *cur, unsigned int type)
{
	u64 cur_tid = cur->pid;

	struct oplus_task_struct *ots = get_oplus_task_struct(p);

	if (IS_ERR_OR_NULL(ots))
		return;

	ots->wake_tid = cur_tid | (type << 16);
}

static void update_running_start_time(struct task_struct *prev, struct task_struct *next)
{
	u64 clock, delay;
	struct oplus_task_struct *ots_prev = get_oplus_task_struct(prev);
	struct oplus_task_struct *ots_next = get_oplus_task_struct(next);

	clock = cpu_clock(prev->cpu);
	if (!IS_ERR_OR_NULL(ots_prev)) {
		delay = clock - ots_prev->running_start_time;
		update_task_sched_info(prev, delay, 0, prev->cpu);
		ots_prev->running_start_time = 0;
	}

	if (!IS_ERR_OR_NULL(ots_next)) {
		ots_next->running_start_time = clock;
	}
}

static void put_in_arr(struct task_sched_info *info, u64 *data_arr)
{
	unsigned long flags;

	spin_lock_irqsave(&read_write_lock, flags);

	data_arr[arr_write] = info->sched_info_one;
	data_arr[arr_write + 1] = info->sched_info_two;

	arr_write = (arr_write + 2) < BUFFER_SIZE ? arr_write + 2 : 0;
	dt += 2;

	if (arr_read == arr_write)
		arr_read = (arr_read + 2) < BUFFER_SIZE ? arr_read + 2 : 0;

	spin_unlock_irqrestore(&read_write_lock, flags);

	if (dt >= NOTIFY_SIZE) {
		ctp_send_message = true;
		dt -= NOTIFY_SIZE;
	}
}

static u64 is_target_pid(int tgid)
{
	unsigned int num = 0;

	while (num < target_pids_num) {
		if (target_pids[num] == tgid)
			return num;
		num++;
	}

	return MAX_PID_NUM;
}

static void update_backtrace_info(struct task_struct *p, u64 start_time)
{
	struct task_sched_info task_backtrace;

	task_backtrace.sched_info_one = task_sched_info_backtrace | (p->pid << 8);
	task_backtrace.sched_info_two = (u64)get_wchan(p);

	if (!d_convert_info)
		d_convert_info = (unsigned long)task_backtrace.sched_info_two;
	put_in_arr(&task_backtrace, data_arr);
}

static void update_task_sched_info(struct task_struct *p, u64 delay, int type, int cpu)
{
	struct task_sched_info sched_info;
	u64 p_tid, wake_tid;
	u64 cur_sched_clock;
	u64 pid_num = MAX_PID_NUM;
	struct oplus_task_struct *ots = get_oplus_task_struct(p);

	if (!task_sched_info_enable)
		return;

	if (IS_ERR_OR_NULL(ots))
		return;

	if (!p)
		return;

	if (type > task_sched_info_backtrace || type < task_sched_info_running)
		return;

	if (delay < time_threshold[type])
		return;

	sched_info.sched_info_one = 0;
	sched_info.sched_info_two = 0;
	pid_num = is_target_pid(p->tgid);
	if (pid_num != MAX_PID_NUM) {
		p_tid = p->pid;
		wake_tid = ots->wake_tid;
		cur_sched_clock = cpu_clock(cpu);
		delay = delay >> 20;
		if (delay > DURATION_THRESHOLD)
			delay = DURATION_THRESHOLD;

		switch (type) {
		case task_sched_info_running:
			sched_info.sched_info_one = type | (cpu << 5);
			sched_info.sched_info_one = sched_info.sched_info_one | (cur_sched_clock << 8);
			sched_info.sched_info_two = delay | (p_tid << 24);
			sched_info.sched_info_two =  sched_info.sched_info_two | (pid_num << 58);
			break;

		case task_sched_info_runnable:
			sched_info.sched_info_one = type | (cpu << 5);
			sched_info.sched_info_one = sched_info.sched_info_one | (cur_sched_clock << 8);
			sched_info.sched_info_two = delay | (p_tid << 24);
			sched_info.sched_info_two = sched_info.sched_info_two | (wake_tid << 40);
			sched_info.sched_info_two =  sched_info.sched_info_two | (pid_num << 58);
			break;

		case task_sched_info_D:
			update_backtrace_info(p, cur_sched_clock);

		case task_sched_info_IO:
		case task_sched_info_S:
			wake_tid = wake_tid & (~(1 << 16));
			sched_info.sched_info_one = type | (cpu << 5);
			sched_info.sched_info_one = sched_info.sched_info_one | (cur_sched_clock << 8);
			sched_info.sched_info_two = delay | (p_tid << 24);
			sched_info.sched_info_two = sched_info.sched_info_two | (wake_tid << 40);
			sched_info.sched_info_two =  sched_info.sched_info_two | (pid_num << 58);
		}


		put_in_arr(&sched_info, data_arr);
	}
}

static void update_freq_info(struct cpufreq_policy *policy)
{
	u64 cur, clock, cpu;
	struct task_sched_info freq_info;

	if (!task_sched_info_enable)
		return;

	clock = cpu_clock(policy->cpu);
	cur = (u64)policy->cur;
	cpu = policy->cpu;

	if (prev_freq[policy->cpu] == cur)
		return;

	prev_freq[policy->cpu] = cur;

	freq_info.sched_info_one = task_sched_info_freq | (cpu << 5);
	freq_info.sched_info_one = freq_info.sched_info_one | (clock << 8);
	freq_info.sched_info_two = cur;
	put_in_arr(&freq_info, data_arr);
}

static void update_freq_limit_info(struct cpufreq_policy *policy)
{
	u64 min, max, clock, cur_tid, cpu;
	struct task_sched_info freq_limit_info;

	if (!task_sched_info_enable)
		return;

	cur_tid = current->pid;
	cpu = policy->cpu;
	min = (u64)policy->min;
	max = (u64)policy->max;

	if (prev_freq_min[policy->cpu] == min && prev_freq_max[policy->cpu] == max)
		return;

	prev_freq_min[cpu] = min;
	prev_freq_max[cpu] = max;

	clock = cpu_clock(policy->cpu);

	freq_limit_info.sched_info_one = task_sched_info_freq_limit | (cpu << 5);
	freq_limit_info.sched_info_one = freq_limit_info.sched_info_one | (clock << 8);
	freq_limit_info.sched_info_two = min | (max << 24);
	freq_limit_info.sched_info_two = freq_limit_info.sched_info_two | (cur_tid << 48);
	put_in_arr(&freq_limit_info, data_arr);
}

void update_cpu_isolate_info(int cpu, u64 type)
{
	u64 clock, cur_tid;
	struct task_sched_info isolate_info;

	if (!task_sched_info_enable)
		return;

	clock = cpu_clock(cpu);
	cur_tid = current->pid;

	isolate_info.sched_info_one = task_sched_info_isolate | (cpu << 5);
	isolate_info.sched_info_one = isolate_info.sched_info_one | (clock << 8);
	isolate_info.sched_info_two = type | (cur_tid << 8);
	put_in_arr(&isolate_info, data_arr);
}
EXPORT_SYMBOL(update_cpu_isolate_info);

void update_cpus_isolate_info(struct cpumask *cpus, u64 type)
{
	int cpu;

	if (!task_sched_info_enable)
		return;

	for_each_cpu(cpu, cpus)
		update_cpu_isolate_info(cpu, type);
}
EXPORT_SYMBOL(update_cpus_isolate_info);

static int proc_pids_set_show(struct seq_file *m, void *v)
{
	int i = 0;

	if (!task_sched_info_enable)
		return -EFAULT;

	seq_printf(m, "%llu ", write_pid_time);

	while (i < target_pids_num) {
		seq_printf(m, "%d ", target_pids[i]);
		i++;
	}
	seq_puts(m, "\n");

	return 0;
}

static int proc_pids_set_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_pids_set_show, inode);
}

static ssize_t proc_pids_set_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char buffer[NUM_LEN * MAX_PID_NUM];
	char *all, *single;
	const char *target = NULL;
	int err;
	struct timespec64 ts;

	if (!task_sched_info_enable)
		return -EFAULT;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	all = buffer;

	target_pids_num = 2;
	while ((single = strsep(&all, " ")) != NULL && target_pids_num < MAX_PID_NUM) {
		target = single;

		err = kstrtouint(target, 0, &target_pids[target_pids_num]);
		if (err) {
			target = NULL;
			continue;
		}
		target = NULL;
		target_pids_num++;
	}

	ktime_get_real_ts64(&ts);
	write_pid_time = (u64)ts.tv_sec * 1000000 + (u64)(ts.tv_nsec/1000);

	return count;
}

static const struct proc_ops proc_pids_set_operations = {
	.proc_open	=	proc_pids_set_open,
	.proc_read	=	seq_read,
	.proc_write	=	proc_pids_set_write,
	.proc_lseek	=	seq_lseek,
	.proc_release	=	single_release,
};

static void *sched_start(struct seq_file *m, loff_t *ppos)
{
	int *idx = m->private;

	*ppos = *ppos < last_read_idx ? last_read_idx : *ppos;

	*idx = *ppos;

	if (*idx >= BUFFER_SIZE)
		return NULL;

	return idx;
}

static void *sched_next(struct seq_file *m, void *v, loff_t *ppos)
{
	int *idx = (int *)v;
	int new = (*idx + 256) < data_size ? 256 : (data_size-*idx);

	*idx += new;
	*ppos += new;
	if (*idx == data_size)
		return NULL;

	return idx;
}

static int sched_show(struct seq_file *m, void *v)
{
	int *idx = (int *)v;
	int num = 0;
	int cur = *idx;

	while (num < 256 && cur < data_size) {
		seq_printf(m, "%llu %llu\n", datainfo[cur], datainfo[cur + 1]);
		num += 2;
		cur = num + *idx;
	}

	if (m->count == m->size)
		return 0;

	last_read_idx = cur;

	return 0;
}

static void sched_stop(struct seq_file *m, void *v)
{
}

static const struct seq_operations seq_ops = {
	.start	= sched_start,
	.next	= sched_next,
	.stop	= sched_stop,
	.show	= sched_show,
};

static int sched_buffer_open(struct inode *inode, struct file *file)
{
	unsigned int cur_read, cur_write;
	unsigned long flags;

	unsigned int num1 = 0;

	int *offs;

	if (!task_sched_info_enable)
		return -ENOMEM;

	memset(datainfo, 0, BUFFER_SIZE * sizeof(u64));
	spin_lock_irqsave(&read_write_lock, flags);

	data_size = (arr_write < arr_read) ? (BUFFER_SIZE + arr_write - arr_read) : arr_write - arr_read;
	cur_read = arr_read;
	cur_write = arr_write;

	arr_read = arr_write;

	dt = 0;

	if (cur_write < cur_read) {
		num1 = BUFFER_SIZE - cur_read;
		memcpy(datainfo, data_arr + cur_read, sizeof(u64) * num1);
		memcpy(datainfo + num1, data_arr, sizeof(u64) * cur_write);
	} else
		memcpy(datainfo, data_arr + cur_read, sizeof(u64) * data_size);

	spin_unlock_irqrestore(&read_write_lock, flags);

	last_read_idx = 0;

	offs = __seq_open_private(file, &seq_ops, sizeof(int));

	if (!offs)
		return -ENOMEM;

	return 0;
}

static int sched_buffer_release(struct inode *inode, struct file *file)
{
	last_read_idx = 0;

	return seq_release_private(inode, file);
}

static const struct proc_ops proc_sched_buffer_operations = {
	.proc_open	= sched_buffer_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= sched_buffer_release,
};

static int proc_task_sched_info_enable_show(struct seq_file *m, void *v)
{
	u64 cur_cpu_clock[CPU_NUM];
	struct timespec64 ts;
	unsigned int i = 0;
	u64 wall_time;

	if (!task_sched_info_enable)
		return -EFAULT;

	ktime_get_real_ts64(&ts);
	wall_time = (u64)ts.tv_sec * 1000000 + (u64)(ts.tv_nsec/1000);

	seq_printf(m, "%llu ", wall_time);

	while (i < CPU_NUM) {
		cur_cpu_clock[i] = cpu_clock(i);
		seq_printf(m, "%llu ", cur_cpu_clock[i]);
		i++;
	}
	seq_puts(m, "\n");
	return 0;
}

static int proc_task_sched_info_enable_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_task_sched_info_enable_show, inode);
}

static ssize_t proc_task_sched_info_enable_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char buffer[PROC_NUMBUF];
	int err, enable;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	err = kstrtouint(strstrip(buffer), 0, &enable);
	if (err) {
		sched_err("enable/disable task_sched_info_enable fail. buffer_info: %s\n", buffer);
		return err;
	}

	if (enable) {
		target_pids[0] = systemserver_pid;
		target_pids[1] = surfaceflinger_pid;
		target_pids_num = 2;
	}

	task_sched_info_enable = enable;
	if (!task_sched_info_enable) {
		arr_read = 0;
		arr_write = 0;
		dt = 0;
		memset(data_arr, 0, BUFFER_SIZE * sizeof(u64));
		memset(target_pids, -1, MAX_PID_NUM * sizeof(int));
		target_pids_num = 0;
		write_pid_time = 0;
		d_convert_info = 0;
	}

	return count;
}

static const struct proc_ops proc_task_sched_info_enable_operations = {
	.proc_open	=	proc_task_sched_info_enable_open,
	.proc_write	=	proc_task_sched_info_enable_write,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release	=	single_release,
};

static int proc_sched_info_threshold_show(struct seq_file *m, void *v)
{
	if (!task_sched_info_enable)
		return -EFAULT;

	seq_printf(m, "running:%llu\trunnable:%llu\tblock/IO:%llu\tD:%llu\tS:%llu\n", time_threshold[0],
			time_threshold[1], time_threshold[2], time_threshold[3], time_threshold[4]);
	return 0;
}

static int proc_sched_info_threshold_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_sched_info_threshold_show, inode);
}

static ssize_t proc_sched_info_threshold_write(struct file *file, const char __user *buf, size_t count, loff_t *ppos)
{
	char buffer[NUM_LEN * 5];
	char *all, *single;
	const char *target = NULL;
	int err, threshold_num = 0;

	memset(buffer, 0, sizeof(buffer));

	if (!task_sched_info_enable)
		return -EFAULT;

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	all = buffer;

	while ((single = strsep(&all, " ")) != NULL && threshold_num < MAX_THRESHOLD_NUM) {
		target = single;
		err = kstrtoull(target, 0, &time_threshold[threshold_num]);
		if (err) {
			sched_err("sched_info_threshold: %s\n", target);
			target = NULL;
			continue;
		}
		target = NULL;
		threshold_num++;
	}

	return count;
}

static const struct proc_ops proc_sched_info_threshold_operations = {
	.proc_open	=	proc_sched_info_threshold_open,
	.proc_write	=	proc_sched_info_threshold_write,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release	=	single_release,
};

static int proc_d_convert_show(struct seq_file *m, void *v)
{
	if (!task_sched_info_enable)
		return -EFAULT;

	if (d_convert_info)
		seq_printf(m, "%pS %px\n", (void *)d_convert_info, (void *)d_convert_info);
	else
		seq_puts(m, "\n");

	return 0;
}

static int proc_d_convert_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_d_convert_show, inode);
}

static const struct proc_ops proc_d_convert_operations = {
	.proc_open	=	proc_d_convert_open,
	.proc_read	=	seq_read,
	.proc_lseek	=	seq_lseek,
	.proc_release	=	single_release,
};


#define SCHED_INFO_DIR "task_info"
#define SCHED_INFO_PROC_NODE "task_sched_info"
#define SCHED_INFO_PROC_EXIST_NODE "task_info/task_sched_info"
static struct proc_dir_entry *task_info;
static struct proc_dir_entry *sched_info;

int sched_info_init(void)
{
	struct proc_dir_entry *proc_entry;

	task_info = NULL;
	sched_info = NULL;

	task_sched_info_enable = 0;
	arr_read = 0;
	arr_write = 0;
	target_pids_num = 0;
	dt = 0;
	ctp_send_message = false;
	write_pid_time = 0;
	last_read_idx = 0;
	d_convert_info = 0;

	sched_action_init();

	task_info = proc_mkdir(SCHED_INFO_DIR, NULL);

	if (!task_info)
		sched_info = proc_mkdir(SCHED_INFO_PROC_EXIST_NODE, NULL);
	else
		sched_info = proc_mkdir(SCHED_INFO_PROC_NODE, task_info);

	if (!sched_info) {
		sched_err("create task_sched_info fail\n");
		goto ERROR_INIT_VERSION;
	}

	proc_entry = proc_create("pids_set", 0666, sched_info, &proc_pids_set_operations);
	if (!proc_entry) {
		sched_err("create pids_set fail\n");
		goto ERROR_INIT_VERSION;
	}

	proc_entry = proc_create("sched_buffer", 0666, sched_info, &proc_sched_buffer_operations);
	if (!proc_entry) {
		sched_err("create sched_buffer fail\n");
		goto ERROR_INIT_VERSION;
	}

	proc_entry = proc_create("task_sched_info_enable", 0666, sched_info, &proc_task_sched_info_enable_operations);
	if (!proc_entry) {
		sched_err("create task_sched_info_enable fail\n");
		goto ERROR_INIT_VERSION;
	}

	proc_entry = proc_create("sched_info_threshold", 0666, sched_info, &proc_sched_info_threshold_operations);
	if (!proc_entry) {
		sched_err("create sched_info_threshold fail\n");
		goto ERROR_INIT_VERSION;
	}

	proc_entry = proc_create("d_convert", 0666, sched_info, &proc_d_convert_operations);
	if (!proc_entry) {
		sched_err("create d_convert fail\n");
		goto ERROR_INIT_VERSION;
	}

	return 0;

ERROR_INIT_VERSION:
	remove_proc_entry(SCHED_INFO_PROC_NODE, NULL);
	return -ENOENT;
}


static void ctp_send_message_handler(void *data, struct task_struct *p, struct rq *rq, int user_tick)
{
	if (!task_sched_info_enable)
		return;

	if (ctp_send_message) {
		sched_action_trig(rq->cpu);
		ctp_send_message = false;
	}
}

static void set_task_comm_handler(void *data, struct task_struct *tsk, const char *comm)
{
	get_target_thread_pid(tsk);
}

static void sched_stat_wait_handler(void *data, struct task_struct *tsk, u64 delta)
{
	if (!task_sched_info_enable)
		return;

	update_task_sched_info(tsk, delta, task_sched_info_runnable, task_cpu(tsk));
}

static void sched_stat_sleep_handler(void *data, struct task_struct *tsk, u64 delta)
{
	if (!task_sched_info_enable)
		return;

	update_task_sched_info(tsk, delta, task_sched_info_S, task_cpu(tsk));
}

static void sched_stat_blocked_handler(void *data, struct task_struct *tsk, u64 delta)
{
	if (!task_sched_info_enable)
		return;

	if (tsk->in_iowait)
		update_task_sched_info(tsk, delta, task_sched_info_IO, task_cpu(tsk));
	else
		update_task_sched_info(tsk, delta, task_sched_info_D, task_cpu(tsk));
}

static void sched_switch_handler(void *data, bool preempt, struct task_struct *prev, struct task_struct *next)
{
	if (!task_sched_info_enable)
		return;

	update_wake_tid(prev, next, running_runnable);

	update_running_start_time(prev, next);
}

static void sched_waking_handler(void *data, struct task_struct *p)
{
	if (!task_sched_info_enable)
		return;

	update_wake_tid(p, current, block_runnable);
}

static void cpu_frequency_limits_handler(void *data, struct cpufreq_policy *policy)
{
	if (!task_sched_info_enable)
		return;

	update_freq_limit_info(policy);
}

static void cpufreq_transition_handler(void *data, struct cpufreq_policy *policy)
{
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_FRAME_BOOST)
	fbg_android_rvh_cpufreq_transition(policy);
#endif

	if (!task_sched_info_enable)
		return;

	update_freq_info(policy);
}


int register_sched_info_vendor_hooks(void)
{
	int rc = 0;

	REGISTER_TRACE_VH(trace_android_vh_account_task_time, ctp_send_message_handler);
	REGISTER_TRACE_VH(trace_task_rename, set_task_comm_handler);
	REGISTER_TRACE_VH(trace_sched_stat_wait, sched_stat_wait_handler);
	REGISTER_TRACE_VH(trace_sched_stat_sleep, sched_stat_sleep_handler);
	REGISTER_TRACE_VH(trace_sched_stat_blocked, sched_stat_blocked_handler);
	REGISTER_TRACE_VH(trace_sched_switch, sched_switch_handler);
	REGISTER_TRACE_VH(trace_sched_waking, sched_waking_handler);
	REGISTER_TRACE_VH(trace_cpu_frequency_limits, cpu_frequency_limits_handler);
	REGISTER_TRACE_RVH(trace_android_rvh_cpufreq_transition, cpufreq_transition_handler);

	return rc;
}

static int __init oplus_task_sched_init(void)
{
	int rc;

	rc = sched_info_init();
	if (rc != 0)
		return rc;

	rc = register_sched_info_vendor_hooks();
	if (rc != 0)
		return rc;

	return 0;
}

static int unregister_vendor_hooks(void)
{
	int rc = 0;

	rc = unregister_trace_android_vh_account_task_time(ctp_send_message_handler, NULL);
	if (rc != 0) {
		pr_err("CTP3：unregister_trace_android_vh_account_task_time failed! rc=%d\n", rc);
		return rc;
	}

	rc = unregister_trace_task_rename(set_task_comm_handler, NULL);
	if (rc != 0) {
		pr_err("CTP3：unregister_trace_task_rename failed! rc=%d\n", rc);
		return rc;
	}

	rc = unregister_trace_sched_stat_wait(sched_stat_wait_handler, NULL);
	if (rc != 0) {
		pr_err("CTP3：unregister_trace_sched_stat_wait failed! rc=%d\n", rc);
		return rc;
	}

	rc = unregister_trace_sched_stat_sleep(sched_stat_sleep_handler, NULL);
	if (rc != 0) {
		pr_err("CTP3：unregister_trace_sched_stat_sleep failed! rc=%d\n", rc);
		return rc;
	}

	rc = unregister_trace_sched_stat_blocked(sched_stat_blocked_handler, NULL);
	if (rc != 0) {
		pr_err("CTP3：unregister_trace_sched_stat_blocked failed! rc=%d\n", rc);
		return rc;
	}

	rc = unregister_trace_sched_switch(sched_switch_handler, NULL);
	if (rc != 0) {
		pr_err("CTP3：unregister_trace_sched_switch failed! rc=%d\n", rc);
		return rc;
	}

	rc = unregister_trace_sched_waking(sched_waking_handler, NULL);
	if (rc != 0) {
		pr_err("CTP3：unregister_trace_sched_waking failed! rc=%d\n", rc);
		return rc;
	}

	rc = unregister_trace_cpu_frequency_limits(cpu_frequency_limits_handler, NULL);
	if (rc != 0) {
		pr_err("CTP3：unregister_trace_cpu_frequency_limits failed! rc=%d\n", rc);
		return rc;
	}

	return 0;
}

static void __exit oplus_task_sched_exit(void)
{
	unregister_vendor_hooks();

	if (sched_info) {
		remove_proc_entry(SCHED_INFO_PROC_NODE, NULL);
		sched_info = NULL;
	}
}

module_init(oplus_task_sched_init);
module_exit(oplus_task_sched_exit);
module_param_named(sched_info_ctrl, sched_info_ctrl, bool, S_IRUGO | S_IWUSR);
MODULE_DESCRIPTION("Oplus Task Sched Vender Hooks Driver");
MODULE_LICENSE("GPL v2");
