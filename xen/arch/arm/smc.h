
#ifndef __ARCH_ARM_SMC_H__
#define __ARCH_ARM_SMC_H__

#include <xen/lib.h>
#include <asm/processor.h>
#include <asm/regs.h>

extern int handle_smc(struct cpu_user_regs *regs, int is_32bit);

#endif
