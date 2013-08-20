#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/cpu.h>
#include <xen/cpumask.h>
#include <xen/timer.h>
#include <xen/sched.h>
#include <xen/kernel.h>
#include <xen/mm.h>
#include <asm/gic.h>
#include <asm/suspend.h>
#include <asm/psci.h>

#include "vtimer.h"

struct sleep_saved_context sleep_saved_context[NR_CPUS];
int resume_debug;

extern register_t exynos5_directgo_addr;
extern void continue_new_vcpu(struct vcpu *prev);

#if 1
static int exynos5_resume_vcpu(unsigned long cpuid)
{
    struct vcpu *v;
    struct domain *d = dom0;
    struct vcpu_guest_context *context;
    int rc = 0;

    BUG_ON(cpuid >= d->max_vcpus);

    v = d->vcpu[cpuid];
    BUG_ON(v == NULL);

    context = NULL;
#if 0
    /* Re-initialize vcpu */
    if ((context = alloc_vcpu_guest_context()) == NULL)
        return -1;

    memset(context, 0, sizeof(*context));
    context->user_regs.pc64 = (u64)exynos5_directgo_addr;
    context->user_regs.cpsr = PSR_GUEST32_INIT;
    context->sctlr = SCTLR_BASE;
    context->ttbr0 = 0;
    context->ttbr1 = 0;
    context->ttbcr = 0;
    context->flags = VGCF_online;

    domain_lock(d);
    rc = arch_set_info_guest(v, context);
    domain_unlock(d);

    free_vcpu_guest_context(context);

    if (rc != 0)
        return rc;
#else
    //memset(&v->arch.saved_context, 0, sizeof(v->arch.saved_context));
    //v->arch.saved_context.sp = (register_t)v->arch.cpu_info;
    //v->arch.saved_context.pc = (register_t)continue_new_vcpu;

    v->arch.cpu_info->guest_cpu_user_regs.pc = exynos5_directgo_addr;
    v->arch.cpu_info->guest_cpu_user_regs.cpsr = PSR_GUEST32_INIT;
#endif

    p2m_load_VTTBR(d);

    WRITE_SYSREG(v->arch.vpidr, VPIDR_EL2);
    WRITE_SYSREG(v->arch.vmpidr, VMPIDR_EL2);

#if 0
    stop_timer(&v->arch.phys_timer.timer);
    v->arch.phys_timer.ctl = 0;
    v->arch.phys_timer.cval = NOW();
    stop_timer(&v->arch.virt_timer.timer);
    v->arch.virt_timer.ctl = 0;

    vgic_clear_pending_irqs(v);
#endif

    return rc;
}
#endif

/* Resume the current CPU */
void __cpuinit cpu_resume(unsigned long cpuid, int sleep_abort)
{
    unmap_temp_xen_11(cpuid);

    if (!sleep_abort)
    {
        set_processor_id(cpuid);

        init_traps();

        setup_virt_paging();

        //gic_init_secondary_cpu();

        //gic_route_ppis();

        //init_maintenance_interrupt();
        //init_timer_interrupt();
    
        gic_cpu_restore();

        /* Report this CPU is up */
        //cpumask_set_cpu(cpuid, &cpu_online_map);
        //wmb();

        if (exynos5_resume_vcpu(cpuid) != 0)
            panic("%s: failed to restore context\n", __func__);

        resume_debug = 1;

        local_irq_enable();
        local_abort_enable();
    }

#if 0
    clear_bit(_VPF_down, &current->pause_flags);
    vcpu_wake(current);
#endif

#if 0
    if (!sleep_abort)
    {
        //printk("CPU %u resumed.\n", smp_processor_id());
    
        set_current(idle_vcpu[cpuid]);
        /* startup_cpu_idle_loop will reset the stack for us */
        startup_cpu_idle_loop();
        //switch_context(current, dom0->vcpu[cpuid]);
    }
#endif
}

int cpu_suspend(void* arg, int(*fn)(void*))
{
    //printk("suspend cpu %d\n", current->vcpu_id);

    map_temp_xen_11();

    local_irq_disable();
    gic_cpu_save();
    local_irq_enable();

#if 0
    if ( !test_and_set_bit(_VPF_down, &current->pause_flags) )
        vcpu_sleep_nosync(current);
#endif

    return exynos5_cpu_suspend(arg, fn);
}
