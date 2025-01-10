/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_RISCV_ENTRY_COMMON_H
#define _ASM_RISCV_ENTRY_COMMON_H

#include <asm/stacktrace.h>
#include <linux/sched.h>
#include <asm/uintr.h>

void handle_page_fault(struct pt_regs *regs);
void handle_break(struct pt_regs *regs);

static __always_inline void arch_enter_from_user_mode(struct pt_regs *regs)
{

}
#define arch_enter_from_user_mode arch_enter_from_user_mode

static __always_inline void arch_exit_to_user_mode(void)
{

}
#define arch_exit_to_user_mode arch_exit_to_user_mode


#endif /* _ASM_RISCV_ENTRY_COMMON_H */
