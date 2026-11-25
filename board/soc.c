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
/* UART — Samsung-family layout, DERIVED (see soc.h)                   */
/*                                                                      */
/* No Apple-specific register table for the S5L8900 UART was found; the */
/* layout below is the generic Samsung-family one (offsets attested by  */
/* the S3C/Exynos documentation and drivers, and by FreeBSD's Apple     */
/* "S5L" UART driver using the same ULCON/UCON offsets). Deliberately   */
/* minimal: where the family's variants disagree (FIFO count/full bit   */
/* positions in UFSTAT), only the low 4 bits of the RX count — common   */
/* to every variant — are reported. The UINTP/UINTSP/UINTM trio is the  */
/* Exynos-style convention and is the least certain part.               */
/* ------------------------------------------------------------------ */

#define UART_ULCON    0x00
#define UART_UCON     0x04
#define UART_UFCON    0x08
#define UART_UMCON    0x0C
#define UART_UTRSTAT  0x10
#define UART_UERSTAT  0x14
#define UART_UFSTAT   0x18
#define UART_UMSTAT   0x1C
#define UART_UTXH     0x20
#define UART_URXH     0x24
#define UART_UBRDIV   0x28
#define UART_UINTP    0x30
#define UART_UINTSP   0x34
#define UART_UINTM    0x38

#define UART_UCON_TXMODE      (3u << 2)  /* nonzero = transmitter enabled */
#define UART_UTRSTAT_RXRDY    (1u << 0)
#define UART_UTRSTAT_TXEMPTY  (1u << 1)
#define UART_UTRSTAT_TXDONE   (1u << 2)
#define UART_INT_RXD          (1u << 0)

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
    switch (off) {
    case UART_ULCON:   return u->ulcon;
    case UART_UCON:    return u->ucon;
    case UART_UFCON:   return u->ufcon;
    case UART_UMCON:   return u->umcon;
    case UART_UTRSTAT: /* TX is always immediately "done" — no real timing modeled */
        return UART_UTRSTAT_TXEMPTY | UART_UTRSTAT_TXDONE
             | (u->rx_count ? UART_UTRSTAT_RXRDY : 0);
    case UART_UERSTAT: return 0;
    case UART_UFSTAT:  return u->rx_count > 15 ? 15 : u->rx_count;
    case UART_UMSTAT:  return 0;
    case UART_URXH: {
        if (u->rx_count == 0) return 0;
        uint8_t b = u->rx_fifo[u->rx_head];
        u->rx_head = (u->rx_head + 1) % SOC_UART_RX_FIFO_SIZE;
        u->rx_count--;
        return b;
    }
    case UART_UBRDIV:  return u->ubrdiv;
    case UART_UINTP:   return (u->rx_count ? UART_INT_RXD : 0) & ~u->uintm; /* pending and unmasked */
    case UART_UINTSP:  return (u->rx_count ? UART_INT_RXD : 0);             /* source pending, raw   */
    case UART_UINTM:   return u->uintm;
    default:           return 0;
    }
}

static void uart_write(soc_uart_t *u, uint32_t off, uint32_t val)
{
    switch (off) {
    case UART_ULCON:  u->ulcon = val; break;
    case UART_UCON:   u->ucon = val; break;
    case UART_UFCON:  u->ufcon = val; break;
    case UART_UMCON:  u->umcon = val; break;
    case UART_UBRDIV: u->ubrdiv = val; break;
    case UART_UINTM:  u->uintm = val; break;
    case UART_UTXH:
        if (u->ucon & UART_UCON_TXMODE) {
            uart_capture_append(u, (char)val);
            if (u->tx_echo) { putchar((char)val); fflush(stdout); }
        }
        break;
    default: break; /* read-only status registers and anything unmodeled: ignored */
    }
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
/* Timers — PWM-style layout, DERIVED from the sibling S5L8720's        */
/* documented timer block (see soc.h); the register offsets and bit     */
/* positions below are that page's, the *behavior* is a minimal model:  */
/*                                                                      */
/*  - 8 timers: 0-3 are 16-bit, 4-7 are 32-bit. Per-timer registers at  */
/*    +0x00 config, +0x04 control, +0x08 TCMPB, +0x0C TCNTB,            */
/*    +0x10 prescaler (10 bit), +0x14 TCNTO (observation, read-only).   */
/*  - control bit0 = start/stop, bit1 = manual update (load TCNTB),     */
/*    bit4 = reset counter (bits 1 and 4 are self-clearing here).        */
/*  - config bit4 = compare mode, bit5 = one-shot (else interval),      */
/*    bit24 = count down (timers 4-7 only).                             */
/*  - One counter step per soc_tick(); prescaler/divider/clock select   */
/*    are stored but have no effect (rates are README item B5).         */
/*  - Only timers 4-7 raise interrupts, via the documented IRQ status   */
/*    register at 0x118 (3 flag bits per timer; this model sets bit0).  */
/*    Whether timers 0-3 interrupt, and how, is undocumented.           */
/*  - Writing 0x118 clears the written flag bits (the page only says    */
/*    "write to clear interrupt request").                              */
/* ------------------------------------------------------------------ */

#define TIMER_IRQSTAT   0x118u
#define TIMER_REG_SPAN  0x18u
#define TCFG_COMPARE    (1u << 4)
#define TCFG_ONESHOT    (1u << 5)
#define TCFG_DOWN       (1u << 24)
#define TCTL_RUN        (1u << 0)
#define TCTL_MANUAL     (1u << 1)
#define TCTL_RESET      (1u << 4)

static const uint32_t timer_base_off[SOC_TIMER_COUNT] = {0x00, 0x20, 0x40, 0x60, 0xA0, 0xC0, 0xE0, 0x100};
static const int      timer_irq_shift[4] = {24, 16, 8, 0}; /* IRQ status bits for timers 4,5,6,7 */

static uint32_t timer_mask(int i) { return i >= 4 ? 0xFFFFFFFFu : 0xFFFFu; }

static int timer_find(uint32_t off, uint32_t *reg_off)
{
    for (int i = 0; i < SOC_TIMER_COUNT; i++) {
        if (off >= timer_base_off[i] && off < timer_base_off[i] + TIMER_REG_SPAN) {
            *reg_off = off - timer_base_off[i];
            return i;
        }
    }
    return -1;
}

static uint32_t timer_read(soc_timer_t *t, uint32_t off)
{
    if (off == TIMER_IRQSTAT) {
        uint32_t st = 0;
        for (int k = 0; k < 4; k++) st |= (uint32_t)(t->u[4 + k].flags & 7u) << timer_irq_shift[k];
        return st;
    }
    uint32_t r;
    int i = timer_find(off, &r);
    if (i < 0) return 0;
    soc_timer_unit_t *u = &t->u[i];
    switch (r) {
    case 0x00: return u->cfg;
    case 0x04: return u->run ? TCTL_RUN : 0;
    case 0x08: return u->tcmpb;
    case 0x0C: return u->tcntb;
    case 0x10: return u->presc;
    case 0x14: return u->tcnt;
    default:   return 0;
    }
}

static void timer_write(soc_timer_t *t, uint32_t off, uint32_t val)
{
    if (off == TIMER_IRQSTAT) {
        for (int k = 0; k < 4; k++) t->u[4 + k].flags &= (uint8_t)~((val >> timer_irq_shift[k]) & 7u);
        return;
    }
    uint32_t r;
    int i = timer_find(off, &r);
    if (i < 0) return;
    soc_timer_unit_t *u = &t->u[i];
    uint32_t m = timer_mask(i);
    switch (r) {
    case 0x00: u->cfg = val; break;
    case 0x04:
        if (val & TCTL_MANUAL) u->tcnt = u->tcntb & m;
        if (val & TCTL_RESET)  u->tcnt = 0;
        u->run = (val & TCTL_RUN) != 0;
        break;
    case 0x08: u->tcmpb = val & m; break;
    case 0x0C: u->tcntb = val & m; break;
    case 0x10: u->presc = val & 0x3FFu; break;
    default: break; /* TCNTO is read-only */
    }
}

static void timer_tick(soc_timer_t *t)
{
    for (int i = 0; i < SOC_TIMER_COUNT; i++) {
        soc_timer_unit_t *u = &t->u[i];
        if (!u->run) continue;
        uint32_t m = timer_mask(i);
        bool down = (i >= 4) && (u->cfg & TCFG_DOWN);
        bool match = false;
        if (!down) {
            u->tcnt = (u->tcnt + 1) & m;
            match = (u->cfg & TCFG_COMPARE) ? (u->tcnt == (u->tcmpb & m)) : (u->tcnt == 0);
        } else {
            u->tcnt = (u->tcnt - 1) & m;
            match = (u->tcnt == 0);
        }
        if (!match) continue;
        if (i >= 4) u->flags |= 1u;               /* "handler 1" flag; only timers 4-7 have a documented IRQ status */
        if (u->cfg & TCFG_ONESHOT) u->run = false;
        else u->tcnt = down ? (u->tcntb & m) : 0; /* interval mode restarts */
    }
}

static bool timer_irq_pending(const soc_timer_t *t)
{
    for (int i = 4; i < SOC_TIMER_COUNT; i++) if (t->u[i].flags) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/* soc_tick: advance peripheral time, then re-derive the CPU's IRQ/FIQ */
/* lines from the VIC. Called once per arm1176_step() by the caller.   */
/*                                                                      */
/* TIMING MODEL: this charges exactly one timer tick per CPU            */
/* instruction, which is not remotely cycle-accurate (real hardware's   */
/* timers run off fixed clocks independent of instruction timing) but   */
/* is simple, deterministic, and enough to make timer interrupts and    */
/* boot-time delay loops actually terminate in a test/bring-up setting. */
/* ------------------------------------------------------------------ */

void soc_tick(soc_t *soc)
{
    timer_tick(&soc->timer);
    soc->vic0.raw_intr = timer_irq_pending(&soc->timer)
                       ? (soc->vic0.raw_intr | (1u << SOC_IRQ_TIMER))
                       : (soc->vic0.raw_intr & ~(1u << SOC_IRQ_TIMER));

    bool uart_irq = (soc->uart0.rx_count > 0) && !(soc->uart0.uintm & UART_INT_RXD);
    soc->vic0.raw_intr = uart_irq ? (soc->vic0.raw_intr | (1u << SOC_IRQ_UART0))
                                  : (soc->vic0.raw_intr & ~(1u << SOC_IRQ_UART0));

    arm1176_assert_irq(soc->cpu, vic_irqstatus(&soc->vic0) != 0);
    arm1176_assert_fiq(soc->cpu, vic_fiqstatus(&soc->vic0) != 0);
}

/* ------------------------------------------------------------------ */
/* Clock/PLL controller (CONFIRMED — see soc.h) and the GPIO stub.      */
/*                                                                      */
/* Register offsets and reset values below are sourced from kleemajo's  */
/* iDroid-notes "clock" file (real disassembly of openiBoot/LLB's       */
/* clock_reset routine) — see soc.h's header comment. PLLLOCK is        */
/* modeled as read-only, always reporting "locked": real firmware polls */
/* it after writing a PLLxLCNT register and we have no real PLL         */
/* settling behavior to model, so the honest choice is to let that      */
/* polling loop terminate immediately rather than fabricate a fake      */
/* delay. Every other clock register is a plain read/write cell,        */
/* seeded with the real clock_reset() value where one is known (0       */
/* otherwise) — including the gate registers (GATES1/3/4/5), so code    */
/* that reads back its own gate-enable writes sees consistent state.    */
/*                                                                      */
/* GPIO remains a plain read/write scratch array with no pin modeling — */
/* see soc.h for why this is *not* also standing in for power           */
/* management anymore (that's off-chip I2C on real hardware).           */
/* ------------------------------------------------------------------ */

static void clock_reset_defaults(soc_clock_t *c)
{
    memset(c->regs, 0, sizeof(c->regs));
    /* Values below are exactly what openiBoot's clock_reset() (iBoot
     * variant) writes on real hardware, per the CONFIRMED source. */
    c->regs[SOC_CLOCK_CONFIG0  / 4] = 0x40000000u;
    c->regs[SOC_CLOCK_CONFIG1  / 4] = 0x00000000u;
    c->regs[SOC_CLOCK_CONFIG2  / 4] = 0x80008000u;
    c->regs[SOC_CLOCK_UNKNOWN1 / 4] = 0x80008000u;
    c->regs[SOC_CLOCK_UNKNOWN2 / 4] = 0x00008000u;
    c->regs[SOC_CLOCK_UNKNOWN4 / 4] = 0x00004400u;
    c->regs[SOC_CLOCK_GATES1   / 4] = 0x00035FE7u;
    c->regs[SOC_CLOCK_GATES3   / 4] = 0xFFABAFFFu;
    c->regs[SOC_CLOCK_GATES4   / 4] = 0x00004FFFu;
    c->regs[SOC_CLOCK_GATES5   / 4] = 0x00FDFFFFu;
    /* PLLLOCK is read-only and synthesized in clock_read(); its stored
     * value here is never actually consulted. */
}

static uint32_t clock_read(soc_clock_t *c, uint32_t off)
{
    if (off == SOC_CLOCK_PLLLOCK) return 0xFFFFFFFFu; /* always "locked" — see comment above */
    uint32_t idx = off / 4;
    return (idx < SOC_CLOCK_REGSPACE_WORDS) ? c->regs[idx] : 0;
}

static void clock_write(soc_clock_t *c, uint32_t off, uint32_t val)
{
    if (off == SOC_CLOCK_PLLLOCK) return; /* read-only status register */
    uint32_t idx = off / 4;
    if (idx < SOC_CLOCK_REGSPACE_WORDS) c->regs[idx] = val;
}

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
    if (addr >= SOC_CLOCK_BASE && addr < SOC_CLOCK_BASE + SOC_CLOCK_REGSPACE_WORDS * 4)
        return clock_read(&s->clock, addr - SOC_CLOCK_BASE);
    if (addr >= SOC_GPIO_BASE && addr < SOC_GPIO_BASE + 0x1000)
        return stub_read(s->gpio_regs, SOC_GPIO_SIZE / 4, addr - SOC_GPIO_BASE, false);
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
    if (addr >= SOC_CLOCK_BASE && addr < SOC_CLOCK_BASE + SOC_CLOCK_REGSPACE_WORDS * 4)
        { clock_write(&s->clock, addr - SOC_CLOCK_BASE, val); return; }
    if (addr >= SOC_GPIO_BASE && addr < SOC_GPIO_BASE + 0x1000)
        { stub_write(s->gpio_regs, SOC_GPIO_SIZE / 4, addr - SOC_GPIO_BASE, val); return; }
}

/* 16/8-bit accesses: memory regions get real narrow accessors; every
 * peripheral register in this file is modeled as 32-bit-only (true to
 * typical memory-mapped peripheral blocks of this era), so narrow
 * accesses to those are
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
    clock_reset_defaults(&soc->clock);

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
