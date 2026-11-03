#ifndef ARM1176_INTERNAL_H
#define ARM1176_INTERNAL_H

#include "arm1176.h"

/* arm1176_cpu.c */
uint32_t *arm1176_spsr_ptr(arm1176_t *cpu);
bool      arm1176_cond_passed(arm1176_t *cpu, uint32_t cond);
uint32_t  arm1176_shift(uint32_t value, uint32_t shift_type, uint32_t amount,
                         bool imm_zero_special, bool old_carry, bool *carry_out);
void      arm1176_enter_exception(arm1176_t *cpu, uint32_t new_mode, uint32_t vector,
                                   uint32_t lr_value, bool disable_irq, bool disable_fiq);
bool      arm1176_check_interrupts(arm1176_t *cpu);

/* arm1176_decode.c (ARM state) */
int arm1176_execute_arm(arm1176_t *cpu, uint32_t instr);

/* arm1176_thumb.c (Thumb state) */
int arm1176_execute_thumb(arm1176_t *cpu, uint16_t instr);

/* Raw physical-bus access — used by CP15's page-table walker (which
 * reads descriptors by physical address) and, after translation, by
 * the mem/fetch layer below. Nothing else should call these
 * directly: ARM/Thumb decode always goes through the MMU/TCM-aware
 * bus_read/write and fetch_read functions further down, even when the
 * MMU is disabled (in which case they're a fast identity passthrough). */
static inline uint32_t phys_read32(arm1176_t *cpu, uint32_t addr) {
    return cpu->bus.read32 ? cpu->bus.read32(cpu->bus.ctx, addr) : 0;
}
static inline uint16_t phys_read16(arm1176_t *cpu, uint32_t addr) {
    return cpu->bus.read16 ? cpu->bus.read16(cpu->bus.ctx, addr) : 0;
}
static inline uint8_t phys_read8(arm1176_t *cpu, uint32_t addr) {
    return cpu->bus.read8 ? cpu->bus.read8(cpu->bus.ctx, addr) : 0;
}
static inline void phys_write32(arm1176_t *cpu, uint32_t addr, uint32_t val) {
    if (cpu->bus.write32) cpu->bus.write32(cpu->bus.ctx, addr, val);
}
static inline void phys_write16(arm1176_t *cpu, uint32_t addr, uint16_t val) {
    if (cpu->bus.write16) cpu->bus.write16(cpu->bus.ctx, addr, val);
}
static inline void phys_write8(arm1176_t *cpu, uint32_t addr, uint8_t val) {
    if (cpu->bus.write8) cpu->bus.write8(cpu->bus.ctx, addr, val);
}

/* MMU/TCM-aware data access (implemented in cp15.c). Every LDR/STR/
 * LDM/STM/etc. in the ARM and Thumb decoders calls these. */
static inline uint32_t bus_read32(arm1176_t *cpu, uint32_t addr)  { return cp15_mem_read32(cpu, addr); }
static inline uint16_t bus_read16(arm1176_t *cpu, uint32_t addr)  { return cp15_mem_read16(cpu, addr); }
static inline uint8_t  bus_read8(arm1176_t *cpu, uint32_t addr)   { return cp15_mem_read8(cpu, addr); }
static inline void bus_write32(arm1176_t *cpu, uint32_t addr, uint32_t val) { cp15_mem_write32(cpu, addr, val); }
static inline void bus_write16(arm1176_t *cpu, uint32_t addr, uint16_t val) { cp15_mem_write16(cpu, addr, val); }
static inline void bus_write8(arm1176_t *cpu, uint32_t addr, uint8_t val)   { cp15_mem_write8(cpu, addr, val); }

/* MMU/TCM-aware instruction fetch — separate from bus_read* because
 * fetches check ITCM (not DTCM), enforce the XN execute-permission bit,
 * and raise Prefetch Abort (not Data Abort) on failure. */
static inline uint32_t fetch_read32(arm1176_t *cpu, uint32_t addr) { return cp15_fetch32(cpu, addr); }
static inline uint16_t fetch_read16(arm1176_t *cpu, uint32_t addr) { return cp15_fetch16(cpu, addr); }

/* Convenience for the many call sites in decode.c/thumb.c that need to
 * bail out of a partially-completed multi-access instruction (LDM/STM,
 * push/pop, ...) the moment a data abort has redirected the CPU. */
#define ABORT_CHECK_RETURN(cpu, retval) do { if ((cpu)->abort_taken) return (retval); } while (0)

/* ------------------------------------------------------------------ */
/* Register access shared by both the ARM and Thumb decoders.          */
/*                                                                      */
/* The "PC reads as current instruction + 8 (ARM) / + 4 (Thumb)" rule  */
/* falls out naturally here: arm1176_step() leaves r[15] sitting at    */
/* fetch_addr+4 after an ARM fetch, or fetch_addr+2 after a Thumb      */
/* fetch, so adding the state-dependent delta below lands on the       */
/* architecturally-defined value in both cases.                        */
/* ------------------------------------------------------------------ */

static inline uint32_t reg_read(arm1176_t *cpu, int r)
{
    if (r != 15) return cpu->r[r];
    return cpu->r[15] + ((cpu->cpsr & CPSR_T) ? 2 : 4);
}

/* Plain (non-interworking) write to a register. Used for everything
 * except BX/BLX(register) and the Thumb long-branch-with-link pair. */
static inline void reg_write(arm1176_t *cpu, int r, uint32_t val)
{
    if (r == 15) {
        /* Ordinary writes to PC stay in the current instruction set and
         * are aligned to it: word-aligned in ARM state, halfword-aligned
         * in Thumb state. */
        val &= (cpu->cpsr & CPSR_T) ? ~1u : ~3u;
    }
    cpu->r[r] = val;
}

/* BX/BLX-style writes: bit0 of the target selects Thumb vs ARM state,
 * i.e. this is the one write path that can switch instruction sets. */
static inline void write_pc_bx(arm1176_t *cpu, uint32_t val)
{
    if (val & 1) {
        cpu->cpsr |= CPSR_T;
        cpu->r[15] = val & ~1u;
    } else {
        cpu->cpsr &= ~CPSR_T;
        cpu->r[15] = val & ~3u;
    }
}

static inline uint32_t add_with_carry(uint32_t a, uint32_t b, uint32_t carry_in,
                                       bool *carry_out, bool *overflow_out)
{
    uint64_t result = (uint64_t)a + (uint64_t)b + (uint64_t)(carry_in ? 1 : 0);
    *carry_out = (result >> 32) & 1;
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    int64_t sresult = (int64_t)sa + (int64_t)sb + (carry_in ? 1 : 0);
    *overflow_out = (sresult != (int32_t)sresult);
    return (uint32_t)result;
}

static inline void set_nz(arm1176_t *cpu, uint32_t result)
{
    cpu->cpsr = (cpu->cpsr & ~(CPSR_N | CPSR_Z))
              | (result & 0x80000000u ? CPSR_N : 0)
              | (result == 0 ? CPSR_Z : 0);
}

static inline int32_t sign_extend(uint32_t value, int bits)
{
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((value ^ m) - m);
}

#endif
