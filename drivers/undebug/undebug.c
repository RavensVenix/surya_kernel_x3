#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/seq_file.h>
#include <linux/sysctl.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/ftrace.h>
#include <linux/version.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tfoldi <tfoldi@nospam>");
MODULE_DESCRIPTION("Hide TracerPid and VMA");

#define HIDE_MAPS_MAX_LEN 4096
#define MAX_ENTRIES 100
static char hide_maps[HIDE_MAPS_MAX_LEN];
static char *hide_maps_array[MAX_ENTRIES];
static int hide_maps_count = 0;
static DEFINE_MUTEX(hide_maps_lock);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 7, 0)
static unsigned long lookup_name(const char *name)
{
	struct kprobe kp = {.symbol_name = name};
	unsigned long addr;
	if (register_kprobe(&kp) < 0) return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}
#else
#define lookup_name kallsyms_lookup_name
#endif

/* Compatibility flags for older kernels */
#ifndef FTRACE_OPS_FL_SAVE_REGS
#define FTRACE_OPS_FL_SAVE_REGS              (1 << 0)
#endif

#ifndef FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED
#define FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED FTRACE_OPS_FL_SAVE_REGS
#endif

#ifndef FTRACE_OPS_FL_RECURSION_SAFE
#define FTRACE_OPS_FL_RECURSION_SAFE         (1 << 1)
#endif

#ifndef FTRACE_OPS_FL_RECURSION
#define FTRACE_OPS_FL_RECURSION              FTRACE_OPS_FL_RECURSION_SAFE
#endif

#ifndef FTRACE_OPS_FL_IPMODIFY
#define FTRACE_OPS_FL_IPMODIFY               (1 << 2)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 11, 0)
#define ftrace_regs pt_regs
static inline struct pt_regs *ftrace_get_regs(struct ftrace_regs *fregs) { return fregs; }
#endif

#define USE_FENTRY_OFFSET 0

struct ftrace_hook {
	const char *name;
	void *function;
	void *original;
	unsigned long address;
	struct ftrace_ops ops;
};

static int fh_resolve_hook_address(struct ftrace_hook *hook)
{
	hook->address = lookup_name(hook->name);
	if (!hook->address) {
		pr_err("UNDEBUG: unresolved symbol: %s\n", hook->name);
		return -ENOENT;
	}

#if USE_FENTRY_OFFSET
	*((unsigned long *)hook->original) = hook->address + MCOUNT_INSN_SIZE;
#else
	*((unsigned long *)hook->original) = hook->address;
#endif

	return 0;
}

static void notrace fh_ftrace_thunk(unsigned long ip, unsigned long parent_ip,
                                    struct ftrace_ops *ops, struct ftrace_regs *fregs)
{
	struct pt_regs *regs = ftrace_get_regs(fregs);
	struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);

#if USE_FENTRY_OFFSET
#ifdef CONFIG_ARM64
	regs->pc = (unsigned long)hook->function;
#else
	regs->ip = (unsigned long)hook->function;
#endif
#else
	if (!within_module(parent_ip, THIS_MODULE))
#ifdef CONFIG_ARM64
		regs->pc = (unsigned long)hook->function;
#else
		regs->ip = (unsigned long)hook->function;
#endif
#endif
}

static int fh_install_hook(struct ftrace_hook *hook)
{
	int err = fh_resolve_hook_address(hook);
	if (err) return err;

	hook->ops.func = fh_ftrace_thunk;
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS_IF_SUPPORTED |
	                  FTRACE_OPS_FL_RECURSION |
	                  FTRACE_OPS_FL_IPMODIFY;

	err = ftrace_set_filter(&hook->ops, hook->name, strlen(hook->name), 0);
	if (err) return err;

	err = register_ftrace_function(&hook->ops);
	if (err) {
		ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
		return err;
	}

	return 0;
}

static void fh_remove_hook(struct ftrace_hook *hook)
{
	unregister_ftrace_function(&hook->ops);
	ftrace_set_filter(&hook->ops, NULL, 0, 1);
}

static int fh_install_hooks(struct ftrace_hook *hooks, size_t count)
{
	int i, err;
	for (i = 0; i < count; i++) {
		err = fh_install_hook(&hooks[i]);
		if (err) goto error;
	}
	return 0;
error:
	while (i--) fh_remove_hook(&hooks[i]);
	return err;
}

static void fh_remove_hooks(struct ftrace_hook *hooks, size_t count)
{
	size_t i;
	for (i = 0; i < count; i++) fh_remove_hook(&hooks[i]);
}

static void parse_hide_maps(void)
{
	char *str, *token;
	int i = 0;

	mutex_lock(&hide_maps_lock);
	for (i = 0; i < hide_maps_count; i++) {
		kfree(hide_maps_array[i]);
		hide_maps_array[i] = NULL;
	}
	hide_maps_count = 0;

	str = kstrdup(hide_maps, GFP_KERNEL);
	if (!str) {
		mutex_unlock(&hide_maps_lock);
		return;
	}

	for (i = 0; i < MAX_ENTRIES; i++) {
		token = strsep(&str, ",");
		if (!token) break;
		token = strim(token);
		if (!strlen(token)) continue;

		hide_maps_array[hide_maps_count] = kstrdup(token, GFP_KERNEL);
		if (!hide_maps_array[hide_maps_count]) break;
		hide_maps_count++;
	}

	kfree(str);
	mutex_unlock(&hide_maps_lock);
}

static asmlinkage void (*orig_show_map_vma)(struct seq_file *, struct vm_area_struct *);
static asmlinkage void handle_show_map_vma(struct seq_file *m, struct vm_area_struct *vma)
{
	struct file *file = vma->vm_file;
	int i;
	bool hide = false;

	if (!file) goto passthrough;

	mutex_lock(&hide_maps_lock);
	for (i = 0; i < hide_maps_count; i++) {
		if (strstr(file->f_path.dentry->d_iname, hide_maps_array[i])) {
			hide = true;
			break;
		}
	}
	mutex_unlock(&hide_maps_lock);

	if (hide) return;

passthrough:
	orig_show_map_vma(m, vma);
}

static asmlinkage int (*orig_proc_pid_status)(struct seq_file *, struct pid_namespace *, struct pid *, struct task_struct *);
static asmlinkage int hooked_proc_pid_status(struct seq_file *m, struct pid_namespace *ns,
                                             struct pid *pid, struct task_struct *task)
{
	int ret;
	unsigned int old_ptrace;

	if (!task) return orig_proc_pid_status(m, ns, pid, task);

	old_ptrace = task->ptrace;
	task->ptrace = 0;
	ret = orig_proc_pid_status(m, ns, pid, task);
	task->ptrace = old_ptrace;

	return ret;
}

#ifdef CONFIG_ARM64
#define SYSCALL_NAME(name) "__arm64_" name
#else
#define SYSCALL_NAME(name) name
#endif

#define HOOK(_name, _function, _original) \
	{ .name = SYSCALL_NAME(_name), .function = (_function), .original = (_original) }

static struct ftrace_hook hooks[] = {
	HOOK("proc_pid_status", hooked_proc_pid_status, &orig_proc_pid_status),
	HOOK("show_map_vma", handle_show_map_vma, &orig_show_map_vma),
};

static int hide_maps_proc_handler(struct ctl_table *table, int write,
                                  void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret = proc_dostring(table, write, buffer, lenp, ppos);
	if (!ret && write) {
		parse_hide_maps();
	}
	return ret;
}

static struct ctl_table undebug_table[] = {
	{
		.procname = "hide_maps",
		.data = hide_maps,
		.maxlen = HIDE_MAPS_MAX_LEN,
		.mode = 0644,
		.proc_handler = hide_maps_proc_handler,
	},
	{}
};

static struct ctl_table undebug_root[] = {
	{
		.procname = "undebug",
		.mode = 0555,
		.child = undebug_table,
	},
	{}
};

static struct ctl_table_header *undebug_sysctl;

static int __init undebug_init(void)
{
	int err = fh_install_hooks(hooks, sizeof(hooks) / sizeof(hooks[0]));
	if (err) return err;

	undebug_sysctl = register_sysctl_table(undebug_root);
	if (!undebug_sysctl) return -ENOMEM;

	pr_info("UNDEBUG: module loaded.\n");
	return 0;
}
late_initcall(undebug_init);

static void __exit undebug_exit(void)
{
	fh_remove_hooks(hooks, sizeof(hooks) / sizeof(hooks[0]));
	unregister_sysctl_table(undebug_sysctl);
	pr_info("UNDEBUG: module unloaded.\n");
}
module_exit(undebug_exit);
