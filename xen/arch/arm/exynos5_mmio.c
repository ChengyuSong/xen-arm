#include <xen/config.h>
#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/ctype.h>
#include <xen/domain_page.h>
#include <xen/cpu.h>
#include <asm/io.h>

#include "io.h"

#define CONFIG_EXYNOS5410

#define EXYNOS5410_PA_SYSRAM_NS 0x02073000
#define EXYNOS_SYSRAM_NS ((volatile void *)FIXMAP_ADDR(FIXMAP_EXYNOS))
#define S5P_PMU_CORE_CONFIG ((volatile void *)FIXMAP_ADDR(FIXMAP_S5P_PMU))

#ifdef CONFIG_EXYNOS5410
#define EXYNOS5_SYSRAM_NS_START EXYNOS5410_PA_SYSRAM_NS
#define EXYNOS5_SYSRAM_NS_END   (EXYNOS5410_PA_SYSRAM_NS + 0x1000)
#endif

#define REG0_RESUME_ADDR    0x08
#define REG1_RESUME_FLAG    0x0C
#define REG2_UNKNOWN        0x10
#define REG3_UNKNOWN        0x14
#define REG4_SWITCH_ADDR    0x18
#define REG5_BOOT_ADDR      0x1C
#define REG6_DIRECTGO_FLAG  0x20
#define REG7_DIRECTGO_ADDR  0x24
#define CPU0_STATE          0x28
#define CPU1_STATE          0x2C
#define CPU2_STATE          0x30
#define CPU3_STATE          0x34
#define GIC0_STATE          0x38
#define GIC1_STATE          0x3C
#define GIC2_STATE          0x40
#define GIC3_STATE          0x44

extern char start[];

extern void exynos5_cpu_resume(void);
extern void exynos5_cpu_switch(void);

register_t exynos5_resume_addr;
register_t exynos5_switch_addr;
register_t exynos5_boot_addr;
register_t exynos5_directgo_addr;

static unsigned int exynos5_get_booting_cpu(void)
{
    unsigned int val, i;

    for (i = 0; i < 8; i++)
    {
        val = ioreadl(S5P_PMU_CORE_CONFIG + i*0x80 + 0x4);
        
        /* Find the first off CPU */
        if ((val & 0x3) != 0x3)
        {
            //printk("EXYNOS_PMU: CPU %u powered up\n", (i - 1));
            return (i - 1);
        }
    }

    return 0;
}

static int exynos5_sysram_ns_check(struct vcpu *v, paddr_t addr)
{
    struct domain *d = v->domain;

    return d->domain_id == 0 && 
           addr >= EXYNOS5_SYSRAM_NS_START && 
           addr < EXYNOS5_SYSRAM_NS_END;
}

static int exynos5_sysram_ns_read(struct vcpu *v, mmio_info_t *info)
{
    struct hsr_dabt dabt = info->dabt;
    int offset = (int)(info->gpa - EXYNOS5_SYSRAM_NS_START);

    switch ( offset )
    {
    default:
        printk("EXYNOS5_SYSRAM_NS: unhandled read r%d offset %#08x\n",
               dabt.reg, offset);
        domain_crash_synchronous();
    }
}

static int exynos5_sysram_ns_write(struct vcpu *v, mmio_info_t *info)
{
    struct hsr_dabt dabt = info->dabt;
    struct cpu_user_regs *regs = guest_cpu_user_regs();
    register_t *r = select_user_reg(regs, dabt.reg);
    int offset = (int)(info->gpa - EXYNOS5_SYSRAM_NS_START);
    static unsigned int cpu = 1;
    int ret;

    switch ( offset )
    {
    case REG0_RESUME_ADDR:
        printk("EXYNOS5_SYSRAM_NS: set resume addr to 0x%"PRIregister"\n",
               *r);
        iowritel(EXYNOS_SYSRAM_NS + offset, *r);
        return 1;
    case REG1_RESUME_FLAG:
        printk("EXYNOS5_SYSRAM_NS: set resume flag to 0x%"PRIregister"\n",
               *r);
        iowritel(EXYNOS_SYSRAM_NS + offset, *r);
        return 1;
    case REG2_UNKNOWN:
        printk("EXYNOS5_SYSRAM_NS: set REG2_UNKNOWN to 0x%"PRIregister"\n",
               *r);
        iowritel(EXYNOS_SYSRAM_NS + offset, *r);
        return 1;
    case REG3_UNKNOWN:
        printk("EXYNOS5_SYSRAM_NS: set REG3_UNKNOWN to 0x%"PRIregister"\n",
               *r);
        iowritel(EXYNOS_SYSRAM_NS + offset, *r);
        return 1;
    case REG4_SWITCH_ADDR:
        printk("EXYNOS5_SYSRAM_NS: set switch addr to 0x%"PRIregister"\n",
               *r);
        iowritel(EXYNOS_SYSRAM_NS + offset, *r);
        return 1;
    case REG5_BOOT_ADDR:
        //printk("EXYNOS5_SYSRAM_NS: set boot addr to 0x%"PRIregister"\n",
        //       *r);

        exynos5_boot_addr = *r;
        iowritel(EXYNOS_SYSRAM_NS + offset, virt_to_maddr(start));

        //FIXME find a better way to get the boot cpuid
        if (exynos5_get_booting_cpu() == cpu) 
        {
            sev();

            ret = cpu_up(cpu); 
            if (ret != 0) {
                panic("Failed to bring up CPU %u (error %d)\n", cpu, ret);
            }
            printk("CPU %u up, return to guest\n", cpu);

            cpu += 1;
        } 

        return 1;
    case REG6_DIRECTGO_FLAG:
        //printk("EXYNOS5_SYSRAM_NS: set directogo flag to 0x%"PRIregister"\n",
        //       *r);
        iowritel(EXYNOS_SYSRAM_NS + offset, *r);
        return 1;
    case REG7_DIRECTGO_ADDR:
        //printk("EXYNOS5_SYSRAM_NS: set directogo addr to 0x%"PRIregister"\n",
        //       *r);
        /* Use our own resume fn */
        exynos5_directgo_addr = *r;
        iowritel(EXYNOS_SYSRAM_NS + offset, virt_to_maddr(exynos5_cpu_resume));
        return 1;
    case CPU0_STATE ... CPU3_STATE:
        //printk("EXYNOS5_SYSRAM_NS: set CPU%d state to 0x%"PRIregister"\n",
        //       (offset - CPU0_STATE)/4, *r);
        iowritel(EXYNOS_SYSRAM_NS + offset, *r);
        return 1;
    case GIC0_STATE ... GIC3_STATE:
        printk("EXYNOS5_SYSRAM_NS: set GIC%d state to 0x%"PRIregister"\n",
               (offset - GIC0_STATE)/4, *r);
        iowritel(EXYNOS_SYSRAM_NS + offset, *r);
        return 1;
    default:
        printk("EXYNOS5_SYSRAM_NS: unhandled write r%d=%"PRIregister" offset %#08x\n",
               dabt.reg, *r, offset);
        domain_crash_synchronous();
    }
}

const struct mmio_handler exynos5_sysram_ns_handler = {
    .check_handler = exynos5_sysram_ns_check,
    .read_handler  = exynos5_sysram_ns_read,
    .write_handler = exynos5_sysram_ns_write,
};
