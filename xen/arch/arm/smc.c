#include <xen/config.h>
#include <xen/lib.h>
#include <xen/smp.h>
#include <xen/sched.h>
#include <asm/current.h>
#include <asm/suspend.h>

#include "smc.h"

#define SMC_CMD_INIT        (-1)
#define SMC_CMD_INFO        (-2)
/* For Power Management */
#define SMC_CMD_SLEEP       (-3)
#define SMC_CMD_CPU1BOOT    (-4)
#define SMC_CMD_CPU0AFTR    (-5)
#define SMC_CMD_SAVE        (-6)
#define SMC_CMD_SHUTDOWN    (-7)

/* For CP15 Access */
#define SMC_CMD_C15RESUME   (-11)
/* For L2 Cache Access */
#define SMC_CMD_L2X0CTRL    (-21)
#define SMC_CMD_L2X0SETUP1  (-22)
#define SMC_CMD_L2X0SETUP2  (-23)
#define SMC_CMD_L2X0INVALL  (-24)
#define SMC_CMD_L2X0DEBUG   (-25)
#define SMC_CMD_SWRESET     (-26)

#define MC_SMC_TRACE        (-31)

#define MC_SMC_YIELD        (3)
#define MC_SMC_SIQ          (4)

/* For Accessing CP15/SFR (General) */
#define SMC_CMD_REG         (-101)

#define CONFIG_PM 0

extern int forward_smc(void *regs);
extern register_t exynos5_directgo_addr;

int handle_smc(struct cpu_user_regs *regs, int is_32bit)
{
    
    /* Handle Samsung specific SMC calls */
    switch ((long)regs->r0)
    {
    case SMC_CMD_SHUTDOWN:
#if CONFIG_PM
        if (smp_processor_id() == 0)
            break;

        if (cpu_suspend(regs, forward_smc))
        {
            /* in case failed (ret != 0), we need to resume */
            printk("cpu_suspend failed on %d\n", current->vcpu_id);
            cpu_resume(current->vcpu_id, 1);
            regs->pc += is_32bit ? 4 : 2;
        }
        return 1;
#endif
    case SMC_CMD_SAVE:
#if CONFIG_PM
        if (smp_processor_id() != 0)
            forward_smc(regs);
#endif
        break;

    case MC_SMC_TRACE:
    case MC_SMC_YIELD:
    case MC_SMC_SIQ:
        forward_smc(regs);
        break;

    case SMC_CMD_SLEEP:
    case SMC_CMD_CPU0AFTR:
    case SMC_CMD_CPU1BOOT:
    case SMC_CMD_INIT:
    case SMC_CMD_INFO:
    case SMC_CMD_L2X0CTRL:
    case SMC_CMD_L2X0SETUP1:
    case SMC_CMD_L2X0SETUP2:
    case SMC_CMD_L2X0INVALL:
    case SMC_CMD_L2X0DEBUG:
    case SMC_CMD_SWRESET:

        printk("!!! SMC @ 0x%08x (0x%08x, 0x%08x, 0x%08x, 0x%08x) ... \n", 
               regs->pc32, regs->r0, regs->r1, regs->r2, regs->r3);

        forward_smc(regs);

        printk(" done\n");

        break;
    default:

        printk("!!! Unknown SMC @ 0x%08x (0x%08x, 0x%08x, 0x%08x, 0x%08x)\n", 
               regs->pc32, regs->r0, regs->r1, regs->r2, regs->r3);

        forward_smc(regs);
        //return 0;
    }

    regs->pc += is_32bit ? 4 : 2;
    return 1;
}
