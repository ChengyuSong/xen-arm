/*
 * xen/arch/arm/smpboot.c
 *
 * Dummy smpboot support
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

#include <xen/cpu.h>
#include <xen/cpumask.h>
#include <xen/delay.h>
#include <xen/domain_page.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/smp.h>
#include <xen/softirq.h>
#include <xen/timer.h>
#include <xen/irq.h>
#include <asm/gic.h>

cpumask_t cpu_online_map;
EXPORT_SYMBOL(cpu_online_map);
cpumask_t cpu_present_map;
EXPORT_SYMBOL(cpu_online_map);
cpumask_t cpu_possible_map;
EXPORT_SYMBOL(cpu_possible_map);

extern register_t exynos5_boot_addr;

struct cpuinfo_arm cpu_data[NR_CPUS];

/* Fake one node for now. See also include/asm-arm/numa.h */
nodemask_t __read_mostly node_online_map = { { [0] = 1UL } };

/* Xen stack for bringing up the first CPU. */
static unsigned char __initdata cpu0_boot_stack[STACK_SIZE]
       __attribute__((__aligned__(STACK_SIZE)));

/* Pointer to the stack, used by head.S when entering C */
unsigned char *init_stack = cpu0_boot_stack;

/* Shared state for coordinating CPU bringup */
unsigned long smp_up_cpu = 0;
static bool_t cpu_is_dead = 0;

/* Number of non-boot CPUs ready to enter C */
unsigned long __initdata ready_cpus = 0;

/* ID of the PCPU we're running on */
DEFINE_PER_CPU(unsigned int, cpu_id);
/* XXX these seem awfully x86ish... */
/* representing HT siblings of each logical CPU */
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_sibling_mask);
/* representing HT and core siblings of each logical CPU */
DEFINE_PER_CPU_READ_MOSTLY(cpumask_var_t, cpu_core_mask);

static void setup_cpu_sibling_map(int cpu)
{
    if ( !zalloc_cpumask_var(&per_cpu(cpu_sibling_mask, cpu)) ||
         !zalloc_cpumask_var(&per_cpu(cpu_core_mask, cpu)) )
        panic("No memory for CPU sibling/core maps\n");

    /* A CPU is a sibling with itself and is always on its own core. */
    cpumask_set_cpu(cpu, per_cpu(cpu_sibling_mask, cpu));
    cpumask_set_cpu(cpu, per_cpu(cpu_core_mask, cpu));
}

void __init
smp_clear_cpu_maps (void)
{
    cpumask_clear(&cpu_possible_map);
    cpumask_clear(&cpu_online_map);
    cpumask_set_cpu(0, &cpu_online_map);
    cpumask_set_cpu(0, &cpu_possible_map);
}

int __init
smp_get_max_cpus (void)
{
    int i, max_cpus = 0;

    for ( i = 0; i < nr_cpu_ids; i++ )
        if ( cpu_possible(i) )
            max_cpus++;

    return max_cpus;
}


void __init
smp_prepare_cpus (unsigned int max_cpus)
{
    cpumask_copy(&cpu_present_map, &cpu_possible_map);

    setup_cpu_sibling_map(0);
}

void __init
make_cpus_ready(unsigned int max_cpus, unsigned long boot_phys_offset)
{
    unsigned long *gate;
    paddr_t gate_pa;
    int i;

    printk("Waiting for %i other CPUs to be ready\n", max_cpus - 1);
    /* We use the unrelocated copy of smp_up_cpu as that's the one the
     * others can see. */ 
    gate_pa = ((paddr_t) (unsigned long) &smp_up_cpu) + boot_phys_offset;
    gate = map_domain_page(gate_pa >> PAGE_SHIFT) + (gate_pa & ~PAGE_MASK); 
    for ( i = 1; i < max_cpus; i++ )
    {
        /* Tell the next CPU to get ready */
        /* TODO: handle boards where CPUIDs are not contiguous */
        *gate = i;
        flush_xen_dcache(*gate);
        isb();
        sev();
        /* And wait for it to respond */
        while ( ready_cpus < i )
            smp_rmb();
    }
    unmap_domain_page(gate);
}

/* Boot the current CPU */
void __cpuinit start_secondary(unsigned long boot_phys_offset,
                               unsigned long fdt_paddr,
                               unsigned long cpuid)
{
    struct vcpu *v;
    struct cpu_user_regs *regs;

    memset(get_cpu_info(), 0, sizeof (struct cpu_info));

    /* TODO: handle boards where CPUIDs are not contiguous */
    set_processor_id(cpuid);

    current_cpu_data = boot_cpu_data;
    identify_cpu(&current_cpu_data);

    init_traps();

    setup_virt_paging();

    mmu_init_secondary_cpu();

    gic_init_secondary_cpu();

    init_secondary_IRQ();

    gic_route_ppis();

    init_maintenance_interrupt();
    init_timer_interrupt();

    set_current(idle_vcpu[cpuid]);

    setup_cpu_sibling_map(cpuid);

    /* Run local notifiers */
    notify_cpu_starting(cpuid);
    wmb();

    /* Now report this CPU is up */
    cpumask_set_cpu(cpuid, &cpu_online_map);
    wmb();

    /* Setup dom0 boot pc */
    {
        v = alloc_vcpu(dom0, cpuid, cpuid);
        if (v == NULL) 
        {
            panic("Failed to allocate dom0 vcpu %lu on pcpu %lu\n", cpuid, cpuid);
        }

        p2m_load_VTTBR(dom0);

        v->is_initialised = 1;
        clear_bit(_VPF_down, &v->pause_flags);

        regs = &v->arch.cpu_info->guest_cpu_user_regs;
        memset(regs, 0, sizeof(*regs));
        regs->pc = exynos5_boot_addr;
        regs->cpsr = PSR_GUEST32_INIT;

        printk("Guest secondary PC = 0x%08x\n", regs->pc);

        vcpu_wake(v);
    }

    local_irq_enable();
    local_abort_enable();

    printk(XENLOG_DEBUG "CPU %u booted.\n", smp_processor_id());

    startup_cpu_idle_loop();
}

/* Shut down the current CPU */
void __cpu_disable(void)
{
    unsigned int cpu = get_processor_id();

    local_irq_disable();
    gic_disable_cpu();
    /* Allow any queued timer interrupts to get serviced */
    local_irq_enable();
    mdelay(1);
    local_irq_disable();

    /* It's now safe to remove this processor from the online map */
    cpumask_clear_cpu(cpu, &cpu_online_map);

    if ( cpu_disable_scheduler(cpu) )
        BUG();
    mb();

    /* Return to caller; eventually the IPI mechanism will unwind and the 
     * scheduler will drop to the idle loop, which will call stop_cpu(). */
}

void stop_cpu(void)
{
    local_irq_disable();
    cpu_is_dead = 1;
    /* Make sure the write happens before we sleep forever */
    dsb();
    isb();
    while ( 1 )
        wfi();
}

/* Bring up a remote CPU */
int __cpu_up(unsigned int cpu)
{
    int rc;

    map_temp_xen_11();

    rc = init_secondary_pagetables(cpu);
    if ( rc < 0 )
        return rc;

    /* Tell the remote CPU which stack to boot on. */
    init_stack = idle_vcpu[cpu]->arch.stack;

    /* Unblock the CPU.  It should be waiting in the loop in head.S
     * for an event to arrive when smp_up_cpu matches its cpuid. */
    smp_up_cpu = cpu;
    /* we need to make sure that the change to smp_up_cpu is visible to
     * secondary cpus with D-cache off */
    flush_xen_dcache(smp_up_cpu);
    isb();
    sev();

    while ( !cpu_online(cpu) )
    {
        cpu_relax();
        process_pending_softirqs();
    }

    unmap_temp_xen_11(smp_processor_id());

    return 0;
}

/* Wait for a remote CPU to die */
void __cpu_die(unsigned int cpu)
{
    unsigned int i = 0;

    while ( !cpu_is_dead )
    {
        mdelay(100);
        cpu_relax();
        process_pending_softirqs();
        if ( (++i % 10) == 0 )
            printk(KERN_ERR "CPU %u still not dead...\n", cpu);
        mb();
    }
    cpu_is_dead = 0;
    mb();
}


/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
