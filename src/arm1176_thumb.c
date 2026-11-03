/*
 * arm1176_thumb.c — Thumb-state instruction decode & execute.
 *
 * ARM1176JZF-S implements ARMv6, which includes the classic 16-bit
 * Thumb instruction set (often called "Thumb-1") plus the ARMv5T
 * BLX(suffix) addition — but not Thumb-2 (that arrived with ARMv6T2 /
 * Cortex cores). This file covers all 19 of the traditional Thumb
 * instruction formats plus BLX.
 *
 * Not yet covered (ARMv6-specific Thumb extras, lower priority — see
 * README): CPS, SETEND, REV/REV16/REVSH, SXTB/SXTH/UXTB/UXTH,
 * BKPT(Thumb).
 */

#include <stdio.h>
#include "arm1176.h"
#include "arm1176_internal.h"

#define TBF(instr, hi, lo) (((instr) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

/* Thumb's own "write to PC without switching state" rule: stays in
 * Thumb, halfword-aligned. Used by ADD/MOV hi-register ops and the
 * long-branch-with-link pair — never by BX/BLX, which use write_pc_bx. */
static inline void write_pc_thumb(arm1176_t *cpu, uint32_t val)
{
    cpu->r[15] = val & ~1u;
}

/* ------------------------------------------------------------------ */
/* Format 1: move shifted register — LSL/LSR/ASR Rd, Rs, #imm5         */
/* ------------------------------------------------------------------ */

static int fmt1_move_shifted(arm1176_t *cpu, uint16_t instr)
{
    uint32_t op = TBF(instr, 12, 11);
    uint32_t offset5 = TBF(instr, 10, 6);
    uint32_t rs = TBF(instr, 5, 3), rd = TBF(instr, 2, 0);

    bool old_c = cpu->cpsr & CPSR_C, carry_out;
    uint32_t result = arm1176_shift(reg_read(cpu, rs), op, offset5, offset5 == 0, old_c, &carry_out);

    reg_write(cpu, rd, result);
    set_nz(cpu, result);
    cpu->cpsr = (cpu->cpsr & ~CPSR_C) | (carry_out ? CPSR_C : 0);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Format 2: add/subtract — ADD/SUB Rd, Rs, Rn|#imm3                   */
/* ------------------------------------------------------------------ */

static int fmt2_add_sub(arm1176_t *cpu, uint16_t instr)
{
    bool imm = TBF(instr, 10, 10);
    bool sub = TBF(instr, 9, 9);
    uint32_t rn_or_imm = TBF(instr, 8, 6);
    uint32_t rs = TBF(instr, 5, 3), rd = TBF(instr, 2, 0);

    uint32_t op2 = imm ? rn_or_imm : reg_read(cpu, rn_or_imm);
    uint32_t op1 = reg_read(cpu, rs);
    bool carry, overflow;
    uint32_t result = sub ? add_with_carry(op1, ~op2, 1, &carry, &overflow)
                           : add_with_carry(op1, op2, 0, &carry, &overflow);
    reg_write(cpu, rd, result);
    set_nz(cpu, result);
    cpu->cpsr = (cpu->cpsr & ~(CPSR_C | CPSR_V))
              | (carry ? CPSR_C : 0) | (overflow ? CPSR_V : 0);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Format 3: move/compare/add/subtract immediate                       */
/* ------------------------------------------------------------------ */

static int fmt3_mcas_immediate(arm1176_t *cpu, uint16_t instr)
{
    uint32_t op = TBF(instr, 12, 11);
    uint32_t rd = TBF(instr, 10, 8);
    uint32_t imm8 = TBF(instr, 7, 0);

    if (op == 0) { /* MOV: only N,Z updated */
        reg_write(cpu, rd, imm8);
        set_nz(cpu, imm8);
        return 1;
    }

    uint32_t op1 = reg_read(cpu, rd);
    bool carry, overflow;
    uint32_t result;
    bool write_back = true;

    if (op == 1) { /* CMP */
        result = add_with_carry(op1, ~imm8, 1, &carry, &overflow);
        write_back = false;
    } else if (op == 2) { /* ADD */
        result = add_with_carry(op1, imm8, 0, &carry, &overflow);
    } else { /* SUB */
        result = add_with_carry(op1, ~imm8, 1, &carry, &overflow);
    }

    if (write_back) reg_write(cpu, rd, result);
    set_nz(cpu, result);
    cpu->cpsr = (cpu->cpsr & ~(CPSR_C | CPSR_V))
              | (carry ? CPSR_C : 0) | (overflow ? CPSR_V : 0);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Format 4: ALU operations                                            */
/* ------------------------------------------------------------------ */

static int fmt4_alu(arm1176_t *cpu, uint16_t instr)
{
    uint32_t op = TBF(instr, 9, 6);
    uint32_t rs = TBF(instr, 5, 3), rd = TBF(instr, 2, 0);
    uint32_t op1 = reg_read(cpu, rd), op2 = reg_read(cpu, rs);

    uint32_t result = 0;
    bool write_back = true, logical = false, is_shift = false;
    bool carry_out = cpu->cpsr & CPSR_C, overflow_out = cpu->cpsr & CPSR_V;

    switch (op) {
        case 0x0: result = op1 & op2; logical = true; break;                       /* AND */
        case 0x1: result = op1 ^ op2; logical = true; break;                       /* EOR */
        case 0x2: is_shift = true; result = arm1176_shift(op1, 0, op2 & 0xFF, false, carry_out, &carry_out); break; /* LSL */
        case 0x3: is_shift = true; result = arm1176_shift(op1, 1, op2 & 0xFF, (op2 & 0xFF) != 0, carry_out, &carry_out); break; /* LSR */
        case 0x4: is_shift = true; result = arm1176_shift(op1, 2, op2 & 0xFF, (op2 & 0xFF) != 0, carry_out, &carry_out); break; /* ASR */
        case 0x5: result = add_with_carry(op1, op2, (cpu->cpsr & CPSR_C) ? 1 : 0, &carry_out, &overflow_out); break; /* ADC */
        case 0x6: result = add_with_carry(op1, ~op2, (cpu->cpsr & CPSR_C) ? 1 : 0, &carry_out, &overflow_out); break; /* SBC */
        case 0x7: { /* ROR by register amount (bottom byte of Rs) */
            is_shift = true;
            uint32_t amt = op2 & 0xFF;
            if (amt == 0) {
                result = op1; /* value and carry both unaffected */
            } else {
                uint32_t rot = amt & 31;
                if (rot == 0) { result = op1; carry_out = (op1 >> 31) & 1; }
                else {
                    carry_out = (op1 >> (rot - 1)) & 1;
                    result = (op1 >> rot) | (op1 << (32 - rot));
                }
            }
            break;
        }
        case 0x8: result = op1 & op2; logical = true; write_back = false; break;   /* TST */
        case 0x9: result = add_with_carry(0, ~op2, 1, &carry_out, &overflow_out); break; /* NEG (0-op2) */
        case 0xA: result = add_with_carry(op1, ~op2, 1, &carry_out, &overflow_out); write_back = false; break; /* CMP */
        case 0xB: result = add_with_carry(op1, op2, 0, &carry_out, &overflow_out); write_back = false; break;  /* CMN */
        case 0xC: result = op1 | op2; logical = true; break;                       /* ORR */
        case 0xD: result = op1 * op2; logical = true; break;                       /* MUL (C unpredictable, leave as-is; V unaffected) */
        case 0xE: result = op1 & ~op2; logical = true; break;                      /* BIC */
        case 0xF: result = ~op2; logical = true; break;                            /* MVN */
        default: break;
    }

    if (write_back) reg_write(cpu, rd, result);
    set_nz(cpu, result);
    if (is_shift || logical) {
        /* logical ops with a plain register operand leave C untouched
         * unless it's one of the shift-by-register forms above, which
         * already computed carry_out from the shift itself. NEG/MUL are
         * handled by their own branches above (NEG goes via
         * add_with_carry, MUL leaves carry alone). */
        if (is_shift) cpu->cpsr = (cpu->cpsr & ~CPSR_C) | (carry_out ? CPSR_C : 0);
    } else {
        cpu->cpsr = (cpu->cpsr & ~(CPSR_C | CPSR_V))
                  | (carry_out ? CPSR_C : 0) | (overflow_out ? CPSR_V : 0);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Format 5: Hi register operations / branch exchange                  */
/* ------------------------------------------------------------------ */

static int fmt5_hi_reg_bx(arm1176_t *cpu, uint16_t instr)
{
    uint32_t op = TBF(instr, 9, 8);
    bool h1 = TBF(instr, 7, 7), h2 = TBF(instr, 6, 6);
    uint32_t rs = TBF(instr, 5, 3) + (h2 ? 8 : 0);
    uint32_t rd = TBF(instr, 2, 0) + (h1 ? 8 : 0);

    uint32_t src = reg_read(cpu, rs);

    switch (op) {
        case 0x0: { /* ADD Rd, Rd, Rs — flags NOT updated */
            uint32_t result = reg_read(cpu, rd) + src;
            if (rd == 15) write_pc_thumb(cpu, result); else reg_write(cpu, rd, result);
            return 1;
        }
        case 0x1: { /* CMP Rd, Rs — flags updated, no write */
            bool carry, overflow;
            uint32_t result = add_with_carry(reg_read(cpu, rd), ~src, 1, &carry, &overflow);
            set_nz(cpu, result);
            cpu->cpsr = (cpu->cpsr & ~(CPSR_C | CPSR_V))
                      | (carry ? CPSR_C : 0) | (overflow ? CPSR_V : 0);
            return 1;
        }
        case 0x2: /* MOV Rd, Rs — flags NOT updated */
            if (rd == 15) write_pc_thumb(cpu, src); else reg_write(cpu, rd, src);
            return 1;
        case 0x3: /* BX/BLX Rs */
            if (h1) cpu->r[14] = cpu->r[15] | 1; /* BLX: LR = return addr, stay-in-Thumb bit set */
            write_pc_bx(cpu, src);
            return 3;
        default:
            return 1;
    }
}

/* ------------------------------------------------------------------ */
/* Format 6: PC-relative load — LDR Rd, [PC, #imm8*4]                  */
/* ------------------------------------------------------------------ */

static int fmt6_pc_rel_load(arm1176_t *cpu, uint16_t instr)
{
    uint32_t rd = TBF(instr, 10, 8);
    uint32_t imm8 = TBF(instr, 7, 0);
    uint32_t addr = (reg_read(cpu, 15) & ~3u) + imm8 * 4;
    uint32_t val = bus_read32(cpu, addr);
    ABORT_CHECK_RETURN(cpu, 3);
    reg_write(cpu, rd, val);
    return 3;
}

/* ------------------------------------------------------------------ */
/* Formats 7 & 8: load/store with register offset                      */
/* ------------------------------------------------------------------ */

static int fmt7_load_store_reg(arm1176_t *cpu, uint16_t instr)
{
    bool l = TBF(instr, 11, 11), b = TBF(instr, 10, 10);
    uint32_t ro = TBF(instr, 8, 6), rb = TBF(instr, 5, 3), rd = TBF(instr, 2, 0);
    uint32_t addr = reg_read(cpu, rb) + reg_read(cpu, ro);

    if (l) {
        uint32_t val = b ? bus_read8(cpu, addr) : bus_read32(cpu, addr);
        ABORT_CHECK_RETURN(cpu, 3);
        reg_write(cpu, rd, val);
    } else {
        if (b) bus_write8(cpu, addr, (uint8_t)reg_read(cpu, rd)); else bus_write32(cpu, addr, reg_read(cpu, rd));
        ABORT_CHECK_RETURN(cpu, 2);
    }
    return l ? 3 : 2;
}

static int fmt8_load_store_sext(arm1176_t *cpu, uint16_t instr)
{
    bool h = TBF(instr, 11, 11), s = TBF(instr, 10, 10);
    uint32_t ro = TBF(instr, 8, 6), rb = TBF(instr, 5, 3), rd = TBF(instr, 2, 0);
    uint32_t addr = reg_read(cpu, rb) + reg_read(cpu, ro);

    if (!s && !h) { bus_write16(cpu, addr, (uint16_t)reg_read(cpu, rd)); ABORT_CHECK_RETURN(cpu, 2); return 2; }      /* STRH  */
    if (!s &&  h) { uint32_t v = bus_read16(cpu, addr); ABORT_CHECK_RETURN(cpu, 3); reg_write(cpu, rd, v); return 3; } /* LDRH  */
    if ( s && !h) { uint32_t v = (uint32_t)(int32_t)(int8_t)bus_read8(cpu, addr); ABORT_CHECK_RETURN(cpu, 3); reg_write(cpu, rd, v); return 3; } /* LDSB */
    { uint32_t v = (uint32_t)(int32_t)(int16_t)bus_read16(cpu, addr); ABORT_CHECK_RETURN(cpu, 3); reg_write(cpu, rd, v); } /* LDSH */
    return 3;
}

/* ------------------------------------------------------------------ */
/* Format 9: load/store with immediate offset (word/byte)              */
/* ------------------------------------------------------------------ */

static int fmt9_load_store_imm(arm1176_t *cpu, uint16_t instr)
{
    bool b = TBF(instr, 12, 12), l = TBF(instr, 11, 11);
    uint32_t offset5 = TBF(instr, 10, 6);
    uint32_t rb = TBF(instr, 5, 3), rd = TBF(instr, 2, 0);
    uint32_t offset = b ? offset5 : offset5 * 4;
    uint32_t addr = reg_read(cpu, rb) + offset;

    if (l) {
        uint32_t val = b ? bus_read8(cpu, addr) : bus_read32(cpu, addr);
        ABORT_CHECK_RETURN(cpu, 3);
        reg_write(cpu, rd, val);
    } else {
        if (b) bus_write8(cpu, addr, (uint8_t)reg_read(cpu, rd)); else bus_write32(cpu, addr, reg_read(cpu, rd));
        ABORT_CHECK_RETURN(cpu, 2);
    }
    return l ? 3 : 2;
}

/* ------------------------------------------------------------------ */
/* Format 10: load/store halfword (immediate offset)                   */
/* ------------------------------------------------------------------ */

static int fmt10_load_store_half(arm1176_t *cpu, uint16_t instr)
{
    bool l = TBF(instr, 11, 11);
    uint32_t offset5 = TBF(instr, 10, 6);
    uint32_t rb = TBF(instr, 5, 3), rd = TBF(instr, 2, 0);
    uint32_t addr = reg_read(cpu, rb) + offset5 * 2;

    if (l) {
        uint32_t val = bus_read16(cpu, addr);
        ABORT_CHECK_RETURN(cpu, 3);
        reg_write(cpu, rd, val);
    } else {
        bus_write16(cpu, addr, (uint16_t)reg_read(cpu, rd));
        ABORT_CHECK_RETURN(cpu, 2);
    }
    return l ? 3 : 2;
}

/* ------------------------------------------------------------------ */
/* Format 11: SP-relative load/store                                   */
/* ------------------------------------------------------------------ */

static int fmt11_sp_rel(arm1176_t *cpu, uint16_t instr)
{
    bool l = TBF(instr, 11, 11);
    uint32_t rd = TBF(instr, 10, 8);
    uint32_t imm8 = TBF(instr, 7, 0);
    uint32_t addr = reg_read(cpu, 13) + imm8 * 4;

    if (l) {
        uint32_t val = bus_read32(cpu, addr);
        ABORT_CHECK_RETURN(cpu, 3);
        reg_write(cpu, rd, val);
    } else {
        bus_write32(cpu, addr, reg_read(cpu, rd));
        ABORT_CHECK_RETURN(cpu, 2);
    }
    return l ? 3 : 2;
}

/* ------------------------------------------------------------------ */
/* Format 12: load address — ADD Rd, PC|SP, #imm8*4                    */
/* ------------------------------------------------------------------ */

static int fmt12_load_address(arm1176_t *cpu, uint16_t instr)
{
    bool sp = TBF(instr, 11, 11);
    uint32_t rd = TBF(instr, 10, 8);
    uint32_t imm8 = TBF(instr, 7, 0);
    uint32_t base = sp ? reg_read(cpu, 13) : (reg_read(cpu, 15) & ~3u);
    reg_write(cpu, rd, base + imm8 * 4);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Format 13: add offset to SP                                         */
/* ------------------------------------------------------------------ */

static int fmt13_add_sp_offset(arm1176_t *cpu, uint16_t instr)
{
    bool negative = TBF(instr, 7, 7);
    uint32_t imm7 = TBF(instr, 6, 0);
    uint32_t sp = reg_read(cpu, 13);
    reg_write(cpu, 13, negative ? sp - imm7 * 4 : sp + imm7 * 4);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Format 14: push/pop registers                                       */
/* ------------------------------------------------------------------ */

static int fmt14_push_pop(arm1176_t *cpu, uint16_t instr)
{
    bool l = TBF(instr, 11, 11);
    bool r_bit = TBF(instr, 8, 8);
    uint32_t list = TBF(instr, 7, 0);

    int count = 0;
    for (int i = 0; i < 8; i++) if (list & (1u << i)) count++;
    if (r_bit) count++;

    if (!l) {
        /* PUSH: store low-to-high at descending addresses, then LR
         * (if R set) at the highest address, SP -= 4*count overall. */
        uint32_t addr = reg_read(cpu, 13) - (uint32_t)count * 4;
        uint32_t sp_new = addr;
        for (int i = 0; i < 8; i++) {
            if (list & (1u << i)) {
                bus_write32(cpu, addr, reg_read(cpu, i));
                if (cpu->abort_taken) return 1 + count;
                addr += 4;
            }
        }
        if (r_bit) { bus_write32(cpu, addr, reg_read(cpu, 14)); ABORT_CHECK_RETURN(cpu, 1 + count); }
        reg_write(cpu, 13, sp_new);
    } else {
        /* POP: load low-to-high from ascending addresses, then PC (if
         * R set) — PC load interworks (ARMv5+: branches, and can
         * switch to ARM state if bit0 of the loaded value is clear). */
        uint32_t addr = reg_read(cpu, 13);
        for (int i = 0; i < 8; i++) {
            if (list & (1u << i)) {
                uint32_t val = bus_read32(cpu, addr);
                if (cpu->abort_taken) return 1 + count;
                reg_write(cpu, i, val);
                addr += 4;
            }
        }
        if (r_bit) {
            uint32_t val = bus_read32(cpu, addr);
            ABORT_CHECK_RETURN(cpu, 1 + count);
            write_pc_bx(cpu, val);
            addr += 4;
        }
        reg_write(cpu, 13, addr);
    }
    return 1 + count;
}

/* ------------------------------------------------------------------ */
/* Format 15: multiple load/store — STMIA/LDMIA Rb!, {Rlist}           */
/* ------------------------------------------------------------------ */

static int fmt15_multiple_transfer(arm1176_t *cpu, uint16_t instr)
{
    bool l = TBF(instr, 11, 11);
    uint32_t rb = TBF(instr, 10, 8);
    uint32_t list = TBF(instr, 7, 0);

    int count = 0;
    for (int i = 0; i < 8; i++) if (list & (1u << i)) count++;

    uint32_t addr = reg_read(cpu, rb);
    bool rb_in_list = list & (1u << rb);

    for (int i = 0; i < 8; i++) {
        if (!(list & (1u << i))) continue;
        if (l) {
            uint32_t val = bus_read32(cpu, addr);
            if (cpu->abort_taken) return 1 + count;
            reg_write(cpu, i, val);
        } else {
            bus_write32(cpu, addr, reg_read(cpu, i));
            if (cpu->abort_taken) return 1 + count;
        }
        addr += 4;
    }

    /* If Rb is in the list on a load, the loaded value wins over the
     * writeback value (matches common hardware/emulator behavior for
     * this UNPREDICTABLE-per-spec case). */
    if (!(l && rb_in_list)) reg_write(cpu, rb, addr);
    return 1 + count;
}

/* ------------------------------------------------------------------ */
/* Format 16: conditional branch                                       */
/* ------------------------------------------------------------------ */

static int fmt16_cond_branch(arm1176_t *cpu, uint16_t instr)
{
    uint32_t cond = TBF(instr, 11, 8);
    int32_t offset = sign_extend(TBF(instr, 7, 0), 8) * 2;
    if (!arm1176_cond_passed(cpu, cond)) return 1;
    write_pc_thumb(cpu, reg_read(cpu, 15) + (uint32_t)offset);
    return 3;
}

/* ------------------------------------------------------------------ */
/* Format 17: software interrupt                                       */
/* ------------------------------------------------------------------ */

static int fmt17_swi(arm1176_t *cpu, uint16_t instr)
{
    (void)instr;
    uint32_t next = cpu->r[15];
    arm1176_enter_exception(cpu, ARM_MODE_SVC, VEC_SWI, next, true, false);
    return 3;
}

/* ------------------------------------------------------------------ */
/* Format 18: unconditional branch                                     */
/* ------------------------------------------------------------------ */

static int fmt18_uncond_branch(arm1176_t *cpu, uint16_t instr)
{
    int32_t offset = sign_extend(TBF(instr, 10, 0), 11) * 2;
    write_pc_thumb(cpu, reg_read(cpu, 15) + (uint32_t)offset);
    return 3;
}

/* ------------------------------------------------------------------ */
/* Format 19: long branch with link (BL), and BLX(suffix)              */
/*                                                                      */
/* This is genuinely two independent 16-bit instructions executed back */
/* to back — LR is used as scratch space to carry the high part of the */
/* offset between them, exactly as real ARM hardware does it.          */
/* ------------------------------------------------------------------ */

static int fmt19_long_branch(arm1176_t *cpu, uint16_t instr)
{
    uint32_t h = TBF(instr, 12, 11);
    uint32_t offset11 = TBF(instr, 10, 0);

    if (h == 0x2) {
        /* First halfword: stage the high 21 bits of the offset in LR. */
        int32_t hi = sign_extend(offset11, 11) << 12;
        cpu->r[14] = reg_read(cpu, 15) + (uint32_t)hi;
        return 1;
    }

    /* Second halfword: h==3 is BL suffix (stay Thumb), h==1 is BLX
     * suffix (switch to ARM) — BLX suffix is ARMv5T+ only. */
    uint32_t next_instr = cpu->r[15] | 1; /* bit0 set: return stays in Thumb */
    uint32_t target = cpu->r[14] + (offset11 << 1);
    cpu->r[14] = next_instr;

    if (h == 0x3) {
        write_pc_thumb(cpu, target);
    } else { /* h == 0x1 : BLX suffix */
        cpu->cpsr &= ~CPSR_T;
        cpu->r[15] = target & ~3u;
    }
    return 3;
}

/* ------------------------------------------------------------------ */
/* Top-level Thumb dispatch                                             */
/* ------------------------------------------------------------------ */

int arm1176_execute_thumb(arm1176_t *cpu, uint16_t instr)
{
    uint32_t top5 = TBF(instr, 15, 11);
    uint32_t top3 = TBF(instr, 15, 13);
    uint32_t top4 = TBF(instr, 15, 12);

    switch (top3) {
        case 0x0: /* 000... : format 1, or format 2 when bits12:11==11 */
            if (TBF(instr, 12, 11) == 0x3) return fmt2_add_sub(cpu, instr);
            return fmt1_move_shifted(cpu, instr);
        case 0x1: /* 001... : format 3 */
            return fmt3_mcas_immediate(cpu, instr);
        default: break;
    }

    if (top4 == 0x4) { /* 0100... covers formats 4, 5, and 6 (0100 0..0110 1) */
        if (TBF(instr, 11, 10) == 0x0) { /* 010000 : format 4 */
            return fmt4_alu(cpu, instr);
        }
        if (TBF(instr, 11, 10) == 0x1) { /* 010001 : format 5 */
            return fmt5_hi_reg_bx(cpu, instr);
        }
        /* 01001xx : format 6 */
        return fmt6_pc_rel_load(cpu, instr);
    }

    if (top4 == 0x5) { /* 0101... : formats 7 and 8, split on bit9 */
        if (TBF(instr, 9, 9) == 0) return fmt7_load_store_reg(cpu, instr);
        return fmt8_load_store_sext(cpu, instr);
    }

    if (TBF(instr, 15, 13) == 0x3) { /* 011... : format 9 */
        return fmt9_load_store_imm(cpu, instr);
    }

    if (top4 == 0x8) return fmt10_load_store_half(cpu, instr);   /* 1000 */
    if (top4 == 0x9) return fmt11_sp_rel(cpu, instr);            /* 1001 */
    if (top4 == 0xA) return fmt12_load_address(cpu, instr);      /* 1010 */

    if (top4 == 0xB) { /* 1011... : formats 13 & 14 */
        if (TBF(instr, 11, 8) == 0x0) return fmt13_add_sp_offset(cpu, instr);
        if (TBF(instr, 10, 9) == 0x2) return fmt14_push_pop(cpu, instr);
        /* TODO: other 1011xxxx encodings here are ARMv6-era additions
         * not yet implemented — CPS (1011 0110 011x xxxx), SETEND
         * (1011 0110 010x xxxx), REV/REV16/REVSH (1011 1010 xxxx xxxx),
         * SXTB/SXTH/UXTB/UXTH (1011 0010 xxxx xxxx). Falls through to a
         * silent no-op for now rather than executing or halting. */
        return 1;
    }

    if (top4 == 0xC) return fmt15_multiple_transfer(cpu, instr); /* 1100 */

    if (top4 == 0xD) {
        if (TBF(instr, 11, 8) == 0xF) return fmt17_swi(cpu, instr); /* 1101 1111 */
        return fmt16_cond_branch(cpu, instr);                       /* 1101 xxxx (cond != 1111) */
    }

    if (top5 == 0x1C) return fmt18_uncond_branch(cpu, instr);     /* 11100 */
    if (top3 == 0x7) return fmt19_long_branch(cpu, instr);        /* 1111... (and 11101 BLX suffix) */

    return 1; /* undefined encoding */
}
