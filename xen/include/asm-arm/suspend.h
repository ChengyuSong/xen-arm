#ifndef __ARM_SUSPEND_H__
#define __ARM_SUSPEND_H__

#ifndef __ASSEMBLY__
struct sleep_saved_context
{
    uint32_t hsctlr;
    uint32_t httbr_low;
    uint32_t httbr_high;
    uint32_t sp;
};
#endif

extern void cpu_resume(unsigned long cpuid, int sleep_abort);
extern int cpu_suspend(void*, int (*)(void*));

extern void exynos5_cpu_resume(void);
extern int exynos5_cpu_suspend(void*, int (*)(void*));

#endif
