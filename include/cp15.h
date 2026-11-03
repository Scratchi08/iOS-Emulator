/*
 * cp15.h — CP15 system control coprocessor for the ARM1176JZF-S core:
 * identification registers, the VMSAv6 MMU (short-descriptor format),
 * cache maintenance operations, and Tightly-Coupled Memory (TCM).
 *
 * Scope / fidelity notes (see also the longer discussion in README.md):
 *
 *   - MMU: VMSAv6 "short descriptor" format, subpages-disabled mode
 *     (SCTLR.XP is tracked but the extended AP[2]/APX permission model
 *     it selects is NOT implemented — we always use the classic 2-bit
 *     AP permission table). This matches what essentially all ARMv6-era
 *     OS kernels used in practice, XNU included.
 *   - Supersections (16MB) are not implemented — treated as a
 *     translation fault if a first-level descriptor sets the
 *     supersection bit.
 *   - The large-page (64KB) descriptor field layout is a best-effort
 *     reconstruction and is lower-confidence than the section and
 *     small-page layouts; flagged at the point of use. Small pages
 *     (4KB, the overwhelmingly common case) and sections (1MB) are
 *     implemented with high confidence, matching the ARMv6 hardware PTE
 *     bit layout used by Linux's arch/arm short-descriptor header.
 *   - No ASID tagging: the TLB and DACR/permission checks don't
 *     distinguish address spaces, so a context switch should invalidate
 *     the whole TLB (which any correct OS does anyway via c8 ops).
 *   - Cache maintenance (c7) and TLB maintenance (c8) operations are
 *     accepted and update the (trivial) TLB/no-op cache model, but no
 *     timing or real cache-line state is modeled — memory is always
 *     coherent in this emulator, so "clean"/"invalidate" have nothing
 *     to actually do beyond keeping software from getting stuck
 *     spin-waiting on a completion bit we don't model.
 *   - FCSE (Fast Context Switch Extension) is not implemented; FCSEIDR
 *     is stored but doesn't affect translation.
 */

#ifndef CP15_H
#define CP15_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declaration only — avoids a circular include with arm1176.h,
 * which is the one that defines the full `struct ARM1176` / arm1176_t. */
struct ARM1176;

#define CP15_TLB_ENTRIES 64

typedef struct {
    bool     valid;
    uint32_t va_page;   /* VA >> 12 */
    uint32_t pa_page;   /* PA >> 12 << 12 (page-aligned physical base) */
    uint8_t  ap;         /* 2-bit classic access permission            */
    uint8_t  domain;
    bool     xn;
    bool     is_section; /* bookkeeping only, doesn't affect lookup    */
} cp15_tlb_entry_t;

typedef struct {
    /* c0 — identification (read-only). Values below are the
     * documented ARM1176JZF-S r0p7 values (as used in e.g. the
     * Raspberry Pi 1's BCM2835); the real S5L8900 may be a slightly
     * different revision, but this is close enough for OS code that
     * just checks the implementer/part number. */
    uint32_t midr;   /* Main ID Register:      0x410FB767 */
    uint32_t ctr;    /* Cache Type Register:    0x1D152152 */
    uint32_t tcmtr;  /* TCM Type Register: reports ITCM/DTCM presence */
    uint32_t tlbtr;  /* TLB Type Register */
    uint32_t id_pfr0, id_pfr1;
    uint32_t id_mmfr0, id_mmfr1, id_mmfr2, id_mmfr3;

    /* c1 — control */
    uint32_t sctlr;  /* System Control Register */
    uint32_t actlr;  /* Auxiliary Control Register (impl. defined, stored only) */
    uint32_t cpacr;  /* Coprocessor Access Control (for future VFP work) */

    /* c2 — translation table base */
    uint32_t ttbr0, ttbr1;
    uint32_t ttbcr;

    /* c3 — domain access control */
    uint32_t dacr;

    /* c5 — fault status */
    uint32_t dfsr, ifsr;

    /* c6 — fault address */
    uint32_t dfar, ifar;

    /* c9 — TCM region config. Base/size come from the region registers;
     * the backing store is allocated lazily (and lazily resized) when
     * software programs a size. */
    uint32_t itcm_region_reg, dtcm_region_reg;
    bool     itcm_enabled, dtcm_enabled;
    uint32_t itcm_base, itcm_size;
    uint32_t dtcm_base, dtcm_size;
    uint8_t  *itcm, *dtcm;

    /* c13 — process/context/thread ID registers (stored, not used to
     * affect translation — no FCSE). */
    uint32_t fcseidr, contextidr;
    uint32_t tpidrurw, tpidruro, tpidrprw;

    /* Simple direct-mapped TLB, indexed by (VA>>12) % CP15_TLB_ENTRIES. */
    cp15_tlb_entry_t tlb[CP15_TLB_ENTRIES];
} cp15_t;

/* SCTLR bit positions actually consulted by this implementation. Many
 * other bits exist on real hardware and are stored but not acted upon
 * (documented in cp15.c next to the MCR handler for c1). */
#define SCTLR_M   (1u << 0)   /* MMU enable                       */
#define SCTLR_A   (1u << 1)   /* alignment fault checking (not enforced, TODO) */
#define SCTLR_C   (1u << 2)   /* data cache enable (no-op)        */
#define SCTLR_I   (1u << 12)  /* instruction cache enable (no-op) */
#define SCTLR_V   (1u << 13)  /* high exception vectors (0xFFFF0000) */
#define SCTLR_XP  (1u << 23)  /* extended page tables (tracked, not applied — see file header) */

void     cp15_reset(struct ARM1176 *cpu);

/* MRC/MCR dispatch, called from the ARM decoder's coprocessor-space
 * handler for coproc == 15. */
uint32_t cp15_mrc(struct ARM1176 *cpu, uint32_t crn, uint32_t opc1, uint32_t crm, uint32_t opc2);
void     cp15_mcr(struct ARM1176 *cpu, uint32_t crn, uint32_t opc1, uint32_t crm, uint32_t opc2, uint32_t value);

/* MMU/TCM-aware memory access. These are what the ARM/Thumb decoders
 * and the fetch loop actually call (as bus_read, bus_write, and
 * fetch_read in arm1176_internal.h, which map onto these). On a translation fault
 * they set the appropriate FSR/FAR, raise the abort exception via
 * arm1176_enter_exception, set cpu->abort_taken = true, and return a
 * dummy value (0) for reads. Callers must check cpu->abort_taken after
 * any of these and stop processing the current instruction if set. */
uint32_t cp15_mem_read32(struct ARM1176 *cpu, uint32_t vaddr);
uint16_t cp15_mem_read16(struct ARM1176 *cpu, uint32_t vaddr);
uint8_t  cp15_mem_read8(struct ARM1176 *cpu, uint32_t vaddr);
void     cp15_mem_write32(struct ARM1176 *cpu, uint32_t vaddr, uint32_t val);
void     cp15_mem_write16(struct ARM1176 *cpu, uint32_t vaddr, uint16_t val);
void     cp15_mem_write8(struct ARM1176 *cpu, uint32_t vaddr, uint8_t val);

uint32_t cp15_fetch32(struct ARM1176 *cpu, uint32_t vaddr);
uint16_t cp15_fetch16(struct ARM1176 *cpu, uint32_t vaddr);

#endif /* CP15_H */
