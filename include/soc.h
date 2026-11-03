/*
 * soc.h — S5L8900 SoC/board layer: physical memory map and the
 * peripherals needed to actually boot something (UART console,
 * interrupt controller, a timer, and clock/power stubs).
 *
 * This sits *outside* the CPU core (unlike cp15.h/vfp.h/jazelle.h,
 * which are coprocessors physically part of the ARM1176 die) — it's
 * the board the core is plugged into, connected purely through the
 * arm_bus_t function-pointer interface from arm1176.h. Nothing in
 * here reaches back into CPU internals except to raise/lower the
 * IRQ line via arm1176_assert_irq().
 *
 * ============================================================
 * FIDELITY / SCOPE — read this before trusting any address below
 * ============================================================
 *
 * The S5L8900 (Samsung, used in the original iPhone, iPhone 3G, and
 * 1st-gen iPod touch) has no public datasheet. Everything known about
 * it comes from community reverse engineering of iBoot/the bootrom
 * (mostly the iPhone Dev Team / iDroid Project's openiBoot, which
 * genuinely ran on real iPhone 2G hardware). This file distinguishes
 * three confidence levels; every #define below is commented with
 * which one it is:
 *
 *   CONFIRMED   — sourced from a specific community reference for this
 *                 exact chip. Currently: the SecureROM/VROM base
 *                 address (0x20000000, per TheAppleWiki's "VROM
 *                 (S5L8900)" article) and the SDRAM capacity (128MiB,
 *                 per TheAppleWiki's "S5L8900" article). The general
 *                 boot flow — bootrom validates and runs the LLB from
 *                 NOR, LLB runs from SRAM, iBoot runs from DRAM — is
 *                 also confirmed from community sources.
 *   GROUNDED     — not S5L8900-specific, but a real, standard, publicly
 *                  documented ARM PrimeCell peripheral that openiBoot's
 *                  own interrupt.c confirms this SoC family actually
 *                  uses (its code literally names VICINTENABLE,
 *                  VICINTENCLEAR, VICINTSELECT, VICSWPRIORITYMASK,
 *                  VICVECTADDRS — stock PL192 VIC register names). The
 *                  UART (PL011-style) and timer (SP804-style) register
 *                  *layouts* here are similarly standard, well-
 *                  documented ARM PrimeCells; whether the S5L8900 uses
 *                  exactly these two is a reasonable but unconfirmed
 *                  bet, not a verified fact.
 *   PLACEHOLDER  — a base address or size picked to give every region
 *                  a sensible, non-overlapping home so the emulator
 *                  works end-to-end, but not sourced from anywhere.
 *                  These are the ones to replace first if real
 *                  hardware register dumps or openiBoot's actual
 *                  plat-s5l8900 hardware headers become available.
 *
 * Since we currently only run hand-assembled test firmware (which by
 * construction agrees with whatever addresses this file defines),
 * none of this affects correctness *today*. It matters the moment
 * real Apple-signed firmware images are the test target, since those
 * hardcode the real addresses.
 */

#ifndef SOC_H
#define SOC_H

#include <stdint.h>
#include <stdbool.h>
#include "arm1176.h"

/* ------------------------------------------------------------------ */
/* Physical memory map                                                  */
/* ------------------------------------------------------------------ */

#define SOC_SECUREROM_BASE   0x20000000u   /* CONFIRMED (TheAppleWiki: VROM (S5L8900)) */
#define SOC_SECUREROM_SIZE   0x00040000u   /* PLACEHOLDER: 256KB, real size unconfirmed */

#define SOC_SRAM_BASE        0x22000000u   /* PLACEHOLDER */
#define SOC_SRAM_SIZE        0x00020000u   /* PLACEHOLDER: 128KB */

#define SOC_SDRAM_BASE       0x08000000u   /* PLACEHOLDER base address... */
#define SOC_SDRAM_SIZE       0x08000000u   /* ...but CONFIRMED size: 128MiB (TheAppleWiki: S5L8900) */

#define SOC_NOR_BASE         0x1C000000u   /* PLACEHOLDER */
#define SOC_NOR_SIZE         0x00100000u   /* PLACEHOLDER: 1MB */

#define SOC_PERIPH_BASE      0x38000000u   /* PLACEHOLDER region for all peripherals below */
#define SOC_UART0_BASE       (SOC_PERIPH_BASE + 0x0000u)  /* GROUNDED layout (PL011-style), PLACEHOLDER address */
#define SOC_VIC0_BASE        (SOC_PERIPH_BASE + 0x1000u)  /* GROUNDED layout (PL192, confirmed used by this SoC family), PLACEHOLDER address */
#define SOC_TIMER_BASE       (SOC_PERIPH_BASE + 0x2000u)  /* GROUNDED layout (SP804-style), PLACEHOLDER address */
#define SOC_CLOCK_BASE       (SOC_PERIPH_BASE + 0x3000u)  /* Stub only, see below */
#define SOC_GPIO_BASE        (SOC_PERIPH_BASE + 0x4000u)  /* Stub only, see below */
#define SOC_PERIPH_SIZE      0x00005000u

/* IRQ line numbers on VIC0 used by the peripherals this file models.
 * PLACEHOLDER: real S5L8900 IRQ assignments are unconfirmed. */
#define SOC_IRQ_UART0        1
#define SOC_IRQ_TIMER        2

/* ------------------------------------------------------------------ */
/* Peripheral state                                                     */
/* ------------------------------------------------------------------ */

#define SOC_VIC_NUM_IRQS 32

typedef struct {
    uint32_t raw_intr;      /* one bit per IRQ line, level-sensitive request state */
    uint32_t int_enable;    /* enabled mask */
    uint32_t int_select;    /* 0 = routed to IRQ, 1 = routed to FIQ */
    uint32_t soft_int;      /* software-forced bits (ORed into raw_intr) */
    uint32_t vect_addr[SOC_VIC_NUM_IRQS]; /* per-IRQ vector address table */
    uint32_t def_vect_addr;
    uint32_t sw_priority_mask;
    uint32_t protection;
} soc_vic_t;

#define SOC_UART_RX_FIFO_SIZE 64

typedef struct {
    uint8_t  rx_fifo[SOC_UART_RX_FIFO_SIZE];
    uint32_t rx_head, rx_tail, rx_count;
    uint32_t ibrd, fbrd, lcr_h, cr, imsc;

    /* Transmitted bytes are both appended here (a growable capture
     * buffer, mainly for tests) and, if tx_echo is set, written
     * straight to stdout so a running firmware image's console output
     * is visible live. */
    char    *tx_capture;
    uint32_t tx_capture_len, tx_capture_cap;
    bool     tx_echo;
} soc_uart_t;

typedef struct {
    uint32_t load, bg_load;
    uint32_t value;
    uint32_t control;   /* bit7=enable, bit6=periodic, bit5=irq-enable, bit1=32-bit */
    bool     interrupt_pending;
} soc_timer_t;

typedef struct {
    arm1176_t   *cpu;   /* for raising/lowering IRQ */

    uint8_t     *securerom;   /* SOC_SECUREROM_SIZE bytes, read-only to the CPU */
    uint8_t     *sram;        /* SOC_SRAM_SIZE bytes, read/write */
    uint8_t     *sdram;       /* SOC_SDRAM_SIZE bytes, read/write */
    uint8_t     *nor;         /* SOC_NOR_SIZE bytes, read-only to the CPU for now (see TODO in soc.c) */

    soc_vic_t    vic0;
    soc_uart_t   uart0;
    soc_timer_t  timer;

    uint32_t     clock_regs[SOC_PERIPH_SIZE / 4]; /* generic RAZ/WI-with-ready-bits stub, see soc.c */
    uint32_t     gpio_regs[SOC_PERIPH_SIZE / 4];  /* generic RW stub */
} soc_t;

/* Allocates and zeroes all backing memory, connects `cpu`'s bus to this
 * SoC, and sets cpu->r[15] to SOC_SECUREROM_BASE (matching where real
 * hardware's reset vector actually points on this chip — see the
 * CONFIRMED note above — rather than the generic ARM convention of
 * 0x00000000 that arm1176_reset() uses by itself). Call
 * arm1176_reset(cpu) *before* this, then this after. */
void soc_init(soc_t *soc, arm1176_t *cpu);
void soc_destroy(soc_t *soc);

/* Loads a raw binary file into SecureROM/NOR/SDRAM at offset 0 (or
 * returns false on error). Convenience for tests and bring-up. */
bool soc_load_file(soc_t *soc, uint8_t *region, uint32_t region_size, const char *path);

/* Advances peripheral time by one "tick" (currently: called once per
 * arm1176_step() by the caller — see the timing-model note in soc.c).
 * Ticks the timer and re-evaluates the VIC's IRQ line into the CPU. */
void soc_tick(soc_t *soc);

/* Feed a byte into UART0's RX FIFO as if it arrived over the wire
 * (for tests, or a future host-console-input bridge). */
void soc_uart_rx_push(soc_t *soc, uint8_t byte);

#endif /* SOC_H */
