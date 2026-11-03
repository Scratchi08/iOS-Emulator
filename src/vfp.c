/*
 * vfp.c — see vfp.h for the fidelity/scope notes and encoding
 * confidence levels.
 */

#include <string.h>
#include <math.h>
#include "arm1176.h"
#include "arm1176_internal.h"

#define VBF(instr, hi, lo) (((instr) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1))

/* ------------------------------------------------------------------ */
/* Reset                                                                */
/* ------------------------------------------------------------------ */

void vfp_reset(arm1176_t *cpu)
{
    vfp_t *v = &cpu->vfp;
    memset(v, 0, sizeof(*v));
    /* FPSID: implementer=ARM(0x41), reserved bits, VFPv2 architecture
     * (bits19:16=0010), part number, variant/revision. Value chosen to
     * plausibly identify as an ARM1176-class VFPv2 unit; software that
     * actually branches on exact FPSID bits is rare. */
    v->fpsid = 0x41022000u;
    v->fpexc = 0; /* VFP starts disabled; software must set FPEXC.EN */
}

/* ------------------------------------------------------------------ */
/* Register access helpers                                             */
/* ------------------------------------------------------------------ */

static inline float get_s(vfp_t *v, int n) { float f; memcpy(&f, &v->regs[n], 4); return f; }
static inline void set_s(vfp_t *v, int n, float f) { memcpy(&v->regs[n], &f, 4); }

static inline double get_d(vfp_t *v, int n)
{
    uint64_t bits = ((uint64_t)v->regs[2 * n + 1] << 32) | v->regs[2 * n];
    double d; memcpy(&d, &bits, 8); return d;
}
static inline void set_d(vfp_t *v, int n, double d)
{
    uint64_t bits; memcpy(&bits, &d, 8);
    v->regs[2 * n] = (uint32_t)bits;
    v->regs[2 * n + 1] = (uint32_t)(bits >> 32);
}

static inline void set_nzcv(vfp_t *v, bool n, bool z, bool c, bool ov)
{
    v->fpscr = (v->fpscr & 0x0FFFFFFFu)
             | (n ? FPSCR_N : 0) | (z ? FPSCR_Z : 0)
             | (c ? FPSCR_C : 0) | (ov ? FPSCR_V : 0);
}

/* FPCompare per the ARM ARM: unordered (either operand NaN) forces
 * N=0,Z=0,C=1,V=1 and (for the signaling form, or any signaling NaN)
 * sets the Invalid Operation cumulative flag. */
static void compare_f32(vfp_t *v, float a, float b, bool signal_nans)
{
    if (isnan(a) || isnan(b)) {
        set_nzcv(v, false, false, true, true);
        if (signal_nans) v->fpscr |= FPSCR_IOC;
        return;
    }
    if (a == b) set_nzcv(v, false, true, true, false);
    else if (a < b) set_nzcv(v, true, false, false, false);
    else set_nzcv(v, false, false, true, false);
}
static void compare_f64(vfp_t *v, double a, double b, bool signal_nans)
{
    if (isnan(a) || isnan(b)) {
        set_nzcv(v, false, false, true, true);
        if (signal_nans) v->fpscr |= FPSCR_IOC;
        return;
    }
    if (a == b) set_nzcv(v, false, true, true, false);
    else if (a < b) set_nzcv(v, true, false, false, false);
    else set_nzcv(v, false, false, true, false);
}

/* ------------------------------------------------------------------ */
/* FPEXC.EN gating — shared by every VFP entry point except VMRS/VMSR  */
/* to FPEXC itself (which must remain accessible to let software turn  */
/* the unit on in the first place).                                    */
/* ------------------------------------------------------------------ */

static bool vfp_enabled_or_trap(arm1176_t *cpu)
{
    if (cpu->vfp.fpexc & FPEXC_EN) return true;
    arm1176_enter_exception(cpu, ARM_MODE_UND, VEC_UNDEF, cpu->current_instr_addr + 4, true, false);
    return false;
}

/* ------------------------------------------------------------------ */
/* MRC/MCR-shaped: VMOV(core<->single) [opc1==000], VMRS/VMSR [opc1==111]*/
/* ------------------------------------------------------------------ */

int vfp_reg_transfer(arm1176_t *cpu, uint32_t instr)
{
    uint32_t opc1 = VBF(instr, 23, 21);
    bool l = VBF(instr, 20, 20);
    uint32_t crn = VBF(instr, 19, 16);
    uint32_t rt = VBF(instr, 15, 12);

    if (opc1 == 0x7) {
        /* VMRS/VMSR: crn selects the system register. */
        bool is_fpexc = (crn == 8);
        if (!is_fpexc && !vfp_enabled_or_trap(cpu)) return 3;

        if (l) { /* VMRS Rt, <spec_reg> */
            uint32_t val;
            switch (crn) {
                case 0: val = cpu->vfp.fpsid; break;
                case 1: val = cpu->vfp.fpscr; break;
                case 8: val = cpu->vfp.fpexc; break;
                default: val = 0; break;
            }
            if (rt == 15) cpu->cpsr = (cpu->cpsr & 0x0FFFFFFFu) | (val & 0xF0000000u);
            else reg_write(cpu, rt, val);
        } else { /* VMSR <spec_reg>, Rt */
            uint32_t val = reg_read(cpu, rt);
            switch (crn) {
                case 1: cpu->vfp.fpscr = val; break;
                case 8: cpu->vfp.fpexc = val; break;
                default: break; /* FPSID is read-only: write ignored */
            }
        }
        return 3;
    }

    if (opc1 == 0x0) {
        /* VMOV (core<->single): n = Vn:N */
        if (!vfp_enabled_or_trap(cpu)) return 3;
        bool n_bit = VBF(instr, 7, 7);
        uint32_t sn = (crn << 1) | n_bit;
        if (l) reg_write(cpu, rt, cpu->vfp.regs[sn]);
        else   cpu->vfp.regs[sn] = reg_read(cpu, rt);
        return 3;
    }

    return 1; /* other opc1 values in this shape aren't defined for VFP */
}

/* ------------------------------------------------------------------ */
/* MRRC/MCRR-shaped: VMOV (2 core regs <-> double)                     */
/* ------------------------------------------------------------------ */

int vfp_two_reg_transfer(arm1176_t *cpu, uint32_t instr)
{
    if (!vfp_enabled_or_trap(cpu)) return 3;

    bool op = VBF(instr, 20, 20); /* op==1: to core regs; op==0: from core regs */
    uint32_t rt2 = VBF(instr, 19, 16);
    uint32_t rt = VBF(instr, 15, 12);
    bool m_bit = VBF(instr, 5, 5);
    uint32_t vm = VBF(instr, 3, 0);
    uint32_t dm = (m_bit << 4) | vm; /* M:Vm */

    if (dm >= 16) return 3; /* VFPv2 only has D0-D15; higher indices don't exist on this core */

    if (op) {
        uint32_t lo = cpu->vfp.regs[2 * dm], hi = cpu->vfp.regs[2 * dm + 1];
        reg_write(cpu, rt, lo);
        reg_write(cpu, rt2, hi);
    } else {
        cpu->vfp.regs[2 * dm]     = reg_read(cpu, rt);
        cpu->vfp.regs[2 * dm + 1] = reg_read(cpu, rt2);
    }
    return 3;
}

/* ------------------------------------------------------------------ */
/* LDC/STC-shaped: VLDR/VSTR (single transfer) and VLDM/VSTM (multiple)*/
/* ------------------------------------------------------------------ */

int vfp_load_store(arm1176_t *cpu, uint32_t instr)
{
    if (!vfp_enabled_or_trap(cpu)) return 3;

    bool p = VBF(instr, 24, 24), u = VBF(instr, 23, 23), d_bit = VBF(instr, 22, 22);
    bool w = VBF(instr, 21, 21), l = VBF(instr, 20, 20);
    uint32_t rn = VBF(instr, 19, 16), vd = VBF(instr, 15, 12);
    uint32_t coproc = VBF(instr, 11, 8);
    bool single = (coproc == 0xA);
    uint32_t imm8 = VBF(instr, 7, 0);

    uint32_t base = (rn == 15) ? (reg_read(cpu, 15) & ~3u) : reg_read(cpu, rn);

    if (p && !w) {
        /* VLDR/VSTR: single register, no writeback, address always
         * base +/- imm8*4 (computed once, never affects Rn). */
        uint32_t addr = u ? base + imm8 * 4 : base - imm8 * 4;
        int d = single ? (int)((vd << 1) | d_bit) : (int)((d_bit << 4) | vd);
        if (single) {
            if (l) { uint32_t val = bus_read32(cpu, addr); ABORT_CHECK_RETURN(cpu, 4); cpu->vfp.regs[d] = val; }
            else   { bus_write32(cpu, addr, cpu->vfp.regs[d]); ABORT_CHECK_RETURN(cpu, 4); }
        } else {
            if (d >= 16) return 4;
            if (l) {
                uint32_t lo = bus_read32(cpu, addr); ABORT_CHECK_RETURN(cpu, 4);
                uint32_t hi = bus_read32(cpu, addr + 4); ABORT_CHECK_RETURN(cpu, 4);
                cpu->vfp.regs[2 * d] = lo; cpu->vfp.regs[2 * d + 1] = hi;
            } else {
                bus_write32(cpu, addr, cpu->vfp.regs[2 * d]); ABORT_CHECK_RETURN(cpu, 4);
                bus_write32(cpu, addr + 4, cpu->vfp.regs[2 * d + 1]); ABORT_CHECK_RETURN(cpu, 4);
            }
        }
        return 4;
    }

    /* VLDM/VSTM: register list starting at (single?Vd:D:D:Vd), count from
     * imm8 (single: imm8 regs; double: imm8/2 regs). Address walks from
     * base (U=1, IA) or base-imm32 (U=0, DB) — matches the architecture
     * pseudocode, which doesn't need a separate P check here since the
     * only valid PUW combinations left at this point are IA/DB. */
    int d = single ? (int)((vd << 1) | d_bit) : (int)((d_bit << 4) | vd);
    uint32_t imm32 = imm8 * 4;
    uint32_t addr = u ? base : base - imm32;
    int regs = single ? (int)imm8 : (int)(imm8 / 2);

    for (int r = 0; r < regs; r++) {
        if (single) {
            int sn = d + r;
            if (l) { uint32_t val = bus_read32(cpu, addr); if (cpu->abort_taken) return 1 + regs; cpu->vfp.regs[sn] = val; }
            else   { bus_write32(cpu, addr, cpu->vfp.regs[sn]); if (cpu->abort_taken) return 1 + regs; }
            addr += 4;
        } else {
            int dn = d + r;
            if (dn >= 16) break;
            if (l) {
                uint32_t lo = bus_read32(cpu, addr); if (cpu->abort_taken) return 1 + regs;
                uint32_t hi = bus_read32(cpu, addr + 4); if (cpu->abort_taken) return 1 + regs;
                cpu->vfp.regs[2 * dn] = lo; cpu->vfp.regs[2 * dn + 1] = hi;
            } else {
                bus_write32(cpu, addr, cpu->vfp.regs[2 * dn]); if (cpu->abort_taken) return 1 + regs;
                bus_write32(cpu, addr + 4, cpu->vfp.regs[2 * dn + 1]); if (cpu->abort_taken) return 1 + regs;
            }
            addr += 8;
        }
    }

    if (w) reg_write(cpu, rn, u ? base + imm32 : base - imm32);
    return 1 + regs;
}

/* ------------------------------------------------------------------ */
/* CDP-shaped: VFP data processing                                     */
/*                                                                      */
/* opc1 = bits[23,21,20] (D at bit22 is a register-extension bit, not  */
/* part of the opcode) selects the operation family; bit6 usually      */
/* picks between two related ops in that family (e.g. VADD/VSUB). The  */
/* "1x11" family (bits23,21,20 = 1,1,1) is the unary/compare/convert   */
/* group, further selected by the Vn field ("opc2") and bit7.          */
/* ------------------------------------------------------------------ */

int vfp_data_processing(arm1176_t *cpu, uint32_t instr)
{
    if (!vfp_enabled_or_trap(cpu)) return 3;

    vfp_t *v = &cpu->vfp;
    bool d_bit = VBF(instr, 22, 22);
    uint32_t vn = VBF(instr, 19, 16);
    uint32_t vd = VBF(instr, 15, 12);
    uint32_t size = VBF(instr, 9, 8); /* 10=single, 11=double (01=half, not supported) */
    bool n_bit = VBF(instr, 7, 7);
    bool bit6 = VBF(instr, 6, 6);
    bool m_bit = VBF(instr, 5, 5);
    uint32_t vm = VBF(instr, 3, 0);
    bool dbl = (size == 3);

    uint32_t opc1_3 = (VBF(instr, 23, 23) << 2) | (VBF(instr, 21, 21) << 1) | VBF(instr, 20, 20);

    int dd = dbl ? (int)((d_bit << 4) | vd) : (int)((vd << 1) | d_bit);
    int dn = dbl ? (int)((n_bit << 4) | vn) : (int)((vn << 1) | n_bit);
    int dm = dbl ? (int)((m_bit << 4) | vm) : (int)((vm << 1) | m_bit);

    /* ---- The "1x11" unary/compare/convert group ---- */
    if (opc1_3 == 0x7) {
        uint32_t opc2 = vn;

        if (opc2 == 0x0 && !bit6) { /* VMOV (register copy) */
            if (dbl) set_d(v, dd, get_d(v, dm)); else set_s(v, dd, get_s(v, dm));
            return 1;
        }
        if (opc2 == 0x0 && bit6) { /* VABS */
            if (dbl) set_d(v, dd, fabs(get_d(v, dm))); else set_s(v, dd, fabsf(get_s(v, dm)));
            return 1;
        }
        if (opc2 == 0x1 && !bit6) { /* VNEG */
            if (dbl) set_d(v, dd, -get_d(v, dm)); else set_s(v, dd, -get_s(v, dm));
            return 1;
        }
        if (opc2 == 0x1 && bit6) { /* VSQRT */
            if (dbl) set_d(v, dd, sqrt(get_d(v, dm))); else set_s(v, dd, sqrtf(get_s(v, dm)));
            return 3;
        }
        if (opc2 == 0x4 || opc2 == 0x5) { /* VCMP / VCMPE, reg or #0.0 */
            bool with_zero = (opc2 == 0x5);
            bool signal_nans = n_bit; /* bit7 is "E" in this sub-shape */
            if (dbl) compare_f64(v, get_d(v, dd), with_zero ? 0.0 : get_d(v, dm), signal_nans);
            else     compare_f32(v, get_s(v, dd), with_zero ? 0.0f : get_s(v, dm), signal_nans);
            return 1;
        }
        if (opc2 == 0x7 && n_bit && bit6) { /* VCVT between single and double */
            if (dbl) set_s(v, dd, (float)get_d(v, dm));  /* size==11: double->single, dd uses single numbering */
            else     set_d(v, dd, (double)get_s(v, dm)); /* size==10: single->double, dd uses double numbering */
            return 3;
        }
        if (opc2 == 0x8) { /* VCVT integer <-> float, opc2==000 handled via to_integer=false path: int->float */
            bool to_unsigned = !n_bit; /* op bit (bit7) here: op==0 unsigned, op==1 signed (see vfp.h notes) */
            uint32_t src = dbl ? v->regs[2 * dm] : v->regs[dm]; /* source is always a 32-bit int in a single-sized slot */
            double result = to_unsigned ? (double)(uint32_t)src : (double)(int32_t)src;
            if (dbl) set_d(v, dd, result); else set_s(v, dd, (float)result);
            return 3;
        }
        if ((opc2 & 0xE) == 0xC) { /* opc2 == 110x or 111x: VCVT float -> integer */
            bool to_unsigned = !(opc2 & 1);
            bool round_zero = n_bit; /* "op" bit7: 1 = round toward zero (the common VCVT form) */
            double src = dbl ? get_d(v, dm) : (double)get_s(v, dm);
            double rounded = round_zero ? trunc(src) : round(src); /* FPSCR-rounding-mode case approximated as round-to-nearest */
            int32_t result;
            if (to_unsigned) {
                if (rounded < 0) result = 0;
                else if (rounded > 4294967295.0) result = (int32_t)0xFFFFFFFFu;
                else result = (int32_t)(uint32_t)rounded;
            } else {
                if (rounded < -2147483648.0) result = INT32_MIN;
                else if (rounded > 2147483647.0) result = INT32_MAX;
                else result = (int32_t)rounded;
            }
            /* Destination is always a single-precision-sized slot, even
             * for the double-precision source variant. */
            v->regs[dbl ? 2 * dd : dd] = (uint32_t)result;
            return 3;
        }
        return 1; /* unimplemented sub-opcode in this group (e.g. VCVTR/half-precision variants) */
    }

    /* ---- Binary arithmetic families ---- */
    switch (opc1_3) {
        case 0x0: /* VMLA (bit6=0) / VMLS (bit6=1) */
            if (dbl) { double r = get_d(v,dn)*get_d(v,dm); set_d(v,dd, bit6 ? get_d(v,dd)-r : get_d(v,dd)+r); }
            else     { float r = get_s(v,dn)*get_s(v,dm);  set_s(v,dd, bit6 ? get_s(v,dd)-r : get_s(v,dd)+r); }
            return 3;
        case 0x1: /* VNMLS (bit6=0) / VNMLA (bit6=1) — bit6 polarity here is best-effort, see vfp.h */
            if (dbl) { double r = get_d(v,dn)*get_d(v,dm); set_d(v,dd, bit6 ? -(get_d(v,dd)+r) : -get_d(v,dd)+r); }
            else     { float r = get_s(v,dn)*get_s(v,dm);  set_s(v,dd, bit6 ? -(get_s(v,dd)+r) : -get_s(v,dd)+r); }
            return 3;
        case 0x2: /* VMUL (bit6=0) / VNMUL (bit6=1) */
            if (dbl) { double r = get_d(v,dn)*get_d(v,dm); set_d(v,dd, bit6 ? -r : r); }
            else     { float r = get_s(v,dn)*get_s(v,dm);  set_s(v,dd, bit6 ? -r : r); }
            return 3;
        case 0x3: /* VADD (bit6=0) / VSUB (bit6=1) */
            if (dbl) set_d(v,dd, bit6 ? get_d(v,dn)-get_d(v,dm) : get_d(v,dn)+get_d(v,dm));
            else     set_s(v,dd, bit6 ? get_s(v,dn)-get_s(v,dm) : get_s(v,dn)+get_s(v,dm));
            return 3;
        case 0x4: /* VDIV */
            if (dbl) set_d(v,dd, get_d(v,dn)/get_d(v,dm));
            else     set_s(v,dd, get_s(v,dn)/get_s(v,dm));
            return 15; /* division is genuinely slower on real hardware; approximate */
        default:
            return 1; /* VFMA/VFNMA family (0x5,0x6): VFPv4+, not present on VFPv2 */
    }
}
