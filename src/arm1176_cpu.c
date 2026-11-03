/*
 * arm1176_cpu.c — reset, mode/register banking, condition evaluation,
 * barrel shifter, and exception entry for the ARM1176JZF-S core.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm1176.h"
#include "arm1176_internal.h"

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

void arm1176_set_bus(arm1176_t *cpu, arm_bus_t bus)
{
    cpu->bus = bus;
}

void arm1176_reset(arm1176_t *cpu)
{
    /* Free any TCM backing store from a previous configuration before
     * the memset below drops the pointers, then let cp15_reset() set
     * up fresh (unconfigured, per real hardware) CP15 state. */
    free(cpu->cp15.itcm);
    free(cpu->cp15.dtcm);

    memset(cpu, 0, sizeof(*cpu));
    cpu->cpsr = ARM_MODE_SVC | CPSR_I | CPSR_F; /* IRQ/FIQ masked, ARM state */
    cpu->r[15] = VEC_RESET;
    cpu->halted = false;
    cp15_reset(cpu);
    vfp_reset(cpu);
}

/* ------------------------------------------------------------------ */
/* Mode name / debug                                                   */
/* ------------------------------------------------------------------ */

const char *arm1176_mode_name(uint32_t mode)
{
    switch (mode & CPSR_MODE_MASK) {
        case ARM_MODE_USR: return "USR";
        case ARM_MODE_FIQ: return "FIQ";
        case ARM_MODE_IRQ: return "IRQ";
        case ARM_MODE_SVC: return "SVC";
        case ARM_MODE_ABT: return "ABT";
        case ARM_MODE_UND: return "UND";
        case ARM_MODE_SYS: return "SYS";
        default:            return "???";
    }
}

void arm1176_dump_state(const arm1176_t *cpu)
{
    printf("---- ARM1176 state (mode=%s%s) ----\n",
           arm1176_mode_name(cpu->cpsr),
           (cpu->cpsr & CPSR_T) ? "/Thumb" : "");
    for (int i = 0; i < 16; i++) {
        printf("r%-2d=%08x%s", i, cpu->r[i], (i % 4 == 3) ? "\n" : "  ");
    }
    printf("cpsr=%08x  N=%d Z=%d C=%d V=%d I=%d F=%d T=%d\n",
           cpu->cpsr,
           !!(cpu->cpsr & CPSR_N), !!(cpu->cpsr & CPSR_Z),
           !!(cpu->cpsr & CPSR_C), !!(cpu->cpsr & CPSR_V),
           !!(cpu->cpsr & CPSR_I), !!(cpu->cpsr & CPSR_F),
           !!(cpu->cpsr & CPSR_T));
    printf("cycles=%llu instrs=%llu\n",
           (unsigned long long)cpu->cycle_count,
           (unsigned long long)cpu->instr_count);
}

/* ------------------------------------------------------------------ */
/* Register banking                                                    */
/* ------------------------------------------------------------------ */

/*
 * ARM register banking rules:
 *   - r0-r7 and r15 are never banked.
 *   - r8-r12 are banked only for FIQ; USR/SYS/SVC/ABT/IRQ/UND all share
 *     one copy (referred to here as the "usr" bank).
 *   - r13 (SP) and r14 (LR) are banked per mode, except USR and SYS
 *     share one copy.
 */
void arm1176_switch_mode(arm1176_t *cpu, uint32_t new_mode)
{
    uint32_t old_mode = cpu->cpsr & CPSR_MODE_MASK;
    new_mode &= CPSR_MODE_MASK;

    if (old_mode == new_mode) {
        cpu->cpsr = (cpu->cpsr & ~CPSR_MODE_MASK) | new_mode;
        return;
    }

    /* Save r8-r12 out of the live register file into whichever bank
     * the *old* mode was using. */
    if (old_mode == ARM_MODE_FIQ) {
        cpu->r8_fiq = cpu->r[8];   cpu->r9_fiq  = cpu->r[9];
        cpu->r10_fiq = cpu->r[10]; cpu->r11_fiq = cpu->r[11];
        cpu->r12_fiq = cpu->r[12];
    } else {
        cpu->r8_usr = cpu->r[8];   cpu->r9_usr  = cpu->r[9];
        cpu->r10_usr = cpu->r[10]; cpu->r11_usr = cpu->r[11];
        cpu->r12_usr = cpu->r[12];
    }

    /* Save r13/r14 into the old mode's bank. */
    switch (old_mode) {
        case ARM_MODE_USR: case ARM_MODE_SYS:
            cpu->r13_usr = cpu->r[13]; cpu->r14_usr = cpu->r[14]; break;
        case ARM_MODE_FIQ:
            cpu->r13_fiq = cpu->r[13]; cpu->r14_fiq = cpu->r[14]; break;
        case ARM_MODE_SVC:
            cpu->r13_svc = cpu->r[13]; cpu->r14_svc = cpu->r[14]; break;
        case ARM_MODE_ABT:
            cpu->r13_abt = cpu->r[13]; cpu->r14_abt = cpu->r[14]; break;
        case ARM_MODE_IRQ:
            cpu->r13_irq = cpu->r[13]; cpu->r14_irq = cpu->r[14]; break;
        case ARM_MODE_UND:
            cpu->r13_und = cpu->r[13]; cpu->r14_und = cpu->r[14]; break;
        default: break; /* mode 0 at boot, before first switch */
    }

    /* Load r8-r12 for the *new* mode. */
    if (new_mode == ARM_MODE_FIQ) {
        cpu->r[8] = cpu->r8_fiq;   cpu->r[9]  = cpu->r9_fiq;
        cpu->r[10] = cpu->r10_fiq; cpu->r[11] = cpu->r11_fiq;
        cpu->r[12] = cpu->r12_fiq;
    } else {
        cpu->r[8] = cpu->r8_usr;   cpu->r[9]  = cpu->r9_usr;
        cpu->r[10] = cpu->r10_usr; cpu->r[11] = cpu->r11_usr;
        cpu->r[12] = cpu->r12_usr;
    }

    /* Load r13/r14 for the new mode. */
    switch (new_mode) {
        case ARM_MODE_USR: case ARM_MODE_SYS:
            cpu->r[13] = cpu->r13_usr; cpu->r[14] = cpu->r14_usr; break;
        case ARM_MODE_FIQ:
            cpu->r[13] = cpu->r13_fiq; cpu->r[14] = cpu->r14_fiq; break;
        case ARM_MODE_SVC:
            cpu->r[13] = cpu->r13_svc; cpu->r[14] = cpu->r14_svc; break;
        case ARM_MODE_ABT:
            cpu->r[13] = cpu->r13_abt; cpu->r[14] = cpu->r14_abt; break;
        case ARM_MODE_IRQ:
            cpu->r[13] = cpu->r13_irq; cpu->r[14] = cpu->r14_irq; break;
        case ARM_MODE_UND:
            cpu->r[13] = cpu->r13_und; cpu->r[14] = cpu->r14_und; break;
        default: break;
    }

    cpu->cpsr = (cpu->cpsr & ~CPSR_MODE_MASK) | new_mode;
}

uint32_t *arm1176_spsr_ptr(arm1176_t *cpu)
{
    switch (cpu->cpsr & CPSR_MODE_MASK) {
        case ARM_MODE_FIQ: return &cpu->spsr_fiq;
        case ARM_MODE_SVC: return &cpu->spsr_svc;
        case ARM_MODE_ABT: return &cpu->spsr_abt;
        case ARM_MODE_IRQ: return &cpu->spsr_irq;
        case ARM_MODE_UND: return &cpu->spsr_und;
        default:           return NULL; /* USR/SYS have no SPSR */
    }
}

/* ------------------------------------------------------------------ */
/* Condition evaluation                                                */
/* ------------------------------------------------------------------ */

bool arm1176_cond_passed(arm1176_t *cpu, uint32_t cond)
{
    bool n = cpu->cpsr & CPSR_N, z = cpu->cpsr & CPSR_Z;
    bool c = cpu->cpsr & CPSR_C, v = cpu->cpsr & CPSR_V;

    switch (cond) {
        case 0x0: return z;             /* EQ */
        case 0x1: return !z;            /* NE */
        case 0x2: return c;             /* CS/HS */
        case 0x3: return !c;            /* CC/LO */
        case 0x4: return n;             /* MI */
        case 0x5: return !n;            /* PL */
        case 0x6: return v;             /* VS */
        case 0x7: return !v;            /* VC */
        case 0x8: return c && !z;       /* HI */
        case 0x9: return !c || z;       /* LS */
        case 0xA: return n == v;        /* GE */
        case 0xB: return n != v;        /* LT */
        case 0xC: return !z && (n==v);  /* GT */
        case 0xD: return z || (n!=v);   /* LE */
        case 0xE: return true;          /* AL */
        case 0xF: return true;          /* unconditional extension space (caller handles) */
        default:  return false;
    }
}

/* ------------------------------------------------------------------ */
/* Barrel shifter                                                      */
/*   Computes (value, carry_out) for the four ARM shift types.         */
/*   shift_amt of 0 with "immediate" shifts has special-cased meaning  */
/*   per the ARM ARM (LSR/ASR #32, ROR #0 == RRX); the caller tells us */
/*   which case applies via `imm_zero_special`.                        */
/* ------------------------------------------------------------------ */

uint32_t arm1176_shift(uint32_t value, uint32_t shift_type, uint32_t amount,
                        bool imm_zero_special, bool old_carry, bool *carry_out)
{
    switch (shift_type & 3) {
    case 0: /* LSL */
        if (amount == 0) { *carry_out = old_carry; return value; }
        if (amount < 32) { *carry_out = (value >> (32 - amount)) & 1; return value << amount; }
        if (amount == 32) { *carry_out = value & 1; return 0; }
        *carry_out = false; return 0;

    case 1: /* LSR */
        if (amount == 0) {
            if (imm_zero_special) { *carry_out = (value >> 31) & 1; return 0; } /* LSR #32 */
            *carry_out = old_carry; return value;
        }
        if (amount < 32) { *carry_out = (value >> (amount - 1)) & 1; return value >> amount; }
        if (amount == 32) { *carry_out = (value >> 31) & 1; return 0; }
        *carry_out = false; return 0;

    case 2: /* ASR */
        if (amount == 0) {
            if (imm_zero_special) amount = 32; /* ASR #32 */
            else { *carry_out = old_carry; return value; }
        }
        if (amount >= 32) {
            bool sign = (value >> 31) & 1;
            *carry_out = sign;
            return sign ? 0xFFFFFFFFu : 0u;
        }
        *carry_out = (value >> (amount - 1)) & 1;
        return (uint32_t)((int32_t)value >> amount);

    case 3: /* ROR / RRX */
        if (amount == 0) {
            if (imm_zero_special) {
                /* RRX: rotate right by 1 through carry */
                uint32_t result = (value >> 1) | (old_carry ? 0x80000000u : 0);
                *carry_out = value & 1;
                return result;
            }
            *carry_out = old_carry; return value;
        }
        amount &= 31;
        if (amount == 0) { *carry_out = (value >> 31) & 1; return value; }
        *carry_out = (value >> (amount - 1)) & 1;
        return (value >> amount) | (value << (32 - amount));
    }
    *carry_out = old_carry;
    return value;
}

/* ------------------------------------------------------------------ */
/* Exception entry                                                     */
/* ------------------------------------------------------------------ */

void arm1176_enter_exception(arm1176_t *cpu, uint32_t new_mode, uint32_t vector,
                              uint32_t lr_value, bool disable_irq, bool disable_fiq)
{
    uint32_t old_cpsr = cpu->cpsr;

    arm1176_switch_mode(cpu, new_mode);

    uint32_t *spsr = arm1176_spsr_ptr(cpu);
    if (spsr) *spsr = old_cpsr;

    cpu->r[14] = lr_value;

    cpu->cpsr &= ~CPSR_T;              /* exceptions always enter ARM state */
    if (disable_irq) cpu->cpsr |= CPSR_I;
    if (disable_fiq) cpu->cpsr |= CPSR_F;

    cpu->r[15] = vector;
}

void arm1176_assert_irq(arm1176_t *cpu, bool level)
{
    cpu->irq_line = level;
}

void arm1176_assert_fiq(arm1176_t *cpu, bool level)
{
    cpu->fiq_line = level;
}

/* Called once per instruction by arm1176_step() to check pending lines. */
bool arm1176_check_interrupts(arm1176_t *cpu)
{
    if (cpu->fiq_line && !(cpu->cpsr & CPSR_F)) {
        /* PC has already been advanced past the current instruction by
         * the time step() calls us for the *next* instruction, so the
         * return address is simply the current r[15] + 4. */
        arm1176_enter_exception(cpu, ARM_MODE_FIQ, VEC_FIQ, cpu->r[15] + 4, true, true);
        return true;
    }
    if (cpu->irq_line && !(cpu->cpsr & CPSR_I)) {
        arm1176_enter_exception(cpu, ARM_MODE_IRQ, VEC_IRQ, cpu->r[15] + 4, true, false);
        return true;
    }
    return false;
}
