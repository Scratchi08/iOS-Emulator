/*
 * arm1176.h — Public interface for the ARM1176JZF-S CPU core.
 *
 * Part of an iPhone 2G / iPhone OS 1.0 emulation project.
 *
 * Scope of this core (as of this file):
 *   - ARM (32-bit) instruction set, ARMv6 integer subset
 *   - All 7 legacy modes (usr/fiq/irq/svc/abt/und/sys) with correct
 *     register banking
 *   - Data processing, multiply/multiply-long, single & halfword/signed
 *     data transfer, block data transfer, branch/branch-exchange, SWI,
 *     MRS/MSR
 *   - Thumb (16-bit) state: all 19 classic formats plus BLX
 *   - Exception entry (reset/undef/swi/prefetch abort/data abort/irq/fiq)
 *   - CP15: MMU (VMSAv6 short-descriptor walk, TLB, domains, AP/XN
 *     permissions), cache maintenance ops (modeled as no-ops — see
 *     cp15.h), and TCM (ITCM/DTCM)
 *   - VFP11 (VFPv2 floating point): see vfp.h for scope/fidelity notes
 *
 * Explicitly NOT yet implemented (future milestones):
 *   - Jazelle (the "J") — direct bytecode execution
 *   - Precise cycle timing / pipeline hazards (we return an approximate
 *     cycle count per instruction)
 *   - TrustZone secure/non-secure world switching (Monitor mode)
 */

#ifndef ARM1176_H
#define ARM1176_H

#include <stdint.h>
#include <stdbool.h>
#include "cp15.h"
#include "vfp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Processor modes (low 5 bits of CPSR) ---- */
typedef enum {
    ARM_MODE_USR = 0x10,
    ARM_MODE_FIQ = 0x11,
    ARM_MODE_IRQ = 0x12,
    ARM_MODE_SVC = 0x13,
    ARM_MODE_ABT = 0x17,
    ARM_MODE_UND = 0x1B,
    ARM_MODE_SYS = 0x1F,
} arm_mode_t;

/* ---- CPSR bit masks ---- */
#define CPSR_N        (1u << 31)
#define CPSR_Z        (1u << 30)
#define CPSR_C        (1u << 29)
#define CPSR_V        (1u << 28)
#define CPSR_Q        (1u << 27)   /* sticky overflow, DSP extensions   */
#define CPSR_J        (1u << 24)   /* Jazelle state                     */
#define CPSR_E        (1u << 9)    /* endianness                        */
#define CPSR_A        (1u << 8)    /* imprecise data abort disable      */
#define CPSR_I        (1u << 7)    /* IRQ disable                       */
#define CPSR_F        (1u << 6)    /* FIQ disable                       */
#define CPSR_T        (1u << 5)    /* Thumb state                       */
#define CPSR_MODE_MASK 0x1Fu

/* ---- Exception vectors (assuming low vectors at 0x00000000) ---- */
#define VEC_RESET      0x00000000u
#define VEC_UNDEF      0x00000004u
#define VEC_SWI        0x00000008u
#define VEC_PREFETCH   0x0000000Cu
#define VEC_DATA_ABORT 0x00000010u
#define VEC_RESERVED   0x00000014u
#define VEC_IRQ        0x00000018u
#define VEC_FIQ        0x0000001Cu

/*
 * Memory bus: the CPU core never touches memory directly. The board/SoC
 * layer (NOR flash, SDRAM, memory-mapped peripherals, MMU, etc.) is
 * expected to implement this and hand it to arm1176_set_bus().
 */
typedef struct {
    uint8_t  (*read8)(void *ctx, uint32_t addr);
    uint16_t (*read16)(void *ctx, uint32_t addr);
    uint32_t (*read32)(void *ctx, uint32_t addr);
    void     (*write8)(void *ctx, uint32_t addr, uint8_t val);
    void     (*write16)(void *ctx, uint32_t addr, uint16_t val);
    void     (*write32)(void *ctx, uint32_t addr, uint32_t val);
    void     *ctx;
} arm_bus_t;

typedef struct ARM1176 {
    /* Currently-visible register file. r[15] is the PC. */
    uint32_t r[16];

    /* Banked registers (see arm1176_switch_mode for banking rules) */
    uint32_t r8_usr, r9_usr, r10_usr, r11_usr, r12_usr, r13_usr, r14_usr;
    uint32_t r8_fiq, r9_fiq, r10_fiq, r11_fiq, r12_fiq, r13_fiq, r14_fiq;
    uint32_t r13_svc, r14_svc;
    uint32_t r13_abt, r14_abt;
    uint32_t r13_irq, r14_irq;
    uint32_t r13_und, r14_und;

    uint32_t cpsr;
    uint32_t spsr_fiq, spsr_svc, spsr_abt, spsr_irq, spsr_und;

    arm_bus_t bus;

    uint64_t cycle_count;
    uint64_t instr_count;
    bool     halted;      /* set on unrecoverable decode failure, or WFI */
    bool     trace;       /* if true, arm1176_step prints a disassembly-ish trace */

    /* External interrupt request lines, level-sensitive, sampled once
     * per instruction in arm1176_step(). */
    bool     irq_line;
    bool     fiq_line;

    /* CP15 system control coprocessor: MMU, cache-op stubs, TCM. */
    cp15_t   cp15;

    /* VFP11 floating-point coprocessor. */
    vfp_t    vfp;

    /* Address of the instruction currently being fetched/executed, set
     * at the top of arm1176_step() before fetch. Used by CP15's fault
     * handling to compute the correct LR for Prefetch/Data Abort
     * (which use the faulting instruction's own address as a base,
     * regardless of ARM/Thumb state — see arm1176_enter_exception
     * callers in cp15.c). */
    uint32_t current_instr_addr;

    /* Set by the CP15 MMU access functions when a translation fault
     * (or permission/domain fault) redirects execution to an abort
     * vector mid-instruction. Checked by arm1176_step() right after
     * fetch, and by the ARM/Thumb decoders' multi-access instructions
     * (LDM/STM, push/pop, etc.) so they stop issuing further memory
     * accesses using now-stale (post-exception-entry) CPU state. */
    bool     abort_taken;
} arm1176_t;

/* Lifecycle */
/* IMPORTANT: the arm1176_t passed in must be zero-initialized before
 * the very first call (e.g. `arm1176_t cpu = {0};`) — arm1176_reset()
 * frees any existing CP15 TCM allocation before resetting, and a
 * garbage pointer from uninitialized stack memory would crash. Every
 * call after the first is safe regardless, since by then the struct
 * only ever holds either NULL or a real allocation. */
void arm1176_reset(arm1176_t *cpu);
void arm1176_set_bus(arm1176_t *cpu, arm_bus_t bus);

/* Execution: decode + execute exactly one instruction at r[15].
 * Returns the approximate number of core cycles it took. */
int  arm1176_step(arm1176_t *cpu);

/* External interrupt lines. The board layer calls these each time it
 * wants to (re)assert IRQ/FIQ; arm1176_step() samples them once at the
 * top of each instruction, honoring the I/F mask bits in CPSR. */
void arm1176_assert_irq(arm1176_t *cpu, bool level);
void arm1176_assert_fiq(arm1176_t *cpu, bool level);

/* Mode switch helper (exposed mainly for exception entry & testing) */
void arm1176_switch_mode(arm1176_t *cpu, uint32_t new_mode);

/* Debug helpers */
const char *arm1176_mode_name(uint32_t mode);
void arm1176_dump_state(const arm1176_t *cpu);

#ifdef __cplusplus
}
#endif

#endif /* ARM1176_H */
