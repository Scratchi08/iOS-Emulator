/*
 * arm1176_decode.c — ARM-state instruction decode & execute for the
 * ARM1176JZF-S integer core, plus the shared fetch/execute loop
 * (arm1176_step) that dispatches to this file or arm1176_thumb.c
 * depending on CPSR.T.
 *
 * Decode strategy follows the classic ARM ARM bits[27:25] tree.
 */

#include <stdio.h>
#include "arm1176.h"
#include "arm1176_internal.h"
#include "jazelle.h"

#define BF(instr, hi, lo) (((instr) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

/* reg_read/reg_write/write_pc_bx/add_with_carry/set_nz now live in
 * arm1176_internal.h so the Thumb decoder can share them. */

/* ------------------------------------------------------------------ */
/* Operand2 (shifter operand) decoding for data-processing instructions*/
/* ------------------------------------------------------------------ */

static uint32_t decode_operand2(arm1176_t *cpu, uint32_t instr, bool *shifter_carry)
{
    bool old_c = cpu->cpsr & CPSR_C;

    if (BF(instr, 25, 25)) {
        /* Immediate: 8-bit imm rotated right by 2*rotate */
        uint32_t imm = BF(instr, 7, 0);
        uint32_t rot = BF(instr, 11, 8) * 2;
        if (rot == 0) { *shifter_carry = old_c; return imm; }
        uint32_t result = (imm >> rot) | (imm << (32 - rot));
        *shifter_carry = (result >> 31) & 1;
        return result;
    }

    uint32_t rm = BF(instr, 3, 0);
    uint32_t shift_type = BF(instr, 6, 5);
    uint32_t rm_val = reg_read(cpu, rm);

    if (BF(instr, 4, 4) == 0) {
        /* Shift by immediate amount */
        uint32_t amount = BF(instr, 11, 7);
        bool imm_zero_special = (amount == 0); /* LSR/ASR #32, ROR->RRX */
        return arm1176_shift(rm_val, shift_type, amount, imm_zero_special, old_c, shifter_carry);
    } else {
        /* Shift by register (bottom byte of Rs); bit7 must be 0, already
         * guaranteed by the caller not routing multiply/misc here. */
        uint32_t rs = BF(instr, 11, 8);
        uint32_t amount = reg_read(cpu, rs) & 0xFF;
        /* Note: per the ARM ARM, if Rm (or Rn) == PC here, PC should read
         * as instr_addr+12 rather than +8. Not modeled (rare in practice). */
        return arm1176_shift(rm_val, shift_type, amount, false, old_c, shifter_carry);
    }
}

/* ------------------------------------------------------------------ */
/* Data processing                                                     */
/* ------------------------------------------------------------------ */

static int exec_data_processing(arm1176_t *cpu, uint32_t instr)
{
    uint32_t opcode = BF(instr, 24, 21);
    bool s = BF(instr, 20, 20);
    uint32_t rn = BF(instr, 19, 16);
    uint32_t rd = BF(instr, 15, 12);

    bool shifter_carry;
    uint32_t op2 = decode_operand2(cpu, instr, &shifter_carry);
    uint32_t op1 = reg_read(cpu, rn);

    uint32_t result = 0;
    bool write_result = true;
    bool carry_out = cpu->cpsr & CPSR_C;
    bool overflow_out = cpu->cpsr & CPSR_V;
    bool logical = false;

    switch (opcode) {
        case 0x0: result = op1 & op2; logical = true; break;              /* AND */
        case 0x1: result = op1 ^ op2; logical = true; break;              /* EOR */
        case 0x2: result = add_with_carry(op1, ~op2, 1, &carry_out, &overflow_out); break; /* SUB */
        case 0x3: result = add_with_carry(op2, ~op1, 1, &carry_out, &overflow_out); break; /* RSB */
        case 0x4: result = add_with_carry(op1, op2, 0, &carry_out, &overflow_out); break;  /* ADD */
        case 0x5: result = add_with_carry(op1, op2, (cpu->cpsr & CPSR_C) ? 1 : 0, &carry_out, &overflow_out); break; /* ADC */
        case 0x6: result = add_with_carry(op1, ~op2, (cpu->cpsr & CPSR_C) ? 1 : 0, &carry_out, &overflow_out); break; /* SBC */
        case 0x7: result = add_with_carry(op2, ~op1, (cpu->cpsr & CPSR_C) ? 1 : 0, &carry_out, &overflow_out); break; /* RSC */
        case 0x8: result = op1 & op2; logical = true; write_result = false; break; /* TST */
        case 0x9: result = op1 ^ op2; logical = true; write_result = false; break; /* TEQ */
        case 0xA: result = add_with_carry(op1, ~op2, 1, &carry_out, &overflow_out); write_result = false; break; /* CMP */
        case 0xB: result = add_with_carry(op1, op2, 0, &carry_out, &overflow_out); write_result = false; break;  /* CMN */
        case 0xC: result = op1 | op2; logical = true; break;              /* ORR */
        case 0xD: result = op2; logical = true; break;                    /* MOV */
        case 0xE: result = op1 & ~op2; logical = true; break;             /* BIC */
        case 0xF: result = ~op2; logical = true; break;                   /* MVN */
        default: break;
    }

    if (logical) carry_out = shifter_carry;

    if (write_result) {
        if (rd == 15 && s) {
            /* MOVS/ADDS pc,... style exception return */
            uint32_t *spsr = arm1176_spsr_ptr(cpu);
            if (spsr) cpu->cpsr = *spsr;
            write_pc_bx(cpu, result);
            return 3;
        }
        reg_write(cpu, rd, result);
        if (rd == 15) return 3; /* pipeline refill on branch-through-PC */
    }

    if (s) {
        set_nz(cpu, result);
        cpu->cpsr = (cpu->cpsr & ~CPSR_C) | (carry_out ? CPSR_C : 0);
        if (!logical) cpu->cpsr = (cpu->cpsr & ~CPSR_V) | (overflow_out ? CPSR_V : 0);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* PSR transfer: MRS / MSR                                              */
/* ------------------------------------------------------------------ */

static int exec_psr_transfer(arm1176_t *cpu, uint32_t instr)
{
    bool use_spsr = BF(instr, 22, 22);
    bool is_msr = BF(instr, 21, 21);

    if (!is_msr) {
        /* MRS Rd, CPSR|SPSR */
        uint32_t rd = BF(instr, 15, 12);
        uint32_t *spsr = arm1176_spsr_ptr(cpu);
        uint32_t val = use_spsr ? (spsr ? *spsr : 0) : cpu->cpsr;
        reg_write(cpu, rd, val);
        return 1;
    }

    /* MSR CPSR|SPSR_<fields>, Rm|#imm */
    uint32_t operand;
    if (BF(instr, 25, 25)) {
        uint32_t imm = BF(instr, 7, 0);
        uint32_t rot = BF(instr, 11, 8) * 2;
        operand = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
    } else {
        operand = reg_read(cpu, BF(instr, 3, 0));
    }

    uint32_t field_mask = 0;
    if (BF(instr, 16, 16)) field_mask |= 0x000000FFu; /* control  (c) */
    if (BF(instr, 17, 17)) field_mask |= 0x0000FF00u; /* extension(x) */
    if (BF(instr, 18, 18)) field_mask |= 0x00FF0000u; /* status   (s) */
    if (BF(instr, 19, 19)) field_mask |= 0xFF000000u; /* flags    (f) */

    if (use_spsr) {
        uint32_t *spsr = arm1176_spsr_ptr(cpu);
        if (spsr) *spsr = (*spsr & ~field_mask) | (operand & field_mask);
    } else {
        /* Writes to the mode bits go through arm1176_switch_mode so
         * register banking stays correct. Privileged-only: in a full
         * implementation, user mode may only update the flags field;
         * not enforced here yet. */
        uint32_t new_cpsr = (cpu->cpsr & ~field_mask) | (operand & field_mask);
        uint32_t new_mode = new_cpsr & CPSR_MODE_MASK;
        uint32_t old_mode = cpu->cpsr & CPSR_MODE_MASK;
        cpu->cpsr = new_cpsr;
        if ((field_mask & 0xFF) && new_mode != old_mode) {
            /* restore full new_cpsr after switch (switch_mode only touches mode bits) */
            arm1176_switch_mode(cpu, new_mode);
            cpu->cpsr = new_cpsr;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Multiply / multiply-long                                            */
/* ------------------------------------------------------------------ */

static int exec_multiply(arm1176_t *cpu, uint32_t instr)
{
    uint32_t rd = BF(instr, 19, 16), rn = BF(instr, 15, 12);
    uint32_t rs = BF(instr, 11, 8), rm = BF(instr, 3, 0);
    bool accumulate = BF(instr, 21, 21);
    bool s = BF(instr, 20, 20);

    uint32_t result = reg_read(cpu, rm) * reg_read(cpu, rs);
    if (accumulate) result += reg_read(cpu, rn);
    reg_write(cpu, rd, result);
    if (s) { set_nz(cpu, result); /* C is UNPREDICTABLE per spec; left unchanged */ }
    return accumulate ? 2 : 2;
}

static int exec_multiply_long(arm1176_t *cpu, uint32_t instr)
{
    uint32_t rdhi = BF(instr, 19, 16), rdlo = BF(instr, 15, 12);
    uint32_t rs = BF(instr, 11, 8), rm = BF(instr, 3, 0);
    bool is_signed = BF(instr, 22, 22);
    bool accumulate = BF(instr, 21, 21);
    bool s = BF(instr, 20, 20);

    uint64_t result;
    if (is_signed) {
        int64_t a = (int32_t)reg_read(cpu, rm);
        int64_t b = (int32_t)reg_read(cpu, rs);
        result = (uint64_t)(a * b);
    } else {
        result = (uint64_t)reg_read(cpu, rm) * (uint64_t)reg_read(cpu, rs);
    }
    if (accumulate) {
        uint64_t acc = ((uint64_t)reg_read(cpu, rdhi) << 32) | reg_read(cpu, rdlo);
        result += acc;
    }
    reg_write(cpu, rdlo, (uint32_t)result);
    reg_write(cpu, rdhi, (uint32_t)(result >> 32));
    if (s) {
        cpu->cpsr = (cpu->cpsr & ~(CPSR_N | CPSR_Z))
                  | ((result >> 63) & 1 ? CPSR_N : 0)
                  | (result == 0 ? CPSR_Z : 0);
    }
    return 3;
}

static int exec_swp(arm1176_t *cpu, uint32_t instr)
{
    uint32_t rn = BF(instr, 19, 16), rd = BF(instr, 15, 12), rm = BF(instr, 3, 0);
    bool byte = BF(instr, 22, 22);
    uint32_t addr = reg_read(cpu, rn);
    if (byte) {
        uint8_t tmp = bus_read8(cpu, addr);
        ABORT_CHECK_RETURN(cpu, 4);
        bus_write8(cpu, addr, (uint8_t)reg_read(cpu, rm));
        ABORT_CHECK_RETURN(cpu, 4);
        reg_write(cpu, rd, tmp);
    } else {
        uint32_t tmp = bus_read32(cpu, addr);
        ABORT_CHECK_RETURN(cpu, 4);
        bus_write32(cpu, addr, reg_read(cpu, rm));
        ABORT_CHECK_RETURN(cpu, 4);
        reg_write(cpu, rd, tmp);
    }
    return 4;
}

/* ------------------------------------------------------------------ */
/* Single data transfer: LDR/STR (word/byte)                           */
/* ------------------------------------------------------------------ */

static int exec_single_transfer(arm1176_t *cpu, uint32_t instr)
{
    bool imm_offset = !BF(instr, 25, 25); /* note: reversed vs data-processing's I bit */
    bool p = BF(instr, 24, 24), u = BF(instr, 23, 23), b = BF(instr, 22, 22), w = BF(instr, 21, 21), l = BF(instr, 20, 20);
    uint32_t rn = BF(instr, 19, 16), rd = BF(instr, 15, 12);

    uint32_t offset;
    if (imm_offset) {
        offset = BF(instr, 11, 0);
    } else {
        bool carry_dummy;
        bool old_c = cpu->cpsr & CPSR_C;
        uint32_t rm = BF(instr, 3, 0);
        uint32_t shift_type = BF(instr, 6, 5);
        uint32_t amount = BF(instr, 11, 7);
        offset = arm1176_shift(reg_read(cpu, rm), shift_type, amount, amount == 0, old_c, &carry_dummy);
    }

    uint32_t base = reg_read(cpu, rn);
    uint32_t addr = p ? (u ? base + offset : base - offset) : base;

    if (l) {
        uint32_t val = b ? bus_read8(cpu, addr) : bus_read32(cpu, addr);
        ABORT_CHECK_RETURN(cpu, 3);
        /* LDR word: if address is not aligned, a real core rotates the
         * loaded word; not modeled here (TODO). */
        if (rd == 15) { write_pc_bx(cpu, val); }
        else reg_write(cpu, rd, val);
    } else {
        uint32_t val = reg_read(cpu, rd);
        if (b) bus_write8(cpu, addr, (uint8_t)val);
        else   bus_write32(cpu, addr, val);
        ABORT_CHECK_RETURN(cpu, 2);
    }

    if (!p) {
        uint32_t writeback_addr = u ? base + offset : base - offset;
        reg_write(cpu, rn, writeback_addr);
    } else if (w) {
        reg_write(cpu, rn, addr);
    }
    return l ? 3 : 2;
}

/* ------------------------------------------------------------------ */
/* Halfword and signed byte/halfword transfer                          */
/* ------------------------------------------------------------------ */

static int exec_halfword_transfer(arm1176_t *cpu, uint32_t instr)
{
    bool p = BF(instr, 24, 24), u = BF(instr, 23, 23), imm_flag = BF(instr, 22, 22);
    bool w = BF(instr, 21, 21), l = BF(instr, 20, 20);
    uint32_t rn = BF(instr, 19, 16), rd = BF(instr, 15, 12);
    uint32_t sh = BF(instr, 6, 5);

    uint32_t offset = imm_flag ? ((BF(instr, 11, 8) << 4) | BF(instr, 3, 0))
                                : reg_read(cpu, BF(instr, 3, 0));

    uint32_t base = reg_read(cpu, rn);
    uint32_t addr = p ? (u ? base + offset : base - offset) : base;

    if (l) {
        uint32_t val;
        switch (sh) {
            case 1: val = bus_read16(cpu, addr); break;                              /* LDRH  */
            case 2: val = (uint32_t)(int32_t)(int8_t)bus_read8(cpu, addr); break;     /* LDRSB */
            case 3: val = (uint32_t)(int32_t)(int16_t)bus_read16(cpu, addr); break;   /* LDRSH */
            default: val = 0; break;
        }
        ABORT_CHECK_RETURN(cpu, 3);
        reg_write(cpu, rd, val);
    } else {
        if (sh == 1) bus_write16(cpu, addr, (uint16_t)reg_read(cpu, rd));            /* STRH */
        /* signed store variants don't exist; sh==2/3 while L=0 is undefined */
        ABORT_CHECK_RETURN(cpu, 2);
    }

    if (!p) {
        reg_write(cpu, rn, u ? base + offset : base - offset);
    } else if (w) {
        reg_write(cpu, rn, addr);
    }
    return l ? 3 : 2;
}

/* ------------------------------------------------------------------ */
/* Block data transfer: LDM/STM                                        */
/* ------------------------------------------------------------------ */

static int exec_block_transfer(arm1176_t *cpu, uint32_t instr)
{
    bool p = BF(instr, 24, 24), u = BF(instr, 23, 23), s = BF(instr, 22, 22);
    bool w = BF(instr, 21, 21), l = BF(instr, 20, 20);
    uint32_t rn = BF(instr, 19, 16);
    uint32_t list = BF(instr, 15, 0);

    int count = 0;
    for (int i = 0; i < 16; i++) if (list & (1u << i)) count++;
    if (count == 0) return 1; /* degenerate empty list; real hw still defines behavior, skip */

    uint32_t base = cpu->r[rn]; /* rn is never PC in valid encodings */

    /* Registers are always stored/loaded in increasing address order,
     * regardless of IA/IB/DA/DB. The four modes just pick where that
     * ascending run of addresses starts:
     *   IA (U=1,P=0): start = base
     *   IB (U=1,P=1): start = base + 4
     *   DA (U=0,P=0): start = base - 4*count + 4
     *   DB (U=0,P=1): start = base - 4*count
     */
    uint32_t cur = u ? (base + (p ? 4u : 0u))
                     : (base - (uint32_t)count * 4u + (p ? 0u : 4u));

    bool pc_in_list = list & (1u << 15);
    bool use_user_bank = s && !(l && pc_in_list);

    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (l) {
            uint32_t val = bus_read32(cpu, cur);
            if (cpu->abort_taken) return 1 + count; /* stop issuing further accesses; see arm1176.h */
            if (use_user_bank && i >= 8 && i <= 14) {
                /* Access the USR bank regardless of current mode. Simplified:
                 * only meaningfully differs from current regs in FIQ mode. */
                switch (i) {
                    case 8: cpu->r8_usr = val; break;   case 9: cpu->r9_usr = val; break;
                    case 10: cpu->r10_usr = val; break; case 11: cpu->r11_usr = val; break;
                    case 12: cpu->r12_usr = val; break; case 13: cpu->r13_usr = val; break;
                    case 14: cpu->r14_usr = val; break;
                }
            } else if (i == 15) {
                if (s) { /* LDM with ^ and PC in list: also restores CPSR */
                    uint32_t *spsr = arm1176_spsr_ptr(cpu);
                    if (spsr) cpu->cpsr = *spsr;
                    write_pc_bx(cpu, val);
                } else {
                    reg_write(cpu, 15, val);
                }
            } else {
                reg_write(cpu, i, val);
            }
        } else {
            uint32_t val = (i == 15) ? reg_read(cpu, 15) + 4 /* PC special-case, +12 total */ : reg_read(cpu, i);
            bus_write32(cpu, cur, val);
            if (cpu->abort_taken) return 1 + count;
        }
        cur += 4;
    }

    if (w && !(l && pc_in_list)) {
        uint32_t new_base = u ? base + (uint32_t)count * 4 : base - (uint32_t)count * 4;
        cpu->r[rn] = new_base;
    }
    return 1 + count;
}

/* ------------------------------------------------------------------ */
/* Branch / branch-exchange                                            */
/* ------------------------------------------------------------------ */

static int exec_branch(arm1176_t *cpu, uint32_t instr)
{
    bool link = BF(instr, 24, 24);
    int32_t offset = (int32_t)(BF(instr, 23, 0) << 8) >> 6; /* sign-extend *4 */
    uint32_t next = cpu->r[15]; /* return address, already points past this instr */
    uint32_t target = reg_read(cpu, 15) + (uint32_t)offset;
    if (link) cpu->r[14] = next;
    cpu->r[15] = target & ~3u;
    return 3;
}

static int exec_bx_blx(arm1176_t *cpu, uint32_t instr, bool link)
{
    uint32_t rm = BF(instr, 3, 0);
    uint32_t target = reg_read(cpu, rm);
    uint32_t next = cpu->r[15];
    if (link) cpu->r[14] = next;
    write_pc_bx(cpu, target);
    return 3;
}

static int exec_clz(arm1176_t *cpu, uint32_t instr)
{
    uint32_t rd = BF(instr, 15, 12), rm = BF(instr, 3, 0);
    uint32_t val = reg_read(cpu, rm);
    uint32_t count = 0;
    if (val == 0) count = 32;
    else { while (!(val & 0x80000000u)) { count++; val <<= 1; } }
    reg_write(cpu, rd, count);
    return 1;
}

static int exec_blx_immediate(arm1176_t *cpu, uint32_t instr)
{
    bool h = BF(instr, 24, 24);
    int32_t offset = (int32_t)(BF(instr, 23, 0) << 8) >> 6;
    uint32_t next = cpu->r[15];
    uint32_t target = reg_read(cpu, 15) + (uint32_t)offset + (h ? 2 : 0);
    cpu->r[14] = next;
    cpu->cpsr |= CPSR_T;
    cpu->r[15] = target & ~1u;
    return 3;
}

/* ------------------------------------------------------------------ */
/* Top-level ARM instruction dispatch                                  */
/* ------------------------------------------------------------------ */

int arm1176_execute_arm(arm1176_t *cpu, uint32_t instr)
{
    uint32_t cond = BF(instr, 31, 28);

    if (cond == 0xF) {
        /* Unconditional instruction extension space */
        if (BF(instr, 27, 25) == 0x5) return exec_blx_immediate(cpu, instr);
        /* CP/PLD/CLREX/etc. — not yet implemented, treat as NOP */
        return 1;
    }

    if (!arm1176_cond_passed(cpu, cond)) return 1;

    uint32_t bits27_26 = BF(instr, 27, 26);

    if (bits27_26 == 0x0) {
        bool bit25 = BF(instr, 25, 25);
        bool bit24 = BF(instr, 24, 24);
        bool bit23 = BF(instr, 23, 23);
        bool bit21 = BF(instr, 21, 21);
        bool bit20 = BF(instr, 20, 20);
        bool bit7  = BF(instr, 7, 7);
        bool bit4  = BF(instr, 4, 4);
        uint32_t bits6_5 = BF(instr, 6, 5);
        uint32_t bits24_23 = BF(instr, 24, 23);

        if (!bit25 && bit7 && bit4 && bits6_5 == 0) {
            /* bits7:4 == 1001 shape: multiply / multiply-long / swap */
            if (!bit24) {
                return BF(instr, 23, 23) ? exec_multiply_long(cpu, instr)
                                          : exec_multiply(cpu, instr);
            } else if (!bit23 && !bit21 && !bit20) {
                return exec_swp(cpu, instr);
            }
            /* bit24 set but not a recognized SWP encoding: undefined
             * instruction in this space for now. */
            return 1;
        }

        if (!bit25 && bit7 && bit4 && bits6_5 != 0) {
            return exec_halfword_transfer(cpu, instr);
        }

        if (bits24_23 == 0x2 && !bit20 && (bit25 || BF(instr, 11, 4) == 0x00)) {
            /* PSR transfer (MRS/MSR). For the *register* forms
             * (bit25==0), the real encoding fixes bits[11:4] to all
             * zero (MRS: entirely SBZ; MSR-register: SBZ then Rm's
             * upper nibble is 0000 since Rm is bits[3:0] alone) — this
             * is what actually distinguishes them from BX/BXJ/BLX,
             * which live at the same bits[24:20] pattern but force
             * bits[11:8]=1111 as part of their fixed 0xFFF field. Just
             * checking bit4==0 isn't enough: BXJ also has bit4==0.
             * For MSR-immediate (bit25==1), bits[11:4] are simply part
             * of the rotated 8-bit immediate and can be anything. */
            return exec_psr_transfer(cpu, instr);
        }

        /* Misc branch/CLZ space: cond 0001 0010 1111 1111 1111 xxxx Rm */
        if (!bit25 && (BF(instr, 27, 20) == 0x12) && (BF(instr, 19, 8) == 0xFFF)) {
            uint32_t lo = BF(instr, 7, 4);
            if (lo == 0x1) return exec_bx_blx(cpu, instr, false);   /* BX  */
            if (lo == 0x2) return jazelle_bxj(cpu, reg_read(cpu, BF(instr, 3, 0))); /* BXJ (trivial impl: falls back to BX, see jazelle.h) */
            if (lo == 0x3) return exec_bx_blx(cpu, instr, true);    /* BLX */
            /* 0x7 == BKPT, not modeled: treat as NOP */
            return 1;
        }
        if (!bit25 && (BF(instr, 27, 20) == 0x16) &&
            (BF(instr, 19, 16) == 0xF) && (BF(instr, 11, 4) == 0xF1)) {
            return exec_clz(cpu, instr);
        }

        return exec_data_processing(cpu, instr);
    }

    if (bits27_26 == 0x1) {
        if (BF(instr, 25, 25) && BF(instr, 4, 4)) {
            /* Media/undefined extension space (bit25=1,bit4=1) — not
             * part of base ARMv6 integer path we've implemented yet. */
            return 1;
        }
        return exec_single_transfer(cpu, instr);
    }

    if (bits27_26 == 0x2) {
        if (BF(instr, 25, 25)) return exec_branch(cpu, instr);
        return exec_block_transfer(cpu, instr);
    }

    /* bits27_26 == 0x3 */
    if (BF(instr, 24, 24)) {
        /* SWI */
        uint32_t next = cpu->r[15];
        arm1176_enter_exception(cpu, ARM_MODE_SVC, VEC_SWI, next, true, false);
        return 3;
    }

    if (BF(instr, 25, 25) == 1) {
        /* bits27:24 = 1110 : CDP, or MRC/MCR register transfer */
        uint32_t coproc = BF(instr, 11, 8);
        if (BF(instr, 4, 4) == 1) {
            uint32_t opc1 = BF(instr, 23, 21);
            bool l = BF(instr, 20, 20);
            uint32_t crn = BF(instr, 19, 16);
            uint32_t rd = BF(instr, 15, 12);
            uint32_t opc2 = BF(instr, 7, 5);
            uint32_t crm = BF(instr, 3, 0);
            if (coproc == 15) {
                if (l) {
                    uint32_t val = cp15_mrc(cpu, crn, opc1, crm, opc2);
                    if (rd == 15) {
                        /* MRC to r15 is the rare "flags only" form: it
                         * updates NZCV from bits[31:28] of the result
                         * rather than writing a register. */
                        cpu->cpsr = (cpu->cpsr & 0x0FFFFFFFu) | (val & 0xF0000000u);
                    } else {
                        reg_write(cpu, rd, val);
                    }
                } else {
                    cp15_mcr(cpu, crn, opc1, crm, opc2, reg_read(cpu, rd));
                }
                return 1;
            }
            if (coproc == 10 || coproc == 11) {
                /* VMOV(core<->single) and VMRS/VMSR share this shape. */
                return vfp_reg_transfer(cpu, instr);
            }
            if (coproc == 14) {
                /* Jazelle CP14: JIDR/JMCR/JOSCR, all RAZ/WI (trivial
                 * implementation — see jazelle.h). */
                if (l) {
                    uint32_t val = jazelle_mrc(cpu, crn, opc1, crm, opc2);
                    if (rd == 15) cpu->cpsr = (cpu->cpsr & 0x0FFFFFFFu) | (val & 0xF0000000u);
                    else reg_write(cpu, rd, val);
                } else {
                    jazelle_mcr(cpu, crn, opc1, crm, opc2, reg_read(cpu, rd));
                }
                return 1;
            }
            /* Other coprocessors: silent no-op rather than a fault. */
            return 1;
        }
        /* CDP: CP15 defines none; coproc 10/11 is VFP data processing
         * (VADD/VSUB/VMUL/VDIV/VABS/VNEG/VSQRT/VCMP/VCVT/...). */
        if (coproc == 10 || coproc == 11) return vfp_data_processing(cpu, instr);
        return 1;
    }

    /* bits27:25 = 110 : LDC/STC (coprocessor data transfer) space. This
     * also holds a repurposed "reserved" sub-pattern (P=0,U=0,W=0) used
     * for VMOV between two core registers and a doubleword register —
     * distinguished by checking bits[24:21] before treating it as a
     * normal single/multiple VFP register transfer. */
    {
        uint32_t coproc = BF(instr, 11, 8);
        if (coproc == 10 || coproc == 11) {
            if (BF(instr, 24, 21) == 0x2) return vfp_two_reg_transfer(cpu, instr);
            return vfp_load_store(cpu, instr);
        }
    }
    return 1;
}


/* ------------------------------------------------------------------ */
/* Fetch/execute loop                                                   */
/* ------------------------------------------------------------------ */

int arm1176_step(arm1176_t *cpu)
{
    if (cpu->halted) return 0;

    if (arm1176_check_interrupts(cpu)) {
        cpu->cycle_count += 3;
        return 3;
    }

    cpu->abort_taken = false;
    int cycles;

    if (cpu->cpsr & CPSR_T) {
        uint32_t fetch_addr = cpu->r[15] & ~1u;
        cpu->current_instr_addr = fetch_addr;
        uint16_t instr = fetch_read16(cpu, fetch_addr);
        if (cpu->abort_taken) { cpu->instr_count++; cpu->cycle_count += 3; return 3; }
        cpu->r[15] = fetch_addr + 2;
        cycles = arm1176_execute_thumb(cpu, instr);
    } else {
        uint32_t fetch_addr = cpu->r[15] & ~3u;
        cpu->current_instr_addr = fetch_addr;
        uint32_t instr = fetch_read32(cpu, fetch_addr);
        if (cpu->abort_taken) { cpu->instr_count++; cpu->cycle_count += 3; return 3; }
        cpu->r[15] = fetch_addr + 4;
        cycles = arm1176_execute_arm(cpu, instr);
    }

    cpu->instr_count++;
    cpu->cycle_count += (uint64_t)cycles;
    return cycles;
}
