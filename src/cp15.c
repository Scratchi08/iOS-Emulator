/*
 * cp15.c — see cp15.h for the fidelity/scope notes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm1176.h"
#include "arm1176_internal.h"

/* ------------------------------------------------------------------ */
/* Fault status codes (FS[3:0] field of DFSR/IFSR). These four values  */
/* are unchanged from ARMv4 through ARMv6 and are the ones any real OS */
/* fault handler actually branches on.                                 */
/* ------------------------------------------------------------------ */
#define FSR_TRANSLATION_SECTION 0x5
#define FSR_TRANSLATION_PAGE    0x7
#define FSR_DOMAIN_SECTION      0x9
#define FSR_DOMAIN_PAGE         0xB
#define FSR_PERM_SECTION        0xD
#define FSR_PERM_PAGE           0xF

/* ------------------------------------------------------------------ */
/* Reset                                                                */
/* ------------------------------------------------------------------ */

void cp15_reset(arm1176_t *cpu)
{
    cp15_t *c = &cpu->cp15;
    uint8_t *itcm = c->itcm, *dtcm = c->dtcm; /* preserve TCM allocations across reset */
    uint32_t itcm_size = c->itcm_size, dtcm_size = c->dtcm_size;

    memset(c, 0, sizeof(*c));

    c->itcm = itcm; c->itcm_size = itcm_size;
    c->dtcm = dtcm; c->dtcm_size = dtcm_size;

    /* Documented ARM1176JZF-S r0p7 identification values (see cp15.h). */
    c->midr = 0x410FB767u;
    c->ctr  = 0x1D152152u;

    /* TCM Type Register: bit[31] and bit[15] indicate ITCM/DTCM bank 0
     * present; we report "one bank of each, size determined by the
     * region registers" rather than a fixed hardware size, since real
     * ARM1176 TCM size is a synthesis-time option anyway. */
    c->tcmtr = 0x00010001u;

    /* TLB Type Register: report a small unified TLB, no lockdown. */
    c->tlbtr = 0x00000000u;

    /* SCTLR at reset: MMU off, caches off, low vectors — matches real
     * hardware; iBoot/the kernel is responsible for configuring this. */
    c->sctlr = 0;
    c->ttbcr = 0; /* N=0: TTBR0 covers the whole address space */
}

/* ------------------------------------------------------------------ */
/* TCM backing store management                                        */
/* ------------------------------------------------------------------ */

/* c9 TCM region register format used by this emulator:
 *   bits[31:12] base address (4KB aligned)
 *   bits[11:7]  reserved, SBZ
 *   bits[6:2]   size field: size_bytes = 1 << (field + 9), field >= 3
 *               (so field=3 -> 4KB, field=4 -> 8KB, ... field=23 -> 4GB)
 *   bit[1]      reserved, SBZ
 *   bit[0]      enable
 *
 * NOTE: this bit layout is a best-effort reconstruction from general
 * ARM TCM region register documentation (the size-field encoding
 * formula itself, size = 2^(N+9), is the well-documented part; the
 * exact bit *positions* for the ARM1176JZF-S specifically should be
 * double-checked against the real TRM if bit-exact compatibility with
 * real iBoot TCM setup code ever matters). Since our own test/board
 * code is the only thing writing this register for now, correctness
 * here is self-consistent even if it doesn't match real silicon
 * exactly.
 */

static uint32_t tcm_decode_size(uint32_t region_reg)
{
    uint32_t field = (region_reg >> 2) & 0x1F;
    if (field < 3) return 0;
    if (field > 23) field = 23; /* clamp: avoid absurd allocations from garbage writes */
    return 1u << (field + 9);
}

static void tcm_reconfigure(uint8_t **buf, uint32_t *cur_size, uint32_t new_size)
{
    if (new_size == *cur_size && (*buf != NULL || new_size == 0)) return;
    free(*buf);
    *buf = new_size ? calloc(1, new_size) : NULL;
    *cur_size = *buf ? new_size : 0;
}

static inline bool itcm_hit(cp15_t *c, uint32_t va, uint32_t *offset)
{
    if (!c->itcm_enabled || !c->itcm) return false;
    if (va < c->itcm_base || (uint64_t)va >= (uint64_t)c->itcm_base + c->itcm_size) return false;
    *offset = va - c->itcm_base;
    return true;
}

static inline bool dtcm_hit(cp15_t *c, uint32_t va, uint32_t *offset)
{
    if (!c->dtcm_enabled || !c->dtcm) return false;
    if (va < c->dtcm_base || (uint64_t)va >= (uint64_t)c->dtcm_base + c->dtcm_size) return false;
    *offset = va - c->dtcm_base;
    return true;
}

/* Little-endian packing helpers for the TCM byte-array backing store
 * (the system bus does its own endianness handling; TCM is internal
 * to the core so we do it inline here instead of routing through a
 * second bus_t indirection). */
static inline uint32_t le32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }
static inline void put_le32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static inline void put_le16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

/* ------------------------------------------------------------------ */
/* Permission checking                                                  */
/* ------------------------------------------------------------------ */

static inline bool is_privileged(arm1176_t *cpu)
{
    return (cpu->cpsr & CPSR_MODE_MASK) != ARM_MODE_USR;
}

/* Classic 2-bit AP permission table (legacy S/R bits not modeled — see
 * cp15.h). Returns whether the given access is permitted. */
static bool check_ap(uint32_t ap, bool privileged, bool is_write)
{
    switch (ap & 3) {
        case 0: return false;                          /* no access          */
        case 1: return privileged;                      /* priv RW, user none */
        case 2: return privileged || !is_write;          /* priv RW, user RO   */
        case 3: return true;                             /* full access        */
        default: return false;
    }
}

static void raise_fault(arm1176_t *cpu, uint32_t va, bool is_fetch, bool is_write,
                         uint32_t fs, uint32_t domain)
{
    cpu->abort_taken = true;
    if (is_fetch) {
        cpu->cp15.ifsr = fs & 0xF;
        cpu->cp15.ifar = va;
        arm1176_enter_exception(cpu, ARM_MODE_ABT, VEC_PREFETCH,
                                 cpu->current_instr_addr + 4, true, false);
    } else {
        uint32_t dfsr = (fs & 0xF) | ((domain & 0xF) << 4) | (is_write ? (1u << 11) : 0);
        cpu->cp15.dfsr = dfsr;
        cpu->cp15.dfar = va;
        arm1176_enter_exception(cpu, ARM_MODE_ABT, VEC_DATA_ABORT,
                                 cpu->current_instr_addr + 8, true, false);
    }
}

/* ------------------------------------------------------------------ */
/* VMSAv6 short-descriptor page table walk                             */
/* ------------------------------------------------------------------ */

static bool translate(arm1176_t *cpu, uint32_t va, bool is_write, bool is_fetch, uint32_t *out_pa)
{
    cp15_t *c = &cpu->cp15;

    if (!(c->sctlr & SCTLR_M)) { *out_pa = va; return true; } /* MMU off: identity map */

    /* TLB lookup first. */
    uint32_t page = va >> 12;
    uint32_t idx = page % CP15_TLB_ENTRIES;
    cp15_tlb_entry_t *e = &c->tlb[idx];
    if (e->valid && e->va_page == page) {
        uint32_t dacr_bits = (c->dacr >> (e->domain * 2)) & 3;
        if (dacr_bits == 0) {
            raise_fault(cpu, va, is_fetch, is_write,
                        e->is_section ? FSR_DOMAIN_SECTION : FSR_DOMAIN_PAGE, e->domain);
            return false;
        }
        if (dacr_bits != 3) { /* 3 = manager: bypass permission checks */
            if (!check_ap(e->ap, is_privileged(cpu), is_write) || (is_fetch && e->xn)) {
                raise_fault(cpu, va, is_fetch, is_write,
                            e->is_section ? FSR_PERM_SECTION : FSR_PERM_PAGE, e->domain);
                return false;
            }
        }
        *out_pa = e->pa_page | (va & 0xFFFu);
        return true;
    }

    /* Select TTBR0 vs TTBR1 per TTBCR.N (0 => always TTBR0, matching
     * ARMv5-compatible single-table setups). */
    uint32_t ttbr = c->ttbr0;
    uint32_t n = c->ttbcr & 0x7;
    if (n != 0) {
        uint32_t boundary = 1u << (32 - n);
        if (va >= boundary) ttbr = c->ttbr1;
    }

    uint32_t l1_addr = (ttbr & 0xFFFFC000u) | ((va >> 20) << 2);
    uint32_t l1 = phys_read32(cpu, l1_addr);
    uint32_t l1_type = l1 & 3;

    if (l1_type == 0 || l1_type == 3) {
        raise_fault(cpu, va, is_fetch, is_write, FSR_TRANSLATION_SECTION, 0);
        return false;
    }

    if (l1_type == 2) {
        /* Section (1MB) — or supersection (16MB) if bit18 is set, which
         * we don't implement (see cp15.h). */
        if (l1 & (1u << 18)) {
            raise_fault(cpu, va, is_fetch, is_write, FSR_TRANSLATION_SECTION, 0);
            return false;
        }
        uint32_t domain = (l1 >> 5) & 0xF;
        uint32_t dacr_bits = (c->dacr >> (domain * 2)) & 3;
        if (dacr_bits == 0) {
            raise_fault(cpu, va, is_fetch, is_write, FSR_DOMAIN_SECTION, domain);
            return false;
        }
        uint32_t ap = (l1 >> 10) & 3;
        bool xn = (l1 >> 4) & 1;
        if (dacr_bits != 3 && (!check_ap(ap, is_privileged(cpu), is_write) || (is_fetch && xn))) {
            raise_fault(cpu, va, is_fetch, is_write, FSR_PERM_SECTION, domain);
            return false;
        }
        uint32_t pa = (l1 & 0xFFF00000u) | (va & 0x000FFFFFu);

        e->valid = true; e->va_page = page; e->pa_page = pa & ~0xFFFu;
        e->ap = (uint8_t)ap; e->domain = (uint8_t)domain; e->xn = xn; e->is_section = true;
        *out_pa = pa;
        return true;
    }

    /* l1_type == 1: coarse page table (second-level lookup). */
    uint32_t domain = (l1 >> 5) & 0xF;
    uint32_t dacr_bits = (c->dacr >> (domain * 2)) & 3;
    if (dacr_bits == 0) {
        raise_fault(cpu, va, is_fetch, is_write, FSR_DOMAIN_PAGE, domain);
        return false;
    }

    uint32_t l2_addr = (l1 & 0xFFFFFC00u) | (((va >> 12) & 0xFFu) << 2);
    uint32_t l2 = phys_read32(cpu, l2_addr);
    uint32_t l2_type = l2 & 3;

    if (l2_type == 0) {
        raise_fault(cpu, va, is_fetch, is_write, FSR_TRANSLATION_PAGE, domain);
        return false;
    }

    uint32_t ap, pa;
    bool xn;
    if (l2_type == 1) {
        /* Large page (64KB). Field layout here is a best-effort
         * reconstruction (see cp15.h) — small pages and sections are
         * the well-verified paths; flag this if large-page mappings
         * ever misbehave against real firmware. */
        ap = (l2 >> 4) & 3;
        xn = (l2 >> 15) & 1;
        pa = (l2 & 0xFFFF0000u) | (va & 0x0000FFFFu);
    } else {
        /* Small page (4KB), l2_type 2 or 3 — bit0 is XN in ARMv6
         * (this is the hardware layout Linux's short-descriptor
         * headers call PTE_EXT_*, high confidence). */
        ap = (l2 >> 4) & 3;
        xn = l2 & 1;
        pa = (l2 & 0xFFFFF000u) | (va & 0x00000FFFu);
    }

    if (dacr_bits != 3 && (!check_ap(ap, is_privileged(cpu), is_write) || (is_fetch && xn))) {
        raise_fault(cpu, va, is_fetch, is_write, FSR_PERM_PAGE, domain);
        return false;
    }

    e->valid = true; e->va_page = page; e->pa_page = pa & ~0xFFFu;
    e->ap = (uint8_t)ap; e->domain = (uint8_t)domain; e->xn = xn; e->is_section = false;
    *out_pa = pa;
    return true;
}

/* ------------------------------------------------------------------ */
/* Public memory access entry points                                   */
/* ------------------------------------------------------------------ */

uint32_t cp15_mem_read32(arm1176_t *cpu, uint32_t va)
{
    uint32_t off;
    if (dtcm_hit(&cpu->cp15, va, &off)) return le32(cpu->cp15.dtcm + off);
    uint32_t pa;
    if (!translate(cpu, va, false, false, &pa)) return 0;
    return phys_read32(cpu, pa);
}
uint16_t cp15_mem_read16(arm1176_t *cpu, uint32_t va)
{
    uint32_t off;
    if (dtcm_hit(&cpu->cp15, va, &off)) return le16(cpu->cp15.dtcm + off);
    uint32_t pa;
    if (!translate(cpu, va, false, false, &pa)) return 0;
    return phys_read16(cpu, pa);
}
uint8_t cp15_mem_read8(arm1176_t *cpu, uint32_t va)
{
    uint32_t off;
    if (dtcm_hit(&cpu->cp15, va, &off)) return cpu->cp15.dtcm[off];
    uint32_t pa;
    if (!translate(cpu, va, false, false, &pa)) return 0;
    return phys_read8(cpu, pa);
}
void cp15_mem_write32(arm1176_t *cpu, uint32_t va, uint32_t val)
{
    uint32_t off;
    if (dtcm_hit(&cpu->cp15, va, &off)) { put_le32(cpu->cp15.dtcm + off, val); return; }
    uint32_t pa;
    if (!translate(cpu, va, true, false, &pa)) return;
    phys_write32(cpu, pa, val);
}
void cp15_mem_write16(arm1176_t *cpu, uint32_t va, uint16_t val)
{
    uint32_t off;
    if (dtcm_hit(&cpu->cp15, va, &off)) { put_le16(cpu->cp15.dtcm + off, val); return; }
    uint32_t pa;
    if (!translate(cpu, va, true, false, &pa)) return;
    phys_write16(cpu, pa, val);
}
void cp15_mem_write8(arm1176_t *cpu, uint32_t va, uint8_t val)
{
    uint32_t off;
    if (dtcm_hit(&cpu->cp15, va, &off)) { cpu->cp15.dtcm[off] = val; return; }
    uint32_t pa;
    if (!translate(cpu, va, true, false, &pa)) return;
    phys_write8(cpu, pa, val);
}

uint32_t cp15_fetch32(arm1176_t *cpu, uint32_t va)
{
    uint32_t off;
    if (itcm_hit(&cpu->cp15, va, &off)) return le32(cpu->cp15.itcm + off);
    uint32_t pa;
    if (!translate(cpu, va, false, true, &pa)) return 0;
    return phys_read32(cpu, pa);
}
uint16_t cp15_fetch16(arm1176_t *cpu, uint32_t va)
{
    uint32_t off;
    if (itcm_hit(&cpu->cp15, va, &off)) return le16(cpu->cp15.itcm + off);
    uint32_t pa;
    if (!translate(cpu, va, false, true, &pa)) return 0;
    return phys_read16(cpu, pa);
}

/* ------------------------------------------------------------------ */
/* MRC/MCR dispatch                                                      */
/* ------------------------------------------------------------------ */

static void tlb_invalidate_all(cp15_t *c) { memset(c->tlb, 0, sizeof(c->tlb)); }

static void tlb_invalidate_va(cp15_t *c, uint32_t va)
{
    uint32_t page = va >> 12;
    cp15_tlb_entry_t *e = &c->tlb[page % CP15_TLB_ENTRIES];
    if (e->valid && e->va_page == page) e->valid = false;
}

uint32_t cp15_mrc(arm1176_t *cpu, uint32_t crn, uint32_t opc1, uint32_t crm, uint32_t opc2)
{
    cp15_t *c = &cpu->cp15;
    (void)opc1;

    switch (crn) {
        case 0: /* identification registers */
            if (crm == 0) {
                switch (opc2) {
                    case 0: return c->midr;
                    case 1: return c->ctr;
                    case 2: return c->tcmtr;
                    case 3: return c->tlbtr;
                    default: return 0;
                }
            }
            return 0; /* c0,c1..c7 (feature ID regs): not modeled, RAZ */

        case 1:
            switch (opc2) {
                case 0: return c->sctlr;
                case 1: return c->actlr;
                case 2: return c->cpacr;
                default: return 0;
            }

        case 2:
            switch (opc2) {
                case 0: return c->ttbr0;
                case 1: return c->ttbr1;
                case 2: return c->ttbcr;
                default: return 0;
            }

        case 3: return c->dacr;

        case 5: return (opc2 == 0) ? c->dfsr : c->ifsr;
        case 6: return (opc2 == 0) ? c->dfar : c->ifar;

        case 9:
            /* c9,c1,{0,1}: Data/Instruction TCM Region Registers. */
            if (crm == 1) return (opc2 == 0) ? c->dtcm_region_reg : c->itcm_region_reg;
            return 0;

        case 13:
            switch (opc2) {
                case 0: return c->fcseidr;
                case 1: return c->contextidr;
                case 2: return c->tpidrurw;
                case 3: return c->tpidruro;
                case 4: return c->tpidrprw;
                default: return 0;
            }

        default:
            return 0; /* unimplemented register: RAZ rather than fault */
    }
}

void cp15_mcr(arm1176_t *cpu, uint32_t crn, uint32_t opc1, uint32_t crm, uint32_t opc2, uint32_t value)
{
    cp15_t *c = &cpu->cp15;
    (void)opc1;

    switch (crn) {
        case 1:
            switch (opc2) {
                case 0: c->sctlr = value; break;
                case 1: c->actlr = value; break;
                case 2: c->cpacr = value; break;
                default: break;
            }
            /* Any control-register write can change the memory map's
             * shape (MMU on/off) enough that stale TLB entries would
             * be actively wrong rather than just stale; real hardware
             * doesn't require this, but it keeps our simple model
             * honest without needing precise "TLB entries survive
             * SCTLR.M toggling" semantics. */
            tlb_invalidate_all(c);
            break;

        case 2:
            switch (opc2) {
                case 0: c->ttbr0 = value; break;
                case 1: c->ttbr1 = value; break;
                case 2: c->ttbcr = value; break;
                default: break;
            }
            tlb_invalidate_all(c);
            break;

        case 3:
            c->dacr = value;
            /* Domain permissions are cached in TLB entries; changing
             * DACR can change what's allowed without changing the
             * mapping itself, so drop the cache rather than track it. */
            tlb_invalidate_all(c);
            break;

        case 5:
            if (opc2 == 0) c->dfsr = value; else c->ifsr = value;
            break;

        case 6:
            if (opc2 == 0) c->dfar = value; else c->ifar = value;
            break;

        case 7:
            /* Cache maintenance, barriers, and misc. No real cache is
             * modeled (memory is always coherent here), so every one
             * of these is accepted and treated as a completed no-op —
             * including the barriers (c7,c10,{4,5}: DSB/DMB) and
             * prefetch-buffer flush (c7,c5,4) — rather than faulting,
             * since real OS code executes these unconditionally and
             * must not trap. */
            break;

        case 8:
            /* TLB maintenance. */
            switch (crm) {
                case 5: case 6: case 7: /* invalidate I/D/unified TLB */
                    if (opc2 == 0) tlb_invalidate_all(c);
                    else if (opc2 == 1) tlb_invalidate_va(c, value & ~0xFFFu);
                    else tlb_invalidate_all(c); /* by-ASID: no ASID tagging, so invalidate all */
                    break;
                default: break;
            }
            break;

        case 9:
            /* c9,c1,{0,1}: Data/Instruction TCM Region Registers. */
            if (crm == 1) {
                if (opc2 == 0) {
                    c->dtcm_region_reg = value;
                    c->dtcm_enabled = value & 1;
                    c->dtcm_base = value & 0xFFFFF000u;
                    tcm_reconfigure(&c->dtcm, &c->dtcm_size, tcm_decode_size(value));
                } else if (opc2 == 1) {
                    c->itcm_region_reg = value;
                    c->itcm_enabled = value & 1;
                    c->itcm_base = value & 0xFFFFF000u;
                    tcm_reconfigure(&c->itcm, &c->itcm_size, tcm_decode_size(value));
                }
            }
            break;

        case 13:
            switch (opc2) {
                case 0: c->fcseidr = value; break;
                case 1: c->contextidr = value; tlb_invalidate_all(c); break;
                case 2: c->tpidrurw = value; break;
                case 3: c->tpidruro = value; break;
                case 4: c->tpidrprw = value; break;
                default: break;
            }
            break;

        default:
            break; /* unimplemented register: WI (write ignored) */
    }
}
