/*
 * xen/arch/arm/traps.c
 *
 * ARM Trap handlers
 *
 * Copyright (c) 2011 Citrix Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/string.h>
#include <xen/version.h>
#include <xen/smp.h>
#include <xen/symbols.h>
#include <xen/irq.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/errno.h>
#include <xen/hypercall.h>
#include <xen/softirq.h>
#include <xen/domain_page.h>
#include <public/sched.h>
#include <public/xen.h>
#include <asm/event.h>
#include <asm/regs.h>
#include <asm/cpregs.h>
#include <asm/psci.h>

#include "io.h"
#include "vtimer.h"
#include <asm/gic.h>
#include "smc.h"

/* The base of the stack must always be double-word aligned, which means
 * that both the kernel half of struct cpu_user_regs (which is pushed in
 * entry.S) and struct cpu_info (which lives at the bottom of a Xen
 * stack) must be doubleword-aligned in size.  */
static inline void check_stack_alignment_constraints(void) {
#ifdef CONFIG_ARM_64
    BUILD_BUG_ON((sizeof (struct cpu_user_regs)) & 0xf);
    BUILD_BUG_ON((offsetof(struct cpu_user_regs, spsr_el1)) & 0xf);
    BUILD_BUG_ON((offsetof(struct cpu_user_regs, lr)) & 0xf);
    BUILD_BUG_ON((sizeof (struct cpu_info)) & 0xf);
#else
    BUILD_BUG_ON((sizeof (struct cpu_user_regs)) & 0x7);
    BUILD_BUG_ON((offsetof(struct cpu_user_regs, sp_usr)) & 0x7);
    BUILD_BUG_ON((sizeof (struct cpu_info)) & 0x7);
#endif
}

#ifdef CONFIG_ARM_32
static int debug_stack_lines = 20;
#define stack_words_per_line 8
#else
static int debug_stack_lines = 40;
#define stack_words_per_line 4
#endif

integer_param("debug_stack_lines", debug_stack_lines);


void __cpuinit init_traps(void)
{
    /* Setup Hyp vector base */
    WRITE_SYSREG((vaddr_t)hyp_traps_vector, VBAR_EL2);

    /* Setup hypervisor traps */
    WRITE_SYSREG(HCR_PTW|HCR_BSU_OUTER|HCR_AMO|HCR_IMO|HCR_VM|HCR_TWI|HCR_TSC|
                 HCR_TAC, HCR_EL2);
    isb();
}

asmlinkage void __div0(void)
{
    printk("Division by zero in hypervisor.\n");
    BUG();
}

/* XXX could/should be common code */
static void print_xen_info(void)
{
    char taint_str[TAINT_STRING_MAX_LEN];

    printk("----[ Xen-%d.%d%s  %s  debug=%c  %s ]----\n",
           xen_major_version(), xen_minor_version(), xen_extra_version(),
#ifdef CONFIG_ARM_32
           "arm32",
#else
           "arm64",
#endif
           debug_build() ? 'y' : 'n', print_tainted(taint_str));
}

register_t *select_user_reg(struct cpu_user_regs *regs, int reg)
{
    BUG_ON( !guest_mode(regs) );

#ifdef CONFIG_ARM_32
    /*
     * We rely heavily on the layout of cpu_user_regs to avoid having
     * to handle all of the registers individually. Use BUILD_BUG_ON to
     * ensure that things which expect are contiguous actually are.
     */
#define REGOFFS(R) offsetof(struct cpu_user_regs, R)

    switch ( reg ) {
    case 0 ... 7: /* Unbanked registers */
        BUILD_BUG_ON(REGOFFS(r0) + 7*sizeof(register_t) != REGOFFS(r7));
        return &regs->r0 + reg;
    case 8 ... 12: /* Register banked in FIQ mode */
        BUILD_BUG_ON(REGOFFS(r8_fiq) + 4*sizeof(register_t) != REGOFFS(r12_fiq));
        if ( fiq_mode(regs) )
            return &regs->r8_fiq + reg - 8;
        else
            return &regs->r8 + reg - 8;
    case 13 ... 14: /* Banked SP + LR registers */
        BUILD_BUG_ON(REGOFFS(sp_fiq) + 1*sizeof(register_t) != REGOFFS(lr_fiq));
        BUILD_BUG_ON(REGOFFS(sp_irq) + 1*sizeof(register_t) != REGOFFS(lr_irq));
        BUILD_BUG_ON(REGOFFS(sp_svc) + 1*sizeof(register_t) != REGOFFS(lr_svc));
        BUILD_BUG_ON(REGOFFS(sp_abt) + 1*sizeof(register_t) != REGOFFS(lr_abt));
        BUILD_BUG_ON(REGOFFS(sp_und) + 1*sizeof(register_t) != REGOFFS(lr_und));
        switch ( regs->cpsr & PSR_MODE_MASK )
        {
        case PSR_MODE_USR:
        case PSR_MODE_SYS: /* Sys regs are the usr regs */
            if ( reg == 13 )
                return &regs->sp_usr;
            else /* lr_usr == lr in a user frame */
                return &regs->lr;
        case PSR_MODE_FIQ:
            return &regs->sp_fiq + reg - 13;
        case PSR_MODE_IRQ:
            return &regs->sp_irq + reg - 13;
        case PSR_MODE_SVC:
            return &regs->sp_svc + reg - 13;
        case PSR_MODE_ABT:
            return &regs->sp_abt + reg - 13;
        case PSR_MODE_UND:
            return &regs->sp_und + reg - 13;
        case PSR_MODE_MON:
        case PSR_MODE_HYP:
        default:
            BUG();
        }
    case 15: /* PC */
        return &regs->pc;
    default:
        BUG();
    }
#undef REGOFFS
#else
    /* In 64 bit the syndrome register contains the AArch64 register
     * number even if the trap was from AArch32 mode. Except that
     * AArch32 R15 (PC) is encoded as 0b11111.
     */
    if ( reg == 0x1f /* && is aarch32 guest */)
        return &regs->pc;
    return &regs->x0 + reg;
#endif
}

static const char *decode_fsc(uint32_t fsc, int *level)
{
    const char *msg = NULL;

    switch ( fsc & 0x3f )
    {
    case FSC_FLT_TRANS ... FSC_FLT_TRANS + 3:
        msg = "Translation fault";
        *level = fsc & FSC_LL_MASK;
        break;
    case FSC_FLT_ACCESS ... FSC_FLT_ACCESS + 3:
        msg = "Access fault";
        *level = fsc & FSC_LL_MASK;
        break;
    case FSC_FLT_PERM ... FSC_FLT_PERM + 3:
        msg = "Permission fault";
        *level = fsc & FSC_LL_MASK;
        break;

    case FSC_SEA:
        msg = "Synchronous External Abort";
        break;
    case FSC_SPE:
        msg = "Memory Access Synchronous Parity Error";
        break;
    case FSC_APE:
        msg = "Memory Access Asynchronous Parity Error";
        break;
    case FSC_SEATT ... FSC_SEATT + 3:
        msg = "Sync. Ext. Abort Translation Table";
        *level = fsc & FSC_LL_MASK;
        break;
    case FSC_SPETT ... FSC_SPETT + 3:
        msg = "Sync. Parity. Error Translation Table";
        *level = fsc & FSC_LL_MASK;
        break;
    case FSC_AF:
        msg = "Alignment Fault";
        break;
    case FSC_DE:
        msg = "Debug Event";
        break;

    case FSC_LKD:
        msg = "Implementation Fault: Lockdown Abort";
        break;
    case FSC_CPR:
        msg = "Implementation Fault: Coprocossor Abort";
        break;

    default:
        msg = "Unknown Failure";
        break;
    }
    return msg;
}

static const char *fsc_level_str(int level)
{
    switch ( level )
    {
    case -1: return "";
    case 1:  return " at level 1";
    case 2:  return " at level 2";
    case 3:  return " at level 3";
    default: return " (level invalid)";
    }
}

void panic_PAR(uint64_t par)
{
    const char *msg;
    int level = -1;
    int stage = par & PAR_STAGE2 ? 2 : 1;
    int second_in_first = !!(par & PAR_STAGE21);

    msg = decode_fsc( (par&PAR_FSC_MASK) >> PAR_FSC_SHIFT, &level);

    printk("PAR: %016"PRIx64": %s stage %d%s%s\n",
           par, msg,
           stage,
           second_in_first ? " during second stage lookup" : "",
           fsc_level_str(level));

    panic("Error during Hypervisor-to-physical address translation\n");
}

static void cpsr_switch_mode(struct cpu_user_regs *regs, int mode)
{
    uint32_t sctlr = READ_SYSREG32(SCTLR_EL1);

    regs->cpsr &= ~(PSR_MODE_MASK|PSR_IT_MASK|PSR_JAZELLE|PSR_BIG_ENDIAN|PSR_THUMB);

    regs->cpsr |= mode;
    regs->cpsr |= PSR_IRQ_MASK;
    if (sctlr & SCTLR_TE)
        regs->cpsr |= PSR_THUMB;
    if (sctlr & SCTLR_EE)
        regs->cpsr |= PSR_BIG_ENDIAN;
}

static vaddr_t exception_handler(vaddr_t offset)
{
    uint32_t sctlr = READ_SYSREG32(SCTLR_EL1);

    if (sctlr & SCTLR_V)
        return 0xffff0000 + offset;
    else /* always have security exceptions */
        return READ_SYSREG(VBAR_EL1) + offset;
}

/* Injects an Undefined Instruction exception into the current vcpu,
 * PC is the exact address of the faulting instruction (without
 * pipeline adjustments). See TakeUndefInstrException pseudocode in
 * ARM.
 */
static void inject_undef32_exception(struct cpu_user_regs *regs)
{
    uint32_t spsr = regs->cpsr;
    int is_thumb = (regs->cpsr & PSR_THUMB);
    /* Saved PC points to the instruction past the faulting instruction. */
    uint32_t return_offset = is_thumb ? 2 : 4;

    BUG_ON( !is_pv32_domain(current->domain) );

    /* Update processor mode */
    cpsr_switch_mode(regs, PSR_MODE_UND);

    /* Update banked registers */
    regs->spsr_und = spsr;
    regs->lr_und = regs->pc32 + return_offset;

    /* Branch to exception vector */
    regs->pc32 = exception_handler(VECTOR32_UND);
}

#ifdef CONFIG_ARM_64
/* Inject an undefined exception into a 64 bit guest */
static void inject_undef64_exception(struct cpu_user_regs *regs, int instr_len)
{
    union hsr esr = {
        .iss = 0,
        .len = instr_len,
        .ec = HSR_EC_UNKNOWN,
    };

    BUG_ON( is_pv32_domain(current->domain) );

    regs->spsr_el1 = regs->cpsr;
    regs->elr_el1 = regs->pc;

    regs->cpsr = PSR_MODE_EL1h | PSR_ABT_MASK | PSR_FIQ_MASK | \
        PSR_IRQ_MASK | PSR_DBG_MASK;
    regs->pc = READ_SYSREG(VBAR_EL1) + VECTOR64_CURRENT_SPx_SYNC;

    WRITE_SYSREG32(esr.bits, ESR_EL1);
}
#endif

struct reg_ctxt {
    /* Guest-side state */
    uint32_t sctlr_el1, tcr_el1;
    uint64_t ttbr0_el1, ttbr1_el1;
#ifdef CONFIG_ARM_32
    uint32_t dfsr, ifsr;
    uint32_t dfar, ifar;
#else
    uint32_t esr_el1;
    uint64_t far;
    uint32_t ifsr32_el2;
#endif

    /* Hypervisor-side state */
    uint64_t vttbr_el2;
};

static const char *mode_string(uint32_t cpsr)
{
    uint32_t mode;
    static const char *mode_strings[] = {
       [PSR_MODE_USR] = "32-bit Guest USR",
       [PSR_MODE_FIQ] = "32-bit Guest FIQ",
       [PSR_MODE_IRQ] = "32-bit Guest IRQ",
       [PSR_MODE_SVC] = "32-bit Guest SVC",
       [PSR_MODE_MON] = "32-bit Monitor",
       [PSR_MODE_ABT] = "32-bit Guest ABT",
       [PSR_MODE_HYP] = "Hypervisor",
       [PSR_MODE_UND] = "32-bit Guest UND",
       [PSR_MODE_SYS] = "32-bit Guest SYS",
#ifdef CONFIG_ARM_64
       [PSR_MODE_EL3h] = "64-bit EL3h (Monitor, handler)",
       [PSR_MODE_EL3t] = "64-bit EL3t (Monitor, thread)",
       [PSR_MODE_EL2h] = "64-bit EL2h (Hypervisor, handler)",
       [PSR_MODE_EL2t] = "64-bit EL2t (Hypervisor, thread)",
       [PSR_MODE_EL1h] = "64-bit EL1h (Guest Kernel, handler)",
       [PSR_MODE_EL1t] = "64-bit EL1t (Guest Kernel, thread)",
       [PSR_MODE_EL0t] = "64-bit EL0t (Guest User)",
#endif
    };
    mode = cpsr & PSR_MODE_MASK;

    if ( mode > ARRAY_SIZE(mode_strings) )
        return "Unknown";
    return mode_strings[mode] ? : "Unknown";
}

static void show_registers_32(struct cpu_user_regs *regs,
                              struct reg_ctxt *ctxt,
                              int guest_mode,
                              const struct vcpu *v)
{

#ifdef CONFIG_ARM_64
    BUG_ON( ! (regs->cpsr & PSR_MODE_BIT) );
    printk("PC:     %08"PRIx32"\n", regs->pc32);
#else
    printk("PC:     %08"PRIx32, regs->pc);
    if ( !guest_mode )
        print_symbol(" %s", regs->pc);
    printk("\n");
#endif
    printk("CPSR:   %08"PRIx32" MODE:%s\n", regs->cpsr,
           mode_string(regs->cpsr));
    printk("     R0: %08"PRIx32" R1: %08"PRIx32" R2: %08"PRIx32" R3: %08"PRIx32"\n",
           regs->r0, regs->r1, regs->r2, regs->r3);
    printk("     R4: %08"PRIx32" R5: %08"PRIx32" R6: %08"PRIx32" R7: %08"PRIx32"\n",
           regs->r4, regs->r5, regs->r6, regs->r7);
    printk("     R8: %08"PRIx32" R9: %08"PRIx32" R10:%08"PRIx32" R11:%08"PRIx32" R12:%08"PRIx32"\n",
           regs->r8, regs->r9, regs->r10,
#ifdef CONFIG_ARM_64
           regs->r11,
#else
           regs->fp,
#endif
           regs->r12);

    if ( guest_mode )
    {
        printk("USR: SP: %08"PRIx32" LR: %08"PRIregister"\n",
               regs->sp_usr, regs->lr);
        printk("SVC: SP: %08"PRIx32" LR: %08"PRIx32" SPSR:%08"PRIx32"\n",
               regs->sp_svc, regs->lr_svc, regs->spsr_svc);
        printk("ABT: SP: %08"PRIx32" LR: %08"PRIx32" SPSR:%08"PRIx32"\n",
               regs->sp_abt, regs->lr_abt, regs->spsr_abt);
        printk("UND: SP: %08"PRIx32" LR: %08"PRIx32" SPSR:%08"PRIx32"\n",
               regs->sp_und, regs->lr_und, regs->spsr_und);
        printk("IRQ: SP: %08"PRIx32" LR: %08"PRIx32" SPSR:%08"PRIx32"\n",
               regs->sp_irq, regs->lr_irq, regs->spsr_irq);
        printk("FIQ: SP: %08"PRIx32" LR: %08"PRIx32" SPSR:%08"PRIx32"\n",
               regs->sp_fiq, regs->lr_fiq, regs->spsr_fiq);
        printk("FIQ: R8: %08"PRIx32" R9: %08"PRIx32" R10:%08"PRIx32" R11:%08"PRIx32" R12:%08"PRIx32"\n",
               regs->r8_fiq, regs->r9_fiq, regs->r10_fiq, regs->r11_fiq, regs->r11_fiq);
    }
#ifndef CONFIG_ARM_64
    else
    {
        printk("HYP: SP: %08"PRIx32" LR: %08"PRIregister"\n", regs->sp, regs->lr);
    }
#endif
    printk("\n");

    if ( guest_mode )
    {
        printk("     SCTLR: %08"PRIx32"\n", ctxt->sctlr_el1);
        printk("       TCR: %08"PRIx32"\n", ctxt->tcr_el1);
        printk("     TTBR0: %016"PRIx64"\n", ctxt->ttbr0_el1);
        printk("     TTBR1: %016"PRIx64"\n", ctxt->ttbr1_el1);
        printk("      IFAR: %08"PRIx32", IFSR: %08"PRIx32"\n"
               "      DFAR: %08"PRIx32", DFSR: %08"PRIx32"\n",
#ifdef CONFIG_ARM_64
               (uint32_t)(ctxt->far >> 32),
               ctxt->ifsr32_el2,
               (uint32_t)(ctxt->far & 0xffffffff),
               ctxt->esr_el1
#else
               ctxt->ifar, ctxt->ifsr, ctxt->dfar, ctxt->dfsr
#endif
            );
        printk("\n");
    }
}

#ifdef CONFIG_ARM_64
static void show_registers_64(struct cpu_user_regs *regs,
                              struct reg_ctxt *ctxt,
                              int guest_mode,
                              const struct vcpu *v)
{

    BUG_ON( (regs->cpsr & PSR_MODE_BIT) );

    printk("PC:     %016"PRIx64, regs->pc);
    if ( !guest_mode )
        print_symbol(" %s", regs->pc);
    printk("\n");
    printk("LR:     %016"PRIx64"\n", regs->lr);
    if ( guest_mode )
    {
        printk("SP_EL0: %016"PRIx64"\n", regs->sp_el0);
        printk("SP_EL1: %016"PRIx64"\n", regs->sp_el1);
    }
    else
    {
        printk("SP:     %016"PRIx64"\n", regs->sp);
    }
    printk("CPSR:   %08"PRIx32" MODE:%s\n", regs->cpsr,
           mode_string(regs->cpsr));
    printk("     X0: %016"PRIx64"  X1: %016"PRIx64"  X2: %016"PRIx64"\n",
           regs->x0, regs->x1, regs->x2);
    printk("     X3: %016"PRIx64"  X4: %016"PRIx64"  X5: %016"PRIx64"\n",
           regs->x3, regs->x4, regs->x5);
    printk("     X6: %016"PRIx64"  X7: %016"PRIx64"  X8: %016"PRIx64"\n",
           regs->x6, regs->x7, regs->x8);
    printk("     X9: %016"PRIx64" X10: %016"PRIx64" X11: %016"PRIx64"\n",
           regs->x9, regs->x10, regs->x11);
    printk("    X12: %016"PRIx64" X13: %016"PRIx64" X14: %016"PRIx64"\n",
           regs->x12, regs->x13, regs->x14);
    printk("    X15: %016"PRIx64" X16: %016"PRIx64" X17: %016"PRIx64"\n",
           regs->x15, regs->x16, regs->x17);
    printk("    X18: %016"PRIx64" X19: %016"PRIx64" X20: %016"PRIx64"\n",
           regs->x18, regs->x19, regs->x20);
    printk("    X21: %016"PRIx64" X22: %016"PRIx64" X23: %016"PRIx64"\n",
           regs->x21, regs->x22, regs->x23);
    printk("    X24: %016"PRIx64" X25: %016"PRIx64" X26: %016"PRIx64"\n",
           regs->x24, regs->x25, regs->x26);
    printk("    X27: %016"PRIx64" X28: %016"PRIx64"  FP: %016"PRIx64"\n",
           regs->x27, regs->x28, regs->fp);
    printk("\n");

    if ( guest_mode )
    {
        printk("   ELR_EL1: %016"PRIx64"\n", regs->elr_el1);
        printk("   ESR_EL1: %08"PRIx32"\n", ctxt->esr_el1);
        printk("   FAR_EL1: %016"PRIx64"\n", ctxt->far);
        printk("\n");
        printk(" SCTLR_EL1: %08"PRIx32"\n", ctxt->sctlr_el1);
        printk("   TCR_EL1: %08"PRIx32"\n", ctxt->tcr_el1);
        printk(" TTBR0_EL1: %016"PRIx64"\n", ctxt->ttbr0_el1);
        printk(" TTBR1_EL1: %016"PRIx64"\n", ctxt->ttbr1_el1);
        printk("\n");
    }
}
#endif

static void _show_registers(struct cpu_user_regs *regs,
                            struct reg_ctxt *ctxt,
                            int guest_mode,
                            const struct vcpu *v)
{
    print_xen_info();

    printk("CPU:    %d\n", smp_processor_id());

    if ( guest_mode )
    {
        if ( is_pv32_domain(v->domain) )
            show_registers_32(regs, ctxt, guest_mode, v);
#ifdef CONFIG_ARM_64
        else if ( is_pv64_domain(v->domain) )
            show_registers_64(regs, ctxt, guest_mode, v);
#endif
    }
    else
    {
#ifdef CONFIG_ARM_64
        show_registers_64(regs, ctxt, guest_mode, v);
#else
        show_registers_32(regs, ctxt, guest_mode, v);
#endif
    }
    printk("  VTCR_EL2: %08"PRIx32"\n", READ_SYSREG32(VTCR_EL2));
    printk(" VTTBR_EL2: %016"PRIx64"\n", ctxt->vttbr_el2);
    printk("\n");

    printk(" SCTLR_EL2: %08"PRIx32"\n", READ_SYSREG32(SCTLR_EL2));
    printk("   HCR_EL2: %016"PRIregister"\n", READ_SYSREG(HCR_EL2));
    printk(" TTBR0_EL2: %016"PRIx64"\n", READ_SYSREG64(TTBR0_EL2));
    printk("\n");
    printk("   ESR_EL2: %08"PRIx32"\n", READ_SYSREG32(ESR_EL2));
    printk(" HPFAR_EL2: %016"PRIregister"\n", READ_SYSREG(HPFAR_EL2));

#ifdef CONFIG_ARM_32
    printk("     HDFAR: %08"PRIx32"\n", READ_CP32(HDFAR));
    printk("     HIFAR: %08"PRIx32"\n", READ_CP32(HIFAR));
#else
    printk("   FAR_EL2: %016"PRIx64"\n", READ_SYSREG64(FAR_EL2));
#endif
    printk("\n");
}

void show_registers(struct cpu_user_regs *regs)
{
    struct reg_ctxt ctxt;
    ctxt.sctlr_el1 = READ_SYSREG(SCTLR_EL1);
    ctxt.tcr_el1 = READ_SYSREG(TCR_EL1);
    ctxt.ttbr0_el1 = READ_SYSREG64(TTBR0_EL1);
    ctxt.ttbr1_el1 = READ_SYSREG64(TTBR1_EL1);
#ifdef CONFIG_ARM_32
    ctxt.dfar = READ_CP32(DFAR);
    ctxt.ifar = READ_CP32(IFAR);
    ctxt.dfsr = READ_CP32(DFSR);
    ctxt.ifsr = READ_CP32(IFSR);
#else
    ctxt.far = READ_SYSREG(FAR_EL1);
    ctxt.esr_el1 = READ_SYSREG(ESR_EL1);
    ctxt.ifsr32_el2 = READ_SYSREG(IFSR32_EL2);
#endif
    ctxt.vttbr_el2 = READ_SYSREG64(VTTBR_EL2);

    _show_registers(regs, &ctxt, guest_mode(regs), current);
}

void vcpu_show_registers(const struct vcpu *v)
{
    struct reg_ctxt ctxt;
    ctxt.sctlr_el1 = v->arch.sctlr;
    ctxt.tcr_el1 = v->arch.ttbcr;
    ctxt.ttbr0_el1 = v->arch.ttbr0;
    ctxt.ttbr1_el1 = v->arch.ttbr1;
#ifdef CONFIG_ARM_32
    ctxt.dfar = v->arch.dfar;
    ctxt.ifar = v->arch.ifar;
    ctxt.dfsr = v->arch.dfsr;
    ctxt.ifsr = v->arch.ifsr;
#else
    ctxt.far = v->arch.far;
    ctxt.esr_el1 = v->arch.esr;
    ctxt.ifsr32_el2 = v->arch.ifsr;
#endif

    ctxt.vttbr_el2 = v->domain->arch.vttbr;

    _show_registers(&v->arch.cpu_info->guest_cpu_user_regs, &ctxt, 1, v);
}

static void show_guest_stack(struct vcpu *v, struct cpu_user_regs *regs)
{
    int i;
    vaddr_t sp;
    paddr_t stack_phys;
    void *mapped;
    unsigned long *stack, addr;

    switch ( regs->cpsr & PSR_MODE_MASK )
    {
    case PSR_MODE_USR:
    case PSR_MODE_SYS:
#ifdef CONFIG_ARM_64
    case PSR_MODE_EL0t:
#endif
        printk("No stack trace for guest user-mode\n");
        return;

    case PSR_MODE_FIQ:
    case PSR_MODE_IRQ:
    case PSR_MODE_SVC:
    case PSR_MODE_ABT:
    case PSR_MODE_UND:
        printk("No stack trace for 32-bit guest kernel-mode\n");
        return;

#ifdef CONFIG_ARM_64
    case PSR_MODE_EL1t:
        sp = regs->sp_el0;
        break;
    case PSR_MODE_EL1h:
        sp = regs->sp_el1;
        break;
#endif

    case PSR_MODE_HYP:
    case PSR_MODE_MON:
#ifdef CONFIG_ARM_64
    case PSR_MODE_EL3h:
    case PSR_MODE_EL3t:
    case PSR_MODE_EL2h:
    case PSR_MODE_EL2t:
#endif
    default:
        BUG();
        return;
    }

    printk("Guest stack trace from sp=%"PRIvaddr":\n  ", sp);

    if ( gvirt_to_maddr(sp, &stack_phys) )
    {
        printk("Failed to convert stack to physical address\n");
        return;
    }

    mapped = map_domain_page(stack_phys >> PAGE_SHIFT);

    stack = mapped + (sp & ~PAGE_MASK);

    for ( i = 0; i < (debug_stack_lines*stack_words_per_line); i++ )
    {
        if ( (((long)stack - 1) ^ ((long)(stack + 1) - 1)) & PAGE_SIZE )
            break;
        addr = *stack;
        if ( (i != 0) && ((i % stack_words_per_line) == 0) )
            printk("\n  ");
        printk(" %p", _p(addr));
        stack++;
    }
    if ( i == 0 )
        printk("Stack empty.");
    printk("\n");
    unmap_domain_page(mapped);

}

#define STACK_BEFORE_EXCEPTION(regs) ((register_t*)(regs)->sp)
#ifdef CONFIG_ARM_32
/* Frame pointer points to the return address:
 * (largest address)
 * | cpu_info
 * | [...]                                   |
 * | return addr      <-----------------,    |
 * | fp --------------------------------+----'
 * | [...]                              |
 * | return addr      <------------,    |
 * | fp ---------------------------+----'
 * | [...]                         |
 * | return addr      <- regs->fp  |
 * | fp ---------------------------'
 * |
 * v (smallest address, sp)
 */
#define STACK_FRAME_BASE(fp)       ((register_t*)(fp) - 1)
#else
/* Frame pointer points to the next frame:
 * (largest address)
 * | cpu_info
 * | [...]                                   |
 * | return addr                             |
 * | fp <-------------------------------, >--'
 * | [...]                              |
 * | return addr                        |
 * | fp <--------------------------, >--'
 * | [...]                         |
 * | return addr      <- regs->fp  |
 * | fp ---------------------------'
 * |
 * v (smallest address, sp)
 */
#define STACK_FRAME_BASE(fp)       ((register_t*)(fp))
#endif
static void show_trace(struct cpu_user_regs *regs)
{
    register_t *frame, next, addr, low, high;

    printk("Xen call trace:\n   ");

    printk("[<%p>]", _p(regs->pc));
    print_symbol(" %s (PC)\n   ", regs->pc);
    printk("[<%p>]", _p(regs->lr));
    print_symbol(" %s (LR)\n   ", regs->lr);

    /* Bounds for range of valid frame pointer. */
    low  = (register_t)(STACK_BEFORE_EXCEPTION(regs));
    high = (low & ~(STACK_SIZE - 1)) +
        (STACK_SIZE - sizeof(struct cpu_info));

    /* The initial frame pointer. */
    next = regs->fp;

    for ( ; ; )
    {
        if ( (next < low) || (next >= high) )
            break;

        /* Ordinary stack frame. */
        frame = STACK_FRAME_BASE(next);
        next  = frame[0];
        addr  = frame[1];

        printk("[<%p>]", _p(addr));
        print_symbol(" %s\n   ", addr);

        low = (register_t)&frame[1];
    }

    printk("\n");
}

void show_stack(struct cpu_user_regs *regs)
{
    register_t *stack = STACK_BEFORE_EXCEPTION(regs), addr;
    int i;

    if ( guest_mode(regs) )
        return show_guest_stack(current, regs);

    printk("Xen stack trace from sp=%p:\n  ", stack);

    for ( i = 0; i < (debug_stack_lines*stack_words_per_line); i++ )
    {
        if ( ((long)stack & (STACK_SIZE-BYTES_PER_LONG)) == 0 )
            break;
        if ( (i != 0) && ((i % stack_words_per_line) == 0) )
            printk("\n  ");

        addr = *stack++;
        printk(" %p", _p(addr));
    }
    if ( i == 0 )
        printk("Stack empty.");
    printk("\n");

    show_trace(regs);
}

void show_execution_state(struct cpu_user_regs *regs)
{
    show_registers(regs);
    show_stack(regs);
}

void vcpu_show_execution_state(struct vcpu *v)
{
    printk("*** Dumping Dom%d vcpu#%d state: ***\n",
           v->domain->domain_id, v->vcpu_id);

    if ( v == current )
    {
        show_execution_state(guest_cpu_user_regs());
        return;
    }

    vcpu_pause(v); /* acceptably dangerous */

    vcpu_show_registers(v);
    if ( !usr_mode(&v->arch.cpu_info->guest_cpu_user_regs) )
        show_guest_stack(v, &v->arch.cpu_info->guest_cpu_user_regs);

    vcpu_unpause(v);
}

void do_unexpected_trap(const char *msg, struct cpu_user_regs *regs)
{
    printk("CPU%d: Unexpected Trap: %s\n", smp_processor_id(), msg);
    show_execution_state(regs);
    while(1);
}

typedef register_t (*arm_hypercall_fn_t)(
    register_t, register_t, register_t, register_t, register_t);

typedef struct {
    arm_hypercall_fn_t fn;
    int nr_args;
} arm_hypercall_t;

#define HYPERCALL(_name, _nr_args)                                   \
    [ __HYPERVISOR_ ## _name ] =  {                                  \
        .fn = (arm_hypercall_fn_t) &do_ ## _name,                    \
        .nr_args = _nr_args,                                         \
    }

#define HYPERCALL_ARM(_name, _nr_args)                        \
    [ __HYPERVISOR_ ## _name ] =  {                                  \
        .fn = (arm_hypercall_fn_t) &do_arm_ ## _name,                \
        .nr_args = _nr_args,                                         \
    }
static arm_hypercall_t arm_hypercall_table[] = {
    HYPERCALL(memory_op, 2),
    HYPERCALL(domctl, 1),
    HYPERCALL(sched_op, 2),
    HYPERCALL(console_io, 3),
    HYPERCALL(xen_version, 2),
    HYPERCALL(event_channel_op, 2),
    HYPERCALL(physdev_op, 2),
    HYPERCALL(sysctl, 2),
    HYPERCALL(hvm_op, 2),
    HYPERCALL(grant_table_op, 3),
    HYPERCALL_ARM(vcpu_op, 3),
};

#define __PSCI_cpu_suspend 0
#define __PSCI_cpu_off     1
#define __PSCI_cpu_on      2
#define __PSCI_migrate     3

typedef int (*arm_psci_fn_t)(uint32_t, register_t);

typedef struct {
    arm_psci_fn_t fn;
    int nr_args;
} arm_psci_t;

#define PSCI(_name, _nr_args)                                  \
    [ __PSCI_ ## _name ] =  {                                  \
        .fn = (arm_psci_fn_t) &do_psci_ ## _name,              \
        .nr_args = _nr_args,                                   \
    }

static arm_psci_t arm_psci_table[] = {
    PSCI(cpu_off, 1),
    PSCI(cpu_on, 2),
};

#ifndef NDEBUG
static void do_debug_trap(struct cpu_user_regs *regs, unsigned int code)
{
    register_t *r;
    uint32_t reg;
    uint32_t domid = current->domain->domain_id;
    switch ( code ) {
    case 0xe0 ... 0xef:
        reg = code - 0xe0;
        r = select_user_reg(regs, reg);
        printk("DOM%d: R%d = 0x%"PRIregister" at 0x%"PRIvaddr"\n",
               domid, reg, *r, regs->pc);
        break;
    case 0xfd:
        printk("DOM%d: Reached %"PRIvaddr"\n", domid, regs->pc);
        break;
    case 0xfe:
        r = select_user_reg(regs, 0);
        printk("%c", (char)(*r & 0xff));
        break;
    case 0xff:
        printk("DOM%d: DEBUG\n", domid);
        show_execution_state(regs);
        break;
    default:
        panic("DOM%d: Unhandled debug trap %#x\n", domid, code);
        break;
    }
}
#endif

static void do_trap_psci(struct cpu_user_regs *regs)
{
    arm_psci_fn_t psci_call = NULL;

    if ( regs->r0 >= ARRAY_SIZE(arm_psci_table) )
    {
        domain_crash_synchronous();
        return;
    }

    psci_call = arm_psci_table[regs->r0].fn;
    if ( psci_call == NULL )
    {
        domain_crash_synchronous();
        return;
    }
    regs->r0 = psci_call(regs->r1, regs->r2);
}

#ifdef CONFIG_ARM_64
#define HYPERCALL_RESULT_REG(r) (r)->x0
#define HYPERCALL_ARG1(r) (r)->x0
#define HYPERCALL_ARG2(r) (r)->x1
#define HYPERCALL_ARG3(r) (r)->x2
#define HYPERCALL_ARG4(r) (r)->x3
#define HYPERCALL_ARG5(r) (r)->x4
#define HYPERCALL_ARGS(r) (r)->x0, (r)->x1, (r)->x2, (r)->x3, (r)->x4
#else
#define HYPERCALL_RESULT_REG(r) (r)->r0
#define HYPERCALL_ARG1(r) (r)->r0
#define HYPERCALL_ARG2(r) (r)->r1
#define HYPERCALL_ARG3(r) (r)->r2
#define HYPERCALL_ARG4(r) (r)->r3
#define HYPERCALL_ARG5(r) (r)->r4
#define HYPERCALL_ARGS(r) (r)->r0, (r)->r1, (r)->r2, (r)->r3, (r)->r4
#endif

static void do_trap_hypercall(struct cpu_user_regs *regs, register_t *nr,
                              unsigned long iss)
{
    arm_hypercall_fn_t call = NULL;
#ifndef NDEBUG
    register_t orig_pc = regs->pc;
#endif

    if ( iss != XEN_HYPERCALL_TAG )
        domain_crash_synchronous();

    if ( *nr >= ARRAY_SIZE(arm_hypercall_table) )
    {
        HYPERCALL_RESULT_REG(regs) = -ENOSYS;
        return;
    }

    call = arm_hypercall_table[*nr].fn;
    if ( call == NULL )
    {
        HYPERCALL_RESULT_REG(regs) = -ENOSYS;
        return;
    }

    HYPERCALL_RESULT_REG(regs) = call(HYPERCALL_ARGS(regs));

#ifndef NDEBUG
    /*
     * Clobber argument registers only if pc is unchanged, otherwise
     * this is a hypercall continuation.
     */
    if ( orig_pc == regs->pc )
    {
        switch ( arm_hypercall_table[*nr].nr_args ) {
        case 5: HYPERCALL_ARG5(regs) = 0xDEADBEEF;
        case 4: HYPERCALL_ARG4(regs) = 0xDEADBEEF;
        case 3: HYPERCALL_ARG3(regs) = 0xDEADBEEF;
        case 2: HYPERCALL_ARG2(regs) = 0xDEADBEEF;
        case 1: /* Don't clobber x0/r0 -- it's the return value */
            break;
        default: BUG();
        }
        *nr = 0xDEADBEEF;
    }
#endif
}

void do_multicall_call(struct multicall_entry *multi)
{
    arm_hypercall_fn_t call = NULL;

    if ( multi->op >= ARRAY_SIZE(arm_hypercall_table) )
    {
        multi->result = -ENOSYS;
        return;
    }

    call = arm_hypercall_table[multi->op].fn;
    if ( call == NULL )
    {
        multi->result = -ENOSYS;
        return;
    }

    multi->result = call(multi->args[0], multi->args[1],
                        multi->args[2], multi->args[3],
                        multi->args[4]);
}

/*
 * stolen from arch/arm/kernel/opcodes.c
 *
 * condition code lookup table
 * index into the table is test code: EQ, NE, ... LT, GT, AL, NV
 *
 * bit position in short is condition code: NZCV
 */
static const unsigned short cc_map[16] = {
        0xF0F0,                 /* EQ == Z set            */
        0x0F0F,                 /* NE                     */
        0xCCCC,                 /* CS == C set            */
        0x3333,                 /* CC                     */
        0xFF00,                 /* MI == N set            */
        0x00FF,                 /* PL                     */
        0xAAAA,                 /* VS == V set            */
        0x5555,                 /* VC                     */
        0x0C0C,                 /* HI == C set && Z clear */
        0xF3F3,                 /* LS == C clear || Z set */
        0xAA55,                 /* GE == (N==V)           */
        0x55AA,                 /* LT == (N!=V)           */
        0x0A05,                 /* GT == (!Z && (N==V))   */
        0xF5FA,                 /* LE == (Z || (N!=V))    */
        0xFFFF,                 /* AL always              */
        0                       /* NV                     */
};

static int check_conditional_instr(struct cpu_user_regs *regs, union hsr hsr)
{
    unsigned long cpsr, cpsr_cond;
    int cond;

    /* Unconditional Exception classes */
    if ( hsr.ec >= 0x10 )
        return 1;

    /* Check for valid condition in hsr */
    cond = hsr.cond.ccvalid ? hsr.cond.cc : -1;

    /* Unconditional instruction */
    if ( cond == 0xe )
        return 1;

    cpsr = regs->cpsr;

    /* If cc is not valid then we need to examine the IT state */
    if ( cond < 0 )
    {
        unsigned long it;

        BUG_ON( !is_pv32_domain(current->domain) || !(cpsr&PSR_THUMB) );

        it = ( (cpsr >> (10-2)) & 0xfc) | ((cpsr >> 25) & 0x3 );

        /* it == 0 => unconditional. */
        if ( it == 0 )
            return 1;

        /* The cond for this instruction works out as the top 4 bits. */
        cond =  ( it >> 4 );
    }

    cpsr_cond = cpsr >> 28;

    if ( !((cc_map[cond] >> cpsr_cond) & 1) )
        return 0;

    return 1;
}

static void advance_pc(struct cpu_user_regs *regs, union hsr hsr)
{
    unsigned long itbits, cond, cpsr = regs->cpsr;

    /* PSR_IT_MASK bits can only be set for 32-bit processors in Thumb mode. */
    BUG_ON( (!is_pv32_domain(current->domain)||!(cpsr&PSR_THUMB))
            && (cpsr&PSR_IT_MASK) );

    if ( is_pv32_domain(current->domain) && (cpsr&PSR_IT_MASK) )
    {
        /* The ITSTATE[7:0] block is contained in CPSR[15:10],CPSR[26:25]
         *
         * ITSTATE[7:5] are the condition code
         * ITSTATE[4:0] are the IT bits
         *
         * If the condition is non-zero then the IT state machine is
         * advanced by shifting the IT bits left.
         *
         * See A2-51 and B1-1148 of DDI 0406C.b.
         */
        cond = (cpsr & 0xe000) >> 13;
        itbits = (cpsr & 0x1c00) >> (10 - 2);
        itbits |= (cpsr & (0x3 << 25)) >> 25;

        if ( (itbits & 0x7) == 0 )
            itbits = cond = 0;
        else
            itbits = (itbits << 1) & 0x1f;

        cpsr &= ~PSR_IT_MASK;
        cpsr |= cond << 13;
        cpsr |= (itbits & 0x1c) << (10 - 2);
        cpsr |= (itbits & 0x3) << 25;

        regs->cpsr = cpsr;
    }

    regs->pc += hsr.len ? 4 : 2;
}

static void do_cp15_32(struct cpu_user_regs *regs,
                       union hsr hsr)
{
    struct hsr_cp32 cp32 = hsr.cp32;
    uint32_t *r = (uint32_t*)select_user_reg(regs, cp32.reg);
    struct vcpu *v = current;

    if ( !check_conditional_instr(regs, hsr) )
    {
        advance_pc(regs, hsr);
        return;
    }

    switch ( hsr.bits & HSR_CP32_REGS_MASK )
    {
    case HSR_CPREG32(CLIDR):
        if ( !cp32.read )
        {
            dprintk(XENLOG_ERR,
                    "attempt to write to read-only register CLIDR\n");
            domain_crash_synchronous();
        }
        *r = READ_SYSREG32(CLIDR_EL1);
        break;
    case HSR_CPREG32(CCSIDR):
        if ( !cp32.read )
        {
            dprintk(XENLOG_ERR,
                    "attempt to write to read-only register CCSIDR\n");
            domain_crash_synchronous();
        }
        *r = READ_SYSREG32(CCSIDR_EL1);
        break;
    case HSR_CPREG32(DCCISW):
        if ( cp32.read )
        {
            dprintk(XENLOG_ERR,
                    "attempt to read from write-only register DCCISW\n");
            domain_crash_synchronous();
        }
#ifdef CONFIG_ARM_32
        WRITE_CP32(*r, DCCISW);
#else
        asm volatile("dc cisw, %0;" : : "r" (*r) : "memory");
#endif
        break;
    case HSR_CPREG32(CNTP_CTL):
    case HSR_CPREG32(CNTP_TVAL):
        if ( !vtimer_emulate(regs, hsr) )
        {
            dprintk(XENLOG_ERR,
                    "failed emulation of 32-bit vtimer CP register access\n");
            domain_crash_synchronous();
        }
        break;
    case HSR_CPREG32(ACTLR):
        if ( cp32.read )
           *r = v->arch.actlr;
        break;
    default:
        printk("%s p15, %d, r%d, cr%d, cr%d, %d @ 0x%"PRIregister"\n",
               cp32.read ? "mrc" : "mcr",
               cp32.op1, cp32.reg, cp32.crn, cp32.crm, cp32.op2, regs->pc);
        panic("unhandled 32-bit CP15 access %#x\n", hsr.bits & HSR_CP32_REGS_MASK);
    }
    advance_pc(regs, hsr);
}

static void do_cp15_64(struct cpu_user_regs *regs,
                       union hsr hsr)
{
    struct hsr_cp64 cp64 = hsr.cp64;

    if ( !check_conditional_instr(regs, hsr) )
    {
        advance_pc(regs, hsr);
        return;
    }

    switch ( hsr.bits & HSR_CP64_REGS_MASK )
    {
    case HSR_CPREG64(CNTPCT):
        if ( !vtimer_emulate(regs, hsr) )
        {
            dprintk(XENLOG_ERR,
                    "failed emulation of 64-bit vtimer CP register access\n");
            domain_crash_synchronous();
        }
        break;
    default:
        printk("%s p15, %d, r%d, r%d, cr%d @ 0x%"PRIregister"\n",
               cp64.read ? "mrrc" : "mcrr",
               cp64.op1, cp64.reg1, cp64.reg2, cp64.crm, regs->pc);
        panic("unhandled 64-bit CP15 access %#x\n", hsr.bits & HSR_CP64_REGS_MASK);
    }
    advance_pc(regs, hsr);
}

#ifdef CONFIG_ARM_64
static void do_sysreg(struct cpu_user_regs *regs,
                      union hsr hsr)
{
    struct hsr_sysreg sysreg = hsr.sysreg;

    switch ( hsr.bits & HSR_SYSREG_REGS_MASK )
    {
    case CNTP_CTL_EL0:
    case CNTP_TVAL_EL0:
        if ( !vtimer_emulate(regs, hsr) )
        {
            dprintk(XENLOG_ERR,
                    "failed emulation of 64-bit vtimer sysreg access\n");
            domain_crash_synchronous();
        }
        break;
    default:
        printk("%s %d, %d, c%d, c%d, %d %s x%d @ 0x%"PRIregister"\n",
               sysreg.read ? "mrs" : "msr",
               sysreg.op0, sysreg.op1,
               sysreg.crn, sysreg.crm,
               sysreg.op2,
               sysreg.read ? "=>" : "<=",
               sysreg.reg, regs->pc);
        panic("unhandled 64-bit sysreg access %#x\n",
              hsr.bits & HSR_SYSREG_REGS_MASK);
    }

    regs->pc += 4;
}
#endif

void dump_guest_s1_walk(struct domain *d, vaddr_t addr)
{
    uint32_t ttbcr = READ_SYSREG32(TCR_EL1);
    uint64_t ttbr0 = READ_SYSREG64(TTBR0_EL1);
    paddr_t paddr;
    uint32_t offset;
    uint32_t *first = NULL, *second = NULL;

    printk("dom%d VA 0x%08"PRIvaddr"\n", d->domain_id, addr);
    printk("    TTBCR: 0x%08"PRIx32"\n", ttbcr);
    printk("    TTBR0: 0x%016"PRIx64" = 0x%"PRIpaddr"\n",
           ttbr0, p2m_lookup(d, ttbr0 & PAGE_MASK));

    if ( ttbcr & TTBCR_EAE )
    {
        printk("Cannot handle LPAE guest PT walk\n");
        return;
    }
    if ( (ttbcr & TTBCR_N_MASK) != 0 )
    {
        printk("Cannot handle TTBR1 guest walks\n");
        return;
    }

    paddr = p2m_lookup(d, ttbr0 & PAGE_MASK);
    if ( paddr == INVALID_PADDR )
    {
        printk("Failed TTBR0 maddr lookup\n");
        goto done;
    }
    first = map_domain_page(paddr>>PAGE_SHIFT);

    offset = addr >> (12+10);
    printk("1ST[0x%"PRIx32"] (0x%"PRIpaddr") = 0x%08"PRIx32"\n",
           offset, paddr, first[offset]);
    if ( !(first[offset] & 0x1) ||
         !(first[offset] & 0x2) )
        goto done;

    paddr = p2m_lookup(d, first[offset] & PAGE_MASK);

    if ( paddr == INVALID_PADDR )
    {
        printk("Failed L1 entry maddr lookup\n");
        goto done;
    }
    second = map_domain_page(paddr>>PAGE_SHIFT);
    offset = (addr >> 12) & 0x3FF;
    printk("2ND[0x%"PRIx32"] (0x%"PRIpaddr") = 0x%08"PRIx32"\n",
           offset, paddr, second[offset]);

done:
    if (second) unmap_domain_page(second);
    if (first) unmap_domain_page(first);
}

static void do_trap_data_abort_guest(struct cpu_user_regs *regs,
                                     union hsr hsr)
{
    struct hsr_dabt dabt = hsr.dabt;
    const char *msg;
    int rc, level = -1;
    mmio_info_t info;

    if ( !check_conditional_instr(regs, hsr) )
    {
        advance_pc(regs, hsr);
        return;
    }

    info.dabt = dabt;
#ifdef CONFIG_ARM_32
    info.gva = READ_CP32(HDFAR);
#else
    info.gva = READ_SYSREG64(FAR_EL2);
#endif

    if (dabt.s1ptw)
        goto bad_data_abort;

    rc = gva_to_ipa(info.gva, &info.gpa);
    if ( rc == -EFAULT )
        goto bad_data_abort;

    /* XXX: Decode the instruction if ISS is not valid */
    if ( !dabt.valid )
        goto bad_data_abort;

    if (handle_mmio(&info))
    {
        advance_pc(regs, hsr);
        return;
    }

bad_data_abort:

    msg = decode_fsc( dabt.dfsc, &level);

    /* XXX inject a suitable fault into the guest */
    printk("Guest data abort: %s%s%s\n"
           "    gva=%"PRIvaddr"\n",
           msg, dabt.s1ptw ? " S2 during S1" : "",
           fsc_level_str(level),
           info.gva);
    if ( !dabt.s1ptw )
        printk("    gpa=%"PRIpaddr"\n", info.gpa);
    if ( dabt.valid )
        printk("    size=%d sign=%d write=%d reg=%d\n",
               dabt.size, dabt.sign, dabt.write, dabt.reg);
    else
        printk("    instruction syndrome invalid\n");
    printk("    eat=%d cm=%d s1ptw=%d dfsc=%d\n",
           dabt.eat, dabt.cache, dabt.s1ptw, dabt.dfsc);
    if ( !dabt.s1ptw )
        dump_p2m_lookup(current->domain, info.gpa);
    else
        dump_guest_s1_walk(current->domain, info.gva);
    show_execution_state(regs);
    domain_crash_synchronous();
}

asmlinkage void do_trap_hypervisor(struct cpu_user_regs *regs)
{
    union hsr hsr = { .bits = READ_SYSREG32(ESR_EL2) };

    switch (hsr.ec) {
    case HSR_EC_WFI_WFE:
        if ( !check_conditional_instr(regs, hsr) )
        {
            advance_pc(regs, hsr);
            return;
        }
        /* at the moment we only trap WFI */
        vcpu_block();
        /* The ARM spec declares that even if local irqs are masked in
         * the CPSR register, an irq should wake up a cpu from WFI anyway.
         * For this reason we need to check for irqs that need delivery,
         * ignoring the CPSR register, *after* calling SCHEDOP_block to
         * avoid races with vgic_vcpu_inject_irq.
         */
        if ( local_events_need_delivery_nomask() )
            vcpu_unblock(current);
        advance_pc(regs, hsr);
        break;
    case HSR_EC_CP15_32:
        if ( ! is_pv32_domain(current->domain) )
            goto bad_trap;
        do_cp15_32(regs, hsr);
        break;
    case HSR_EC_CP15_64:
        if ( ! is_pv32_domain(current->domain) )
            goto bad_trap;
        do_cp15_64(regs, hsr);
        break;
    case HSR_EC_SMC32:
        if ( handle_smc(regs, hsr.len) )
        {
            /* PC is adjusted by the handler */
            break;
        }
        /* Treat unhandled SMC call as undef inst */
        inject_undef32_exception(regs);
        break;
    case HSR_EC_HVC32:
#ifndef NDEBUG
        if ( (hsr.iss & 0xff00) == 0xff00 )
            return do_debug_trap(regs, hsr.iss & 0x00ff);
#endif
        if ( hsr.iss == 0 )
            return do_trap_psci(regs);
        do_trap_hypercall(regs, (register_t *)&regs->r12, hsr.iss);
        break;
#ifdef CONFIG_ARM_64
    case HSR_EC_HVC64:
#ifndef NDEBUG
        if ( (hsr.iss & 0xff00) == 0xff00 )
            return do_debug_trap(regs, hsr.iss & 0x00ff);
#endif
        if ( hsr.iss == 0 )
            return do_trap_psci(regs);
        do_trap_hypercall(regs, &regs->x16, hsr.iss);
        break;
    case HSR_EC_SMC64:
        inject_undef64_exception(regs, hsr.len);
        break;
    case HSR_EC_SYSREG:
        if ( is_pv32_domain(current->domain) )
            goto bad_trap;
        do_sysreg(regs, hsr);
        break;
#endif

    case HSR_EC_DATA_ABORT_GUEST:
        do_trap_data_abort_guest(regs, hsr);
        break;
    default:
 bad_trap:
        printk("Hypervisor Trap. HSR=0x%x EC=0x%x IL=%x Syndrome=%"PRIx32"\n",
               hsr.bits, hsr.ec, hsr.len, hsr.iss);
        do_unexpected_trap("Hypervisor", regs);
    }
}

asmlinkage void do_trap_irq(struct cpu_user_regs *regs)
{
    gic_interrupt(regs, 0);
}

asmlinkage void do_trap_fiq(struct cpu_user_regs *regs)
{
    gic_interrupt(regs, 1);
}

asmlinkage void leave_hypervisor_tail(void)
{
    while (1)
    {
        local_irq_disable();
        if (!softirq_pending(smp_processor_id())) {
            gic_inject();
            return;
        }
        local_irq_enable();
        do_softirq();
    }
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
