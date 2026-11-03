/*
 * soc.c — see soc.h for the memory map and the CONFIRMED/GROUNDED/
 * PLACEHOLDER fidelity notes. Read those before trusting an address.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soc.h"

/* ------------------------------------------------------------------ */
/* Little-endian byte-array helpers (mirrors test_bus.c's style)       */
/* ------------------------------------------------------------------ */

static uint32_t mem_read32(const uint8_t *base, uint32_t size, uint32_t off)
{
    if (off + 3 >= size) return 0;
    return base[off] | (base[off+1]<<8) | (base[off+2]<<16) | ((uint32_t)base[off+3]<<24);
}
static uint16_t mem_read16(const uint8_t *base, uint32_t size, uint32_t off)
{
    if (off + 1 >= size) return 0;
    return (uint16_t)(base[off] | (base[off+1]<<8));
}
static uint8_t mem_read8(const uint8_t *base, uint32_t size, uint32_t off)
{
    return (off < size) ? base[off] : 0;
}
static void mem_write32(uint8_t *base, uint32_t size, uint32_t off, uint32_t val)
{
    if (off + 3 >= size) return;
    base[off]=val; base[off+1]=val>>8; base[off+2]=val>>16; base[off+3]=val>>24;
}
static void mem_write16(uint8_t *base, uint32_t size, uint32_t off, uint16_t val)
{
    if (off + 1 >= size) return;
    base[off]=(uint8_t)val; base[off+1]=(uint8_t)(val>>8);
}
static void mem_write8(uint8_t *base, uint32_t size, uint32_t off, uint8_t val)
{
    if (off < size) base[off] = val;
}

/* ------------------------------------------------------------------ */
/* VIC (PL192-style) — see soc.h, GROUNDED layout                      */
/* ------------------------------------------------------------------ */

static uint32_t vic_irqstatus(soc_vic_t *v) { return (v->raw_intr | v->soft_int) & v->int_enable & ~v->int_select; }
static uint32_t vic_fiqstatus(soc_vic_t *v) { return (v->raw_intr | v->soft_int) & v->int_enable & v->int_select; }

static uint32_t vic_read(soc_vic_t *v, uint32_t off)
{
    if (off == 0x000) return vic_irqstatus(v);
    if (off == 0x004) return vic_fiqstatus(v);
    if (off == 0x008) return v->raw_intr | v->soft_int;
    if (off == 0x00C) return v->int_select;
    if (off == 0x010) return v->int_enable;
    if (off == 0x018) return v->soft_int;
    if (off == 0x020) return v->protection;
    if (off == 0x024) return v->sw_priority_mask;
    if (off == 0x030) { /* VECTADDR: simplified — lowest-numbered pending IRQ wins, no real priority stack */
        uint32_t pending = vic_irqstatus(v);
        if (pending == 0) return v->def_vect_addr;
        for (int i = 0; i < SOC_VIC_NUM_IRQS; i++) if (pending & (1u << i)) return v->vect_addr[i];
        return v->def_vect_addr;
    }
    if (off == 0x034) return v->def_vect_addr;
    if (off >= 0x100 && off < 0x100 + 4 * SOC_VIC_NUM_IRQS) return v->vect_addr[(off - 0x100) / 4];
    return 0;
}

static void vic_write(soc_vic_t *v, uint32_t off, uint32_t val)
{
    if (off == 0x00C) { v->int_select = val; return; }
    if (off == 0x010) { v->int_enable |= val; return; }       /* INTENABLE: write-1-to-set   */
    if (off == 0x014) { v->int_enable &= ~val; return; }      /* INTENCLEAR: write-1-to-clear */
    if (off == 0x018) { v->soft_int |= val; return; }
    if (off == 0x01C) { v->soft_int &= ~val; return; }
    if (off == 0x020) { v->protection = val; return; }
    if (off == 0x024) { v->sw_priority_mask = val; return; }
    if (off == 0x030) { return; }                             /* VECTADDR write = EOI, no state to update in this simplified model */
    if (off == 0x034) { v->def_vect_addr = val; return; }
    if (off >= 0x100 && off < 0x100 + 4 * SOC_VIC_NUM_IRQS) { v->vect_addr[(off - 0x100) / 4] = val; return; }
    /* VICVECTPRIORITYn and anything else: accepted, no effect (no real priority arbitration modeled) */
}

/* ------------------------------------------------------------------ */
/* UART (PL011-style) — see soc.h, GROUNDED layout                     */
/* ------------------------------------------------------------------ */

#define UART_CR_UARTEN (1u << 0)
#define UART_CR_TXE    (1u << 8)
#define UART_CR_RXE    (1u << 9)
#define UART_IMSC_RXIM (1u << 4)
#define UART_FR_BUSY   (1u << 3)
#define UART_FR_RXFE   (1u << 4)
#define UART_FR_TXFF   (1u << 5)
#define UART_FR_RXFF   (1u << 6)
#define UART_FR_TXFE   (1u << 7)

static void uart_capture_append(soc_uart_t *u, char c)
{
    if (u->tx_capture_len + 1 >= u->tx_capture_cap) {
        uint32_t new_cap = u->tx_capture_cap ? u->tx_capture_cap * 2 : 256;
        char *grown = realloc(u->tx_capture, new_cap);
        if (!grown) return; /* drop the byte rather than crash on OOM */
        u->tx_capture = grown;
        u->tx_capture_cap = new_cap;
    }
    u->tx_capture[u->tx_capture_len++] = c;
    u->tx_capture[u->tx_capture_len] = '\0';
}

static uint32_t uart_read(soc_uart_t *u, uint32_t off)
{
    if (off == 0x00) { /* DR */
        if (u->rx_count == 0) return 0;
        uint8_t b = u->rx_fifo[u->rx_head];
        u->rx_head = (u->rx_head + 1) % SOC_UART_RX_FIFO_SIZE;
        u->rx_count--;
        return b;
    }
    if (off == 0x18) { /* FR */
        uint32_t fr = UART_FR_TXFE; /* TX is always immediately "done" — no real timing modeled */
        if (u->rx_count == 0) fr |= UART_FR_RXFE;
        if (u->rx_count == SOC_UART_RX_FIFO_SIZE) fr |= UART_FR_RXFF;
        return fr;
    }
    if (off == 0x24) return u->ibrd;
    if (off == 0x28) return u->fbrd;
    if (off == 0x2C) return u->lcr_h;
    if (off == 0x30) return u->cr;
    if (off == 0x38) return u->imsc;
    if (off == 0x3C) return (u->rx_count > 0) ? UART_IMSC_RXIM : 0; /* RIS */
    if (off == 0x40) return ((u->rx_count > 0) ? UART_IMSC_RXIM : 0) & u->imsc; /* MIS */
    return 0;
}

static void uart_write(soc_uart_t *u, uint32_t off, uint32_t val)
{
    if (off == 0x00) { /* DR */
        if ((u->cr & UART_CR_UARTEN) && (u->cr & UART_CR_TXE)) {
            uart_capture_append(u, (char)val);
            if (u->tx_echo) { putchar((char)val); fflush(stdout); }
        }
        return;
    }
    if (off == 0x24) { u->ibrd = val; return; }
    if (off == 0x28) { u->fbrd = val; return; }
    if (off == 0x2C) { u->lcr_h = val; return; }
    if (off == 0x30) { u->cr = val; return; }
    if (off == 0x38) { u->imsc = val; return; }
    if (off == 0x44) { return; } /* ICR: RX status is level-derived from the FIFO, nothing to latch-clear */
}

void soc_uart_rx_push(soc_t *soc, uint8_t byte)
{
    soc_uart_t *u = &soc->uart0;
    if (u->rx_count >= SOC_UART_RX_FIFO_SIZE) return; /* drop on overflow, like a real FIFO would */
    u->rx_fifo[u->rx_tail] = byte;
    u->rx_tail = (u->rx_tail + 1) % SOC_UART_RX_FIFO_SIZE;
    u->rx_count++;
}

/* ------------------------------------------------------------------ */
/* Timer (SP804-style, single channel) — see soc.h, GROUNDED layout    */
/* ------------------------------------------------------------------ */

#define TIMER_CTRL_ENABLE   (1u << 7)
#define TIMER_CTRL_PERIODIC (1u << 6)
#define TIMER_CTRL_IRQEN    (1u << 5)

static void timer_tick(soc_timer_t *t)
{
    if (!(t->control & TIMER_CTRL_ENABLE)) return;
    if (t->value == 0) {
        if (t->control & TIMER_CTRL_IRQEN) t->interrupt_pending = true;
        if (t->control & TIMER_CTRL_PERIODIC) t->value = t->bg_load;
        /* else: free-running one-shot stays at 0 until reloaded */
        return;
    }
    t->value--;
}

static uint32_t timer_read(soc_timer_t *t, uint32_t off)
{
    if (off == 0x00) return t->load;
    if (off == 0x04) return t->value;
    if (off == 0x08) return t->control;
    if (off == 0x10) return t->interrupt_pending ? 1u : 0u;                                  /* RIS */
    if (off == 0x14) return (t->interrupt_pending && (t->control & TIMER_CTRL_IRQEN)) ? 1u : 0u; /* MIS */
    if (off == 0x18) return t->bg_load;
    return 0;
}

static void timer_write(soc_timer_t *t, uint32_t off, uint32_t val)
{
    if (off == 0x00) { t->load = val; t->value = val; return; }
    if (off == 0x08) { t->control = val; return; }
    if (off == 0x0C) { t->interrupt_pending = false; return; } /* INTCLR */
    if (off == 0x18) { t->bg_load = val; return; }
}

/* ------------------------------------------------------------------ */
/* soc_tick: advance peripheral time, then re-derive the CPU's IRQ/FIQ */
/* lines from the VIC. Called once per arm1176_step() by the caller.   */
/*                                                                      */
/* TIMING MODEL: this charges exactly one timer tick per CPU            */
/* instruction, which is not remotely cycle-accurate (real hardware's   */
/* timer runs off a fixed clock independent of instruction timing) but  */
/* is simple, deterministic, and enough to make timer interrupts and    */
/* boot-time delay loops actually terminate in a test/bring-up setting. */
/* ------------------------------------------------------------------ */

void soc_tick(soc_t *soc)
{
    timer_tick(&soc->timer);
    soc->vic0.raw_intr = (soc->timer.interrupt_pending && (soc->timer.control & TIMER_CTRL_IRQEN))
                       ? (soc->vic0.raw_intr | (1u << SOC_IRQ_TIMER))
                       : (soc->vic0.raw_intr & ~(1u << SOC_IRQ_TIMER));

    bool uart_irq = (soc->uart0.rx_count > 0) && (soc->uart0.imsc & UART_IMSC_RXIM);
    soc->vic0.raw_intr = uart_irq ? (soc->vic0.raw_intr | (1u << SOC_IRQ_UART0))
                                  : (soc->vic0.raw_intr & ~(1u << SOC_IRQ_UART0));

    arm1176_assert_irq(soc->cpu, vic_irqstatus(&soc->vic0) != 0);
    arm1176_assert_fiq(soc->cpu, vic_fiqstatus(&soc->vic0) != 0);
}

/* ------------------------------------------------------------------ */
/* Clock/power and GPIO: intentionally trivial stubs.                   */
/*                                                                      */
/* Real clock/PLL/power-management registers are entirely SoC-specific  */
/* and undocumented for the S5L8900. Modeling them precisely is out of  */
/* scope for now (see soc.h); what actually matters in practice is that */
/* firmware's inevitable "spin until the PLL reports locked" polling    */
/* loops terminate instead of hanging forever. So: any clock register   */
/* that has never been written reads back as 0xFFFFFFFF (all status     */
/* bits appear set, regardless of which ones real firmware is actually  */
/* checking); once software writes a register, further reads return    */
/* exactly what was written, like an ordinary control register.         */
/* GPIO is a plain read/write scratch array with no pin modeling.       */
/* ------------------------------------------------------------------ */

static uint32_t stub_read(uint32_t *regs, uint32_t count, uint32_t off, bool default_all_ones)
{
    uint32_t idx = off / 4;
    if (idx >= count) return 0;
    if (regs[idx] == 0 && default_all_ones) return 0xFFFFFFFFu;
    return regs[idx];
}
static void stub_write(uint32_t *regs, uint32_t count, uint32_t off, uint32_t val)
{
    uint32_t idx = off / 4;
    if (idx < count) regs[idx] = val;
}

/* ------------------------------------------------------------------ */
/* Top-level bus dispatch                                               */
/* ------------------------------------------------------------------ */

static uint32_t soc_bus_read32(void *ctx, uint32_t addr)
{
    soc_t *s = ctx;
    if (addr >= SOC_SECUREROM_BASE && addr < SOC_SECUREROM_BASE + SOC_SECUREROM_SIZE)
        return mem_read32(s->securerom, SOC_SECUREROM_SIZE, addr - SOC_SECUREROM_BASE);
    if (addr >= SOC_SRAM_BASE && addr < SOC_SRAM_BASE + SOC_SRAM_SIZE)
        return mem_read32(s->sram, SOC_SRAM_SIZE, addr - SOC_SRAM_BASE);
    if (addr >= SOC_SDRAM_BASE && addr < SOC_SDRAM_BASE + SOC_SDRAM_SIZE)
        return mem_read32(s->sdram, SOC_SDRAM_SIZE, addr - SOC_SDRAM_BASE);
    if (addr >= SOC_NOR_BASE && addr < SOC_NOR_BASE + SOC_NOR_SIZE)
        return mem_read32(s->nor, SOC_NOR_SIZE, addr - SOC_NOR_BASE);
    if (addr >= SOC_UART0_BASE && addr < SOC_UART0_BASE + 0x1000)
        return uart_read(&s->uart0, addr - SOC_UART0_BASE);
    if (addr >= SOC_VIC0_BASE && addr < SOC_VIC0_BASE + 0x1000)
        return vic_read(&s->vic0, addr - SOC_VIC0_BASE);
    if (addr >= SOC_TIMER_BASE && addr < SOC_TIMER_BASE + 0x1000)
        return timer_read(&s->timer, addr - SOC_TIMER_BASE);
    if (addr >= SOC_CLOCK_BASE && addr < SOC_CLOCK_BASE + 0x1000)
        return stub_read(s->clock_regs, SOC_PERIPH_SIZE / 4, addr - SOC_CLOCK_BASE, true);
    if (addr >= SOC_GPIO_BASE && addr < SOC_GPIO_BASE + 0x1000)
        return stub_read(s->gpio_regs, SOC_PERIPH_SIZE / 4, addr - SOC_GPIO_BASE, false);
    return 0;
}

static void soc_bus_write32(void *ctx, uint32_t addr, uint32_t val)
{
    soc_t *s = ctx;
    if (addr >= SOC_SECUREROM_BASE && addr < SOC_SECUREROM_BASE + SOC_SECUREROM_SIZE)
        return; /* ROM: writes ignored */
    if (addr >= SOC_SRAM_BASE && addr < SOC_SRAM_BASE + SOC_SRAM_SIZE)
        { mem_write32(s->sram, SOC_SRAM_SIZE, addr - SOC_SRAM_BASE, val); return; }
    if (addr >= SOC_SDRAM_BASE && addr < SOC_SDRAM_BASE + SOC_SDRAM_SIZE)
        { mem_write32(s->sdram, SOC_SDRAM_SIZE, addr - SOC_SDRAM_BASE, val); return; }
    if (addr >= SOC_NOR_BASE && addr < SOC_NOR_BASE + SOC_NOR_SIZE)
        return; /* TODO: real NOR flash write/erase state machine; read-only for now */
    if (addr >= SOC_UART0_BASE && addr < SOC_UART0_BASE + 0x1000)
        { uart_write(&s->uart0, addr - SOC_UART0_BASE, val); return; }
    if (addr >= SOC_VIC0_BASE && addr < SOC_VIC0_BASE + 0x1000)
        { vic_write(&s->vic0, addr - SOC_VIC0_BASE, val); return; }
    if (addr >= SOC_TIMER_BASE && addr < SOC_TIMER_BASE + 0x1000)
        { timer_write(&s->timer, addr - SOC_TIMER_BASE, val); return; }
    if (addr >= SOC_CLOCK_BASE && addr < SOC_CLOCK_BASE + 0x1000)
        { stub_write(s->clock_regs, SOC_PERIPH_SIZE / 4, addr - SOC_CLOCK_BASE, val); return; }
    if (addr >= SOC_GPIO_BASE && addr < SOC_GPIO_BASE + 0x1000)
        { stub_write(s->gpio_regs, SOC_PERIPH_SIZE / 4, addr - SOC_GPIO_BASE, val); return; }
}

/* 16/8-bit accesses: memory regions get real narrow accessors; every
 * peripheral register in this file is modeled as 32-bit-only (true to
 * real PL011/PL192/SP804 hardware), so narrow accesses to those are
 * serviced by widening through the 32-bit path — a reasonable
 * approximation for register windows that real firmware always
 * accesses with LDR/STR (word) anyway. */

static uint16_t soc_bus_read16(void *ctx, uint32_t addr)
{
    soc_t *s = ctx;
    if (addr >= SOC_SECUREROM_BASE && addr < SOC_SECUREROM_BASE + SOC_SECUREROM_SIZE)
        return mem_read16(s->securerom, SOC_SECUREROM_SIZE, addr - SOC_SECUREROM_BASE);
    if (addr >= SOC_SRAM_BASE && addr < SOC_SRAM_BASE + SOC_SRAM_SIZE)
        return mem_read16(s->sram, SOC_SRAM_SIZE, addr - SOC_SRAM_BASE);
    if (addr >= SOC_SDRAM_BASE && addr < SOC_SDRAM_BASE + SOC_SDRAM_SIZE)
        return mem_read16(s->sdram, SOC_SDRAM_SIZE, addr - SOC_SDRAM_BASE);
    if (addr >= SOC_NOR_BASE && addr < SOC_NOR_BASE + SOC_NOR_SIZE)
        return mem_read16(s->nor, SOC_NOR_SIZE, addr - SOC_NOR_BASE);
    return (uint16_t)soc_bus_read32(ctx, addr & ~3u);
}
static uint8_t soc_bus_read8(void *ctx, uint32_t addr)
{
    soc_t *s = ctx;
    if (addr >= SOC_SECUREROM_BASE && addr < SOC_SECUREROM_BASE + SOC_SECUREROM_SIZE)
        return mem_read8(s->securerom, SOC_SECUREROM_SIZE, addr - SOC_SECUREROM_BASE);
    if (addr >= SOC_SRAM_BASE && addr < SOC_SRAM_BASE + SOC_SRAM_SIZE)
        return mem_read8(s->sram, SOC_SRAM_SIZE, addr - SOC_SRAM_BASE);
    if (addr >= SOC_SDRAM_BASE && addr < SOC_SDRAM_BASE + SOC_SDRAM_SIZE)
        return mem_read8(s->sdram, SOC_SDRAM_SIZE, addr - SOC_SDRAM_BASE);
    if (addr >= SOC_NOR_BASE && addr < SOC_NOR_BASE + SOC_NOR_SIZE)
        return mem_read8(s->nor, SOC_NOR_SIZE, addr - SOC_NOR_BASE);
    return (uint8_t)soc_bus_read32(ctx, addr & ~3u);
}
static void soc_bus_write16(void *ctx, uint32_t addr, uint16_t val)
{
    soc_t *s = ctx;
    if (addr >= SOC_SRAM_BASE && addr < SOC_SRAM_BASE + SOC_SRAM_SIZE)
        { mem_write16(s->sram, SOC_SRAM_SIZE, addr - SOC_SRAM_BASE, val); return; }
    if (addr >= SOC_SDRAM_BASE && addr < SOC_SDRAM_BASE + SOC_SDRAM_SIZE)
        { mem_write16(s->sdram, SOC_SDRAM_SIZE, addr - SOC_SDRAM_BASE, val); return; }
    if (addr >= SOC_SECUREROM_BASE && addr < SOC_SECUREROM_BASE + SOC_SECUREROM_SIZE) return;
    if (addr >= SOC_NOR_BASE && addr < SOC_NOR_BASE + SOC_NOR_SIZE) return;
    soc_bus_write32(ctx, addr & ~3u, val);
}
static void soc_bus_write8(void *ctx, uint32_t addr, uint8_t val)
{
    soc_t *s = ctx;
    if (addr >= SOC_SRAM_BASE && addr < SOC_SRAM_BASE + SOC_SRAM_SIZE)
        { mem_write8(s->sram, SOC_SRAM_SIZE, addr - SOC_SRAM_BASE, val); return; }
    if (addr >= SOC_SDRAM_BASE && addr < SOC_SDRAM_BASE + SOC_SDRAM_SIZE)
        { mem_write8(s->sdram, SOC_SDRAM_SIZE, addr - SOC_SDRAM_BASE, val); return; }
    if (addr >= SOC_SECUREROM_BASE && addr < SOC_SECUREROM_BASE + SOC_SECUREROM_SIZE) return;
    if (addr >= SOC_NOR_BASE && addr < SOC_NOR_BASE + SOC_NOR_SIZE) return;
    soc_bus_write32(ctx, addr & ~3u, val);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */

void soc_init(soc_t *soc, arm1176_t *cpu)
{
    memset(soc, 0, sizeof(*soc));
    soc->cpu = cpu;
    soc->securerom = calloc(1, SOC_SECUREROM_SIZE);
    soc->sram      = calloc(1, SOC_SRAM_SIZE);
    soc->sdram     = calloc(1, SOC_SDRAM_SIZE);
    soc->nor       = calloc(1, SOC_NOR_SIZE);

    arm_bus_t bus;
    bus.read8 = soc_bus_read8;   bus.read16 = soc_bus_read16;   bus.read32 = soc_bus_read32;
    bus.write8 = soc_bus_write8; bus.write16 = soc_bus_write16; bus.write32 = soc_bus_write32;
    bus.ctx = soc;
    arm1176_set_bus(cpu, bus);

    /* Real S5L8900 reset execution begins at the boot ROM, not the
     * generic ARM convention of 0x00000000 — see the CONFIRMED note
     * in soc.h. Call arm1176_reset(cpu) before this function so the
     * rest of the CPU state (mode, banked registers, CP15/VFP) is at
     * its normal reset value; this just corrects the entry point. */
    cpu->r[15] = SOC_SECUREROM_BASE;
}

void soc_destroy(soc_t *soc)
{
    free(soc->securerom);
    free(soc->sram);
    free(soc->sdram);
    free(soc->nor);
    free(soc->uart0.tx_capture);
    memset(soc, 0, sizeof(*soc));
}

bool soc_load_file(soc_t *soc, uint8_t *region, uint32_t region_size, const char *path)
{
    (void)soc;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(region, 1, region_size, f);
    fclose(f);
    return n > 0;
}
