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
 * four confidence levels; every #define below is commented with
 * which one it is:
 *
 *   CONFIRMED   — sourced from a specific community reference for this
 *                 exact chip. Currently: the SecureROM/VROM base address
 *                 (0x20000000, TheAppleWiki's "VROM (S5L8900)" article),
 *                 the SDRAM capacity (128MiB, TheAppleWiki's "S5L8900"
 *                 article), and the entire clock controller (base
 *                 address, register offsets, and real power-on-reset
 *                 values), sourced from kleemajo's iDroid-notes
 *                 repository (github.com/kleemajo/iDroid-notes, file
 *                 "clock"), which documents register-level behavior
 *                 reverse-engineered directly from openiBoot/LLB
 *                 disassembly and explicitly cross-references S5L8900-
 *                 specific values (e.g. the PLL0CON main-divider field
 *                 width) alongside the sibling S5L8720's, strongly
 *                 indicating the two chips share this controller. The
 *                 general boot flow — bootrom validates and runs the
 *                 LLB from NOR, LLB runs from SRAM, iBoot runs from
 *                 DRAM — is also confirmed from community sources.
 *
 *                 NEW this round — the peripheral map. Apple's own
 *                 device tree for the iPod touch 1G (an S5L8900X
 *                 device; dump: github.com/dizimka, gist 2922441,
 *                 "ioRegDumpIPT1G") lists an IODeviceMemory address
 *                 for every on-chip block. Each peripheral's `reg` is
 *                 an offset into the "arm-io" window, which the same
 *                 tree maps at 0x38000000. From it: VIC0 0x38E00000
 *                 and VIC1 0x38E01000 (node compatible = "vic,pl192",
 *                 so the PL192 claim below is now Apple's own label,
 *                 not just openiBoot's register names), GPIO
 *                 0x3E400000, and — independently re-confirming the
 *                 earlier source — the clock controller at 0x3C500000.
 *                 UART0 at 0x3CC00000 is corroborated by a real iPhone
 *                 OS kernel boot log from the S5LBox emulator
 *                 ("AppleS5L8900XSerial ... uart0 at 0x3cc00000") and
 *                 by The iPhone Wiki's sibling-chip (S5L8720) page. A
 *                 second S5L8900 IOReg (iPod1,1, pastebin VubPbsE0)
 *                 repeats the same addresses and supplies the clock
 *                 rates below. Caveat: these dumps are from an iPod
 *                 touch; the iPhone 2G shares the SoC, so the SoC's own
 *                 blocks are the same, but board-level peripherals
 *                 (baseband UART, etc.) differ.
 *   INFERRED     — not stated by any source, but follows from confirmed
 *                  facts by reasoning, so more trustworthy than a
 *                  placeholder and less than CONFIRMED. SDRAM base
 *                  0x08000000: the same device tree puts the 12MB
 *                  framebuffer ("vram") at 0x0F400000 and the panic RAM
 *                  ("pram") at 0x0FFFC000, both of which must lie
 *                  inside the 128MiB SDRAM, which pins its base to
 *                  0x08000000. SRAM base 0x22000000: the AMC node's
 *                  second register window is 0x2C000 bytes at that
 *                  address and openiBoot-era sources say the LLB runs
 *                  from SRAM; separately, the QEMU-iOS author reports
 *                  the bootrom jumping into code at 0x22000000 that is
 *                  absent from public ROM dumps. Consistent with SRAM,
 *                  but the size here is only a lower bound.
 *   GROUNDED     — not S5L8900-specific, but a real, standard, publicly
 *                  documented ARM PrimeCell peripheral that openiBoot's
 *                  own interrupt.c confirms this SoC family actually
 *                  uses (its code literally names VICINTENABLE,
 *                  VICINTENCLEAR, VICINTSELECT, VICSWPRIORITYMASK,
 *                  VICVECTADDRS — stock PL192 VIC register names). The
 *                  UART (PL011-style) and timer (SP804-style) register
 *                  *layouts* here are standard ARM PrimeCells used as a
 *                  stand-in, and should be treated as likely WRONG for
 *                  this chip: the S5L8900's serial driver is Apple's
 *                  "AppleS5L8900XSerial" and the Linux binding list
 *                  carries a separate `apple,s5l-uart`, i.e. a
 *                  Samsung-family UART rather than a PL011. Their
 *                  *base addresses* are now real (UART0) or still
 *                  placeholder (timer); the register layouts are
 *                  unverified either way (see README item B1).
 *   PLACEHOLDER  — a base address or size picked to give every region
 *                  a sensible, non-overlapping home so the emulator
 *                  works end-to-end, but not sourced from anywhere.
 *                  These are the ones to replace first if real
 *                  hardware register dumps or openiBoot's actual
 *                  plat-s5l8900 hardware headers become available.
 *
 * IMPORTANT CORRECTION from this round of research: openiBoot's real
 * plat-s5l8900/pmu.c talks to the PMU (power management: battery/
 * charger, RTC, power-off) entirely over I2C to an external companion
 * chip — it is NOT a block of memory-mapped registers at all. The
 * SOC_CLOCK_BASE block below is therefore genuinely just the clock/PLL
 * controller (which *is* memory-mapped, and is what's now CONFIRMED);
 * this file no longer describes it as "clock/power" and doesn't attempt
 * to model the I2C PMU (see the NOT-implemented-yet list in README.md).
 * Also newly learned: NOR flash access on real hardware goes through
 * an SPI controller (openiBoot's clock-gate table shows "spi0 (nor)"),
 * not a flat memory-mapped flash window the way SOC_NOR_BASE models it
 * here — that's a known simplification now backed by a concrete reason
 * rather than just a guess, and the next thing to fix if NOR write
 * support becomes a priority (see README.md).
 *
 * Since we currently only run hand-assembled test firmware (which by
 * construction agrees with whatever addresses this file defines),
 * the PLACEHOLDER items don't affect correctness *today*. It matters
 * the moment real Apple-signed firmware images are the test target,
 * since those hardcode the real addresses.
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

#define SOC_SRAM_BASE        0x22000000u   /* INFERRED, see above */
#define SOC_SRAM_SIZE        0x0002C000u   /* INFERRED lower bound (AMC window length); real size unconfirmed */

#define SOC_SDRAM_BASE       0x08000000u   /* INFERRED (framebuffer + panic RAM must fit inside) */
#define SOC_SDRAM_SIZE       0x08000000u   /* CONFIRMED: 128MiB (TheAppleWiki: S5L8900) */

#define SOC_NOR_BASE         0x1C000000u   /* PLACEHOLDER (and real NOR is SPI-attached — see the correction above) */
#define SOC_NOR_SIZE         0x00100000u   /* PLACEHOLDER: 1MB */

/* The "arm-io" peripheral window. CONFIRMED: device-tree `ranges` maps
 * the peripherals' offset space at 0x38000000, and every peripheral
 * `reg` below is that offset (so absolute = 0x38000000 + reg). */
#define SOC_ARMIO_BASE       0x38000000u

#define SOC_VIC0_BASE        0x38E00000u   /* CONFIRMED (PL192, first of two) */
#define SOC_VIC1_BASE        0x38E01000u   /* CONFIRMED address; NOT YET MODELED — README item B2 */
#define SOC_UART0_BASE       0x3CC00000u   /* CONFIRMED address; register LAYOUT unverified (see GROUNDED note) */
#define SOC_GPIO_BASE        0x3E400000u   /* CONFIRMED address; register layout unmodeled (plain scratch array) */
#define SOC_GPIO_SIZE        0x00001000u

/* PLACEHOLDER: the one peripheral we model whose real address is still
 * unknown. Sits inside the arm-io window at an offset where no device
 * seen in the transcribed part of the device tree lives. */
#define SOC_TIMER_BASE       (SOC_ARMIO_BASE + 0x2000u)

/* CONFIRMED addresses of other on-chip blocks (same device tree), listed
 * so the not-yet-modeled ones (README item B6) start from real values
 * instead of a fresh search. None of these are modeled. Per-device IRQ
 * numbers are in the same dump and are deliberately NOT transcribed
 * here — that belongs to B2, done with the source open. */
#define SOC_SHA1_BASE        0x38000000u
#define SOC_DMAC0_BASE       0x38200000u   /* PL080 DMA controller */
#define SOC_USBOTG_BASE      0x38400000u
#define SOC_AMC_BASE         0x38500000u
#define SOC_ADM_BASE         0x38800000u
#define SOC_CLCD_BASE        0x38900000u   /* display controller */
#define SOC_NANDCTL_BASE     0x38A00000u   /* "flash-controller0" */
#define SOC_CEATA_BASE       0x38B00000u
#define SOC_AES_BASE         0x38C00000u
#define SOC_SDIO_BASE        0x38D00000u
#define SOC_EDGEIC_BASE      0x38E02000u
#define SOC_DMAC1_BASE       0x39900000u
#define SOC_POWER_BASE       0x39A00000u

/* CONFIRMED clock rates from the iPod1,1 IOReg (device-tree properties):
 * crystal 24MHz, timebase 6MHz, CPU 412MHz, bus 103MHz, peripheral
 * 51.5MHz, memory ~137.33MHz. Not used by the model yet (README B5). */
#define SOC_CLK_CRYSTAL_HZ     24000000u
#define SOC_CLK_TIMEBASE_HZ     6000000u
#define SOC_CLK_CPU_HZ        412000000u
#define SOC_CLK_BUS_HZ        103000000u
#define SOC_CLK_PERIPH_HZ      51500000u

/* CONFIRMED: the clock/PLL controller — base address, register
 * offsets, and real power-on-reset values, from kleemajo's iDroid-notes
 * "clock" file (see the CONFIRMED note above). Deliberately its own
 * base constant; it sits inside the arm-io window like everything else. */
#define SOC_CLOCK_BASE            0x3C500000u
#define SOC_CLOCK_CONFIG0         0x00u
#define SOC_CLOCK_CONFIG1         0x04u
#define SOC_CLOCK_CONFIG2         0x08u
#define SOC_CLOCK_UNKNOWN1        0x0Cu
#define SOC_CLOCK_UNKNOWN2        0x10u
#define SOC_CLOCK_UNKNOWN3        0x14u
#define SOC_CLOCK_PLL0CON         0x20u
#define SOC_CLOCK_PLL1CON         0x24u
#define SOC_CLOCK_PLL2CON         0x28u
#define SOC_CLOCK_PLL0LCNT        0x30u
#define SOC_CLOCK_PLL1LCNT        0x34u
#define SOC_CLOCK_PLL2LCNT        0x38u
#define SOC_CLOCK_UNKNOWN4        0x3Cu
#define SOC_CLOCK_PLLLOCK         0x40u   /* read-only; always reads back "locked" — see soc.c */
#define SOC_CLOCK_PLLMODE         0x44u
#define SOC_CLOCK_GATES1          0x48u
#define SOC_CLOCK_GATES2          0x4Cu
#define SOC_CLOCK_GATES3          0x58u
#define SOC_CLOCK_GATES4          0x68u
#define SOC_CLOCK_GATES5          0x6Cu
#define SOC_CLOCK_REGSPACE_WORDS  ((SOC_CLOCK_GATES5 / 4) + 1)

/* IRQ line numbers on VIC0 for the two peripherals this file models.
 * PLACEHOLDER: the real UART/timer assignments are not yet transcribed
 * (README B2). They were previously 1 and 2, which collided with GPIO
 * interrupt groups — openiBoot's gpio.c and the device tree both put
 * GPIO on IRQs 0-3 and 0x1f-0x21 — so they now use 5 and 6, which no
 * transcribed device claims. That still isn't a claim about hardware:
 * devices in the un-transcribed part of the tree might use them. Note
 * this chip needs >32 lines in total (two VICs), so VIC0 alone is
 * known-incomplete (B2). */
#define SOC_IRQ_UART0        5
#define SOC_IRQ_TIMER        6

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

/* CONFIRMED register set — see the CONFIRMED note above and soc.c's
 * clock_read/clock_write for behavior (plain RW passthrough for all of
 * these except PLLLOCK, which is read-only and always reads "locked"). */
typedef struct {
    uint32_t regs[SOC_CLOCK_REGSPACE_WORDS];
} soc_clock_t;

typedef struct {
    arm1176_t   *cpu;   /* for raising/lowering IRQ */

    uint8_t     *securerom;   /* SOC_SECUREROM_SIZE bytes, read-only to the CPU */
    uint8_t     *sram;        /* SOC_SRAM_SIZE bytes, read/write */
    uint8_t     *sdram;       /* SOC_SDRAM_SIZE bytes, read/write */
    uint8_t     *nor;         /* SOC_NOR_SIZE bytes, read-only to the CPU for now (see TODO in soc.c) */

    soc_vic_t    vic0;
    soc_uart_t   uart0;
    soc_timer_t  timer;
    soc_clock_t  clock;

    uint32_t     gpio_regs[SOC_GPIO_SIZE / 4];  /* generic RW stub — GPIO only, see the PMU correction above */
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
