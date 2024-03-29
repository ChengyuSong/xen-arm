/*
 * xen/drivers/char/pl011.c
 *
 * Driver for ARM PrimeCell PL011 UART.
 *
 * Tim Deegan <tim@xen.org>
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
#include <xen/console.h>
#include <xen/serial.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <asm/early_printk.h>
#include <xen/device_tree.h>
#include <xen/errno.h>
#include <asm/device.h>
#include <xen/mm.h>
#include <xen/vmap.h>
#include <asm/pl011-uart.h>

static struct pl011 {
    unsigned int baud, clock_hz, data_bits, parity, stop_bits;
    struct dt_irq irq;
    void __iomem *regs;
    /* UART with IRQ line: interrupt-driven I/O. */
    struct irqaction irqaction;
    struct vuart_info vuart;
    /* /\* UART with no IRQ line: periodically-polled I/O. *\/ */
    /* struct timer timer; */
    /* unsigned int timeout_ms; */
    /* bool_t probing, intr_works; */
} pl011_com = {0};

/* These parity settings can be ORed directly into the LCR. */
#define PARITY_NONE  (0)
#define PARITY_ODD   (PEN)
#define PARITY_EVEN  (PEN|EPS)
#define PARITY_MARK  (PEN|SPS)
#define PARITY_SPACE (PEN|EPS|SPS)

#define pl011_read(uart, off)           ioreadl((uart)->regs + (off))
#define pl011_write(uart, off,val)      iowritel((uart)->regs + (off), (val))

static void pl011_interrupt(int irq, void *data, struct cpu_user_regs *regs)
{
    struct serial_port *port = data;
    struct pl011 *uart = port->uart;
    unsigned int status = pl011_read(uart, MIS);

    if ( status )
    {
        do
        {
            pl011_write(uart, ICR, status & ~(TXI|RTI|RXI));

            if ( status & (RTI|RXI) )
                serial_rx_interrupt(port, regs);

            /* TODO
                if ( status & (DSRMI|DCDMI|CTSMI|RIMI) )
                ...
            */

            if ( status & (TXI) )
                serial_tx_interrupt(port, regs);

            status = pl011_read(uart, MIS);
        } while (status != 0);
    }
}

static void __init pl011_init_preirq(struct serial_port *port)
{
    struct pl011 *uart = port->uart;
    unsigned int divisor;

    /* No interrupts, please. */
    pl011_write(uart, IMSC, ALLI);

    /* Definitely no DMA */
    pl011_write(uart, DMACR, 0x0);

    /* Line control and baud-rate generator. */
    if ( uart->baud != BAUD_AUTO )
    {
        /* Baud rate specified: program it into the divisor latch. */
        divisor = (uart->clock_hz << 2) / uart->baud; /* clk << 6 / bd << 4 */
        pl011_write(uart, FBRD, divisor & 0x3f);
        pl011_write(uart, IBRD, divisor >> 6);
    }
    else
    {
        /* Baud rate already set: read it out from the divisor latch. */
        divisor = (pl011_read(uart, IBRD) << 6) | (pl011_read(uart, FBRD));
        uart->baud = (uart->clock_hz << 2) / divisor;
    }
    /* This write must follow FBRD and IBRD writes. */
    pl011_write(uart, LCR_H, (uart->data_bits - 5) << 5
                            | FEN
                            | ((uart->stop_bits - 1) << 3)
                            | uart->parity);
    /* Clear errors */
    pl011_write(uart, RSR, 0);

    /* Mask and clear the interrupts */
    pl011_write(uart, IMSC, ALLI);
    pl011_write(uart, ICR, ALLI);

    /* Enable the UART for RX and TX; no flow ctrl */
    pl011_write(uart, CR, RXE | TXE | UARTEN);
}

static void __init pl011_init_postirq(struct serial_port *port)
{
    struct pl011 *uart = port->uart;
    int rc;

    if ( uart->irq.irq > 0 )
    {
        uart->irqaction.handler = pl011_interrupt;
        uart->irqaction.name    = "pl011";
        uart->irqaction.dev_id  = port;
        if ( (rc = setup_dt_irq(&uart->irq, &uart->irqaction)) != 0 )
            printk("ERROR: Failed to allocate pl011 IRQ %d\n", uart->irq.irq);
    }

    /* Clear pending error interrupts */
    pl011_write(uart, ICR, OEI|BEI|PEI|FEI);

    /* Unmask interrupts */
    pl011_write(uart, IMSC, RTI|DSRMI|DCDMI|CTSMI|RIMI);
}

static void pl011_suspend(struct serial_port *port)
{
    BUG(); // XXX
}

static void pl011_resume(struct serial_port *port)
{
    BUG(); // XXX
}

static unsigned int pl011_tx_ready(struct serial_port *port)
{
    struct pl011 *uart = port->uart;

    return ((pl011_read(uart, FR) & TXFE) ? 16 : 0);
}

static void pl011_putc(struct serial_port *port, char c)
{
    struct pl011 *uart = port->uart;

    pl011_write(uart, DR, (uint32_t)(unsigned char)c);
}

static int pl011_getc(struct serial_port *port, char *pc)
{
    struct pl011 *uart = port->uart;

    if ( pl011_read(uart, FR) & RXFE )
        return 0;

    *pc = pl011_read(uart, DR) & 0xff;
    return 1;
}

static int __init pl011_irq(struct serial_port *port)
{
    struct pl011 *uart = port->uart;
    return ((uart->irq.irq > 0) ? uart->irq.irq : -1);
}

static const struct dt_irq __init *pl011_dt_irq(struct serial_port *port)
{
    struct pl011 *uart = port->uart;

    return &uart->irq;
}

static const struct vuart_info *pl011_vuart(struct serial_port *port)
{
    struct pl011 *uart = port->uart;

    return &uart->vuart;
}

static struct uart_driver __read_mostly pl011_driver = {
    .init_preirq  = pl011_init_preirq,
    .init_postirq = pl011_init_postirq,
    .endboot      = NULL,
    .suspend      = pl011_suspend,
    .resume       = pl011_resume,
    .tx_ready     = pl011_tx_ready,
    .putc         = pl011_putc,
    .getc         = pl011_getc,
    .irq          = pl011_irq,
    .dt_irq_get   = pl011_dt_irq,
    .vuart_info   = pl011_vuart,
};

/* TODO: Parse UART config from the command line */
static int __init pl011_uart_init(struct dt_device_node *dev,
                                  const void *data)
{
    const char *config = data;
    struct pl011 *uart;
    int res;
    u64 addr, size;

    if ( strcmp(config, "") )
    {
        early_printk("WARNING: UART configuration is not supported\n");
    }

    uart = &pl011_com;

    uart->clock_hz  = 0x16e3600;
    uart->baud      = 38400;
    uart->data_bits = 8;
    uart->parity    = PARITY_NONE;
    uart->stop_bits = 1;

    res = dt_device_get_address(dev, 0, &addr, &size);
    if ( res )
    {
        early_printk("pl011: Unable to retrieve the base"
                     " address of the UART\n");
        return res;
    }

    uart->regs = ioremap_attr(addr, size, PAGE_HYPERVISOR_NOCACHE);
    if ( !uart->regs )
    {
        early_printk("pl011: Unable to map the UART memory\n");

        return -ENOMEM;
    }

    res = dt_device_get_irq(dev, 0, &uart->irq);
    if ( res )
    {
        early_printk("pl011: Unable to retrieve the IRQ\n");
        return res;
    }

    uart->vuart.base_addr = addr;
    uart->vuart.size = size;
    uart->vuart.data_off = DR;
    uart->vuart.status_off = FR;
    uart->vuart.status = 0;

    /* Register with generic serial driver. */
    serial_register_uart(SERHND_DTUART, &pl011_driver, uart);

    dt_device_set_used_by(dev, DOMID_XEN);

    return 0;
}

static const char const *pl011_dt_compat[] __initdata =
{
    "arm,pl011",
    NULL
};

DT_DEVICE_START(pl011, "PL011 UART", DEVICE_SERIAL)
        .compatible = pl011_dt_compat,
        .init = pl011_uart_init,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
