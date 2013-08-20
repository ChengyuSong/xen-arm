/*
 * printk() for use before the final page tables are setup.
 *
 * Copyright (C) 2012 Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/stdarg.h>
#include <xen/string.h>
#include <asm/early_printk.h>
#include <xen/mm.h>

void early_putch(char c);
void early_flush(void);

/* Early printk buffer */
static char __initdata buf[1024];

#ifdef EARLY_RAMOOPS_ADDRESS
void __init early_putch(char c)
{
    static uint32_t rammops_offset = 0;
    volatile char *r;
    static uint32_t pfn = EARLY_RAMOOPS_ADDRESS >> PAGE_SHIFT;

    r = (char *)(FIXMAP_ADDR(FIXMAP_CONSOLE) + rammops_offset);
    *r = c;
    rammops_offset += sizeof(char);
    if (rammops_offset >= PAGE_SIZE) {
        clear_fixmap(FIXMAP_CONSOLE);
        if (pfn >= (EARLY_RAMOOPS_ADDRESS + 5 * 0x20000 - 1) >> PAGE_SHIFT)
            pfn = (EARLY_RAMOOPS_ADDRESS >> PAGE_SHIFT) - 1;
        set_fixmap(FIXMAP_CONSOLE, ++pfn, DEV_SHARED);
        rammops_offset = 0;
    }
}

void __init early_flush(void) {}
#endif

static void __init early_puts(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n')
            early_putch('\r');
        early_putch(*s);
        s++;
    }
}

void __init early_vprintk(const char *fmt, va_list args)
{
    vsnprintf(buf, sizeof(buf), fmt, args);
    early_puts(buf);

    /*
     * Wait the UART has finished to transfer all characters before
     * to continue. This will avoid lost characters if Xen abort.
     */
    early_flush();
}

void __init early_printk(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    early_vprintk(fmt, args);
    va_end(args);
}

void __attribute__((noreturn)) __init
early_panic(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    early_vprintk(fmt, args);
    va_end(args);

    while(1);
}
