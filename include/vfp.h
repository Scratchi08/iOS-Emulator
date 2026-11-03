/*
 * vfp.h — VFP11 (VFPv2) floating-point coprocessor for the ARM1176JZF-S
 * core. This is the "F" in ARM1176JZF-S.
 *
 * Scope / fidelity notes:
 *
 *   - Targets VFPv2 as implemented by real ARM1176JZF-S hardware: no
 *     Advanced SIMD/NEON (that's a different, later core family), no
 *     half-precision, no fused multiply-add (VFMA/VFNMA — those are
 *     VFPv4+), no fixed-point <-> float VCVT (that's VFPv3+).
 *   - 32 single-precision registers (S0-S31) physically backing 16
 *     double-precision registers (D0-D15) via the standard aliasing
 *     (Dn = {S(2n+1):S(2n)}).
 *   - Encodings were verified against the ARM Architecture Reference
 *     Manual's instruction encoding tables (not reconstructed from
 *     memory alone) for: VMOV (core<->single), VMOV (2 core<->double),
 *     VMRS/VMSR, VLDR/VSTR, VLDM/VSTM, VADD, VNEG, VCMP/VCMPE, VCVT
 *     (single<->double), VCVT (int<->float). The shared "opc1" family
 *     grouping for the rest of the arithmetic ops (VSUB/VMUL/VNMUL/
 *     VMLA/VMLS/VNMLA/VNMLS/VDIV) was cross-checked from two data
 *     points (VADD, VDIV) plus the well-documented "differ only by
 *     bit6" ADD/SUB pairing pattern; the VMLA/VMLS/VNMLA/VNMLS bit6
 *     polarity (which of each pair is which) is lower-confidence and
 *     flagged at the point of use in vfp.c.
 *   - FPSCR cumulative exception flags (IOC/DZC/OFC/UFC/IXC/IDC) are
 *     only approximately tracked (set for the most obvious cases:
 *     invalid-operation NaNs, division by zero) rather than precisely
 *     matching IEEE 754 semantics on every operation. The NZCV compare
 *     flags (read via FMSTAT / VMRS APSR_nzcv) ARE precise. FZ
 *     (flush-to-zero) and non-default NaN propagation details are not
 *     specially modeled — arithmetic runs on the host FPU via C's
 *     float/double types, which is IEEE 754 compliant for the common
 *     cases but won't bit-exactly match ARM's specific NaN payload or
 *     subnormal-handling rules in every corner case.
 *   - FPEXC.EN gating is implemented (VFP instructions other than
 *     VMRS/VMSR-to-FPEXC itself raise Undefined Instruction when
 *     FPEXC.EN is clear, matching the lazy-FPU-enable trap real OSes
 *     rely on). CPACR (CP15 c1,c0,2) is stored but not enforced —
 *     see cp15.h — so the coarser "FP entirely disabled by the OS"
 *     trap path isn't modeled, only the per-EN-bit one.
 */

#ifndef VFP_H
#define VFP_H

#include <stdint.h>
#include <stdbool.h>

struct ARM1176;

typedef struct {
    uint32_t regs[32];  /* S0..S31; Dn aliases {regs[2n+1]:regs[2n]} */
    uint32_t fpscr;
    uint32_t fpexc;
    uint32_t fpsid;
} vfp_t;

/* FPSCR bit positions actually consulted/set by this implementation. */
#define FPSCR_IOC   (1u << 0)
#define FPSCR_DZC   (1u << 1)
#define FPSCR_OFC   (1u << 2)
#define FPSCR_UFC   (1u << 3)
#define FPSCR_IXC   (1u << 4)
#define FPSCR_IDC   (1u << 7)
#define FPSCR_RMODE_SHIFT 22
#define FPSCR_RMODE_MASK  (3u << FPSCR_RMODE_SHIFT)
#define FPSCR_FZ    (1u << 24)
#define FPSCR_DN    (1u << 25)
#define FPSCR_N     (1u << 31)
#define FPSCR_Z     (1u << 30)
#define FPSCR_C     (1u << 29)
#define FPSCR_V     (1u << 28)

/* FPEXC bit positions. */
#define FPEXC_EN    (1u << 30)
#define FPEXC_EX    (1u << 31)

void vfp_reset(struct ARM1176 *cpu);

/* Called from the ARM decoder's coprocessor-space handler whenever
 * coproc == 10 or 11. Each returns the cycle count as usual; the
 * *_mrc/_mcr and data-processing paths internally check FPEXC.EN and
 * raise Undefined Instruction (via arm1176_enter_exception) when
 * appropriate, exactly like a real disabled-VFP trap. */
int vfp_reg_transfer(struct ARM1176 *cpu, uint32_t instr);   /* MRC/MCR-shaped: VMOV(core<->single), VMRS, VMSR */
int vfp_two_reg_transfer(struct ARM1176 *cpu, uint32_t instr); /* MRRC/MCRR-shaped: VMOV(2 core<->double) */
int vfp_load_store(struct ARM1176 *cpu, uint32_t instr);      /* LDC/STC-shaped: VLDR/VSTR/VLDM/VSTM */
int vfp_data_processing(struct ARM1176 *cpu, uint32_t instr); /* CDP-shaped: VADD/VSUB/VMUL/.../VCVT */

#endif /* VFP_H */
