/*
 * jazelle.h — Jazelle direct bytecode execution extension.
 *
 * Scope: this implements exactly what the ARM Architecture Reference
 * Manual calls the "trivial implementation" of Jazelle (ARMv6 ARM,
 * section on Jazelle extension support) — which is an ARM-sanctioned,
 * fully spec-compliant way to implement the "J" without any real
 * bytecode acceleration hardware:
 *
 *   - The three CP14 configuration/control registers — JIDR (Jazelle
 *     ID Register), JMCR (Main Configuration Register), and JOSCR
 *     (OS Control Register) — are Read-As-Zero and ignore writes
 *     (RAZ/WI). JIDR reading as zero is what tells software "no real
 *     Jazelle hardware is present, don't try to use it" (a genuine
 *     implementation reports a nonzero architecture/implementer field
 *     here).
 *   - BXJ behaves identically to BX: since JIDR reports no hardware
 *     acceleration, entry to actual Jazelle state never succeeds, so
 *     BXJ always falls back to its documented "otherwise" behavior —
 *     branch to Rm, entering Thumb state if bit0 of Rm is set,
 *     otherwise ARM state.
 *
 * This is not a real Java bytecode interpreter. iPhone OS 1.0 almost
 * certainly never attempts to use Jazelle in practice (it shipped
 * without a JVM), so the trivial implementation should be enough for
 * any real firmware that merely probes for it and moves on — which is
 * exactly the case the ARM ARM designed this fallback for.
 *
 * No persistent state is needed (RAZ/WI has nothing to store), so
 * unlike cp15.c/vfp.c there's no reset function here.
 */

#ifndef JAZELLE_H
#define JAZELLE_H

#include <stdint.h>

struct ARM1176;

/* MRC/MCR dispatch for coproc == 14 (the Jazelle CP14 register space).
 * Every register in this space is RAZ/WI in the trivial
 * implementation, so these don't even need to inspect crn/opc1/opc2 —
 * but the parameters are accepted for symmetry with cp15_mrc/cp15_mcr
 * and in case a future, less-trivial implementation wants them. */
uint32_t jazelle_mrc(struct ARM1176 *cpu, uint32_t crn, uint32_t opc1, uint32_t crm, uint32_t opc2);
void     jazelle_mcr(struct ARM1176 *cpu, uint32_t crn, uint32_t opc1, uint32_t crm, uint32_t opc2, uint32_t value);

/* BXJ Rm. Returns the cycle count, matching the ARM decoder's other
 * exec_* functions. Always falls back to BX semantics (see above). */
int jazelle_bxj(struct ARM1176 *cpu, uint32_t rm_value);

#endif /* JAZELLE_H */
