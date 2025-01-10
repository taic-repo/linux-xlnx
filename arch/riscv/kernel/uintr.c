#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/fdtable.h>
#include <linux/anon_inodes.h>
#include <linux/task_work.h>

#include <asm/csr.h>
#include <asm/unistd.h>
#include <asm/uintr.h>

#define pr_fmt(fmt)                                                       \
	"[CPU %d] %s: [%-35s]: " fmt, smp_processor_id(), KBUILD_MODNAME, \
		__func__

static inline bool is_uintr_enabled(struct task_struct *t)
{
	return t->thread.is_uintr_enabled;
}

SYSCALL_DEFINE1(uintr_enable, u64, lq_idx)
{
	struct task_struct *t = current;
	if (is_uintr_enabled(t))
		return 0;
    t->thread.lq_idx = lq_idx;
    t->thread.is_uintr_enabled = true;
	// pr_info("task=%d enable uintr, lq_idx %lx\n", t->pid, lq_idx);
	return 0;
}

asmlinkage void riscv_uintr_clear(struct pt_regs *regs) {
	uintr_clear(regs);
}

void uintr_clear(struct pt_regs *regs) {
	struct task_struct *t = current;
	if (!is_uintr_enabled(t)) {
		return;
	}
	taic_ulq_write_cpuid(t->thread.lq_idx, ~0UL);
}

asmlinkage void riscv_uintr_set(struct pt_regs *regs)
{
	uintr_set(regs);
}

void uintr_set(struct pt_regs *regs)
{
	struct task_struct *t = current;
	/* always delegate user interrupt to read/write uie and uip */
	csr_set(CSR_SIDELEG, IE_USIE);
	unsigned long hartid = smp_processor_id();

	if (!is_uintr_enabled(t)) {
		csr_clear(CSR_UIE, IE_USIE);
		csr_clear(CSR_UIP, IE_USIE);
		return;
	}
    taic_ulq_write_cpuid(t->thread.lq_idx, hartid);

	/* restore U-mode CSRs */
	csr_write(CSR_UIE, regs->uie);
	csr_write(CSR_UEPC, regs->uepc);
	csr_write(CSR_UTVEC, regs->utvec);
	csr_write(CSR_USCRATCH, regs->uscratch);

	// maybe the target is in S-mode,
	// the uip store in the thread_struct is not correct,
	// it needs to be updated
	uint64_t uip = csr_read(CSR_UIP);
    csr_write(CSR_UIP, regs->uip | uip);

	// pr_info("task=%d restore uintr\n", t->pid);
}
