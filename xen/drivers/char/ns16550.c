/******************************************************************************
 * ns16550.c
 * 
 * Driver for 16550-series UARTs. This driver is to be kept within Xen as
 * it permits debugging of seriously-toasted machines (e.g., in situations
 * where a device driver within a guest OS would be inaccessible).
 * 
 * Copyright (c) 2003-2005, K A Fraser
 */

#include <xen/config.h>
#include <xen/console.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/sched.h>
#include <xen/pci.h>
#include <xen/timer.h>
#include <xen/serial.h>
#include <xen/iocap.h>
#include <xen/pci.h>
#include <xen/pci_regs.h>
#include <xen/ns16550-uart.h>
#include <asm/io.h>
#ifdef CONFIG_X86
#include <asm/fixmap.h>
#endif

/*
 * Configure serial port with a string:
 *   <baud>[/<clock_hz>][,DPS[,<io-base>[,<irq>[,<port-bdf>[,<bridge-bdf>]]]]].
 * The tail of the string can be omitted if platform defaults are sufficient.
 * If the baud rate is pre-configured, perhaps by a bootloader, then 'auto'
 * can be specified in place of a numeric baud rate. Polled mode is specified
 * by requesting irq 0.
 */
static char __initdata opt_com1[30] = "";
static char __initdata opt_com2[30] = "";
string_param("com1", opt_com1);
string_param("com2", opt_com2);

static struct ns16550 {
    int baud, clock_hz, data_bits, parity, stop_bits, fifo_size, irq;
    unsigned long io_base;   /* I/O port or memory-mapped I/O address. */
    char __iomem *remapped_io_base;  /* Remapped virtual address of MMIO. */
    /* UART with IRQ line: interrupt-driven I/O. */
    struct irqaction irqaction;
    /* UART with no IRQ line: periodically-polled I/O. */
    struct timer timer;
    struct timer resume_timer;
    unsigned int timeout_ms;
    bool_t intr_works;
    /* PCI card parameters. */
    unsigned int pb_bdf[3]; /* pci bridge BDF */
    unsigned int ps_bdf[3]; /* pci serial port BDF */
    bool_t pb_bdf_enable;   /* if =1, pb-bdf effective, port behind bridge */
    bool_t ps_bdf_enable;   /* if =1, ps_bdf effective, port on pci card */
    u32 bar;
    u16 cr;
    u8 bar_idx;
} ns16550_com[2] = { { 0 } };

static char ns_read_reg(struct ns16550 *uart, int reg)
{
    if ( uart->remapped_io_base == NULL )
        return inb(uart->io_base + reg);
    return readb(uart->remapped_io_base + reg);
}

static void ns_write_reg(struct ns16550 *uart, int reg, char c)
{
    if ( uart->remapped_io_base == NULL )
        return outb(c, uart->io_base + reg);
    writeb(c, uart->remapped_io_base + reg);
}

static void ns16550_interrupt(
    int irq, void *dev_id, struct cpu_user_regs *regs)
{
    struct serial_port *port = dev_id;
    struct ns16550 *uart = port->uart;

    uart->intr_works = 1;

    while ( !(ns_read_reg(uart, UART_IIR) & UART_IIR_NOINT) )
    {
        char lsr = ns_read_reg(uart, UART_LSR);
        if ( lsr & UART_LSR_THRE )
            serial_tx_interrupt(port, regs);
        if ( lsr & UART_LSR_DR )
            serial_rx_interrupt(port, regs);
    }
}

/* Safe: ns16550_poll() runs as softirq so not reentrant on a given CPU. */
static DEFINE_PER_CPU(struct serial_port *, poll_port);

static void __ns16550_poll(struct cpu_user_regs *regs)
{
    struct serial_port *port = this_cpu(poll_port);
    struct ns16550 *uart = port->uart;

    if ( uart->intr_works )
        return; /* Interrupts work - no more polling */

    while ( ns_read_reg(uart, UART_LSR) & UART_LSR_DR )
        serial_rx_interrupt(port, regs);

    if ( ns_read_reg(uart, UART_LSR) & UART_LSR_THRE )
        serial_tx_interrupt(port, regs);

    set_timer(&uart->timer, NOW() + MILLISECS(uart->timeout_ms));
}

static void ns16550_poll(void *data)
{
    this_cpu(poll_port) = data;
#ifdef run_in_exception_handler
    run_in_exception_handler(__ns16550_poll);
#else
    __ns16550_poll(guest_cpu_user_regs());
#endif
}

static unsigned int ns16550_tx_ready(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;

    return ns_read_reg(uart, UART_LSR) & UART_LSR_THRE ? uart->fifo_size : 0;
}

static void ns16550_putc(struct serial_port *port, char c)
{
    struct ns16550 *uart = port->uart;
    ns_write_reg(uart, UART_THR, c);
}

static int ns16550_getc(struct serial_port *port, char *pc)
{
    struct ns16550 *uart = port->uart;

    if ( !(ns_read_reg(uart, UART_LSR) & UART_LSR_DR) )
        return 0;

    *pc = ns_read_reg(uart, UART_RBR);
    return 1;
}

static void pci_serial_early_init(struct ns16550 *uart)
{
    if ( !uart->ps_bdf_enable || uart->io_base >= 0x10000 )
        return;
    
    if ( uart->pb_bdf_enable )
        pci_conf_write16(0, uart->pb_bdf[0], uart->pb_bdf[1], uart->pb_bdf[2],
                         PCI_IO_BASE,
                         (uart->io_base & 0xF000) |
                         ((uart->io_base & 0xF000) >> 8));

    pci_conf_write32(0, uart->ps_bdf[0], uart->ps_bdf[1], uart->ps_bdf[2],
                     PCI_BASE_ADDRESS_0,
                     uart->io_base | PCI_BASE_ADDRESS_SPACE_IO);
    pci_conf_write16(0, uart->ps_bdf[0], uart->ps_bdf[1], uart->ps_bdf[2],
                     PCI_COMMAND, PCI_COMMAND_IO);
}

static void ns16550_setup_preirq(struct ns16550 *uart)
{
    unsigned char lcr;
    unsigned int  divisor;

    uart->intr_works = 0;

    pci_serial_early_init(uart);

    lcr = (uart->data_bits - 5) | ((uart->stop_bits - 1) << 2) | uart->parity;

    /* No interrupts. */
    ns_write_reg(uart, UART_IER, 0);

    /* Line control and baud-rate generator. */
    ns_write_reg(uart, UART_LCR, lcr | UART_LCR_DLAB);
    if ( uart->baud != BAUD_AUTO )
    {
        /* Baud rate specified: program it into the divisor latch. */
        divisor = uart->clock_hz / (uart->baud << 4);
        ns_write_reg(uart, UART_DLL, (char)divisor);
        ns_write_reg(uart, UART_DLM, (char)(divisor >> 8));
    }
    else
    {
        /* Baud rate already set: read it out from the divisor latch. */
        divisor  = ns_read_reg(uart, UART_DLL);
        divisor |= ns_read_reg(uart, UART_DLM) << 8;
        uart->baud = uart->clock_hz / (divisor << 4);
    }
    ns_write_reg(uart, UART_LCR, lcr);

    /* No flow ctrl: DTR and RTS are both wedged high to keep remote happy. */
    ns_write_reg(uart, UART_MCR, UART_MCR_DTR | UART_MCR_RTS);

    /* Enable and clear the FIFOs. Set a large trigger threshold. */
    ns_write_reg(uart, UART_FCR,
                 UART_FCR_ENABLE | UART_FCR_CLRX | UART_FCR_CLTX | UART_FCR_TRG14);
}

static void __init ns16550_init_preirq(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;

    /* I/O ports are distinguished by their size (16 bits). */
    if ( uart->io_base >= 0x10000 )
    {
#ifdef CONFIG_X86
        enum fixed_addresses idx = FIX_COM_BEGIN + (uart - ns16550_com);

        set_fixmap_nocache(idx, uart->io_base);
        uart->remapped_io_base = (void __iomem *)fix_to_virt(idx);
        uart->remapped_io_base += uart->io_base & ~PAGE_MASK;
#else
        uart->remapped_io_base = (char *)ioremap(uart->io_base, 8);
#endif
    }

    ns16550_setup_preirq(uart);

    /* Check this really is a 16550+. Otherwise we have no FIFOs. */
    if ( ((ns_read_reg(uart, UART_IIR) & 0xc0) == 0xc0) &&
         ((ns_read_reg(uart, UART_FCR) & UART_FCR_TRG14) == UART_FCR_TRG14) )
        uart->fifo_size = 16;
}

static void ns16550_setup_postirq(struct ns16550 *uart)
{
    if ( uart->irq > 0 )
    {
        /* Master interrupt enable; also keep DTR/RTS asserted. */
        ns_write_reg(uart,
                     UART_MCR, UART_MCR_OUT2 | UART_MCR_DTR | UART_MCR_RTS);

        /* Enable receive and transmit interrupts. */
        ns_write_reg(uart, UART_IER, UART_IER_ERDAI | UART_IER_ETHREI);
    }

    if ( uart->irq >= 0 )
        set_timer(&uart->timer, NOW() + MILLISECS(uart->timeout_ms));
}

static void __init ns16550_init_postirq(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;
    int rc, bits;

    if ( uart->irq < 0 )
        return;

    serial_async_transmit(port);

    init_timer(&uart->timer, ns16550_poll, port, 0);

    /* Calculate time to fill RX FIFO and/or empty TX FIFO for polling. */
    bits = uart->data_bits + uart->stop_bits + !!uart->parity;
    uart->timeout_ms = max_t(
        unsigned int, 1, (bits * uart->fifo_size * 1000) / uart->baud);

    if ( uart->irq > 0 )
    {
        uart->irqaction.handler = ns16550_interrupt;
        uart->irqaction.name    = "ns16550";
        uart->irqaction.dev_id  = port;
        if ( (rc = setup_irq(uart->irq, &uart->irqaction)) != 0 )
            printk("ERROR: Failed to allocate ns16550 IRQ %d\n", uart->irq);
    }

    ns16550_setup_postirq(uart);

    if ( uart->bar || uart->ps_bdf_enable )
        pci_hide_device(uart->ps_bdf[0], PCI_DEVFN(uart->ps_bdf[1],
                                                   uart->ps_bdf[2]));
}

static void ns16550_suspend(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;

    stop_timer(&uart->timer);

    if ( uart->bar )
       uart->cr = pci_conf_read16(0, uart->ps_bdf[0], uart->ps_bdf[1],
                                  uart->ps_bdf[2], PCI_COMMAND);
}

static void _ns16550_resume(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;

    if ( uart->bar )
    {
       pci_conf_write32(0, uart->ps_bdf[0], uart->ps_bdf[1], uart->ps_bdf[2],
                        PCI_BASE_ADDRESS_0 + uart->bar_idx*4, uart->bar);
       pci_conf_write16(0, uart->ps_bdf[0], uart->ps_bdf[1], uart->ps_bdf[2],
                        PCI_COMMAND, uart->cr);
    }

    ns16550_setup_preirq(port->uart);
    ns16550_setup_postirq(port->uart);
}

static int ns16550_ioport_invalid(struct ns16550 *uart)
{
    return ((((unsigned char)ns_read_reg(uart, UART_LSR)) == 0xff) &&
            (((unsigned char)ns_read_reg(uart, UART_MCR)) == 0xff) &&
            (((unsigned char)ns_read_reg(uart, UART_IER)) == 0xff) &&
            (((unsigned char)ns_read_reg(uart, UART_IIR)) == 0xff) &&
            (((unsigned char)ns_read_reg(uart, UART_LCR)) == 0xff));
}

static int delayed_resume_tries;
static void ns16550_delayed_resume(void *data)
{
    struct serial_port *port = data;
    struct ns16550 *uart = port->uart;

    if ( ns16550_ioport_invalid(port->uart) && delayed_resume_tries-- )
        set_timer(&uart->resume_timer, NOW() + RESUME_DELAY);
    else
        _ns16550_resume(port);
}

static void ns16550_resume(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;

    /*
     * Check for ioport access, before fully resuming operation.
     * On some systems, there is a SuperIO card that provides
     * this legacy ioport on the LPC bus.
     *
     * We need to wait for dom0's ACPI processing to run the proper
     * AML to re-initialize the chip, before we can use the card again.
     *
     * This may cause a small amount of garbage to be written
     * to the serial log while we wait patiently for that AML to
     * be executed. However, this is preferable to spinning in an
     * infinite loop, as seen on a Lenovo T430, when serial was enabled.
     */
    if ( ns16550_ioport_invalid(uart) )
    {
        delayed_resume_tries = RESUME_RETRIES;
        init_timer(&uart->resume_timer, ns16550_delayed_resume, port, 0);
        set_timer(&uart->resume_timer, NOW() + RESUME_DELAY);
    }
    else
        _ns16550_resume(port);
}

#ifdef CONFIG_X86
static void __init ns16550_endboot(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;

    if ( uart->remapped_io_base )
        return;
    if ( ioports_deny_access(dom0, uart->io_base, uart->io_base + 7) != 0 )
        BUG();
}
#else
#define ns16550_endboot NULL
#endif

static int __init ns16550_irq(struct serial_port *port)
{
    struct ns16550 *uart = port->uart;
    return ((uart->irq > 0) ? uart->irq : -1);
}

static struct uart_driver __read_mostly ns16550_driver = {
    .init_preirq  = ns16550_init_preirq,
    .init_postirq = ns16550_init_postirq,
    .endboot      = ns16550_endboot,
    .suspend      = ns16550_suspend,
    .resume       = ns16550_resume,
    .tx_ready     = ns16550_tx_ready,
    .putc         = ns16550_putc,
    .getc         = ns16550_getc,
    .irq          = ns16550_irq
};

static int __init parse_parity_char(int c)
{
    switch ( c )
    {
    case 'n':
        return UART_PARITY_NONE;
    case 'o': 
        return UART_PARITY_ODD;
    case 'e': 
        return UART_PARITY_EVEN;
    case 'm': 
        return UART_PARITY_MARK;
    case 's': 
        return UART_PARITY_SPACE;
    }
    return 0;
}

static void __init parse_pci_bdf(const char **conf, unsigned int bdf[3])
{
    bdf[0] = simple_strtoul(*conf, conf, 16);
    if ( **conf != ':' )
        return;
    (*conf)++;
    bdf[1] = simple_strtoul(*conf, conf, 16);
    if ( **conf != '.' )
        return;
    (*conf)++;
    bdf[2] = simple_strtoul(*conf, conf, 16);
}

static int __init check_existence(struct ns16550 *uart)
{
    unsigned char status, scratch, scratch2, scratch3;

    /*
     * We can't poke MMIO UARTs until they get I/O remapped later. Assume that
     * if we're getting MMIO UARTs, the arch code knows what it's doing.
     */
    if ( uart->io_base >= 0x10000 )
        return 1;

    pci_serial_early_init(uart);
    
    /*
     * Do a simple existence test first; if we fail this,
     * there's no point trying anything else.
     */
    scratch = ns_read_reg(uart, UART_IER);
    ns_write_reg(uart, UART_IER, 0);

    /*
     * Mask out IER[7:4] bits for test as some UARTs (e.g. TL
     * 16C754B) allow only to modify them if an EFR bit is set.
     */
    scratch2 = ns_read_reg(uart, UART_IER) & 0x0f;
    ns_write_reg(uart,UART_IER, 0x0F);
    scratch3 = ns_read_reg(uart, UART_IER) & 0x0f;
    ns_write_reg(uart, UART_IER, scratch);
    if ( (scratch2 != 0) || (scratch3 != 0x0F) )
        return 0;

    /*
     * Check to see if a UART is really there.
     * Use loopback test mode.
     */
    ns_write_reg(uart, UART_MCR, UART_MCR_LOOP | 0x0A);
    status = ns_read_reg(uart, UART_MSR) & 0xF0;
    return (status == 0x90);
}

static int
pci_uart_config (struct ns16550 *uart, int skip_amt, int bar_idx)
{
    uint32_t bar, len;
    int b, d, f, nextf;

    /* NB. Start at bus 1 to avoid AMT: a plug-in card cannot be on bus 0. */
    for ( b = skip_amt ? 1 : 0; b < 0x100; b++ )
    {
        for ( d = 0; d < 0x20; d++ )
        {
            for ( f = 0; f < 8; f = nextf )
            {
                nextf = (f || (pci_conf_read16(0, b, d, f, PCI_HEADER_TYPE) &
                               0x80)) ? f + 1 : 8;

                switch ( pci_conf_read16(0, b, d, f, PCI_CLASS_DEVICE) )
                {
                case 0x0700: /* single port serial */
                case 0x0702: /* multi port serial */
                case 0x0780: /* other (e.g serial+parallel) */
                    break;
                case 0xffff:
                    if ( !f )
                        nextf = 8;
                    /* fall through */
                default:
                    continue;
                }

                bar = pci_conf_read32(0, b, d, f,
                                      PCI_BASE_ADDRESS_0 + bar_idx*4);

                /* Not IO */
                if ( !(bar & PCI_BASE_ADDRESS_SPACE_IO) )
                    continue;

                pci_conf_write32(0, b, d, f, PCI_BASE_ADDRESS_0, ~0u);
                len = pci_conf_read32(0, b, d, f, PCI_BASE_ADDRESS_0);
                pci_conf_write32(0, b, d, f, PCI_BASE_ADDRESS_0 + bar_idx*4, bar);

                /* Not 8 bytes */
                if ( (len & 0xffff) != 0xfff9 )
                    continue;

                uart->ps_bdf[0] = b;
                uart->ps_bdf[1] = d;
                uart->ps_bdf[2] = f;
                uart->bar = bar;
                uart->bar_idx = bar_idx;
                uart->io_base = bar & ~PCI_BASE_ADDRESS_SPACE_IO;
                uart->irq = pci_conf_read8(0, b, d, f, PCI_INTERRUPT_PIN) ?
                    pci_conf_read8(0, b, d, f, PCI_INTERRUPT_LINE) : 0;

                return 0;
            }
        }
    }

    if ( !skip_amt )
        return -1;

    uart->io_base = 0x3f8;
    uart->irq = 0;
    uart->clock_hz  = UART_CLOCK_HZ;

    return 0;
}

#define PARSE_ERR(_f, _a...)                 \
    do {                                     \
        printk( "ERROR: " _f "\n" , ## _a ); \
        return;                              \
    } while ( 0 )

static void __init ns16550_parse_port_config(
    struct ns16550 *uart, const char *conf)
{
    int baud;

    /* No user-specified configuration? */
    if ( (conf == NULL) || (*conf == '\0') )
    {
        /* Some platforms may automatically probe the UART configuartion. */
        if ( uart->baud != 0 )
            goto config_parsed;
        return;
    }

    if ( strncmp(conf, "auto", 4) == 0 )
    {
        uart->baud = BAUD_AUTO;
        conf += 4;
    }
    else if ( (baud = simple_strtoul(conf, &conf, 10)) != 0 )
        uart->baud = baud;

    if ( *conf == '/' )
    {
        conf++;
        uart->clock_hz = simple_strtoul(conf, &conf, 0) << 4;
    }

    if ( *conf == ',' && *++conf != ',' )
    {
        uart->data_bits = simple_strtoul(conf, &conf, 10);

        uart->parity = parse_parity_char(*conf);

        uart->stop_bits = simple_strtoul(conf + 1, &conf, 10);
    }

    if ( *conf == ',' && *++conf != ',' )
    {
        if ( strncmp(conf, "pci", 3) == 0 )
        {
            if ( pci_uart_config(uart, 1/* skip AMT */, uart - ns16550_com) )
                return;
            conf += 3;
        }
        else if ( strncmp(conf, "amt", 3) == 0 )
        {
            if ( pci_uart_config(uart, 0, uart - ns16550_com) )
                return;
            conf += 3;
        }
        else
        {
            uart->io_base = simple_strtoul(conf, &conf, 0);
        }
    }

    if ( *conf == ',' && *++conf != ',' )
        uart->irq = simple_strtol(conf, &conf, 10);

    if ( *conf == ',' && *++conf != ',' )
    {
        uart->ps_bdf_enable = 1;
        parse_pci_bdf(&conf, &uart->ps_bdf[0]);
    }

    if ( *conf == ',' && *++conf != ',' )
    {
        uart->pb_bdf_enable = 1;
        parse_pci_bdf(&conf, &uart->pb_bdf[0]);
    }

 config_parsed:
    /* Sanity checks. */
    if ( (uart->baud != BAUD_AUTO) &&
         ((uart->baud < 1200) || (uart->baud > 115200)) )
        PARSE_ERR("Baud rate %d outside supported range.", uart->baud);
    if ( (uart->data_bits < 5) || (uart->data_bits > 8) )
        PARSE_ERR("%d data bits are unsupported.", uart->data_bits);
    if ( (uart->stop_bits < 1) || (uart->stop_bits > 2) )
        PARSE_ERR("%d stop bits are unsupported.", uart->stop_bits);
    if ( uart->io_base == 0 )
        PARSE_ERR("I/O base address must be specified.");
    if ( !check_existence(uart) )
        PARSE_ERR("16550-compatible serial UART not present");

    /* Register with generic serial driver. */
    serial_register_uart(uart - ns16550_com, &ns16550_driver, uart);
}

void __init ns16550_init(int index, struct ns16550_defaults *defaults)
{
    struct ns16550 *uart;

    if ( (index < 0) || (index > 1) )
        return;

    uart = &ns16550_com[index];

    uart->baud      = (defaults->baud ? :
                       console_has((index == 0) ? "com1" : "com2")
                       ? BAUD_AUTO : 0);
    uart->clock_hz  = UART_CLOCK_HZ;
    uart->data_bits = defaults->data_bits;
    uart->parity    = parse_parity_char(defaults->parity);
    uart->stop_bits = defaults->stop_bits;
    uart->irq       = defaults->irq;
    uart->io_base   = defaults->io_base;
    /* Default is no transmit FIFO. */
    uart->fifo_size = 1;

    ns16550_parse_port_config(uart, (index == 0) ? opt_com1 : opt_com2);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
