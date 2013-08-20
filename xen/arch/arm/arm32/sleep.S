#include <asm/config.h>
#include <asm/page.h>
#include <asm/processor-ca15.h>
#include <asm/platforms/exynos5.h>
#include <asm/asm_defns.h>

#if (defined (EARLY_PRINTK)) && (defined (EARLY_PRINTK_INC))
#include EARLY_PRINTK_INC
#endif

#ifdef EARLY_PRINTK
#define PRINT(_s)   \
    adr     r0, 98f;\
    bl      puts   ;\
    b       99f    ;\
98: .asciz  _s     ;\
    .align  2      ;\
99:
#else
#define PRINT(s)
#endif /* !EARLY_PRINTK */

    .text
    .arm
    .align
    .word 0x2bedf00d

ENTRY(exynos5_cpu_resume)
    cpsid   aif         /* Disable all interrupts */

    /* Invalidate L1 I/D cache */
    mov     r0, #0
    mcr     CP32(r0, TLBIALL)
    mcr     CP32(r0, ICIALLU)

    /* Disable stage 1 translation */
    mrc     CP32(r0, SCTLR)
    bic     r0, r0, #0x00000005
    bic     r0, r0, #0x00002000
    mcr     CP32(r0, SCTLR)

    /* Find out where we are */
    ldr     r0, =exynos5_cpu_resume
    adr     r9, exynos5_cpu_resume
    sub     r10, r9, r0

    /* Get the CPUID */
    mrc     CP32(r0, MPIDR)
    /* bics    r12, r0, #(0xff << 24) */
    /* Use the Linux way */
    and     r12, r0, #0xf

#ifdef EARLY_PRINTK
    ldr     r11, =EARLY_UART_BASE_ADDRESS
    /*PRINT("- CPU ")
    mov     r0, r12
    bl      putn
    PRINT(" resuming -\r\n")*/
#endif

    /* Check current mode */
    mrs     r0, cpsr
    and     r0, r0, #0x1f
    teq     r0, #0x1a
    beq     hyp
    PRINT("- CPU is not resume in Hyp mode -\r\n")
    b       fail

hyp:
    /* Set up memory attribute type tables */
1:  ldr     r0, =MAIR0VAL
    ldr     r1, =MAIR1VAL
    mcr     CP32(r0, MAIR0)
    mcr     CP32(r1, MAIR1)
    mcr     CP32(r0, HMAIR0)
    mcr     CP32(r1, HMAIR1)

    /* Set up HTCR */
    ldr     r0, =0x80002500
    mcr     CP32(r0, HTCR)

    /* Now restore the saved stack
     * unlike Linux, we want to keep the code readonly
     * and the data xn
     */
    ldr     r0, =sleep_saved_context
    add     r0, r0, r10
    add     r0, r0, r12, lsl #4 /* 4 word, HSCTLR, HTTBR, sp */
    ldmia   r0!, {r5 - r7, sp}

    /* Resume context */
    mcrr    CP64(r6, r7, HTTBR)

    /* Resume paging */
    /*PRINT("- Turning on paging -\r\n")*/

    ldr     r1, =paging
    dsb
    mcr     CP32(r5, HSCTLR)
    isb
    mov     pc, r1

paging:

#ifdef EARLY_PRINTK
    ldr     r11, =FIXMAP_ADDR(FIXMAP_CONSOLE)
#endif

    /*PRINT("- Ready -\r\n")*/

    mov     r0, r12
    mov     r1, #0
    bl      cpu_resume
    mov     r0, #0
    ldmfd   sp!, {r2 - r11, pc}

fail:
    PRINT("- Resume failed -\r\n")
1:  wfe
    b       1b


/* 
 * Use Linux code as template
 * r0 = suspend fn arg0
 * r1 = suspend fn
 */
ENTRY(exynos5_cpu_suspend)
    stmfd   sp!, {r2 - r11, lr}

    /* Get current HSCTLR */
    mrc     CP32(r5, HSCTLR)

    /* Get current HTTBR */
    mrrc    CP64(r6, r7, HTTBR)

    /* Get current CPUID */
    mrc     CP32(r12, MPIDR)
    and     r12, r12, #0xf

    /* Save the context */
    ldr     r4, =sleep_saved_context
    add     r4, r4, r12, lsl #4
    stmia   r4, {r5 - r7, sp}

    /* Flush all dcache */
    stmfd   sp!, {r0-r1, r4}
    bl      flush_dcache_all
    ldmfd   sp!, {r0-r1, r4}

    /* Flush sleep_saved_context */
    mcr     CP32(r4, DCCIMVAC)
    dsb

    /* Call the suspend fn */
    adr     lr, cpu_suspend_abort
    mov     pc, r1
    
cpu_suspend_abort:
    mov     r0, #1
    /* return */
    ldmfd   sp!, {r2 - r11, pc}

/* from Linux arch/arm/mm/cache-v7.S */
flush_dcache_all:
    dmb
    mrc     CP32(r0, CLIDR)
    ands    r3, r0, #0x70000000
    mov     r3, r3, lsr #23
    beq     finished
    mov     r10, #0
loop1:
    add     r2, r10, r10, lsr #1
    mov     r1, r0, lsr r2
    cmp     r1, #2
    blt     skip
    mrs     r9, cpsr
    cpsid   i
    mcr     CP32(r10, CSSELR)
    isb
    mrc     CP32(r1, CCSIDR)
    msr     cpsr_c, r9
    and     r2, r1, #7
    add     r2, r2, #4
    ldr     r4, =0x3ff
    ands    r4, r4, r1, lsr #3
    clz     r5, r4
    ldr     r7, =0x7fff
    ands    r7, r7, r1, lsr #13
loop2:
    mov     r9, r4
loop3:
    orr     r11, r10, r9, lsl r5
    orr     r11, r11, r7, lsl r2
    mcr     CP32(r11, DCCISW)
    subs    r9, r9, #1
    bge     loop3
    subs    r7, r7, #1
    bge     loop2
skip:
    add     r10, r10, #2
    cmp     r3, r10
    bgt     loop1
finished:
    mov     r10, #0
    mcr     CP32(r10, CSSELR)
    dsb
    isb
    mov     pc, lr

#ifdef EARLY_PRINTK
puts:
    early_uart_ready r11, r1
    ldrb    r1, [r0], #1
    teq     r1, #0
    moveq   pc, lr
    early_uart_transmit r11, r1
    b puts

putn:
    adr     r1, hex
    mov     r3, #8
1:  early_uart_ready r11, r2
    and     r2, r0, #0xf0000000
    ldrb    r2, [r1, r2, lsr #28]
    early_uart_transmit r11, r2
    lsl     r0, #4
    subs    r3, r3, #1
    bne     1b
    mov     pc, lr

hex:
    .ascii  "0123456789abcdef"
    .align  2

#endif
