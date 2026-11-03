/*
 * main.c — smoke tests for the ARM1176 core.
 *
 * Each test hand-assembles a tiny program (ARM or Thumb; encodings
 * verified by hand against the ARM ARM bit layouts) and checks the CPU
 * reaches the expected end state. Not a substitute for a full
 * conformance suite, but it exercises a good cross-section of both
 * instruction sets, including mode switches on the ARM side and the
 * BL two-halfword sequence + push/pop on the Thumb side.
 */

#include <stdio.h>
#include <string.h>
#include "arm1176.h"
#include "test_bus.h"
#include "soc.h"

static void load_words(test_bus_t *tb, uint32_t base, const uint32_t *words, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t addr = base + (uint32_t)(i * 4);
        uint32_t w = words[i];
        tb->mem[addr]     =  w        & 0xFF;
        tb->mem[addr + 1] = (w >> 8)  & 0xFF;
        tb->mem[addr + 2] = (w >> 16) & 0xFF;
        tb->mem[addr + 3] = (w >> 24) & 0xFF;
    }
}

static void load_halfwords(test_bus_t *tb, uint32_t base, const uint16_t *hws, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t addr = base + (uint32_t)(i * 2);
        uint16_t h = hws[i];
        tb->mem[addr]     =  h       & 0xFF;
        tb->mem[addr + 1] = (h >> 8) & 0xFF;
    }
}

static void poke_word(test_bus_t *tb, uint32_t addr, uint32_t val)
{
    tb->mem[addr]     =  val        & 0xFF;
    tb->mem[addr + 1] = (val >> 8)  & 0xFF;
    tb->mem[addr + 2] = (val >> 16) & 0xFF;
    tb->mem[addr + 3] = (val >> 24) & 0xFF;
}

/* ------------------------------------------------------------------ */
/* Test 1 (ARM): sum 1..10 in a loop, store result, spin.              */
/* ------------------------------------------------------------------ */

static int test_arithmetic_loop(void)
{
    static const uint32_t program[] = {
        0xE3A00000, /* 0x00 mov  r0, #0            */
        0xE3A01001, /* 0x04 mov  r1, #1            */
        0xE351000B, /* 0x08 loop: cmp r1, #11      */
        0xAA000002, /* 0x0C bge  end                */
        0xE0800001, /* 0x10 add  r0, r0, r1        */
        0xE2811001, /* 0x14 add  r1, r1, #1        */
        0xEAFFFFFA, /* 0x18 b    loop               */
        0xE3A02A01, /* 0x1C end: mov r2, #0x1000   */
        0xE5820000, /* 0x20 str  r0, [r2]          */
        0xEAFFFFFE, /* 0x24 b    .                  */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    int steps = 0;
    while (cpu.r[15] != 0x24 && steps < 1000 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    uint32_t mem_result = tb.mem[0x1000] | (tb.mem[0x1001] << 8)
                        | (tb.mem[0x1002] << 16) | (tb.mem[0x1003] << 24);

    int ok = (cpu.r[0] == 55) && (cpu.r[15] == 0x24) && (mem_result == 55);
    printf("[test 1, ARM: arithmetic loop + branch + STR]  r0=%u mem[0x1000]=%u pc=0x%02x  -> %s\n",
           cpu.r[0], mem_result, cpu.r[15], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 2 (ARM): multiply, stack setup, BL into a subroutine that uses */
/* STMFD/LDMFD (push/pop) including a return via PC-in-list LDM.       */
/* ------------------------------------------------------------------ */

static int test_multiply_and_stack(void)
{
    static const uint32_t program[] = {
        0xE3A00005, /* 0x00 mov   r0, #5                 */
        0xE3A01006, /* 0x04 mov   r1, #6                 */
        0xE0020190, /* 0x08 mul   r2, r0, r1              */
        0xE3A0DC20, /* 0x0C mov   sp, #0x2000            */
        0xEB000000, /* 0x10 bl    sub (0x18)              */
        0xEAFFFFFE, /* 0x14 b     .   <-- BL returns here */
        0xE92D4010, /* 0x18 sub: stmfd sp!, {r4, lr}      */
        0xE2822001, /* 0x1C add   r2, r2, #1              */
        0xE8BD8010, /* 0x20 ldmfd sp!, {r4, pc}           */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    int steps = 0;
    while (cpu.r[15] != 0x14 && steps < 1000 && !cpu.halted) { arm1176_step(&cpu); steps++; }
    uint32_t pc_before = cpu.r[15];
    arm1176_step(&cpu); /* confirm it's genuinely spinning at 0x14 */
    int stable = (cpu.r[15] == pc_before);

    int ok = (cpu.r[2] == 31) && (cpu.r[15] == 0x14) && stable;
    printf("[test 2, ARM: mul + sp setup + bl + stmfd/ldmfd]  r2=%u pc=0x%02x  -> %s\n",
           cpu.r[2], cpu.r[15], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 3 (ARM): STRH/LDRH round trip, and MRS reading CPSR.           */
/* ------------------------------------------------------------------ */

static int test_halfword_and_psr(void)
{
    static const uint32_t program[] = {
        0xE3A00A03, /* 0x00 mov  r0, #0x3000   */
        0xE3A01056, /* 0x04 mov  r1, #0x56     */
        0xE1C010B0, /* 0x08 strh r1, [r0]      */
        0xE1D020B0, /* 0x0C ldrh r2, [r0]      */
        0xE10F3000, /* 0x10 mrs  r3, cpsr      */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    for (int i = 0; i < 5; i++) arm1176_step(&cpu);

    int ok = (cpu.r[2] == 0x56) && ((cpu.r[3] & CPSR_MODE_MASK) == ARM_MODE_SVC)
           && (cpu.r[3] & CPSR_I) && (cpu.r[3] & CPSR_F);
    printf("[test 3, ARM: strh/ldrh + mrs]  r2=0x%02x cpsr(via mrs)=0x%08x  -> %s\n",
           cpu.r[2], cpu.r[3], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 4 (Thumb): sum 1..10 in a loop, using the low-register formats */
/* (move-shifted-reg, add reg/imm, mov/cmp immediate, conditional and  */
/* unconditional branch, immediate-offset STR).                        */
/* ------------------------------------------------------------------ */

static int test_thumb_arithmetic_loop(void)
{
    static const uint16_t program[] = {
        0x2000, /* 0x00 mov  r0, #0          */
        0x2101, /* 0x02 mov  r1, #1          */
        0x290B, /* 0x04 loop: cmp r1, #11    */
        0xDA02, /* 0x06 bge  end              */
        0x1840, /* 0x08 add  r0, r0, r1      */
        0x3101, /* 0x0A add  r1, #1          */
        0xE7FA, /* 0x0C b    loop             */
        0x2201, /* 0x0E end: mov r2, #1      */
        0x0312, /* 0x10 lsl  r2, r2, #12     */ /* r2 = 0x1000 */
        0x6010, /* 0x12 str  r0, [r2]        */
        0xE7FE, /* 0x14 b    .                */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_halfwords(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));
    cpu.cpsr |= CPSR_T; /* start execution in Thumb state */

    int steps = 0;
    while (cpu.r[15] != 0x14 && steps < 1000 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    uint32_t mem_result = tb.mem[0x1000] | (tb.mem[0x1001] << 8)
                        | (tb.mem[0x1002] << 16) | (tb.mem[0x1003] << 24);

    int ok = !cpu.halted && (cpu.r[0] == 55) && (cpu.r[2] == 0x1000)
           && (cpu.r[15] == 0x14) && (mem_result == 55) && (cpu.cpsr & CPSR_T);
    printf("[test 4, Thumb: arithmetic loop + branch + STR]  r0=%u r2=0x%04x mem[0x1000]=%u pc=0x%02x  -> %s\n",
           cpu.r[0], cpu.r[2], mem_result, cpu.r[15], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 5 (Thumb): MUL, hi-register MOV into SP, BL into a subroutine  */
/* that does push {r4,lr} / pop {r4,pc} — exercises formats 4, 5, 14   */
/* and the two-halfword BL sequence (format 19).                       */
/* ------------------------------------------------------------------ */

static int test_thumb_mul_and_stack(void)
{
    static const uint16_t program[] = {
        0x2005, /* 0x00 mov  r0, #5             */
        0x2106, /* 0x02 mov  r1, #6             */
        0x4348, /* 0x04 mul  r0, r1              */
        0x2201, /* 0x06 mov  r2, #1             */
        0x0352, /* 0x08 lsl  r2, r2, #13        */ /* r2 = 0x2000 */
        0x4695, /* 0x0A mov  sp, r2             */
        0xF000, /* 0x0C bl   sub (prefix)        */
        0xF801, /* 0x0E bl   sub (suffix)        */
        0xE7FE, /* 0x10 b    .   <-- BL returns here */
        0xB510, /* 0x12 sub: push {r4, lr}       */
        0x3001, /* 0x14 add  r0, #1              */
        0xBD10, /* 0x16 pop  {r4, pc}            */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_halfwords(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));
    cpu.cpsr |= CPSR_T;

    int steps = 0;
    while (cpu.r[15] != 0x10 && steps < 1000 && !cpu.halted) { arm1176_step(&cpu); steps++; }
    uint32_t pc_before = cpu.r[15];
    arm1176_step(&cpu); /* confirm it's genuinely spinning at 0x10 */
    int stable = (cpu.r[15] == pc_before);

    int ok = !cpu.halted && (cpu.r[0] == 31) && (cpu.r[13] == 0x2000)
           && (cpu.r[15] == 0x10) && stable && (cpu.cpsr & CPSR_T);
    printf("[test 5, Thumb: mul + mov-hi-sp + bl + push/pop]  r0=%u sp=0x%04x pc=0x%02x  -> %s\n",
           cpu.r[0], cpu.r[13], cpu.r[15], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 6 (ARM + CP15): build an identity-mapped section (VA==PA, full */
/* access) covering the running code, plus a second section mapped    */
/* AP=1 (privileged RW, no user access), enable the MMU, prove a       */
/* privileged access to the restricted section succeeds, then drop to */
/* user mode and confirm the same access now takes a precise Data     */
/* Abort with the expected DFSR (permission fault, domain 0, write).   */
/* ------------------------------------------------------------------ */

static int test_mmu_sections_and_permission_fault(void)
{
    static const uint32_t program[] = {
        0xE3A00801, /* 0x00 mov  r0, #0x00010000        ; L1 table base       */
        0xEE020F10, /* 0x04 mcr  p15, 0, r0, c2, c0, 0   ; TTBR0 = r0          */
        0xE3A00001, /* 0x08 mov  r0, #1                                       */
        0xEE030F10, /* 0x0C mcr  p15, 0, r0, c3, c0, 0   ; DACR: domain0=client*/
        0xEE111F10, /* 0x10 mrc  p15, 0, r1, c1, c0, 0   ; r1 = SCTLR          */
        0xE3811001, /* 0x14 orr  r1, r1, #1              ; set M bit          */
        0xEE011F10, /* 0x18 mcr  p15, 0, r1, c1, c0, 0   ; SCTLR = r1 (MMU on!)*/
        0xE3A02601, /* 0x1C mov  r2, #0x00100000        ; VA in AP=1 section  */
        0xE3A030AB, /* 0x20 mov  r3, #0xAB                                    */
        0xE5823000, /* 0x24 str  r3, [r2]                ; priv write: OK     */
        0xE5924000, /* 0x28 ldr  r4, [r2]                ; priv read: OK      */
        0xE321F0D0, /* 0x2C msr  cpsr_c, #0xD0           ; drop to USR (I/F set)*/
        0xE5823000, /* 0x30 str  r3, [r2]                ; user write: FAULTS */
        0xEAFFFFFE, /* 0x34 b    .   (not reached before the abort redirects) */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x200000); /* 2MB: room for L1 table @0x10000 and data @0x100000 */
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    /* Build the level-1 table by hand (see cp15.c for the exact section
     * descriptor bit layout): index 0 identity-maps VA 0x00000000, index
     * 1 maps VA 0x00100000 -> PA 0x00100000 with AP=1 (priv-only). */
    uint32_t l1_base = 0x00010000;
    poke_word(&tb, l1_base + 0 * 4, 0x00000000u | (3u << 10) | (0u << 5) | 0x2u); /* AP=3, domain0, section */
    poke_word(&tb, l1_base + 1 * 4, 0x00100000u | (1u << 10) | (0u << 5) | 0x2u); /* AP=1, domain0, section */

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    int steps = 0;
    bool aborted = false;
    while (steps < 200 && !cpu.halted) {
        arm1176_step(&cpu);
        steps++;
        if ((cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_ABT) { aborted = true; break; }
    }

    bool dfsr_ok = (cpu.cp15.dfsr & 0xF) == 0xD          /* permission fault, section */
                && ((cpu.cp15.dfsr >> 4) & 0xF) == 0     /* domain 0 */
                && (cpu.cp15.dfsr & (1u << 11)) != 0;    /* WnR: it was a write */
    bool dfar_ok = cpu.cp15.dfar == 0x00100000u;
    bool priv_access_ok = (cpu.r[4] == 0xAB);            /* the privileged STR+LDR round-trip before the fault */

    int ok = aborted && dfsr_ok && dfar_ok && priv_access_ok;
    printf("[test 6, CP15: MMU sections + permission fault]  "
           "priv_rw_ok=%d aborted=%d dfsr=0x%08x dfar=0x%08x  -> %s\n",
           priv_access_ok, aborted, cpu.cp15.dfsr, cpu.cp15.dfar, ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 7 (ARM + CP15): configure a 4KB DTCM region via c9 and prove   */
/* loads/stores through it hit the TCM backing store rather than the   */
/* system bus (MMU stays off for this one — TCM is independent of it). */
/* ------------------------------------------------------------------ */

static int test_tcm(void)
{
    static const uint32_t program[] = {
        0xE3A00101, /* 0x00 mov  r0, #0x40000000                 */
        0xE380000D, /* 0x04 orr  r0, r0, #0xD    ; size=4KB, enable */
        0xEE090F11, /* 0x08 mcr  p15, 0, r0, c9, c1, 0  ; DTCM region reg */
        0xE3A10101, /* 0x0C mov  r1, #0x40000000 ; DTCM VA base   */
        0xE3A02055, /* 0x10 mov  r2, #0x55                        */
        0xE5812000, /* 0x14 str  r2, [r1]                         */
        0xE5913000, /* 0x18 ldr  r3, [r1]                         */
        0xEAFFFFFE, /* 0x1C b    .                                 */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    int steps = 0;
    while (cpu.r[15] != 0x1C && steps < 200 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    int ok = !cpu.halted && (cpu.r[3] == 0x55) && cpu.cp15.dtcm_enabled
           && cpu.cp15.dtcm_base == 0x40000000u && cpu.cp15.dtcm_size == 4096;
    printf("[test 7, CP15: DTCM configure + load/store]  r3=0x%02x dtcm_base=0x%08x dtcm_size=%u  -> %s\n",
           cpu.r[3], cpu.cp15.dtcm_base, cpu.cp15.dtcm_size, ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 8 (ARM + VFP): enable the VFP unit (FPEXC.EN), move two float  */
/* constants into S0/S1 via VMOV(core<->single), VADD.F32 them, move   */
/* the result back to an ARM register, and store it to memory.         */
/* ------------------------------------------------------------------ */

static int test_vfp_add(void)
{
    static const uint32_t program[] = {
        0xE3A00101, /* 0x00 mov  r0, #0x40000000       ; FPEXC.EN bit  */
        0xEEE80A10, /* 0x04 vmsr fpexc, r0                             */
        0xE3A015FE, /* 0x08 mov  r1, #0x3F800000       ; 1.0f          */
        0xEE001A10, /* 0x0C vmov s0, r1                                */
        0xE3A02101, /* 0x10 mov  r2, #0x40000000       ; 2.0f          */
        0xEE002A90, /* 0x14 vmov s1, r2                                */
        0xEE301A20, /* 0x18 vadd.f32 s2, s0, s1                        */
        0xEE113A10, /* 0x1C vmov r3, s2                                */
        0xE3A04A01, /* 0x20 mov  r4, #0x1000                           */
        0xE5843000, /* 0x24 str  r3, [r4]                              */
        0xEAFFFFFE, /* 0x28 b    .                                      */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    int steps = 0;
    while (cpu.r[15] != 0x28 && steps < 200 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    uint32_t mem_result = tb.mem[0x1000] | (tb.mem[0x1001] << 8)
                        | (tb.mem[0x1002] << 16) | (tb.mem[0x1003] << 24);

    int ok = !cpu.halted && (cpu.r[3] == 0x40400000u) && (mem_result == 0x40400000u)
           && (cpu.vfp.fpexc & FPEXC_EN);
    printf("[test 8, VFP: enable + vmov + vadd.f32]  r3=0x%08x (want 0x40400000 = 3.0f)  -> %s\n",
           cpu.r[3], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 9 (ARM + VFP): VCMP + VMRS (transfer FPSCR flags to CPSR) +    */
/* conditional branch — the idiom real compiled code uses for an       */
/* `if (a > b)` on floats.                                              */
/* ------------------------------------------------------------------ */

static int test_vfp_cmp_and_branch(void)
{
    static const uint32_t program[] = {
        0xE3A00101, /* 0x00 mov  r0, #0x40000000                 */
        0xEEE80A10, /* 0x04 vmsr fpexc, r0                        */
        0xE3A01101, /* 0x08 mov  r1, #0x40000000  ; 2.0f          */
        0xEE001A10, /* 0x0C vmov s0, r1                           */
        0xE3A025FE, /* 0x10 mov  r2, #0x3F800000  ; 1.0f          */
        0xEE002A90, /* 0x14 vmov s1, r2                           */
        0xEEB40A60, /* 0x18 vcmp.f32 s0, s1        ; 2.0 > 1.0    */
        0xEEF1FA10, /* 0x1C vmrs APSR_nzcv, fpscr                 */
        0xCA000000, /* 0x20 bgt  end (0x28)                       */
        0xEAFFFFFE, /* 0x24 b    .   (should be skipped)          */
        0xE3A0502A, /* 0x28 end: mov r5, #0x2A                    */
        0xEAFFFFFE, /* 0x2C b    .                                 */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    int steps = 0;
    while (cpu.r[15] != 0x2C && steps < 200 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    int ok = !cpu.halted && (cpu.r[5] == 0x2A) && (cpu.r[15] == 0x2C);
    printf("[test 9, VFP: vcmp.f32 + vmrs(nzcv) + bgt]  r5=0x%02x pc=0x%02x  -> %s\n",
           cpu.r[5], cpu.r[15], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 10 (VFP): a VFP instruction executed before FPEXC.EN is set    */
/* must trap as Undefined Instruction rather than silently running —   */
/* this is the trap real OSes rely on for lazy FPU context switching.  */
/* ------------------------------------------------------------------ */

static int test_vfp_disabled_trap(void)
{
    static const uint32_t program[] = {
        0xEEB40A40, /* 0x00 vcmp.f32 s0, s0   ; FPEXC.EN is 0 at reset */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    arm1176_step(&cpu);

    int ok = ((cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_UND)
           && (cpu.r[15] == VEC_UNDEF) && (cpu.r[14] == 0x04)
           && !(cpu.vfp.fpexc & FPEXC_EN);
    printf("[test 10, VFP: disabled-unit trap on vcmp]  mode=%s pc=0x%02x lr=0x%02x  -> %s\n",
           arm1176_mode_name(cpu.cpsr), cpu.r[15], cpu.r[14], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Test 11 (ARM + Jazelle): JIDR reads as zero (telling software "no   */
/* real Jazelle hardware, don't try to use it"), a write to JMCR is    */
/* silently ignored (WI), and BXJ falls back to plain BX semantics —   */
/* including interworking into Thumb state, since Jazelle entry can    */
/* never succeed with JIDR reporting no acceleration.                  */
/* ------------------------------------------------------------------ */

static int test_jazelle_trivial_impl(void)
{
    static const uint32_t program[] = {
        0xE3A010FF, /* 0x00 mov r1, #0xFF                            */
        0xEEE21E10, /* 0x04 mcr p14, 7, r1, c2, c0, 0  ; JMCR = 0xFF (ignored) */
        0xEEF00E10, /* 0x08 mrc p14, 7, r0, c0, c0, 0  ; r0 = JIDR (want 0)     */
        0xEEF22E10, /* 0x0C mrc p14, 7, r2, c2, c0, 0  ; r2 = JMCR (want 0, proves WI) */
        0xE3A03019, /* 0x10 mov r3, #0x19              ; BXJ target: 0x18, request Thumb */
        0xE12FFF23, /* 0x14 bxj r3                                    */
        0xE7FE2477, /* 0x18 (thumb) mov r4, #0x77  ;  (thumb) b .     */
    };

    test_bus_t tb;
    test_bus_init(&tb, 0x10000);
    load_words(&tb, 0, program, sizeof(program) / sizeof(program[0]));

    arm1176_t cpu = {0};
    arm1176_reset(&cpu);
    arm1176_set_bus(&cpu, test_bus_get_interface(&tb));

    int steps = 0;
    while (cpu.r[15] != 0x1A && steps < 200 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    int ok = !cpu.halted && (cpu.r[0] == 0) && (cpu.r[2] == 0)
           && (cpu.r[4] == 0x77) && (cpu.cpsr & CPSR_T) && (cpu.r[15] == 0x1A);
    printf("[test 11, Jazelle: JIDR/JMCR RAZ-WI + BXJ->BX(thumb)]  "
           "jidr=0x%x jmcr=0x%x r4=0x%02x thumb=%d pc=0x%02x  -> %s\n",
           cpu.r[0], cpu.r[2], cpu.r[4], !!(cpu.cpsr & CPSR_T), cpu.r[15], ok ? "PASS" : "FAIL");

    test_bus_free(&tb);
    return ok;
}

/* ------------------------------------------------------------------ */
/* SoC tests: exercise board/soc.c's memory-map dispatch and           */
/* peripherals. Unlike the CPU-only tests above, these use soc_init()  */
/* (which also sets the reset PC to SOC_SECUREROM_BASE, matching real  */
/* S5L8900 boot behavior — see soc.h) instead of a bare test_bus_t.    */
/* ------------------------------------------------------------------ */

static void soc_load_rom_words(soc_t *soc, const uint32_t *words, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t off = (uint32_t)(i * 4);
        uint32_t w = words[i];
        soc->securerom[off]     =  w        & 0xFF;
        soc->securerom[off + 1] = (w >> 8)  & 0xFF;
        soc->securerom[off + 2] = (w >> 16) & 0xFF;
        soc->securerom[off + 3] = (w >> 24) & 0xFF;
    }
}

/* Test 12: boots from SOC_SECUREROM_BASE (not 0x0 — proving soc_init's
 * reset-PC override actually takes effect) and writes a value to SDRAM
 * through the bus dispatch, proving SecureROM-fetch and SDRAM-access
 * both route to the right backing store. */

static int test_soc_boot_and_sdram(void)
{
    static const uint32_t program[] = {
        0xE3A0002A, /* mov r0, #0x2A                */
        0xE3A01408, /* mov r1, #0x08000000          */
        0xE5810000, /* str r0, [r1]                 */
        0xEAFFFFFE, /* b   .                         */
    };

    arm1176_t cpu = {0};
    soc_t soc;
    arm1176_reset(&cpu);
    soc_init(&soc, &cpu);
    soc_load_rom_words(&soc, program, sizeof(program) / sizeof(program[0]));

    uint32_t self_loop = SOC_SECUREROM_BASE + 0x0C;
    int steps = 0;
    while (cpu.r[15] != self_loop && steps < 200 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    uint32_t sdram_val = soc.sdram[0] | (soc.sdram[1] << 8) | (soc.sdram[2] << 16) | (soc.sdram[3] << 24);
    int ok = !cpu.halted && (cpu.r[15] == self_loop) && (sdram_val == 0x2A);
    printf("[test 12, SoC: boot at SecureROM + SDRAM write]  pc=0x%08x sdram[0]=0x%02x  -> %s\n",
           cpu.r[15], sdram_val, ok ? "PASS" : "FAIL");

    soc_destroy(&soc);
    return ok;
}

/* Test 13: configures UART0 (CR = UARTEN|TXE) and writes "Hi!" one
 * byte at a time via STR to the data register, checking the bytes show
 * up in the UART's capture buffer — proving the peripheral dispatch
 * and the PL011-style register model both work end to end. */

static int test_soc_uart(void)
{
    static const uint32_t program[] = {
        0xE3A01438, /* mov r1, #0x38000000              ; UART0 base   */
        0xE3A02001, /* mov r2, #1                                     */
        0xE3822C01, /* orr r2, r2, #0x100                             */
        0xE5812030, /* str r2, [r1, #0x30]  ; CR = UARTEN|TXE         */
        0xE3A03048, /* mov r3, #0x48        ; 'H'                     */
        0xE5813000, /* str r3, [r1]                                    */
        0xE3A03069, /* mov r3, #0x69        ; 'i'                     */
        0xE5813000, /* str r3, [r1]                                    */
        0xE3A03021, /* mov r3, #0x21        ; '!'                     */
        0xE5813000, /* str r3, [r1]                                    */
        0xEAFFFFFE, /* b   .                                            */
    };

    arm1176_t cpu = {0};
    soc_t soc;
    arm1176_reset(&cpu);
    soc_init(&soc, &cpu);
    soc_load_rom_words(&soc, program, sizeof(program) / sizeof(program[0]));

    uint32_t self_loop = SOC_SECUREROM_BASE + 0x28;
    int steps = 0;
    while (cpu.r[15] != self_loop && steps < 200 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    int ok = !cpu.halted && (cpu.r[15] == self_loop)
           && soc.uart0.tx_capture && strcmp(soc.uart0.tx_capture, "Hi!") == 0;
    printf("[test 13, SoC: UART0 output capture]  captured=\"%s\"  -> %s\n",
           soc.uart0.tx_capture ? soc.uart0.tx_capture : "(null)", ok ? "PASS" : "FAIL");

    soc_destroy(&soc);
    return ok;
}

/* Test 14: the real end-to-end interrupt path — configure the SP804-
 * style timer, unmask its line on the PL192-style VIC, enable IRQs on
 * the CPU (clear CPSR.I), then step+tick until the timer underflows
 * and the resulting IRQ actually redirects the CPU to VEC_IRQ. */

static int test_soc_timer_irq(void)
{
    static const uint32_t program[] = {
        0xE3A01438, /* mov r1, #0x38000000       ; r1 = 0x38000000            */
        0xE3811C20, /* orr r1, r1, #0x2000       ; r1 = TIMER base (0x38002000)*/
        0xE3A02003, /* mov r2, #3                ; countdown value             */
        0xE5812000, /* str r2, [r1]              ; TIMER LOAD = 3              */
        0xE3A020A0, /* mov r2, #0xA0             ; ENABLE|IRQEN                */
        0xE5812008, /* str r2, [r1, #8]          ; TIMER CONTROL = 0xA0        */
        0xE3A03438, /* mov r3, #0x38000000                                     */
        0xE3833A01, /* orr r3, r3, #0x1000       ; r3 = VIC0 base (0x38001000) */
        0xE3A04004, /* mov r4, #4                ; bit for IRQ line 2 (timer)  */
        0xE5834010, /* str r4, [r3, #0x10]       ; VIC INTENABLE |= bit2       */
        0xE321F053, /* msr cpsr_c, #0x53         ; clear I, keep F + SVC mode  */
        0xEAFFFFFE, /* b   .                     ; wait for the interrupt      */
    };

    arm1176_t cpu = {0};
    soc_t soc;
    arm1176_reset(&cpu);
    soc_init(&soc, &cpu);
    soc_load_rom_words(&soc, program, sizeof(program) / sizeof(program[0]));

    int steps = 0;
    while ((cpu.cpsr & CPSR_MODE_MASK) != ARM_MODE_IRQ && steps < 200 && !cpu.halted) {
        arm1176_step(&cpu);
        soc_tick(&soc);
        steps++;
    }

    uint32_t expected_lr = SOC_SECUREROM_BASE + 0x2C + 4; /* addr of the self-loop + 4 */
    int ok = !cpu.halted && ((cpu.cpsr & CPSR_MODE_MASK) == ARM_MODE_IRQ)
           && (cpu.r[15] == VEC_IRQ) && (cpu.r[14] == expected_lr);
    printf("[test 14, SoC: timer -> VIC -> CPU IRQ]  mode=%s pc=0x%08x lr=0x%08x (want 0x%08x)  -> %s\n",
           arm1176_mode_name(cpu.cpsr), cpu.r[15], cpu.r[14], expected_lr, ok ? "PASS" : "FAIL");

    soc_destroy(&soc);
    return ok;
}

/* Test 15: the CONFIRMED clock controller (see soc.h) — CLOCK_CONFIG0
 * reads back its real documented power-on-reset value (0x40000000),
 * PLLLOCK is read-only and always reports "locked" regardless of what
 * software writes to it, and an ordinary gate register (GATES1) is a
 * normal read/write cell that keeps whatever's written to it. */

static int test_soc_clock(void)
{
    static const uint32_t program[] = {
        0xE3A0143C, /* 0x00 mov r1, #0x3C000000                      */
        0xE3811605, /* 0x04 orr r1, r1, #0x500000  ; r1 = CLOCK base  */
        0xE5912000, /* 0x08 ldr r2, [r1]           ; r2 = CONFIG0     */
        0xE5913040, /* 0x0C ldr r3, [r1, #0x40]    ; r3 = PLLLOCK     */
        0xE3A04099, /* 0x10 mov r4, #0x99                             */
        0xE5814040, /* 0x14 str r4, [r1, #0x40]    ; write PLLLOCK (should be ignored) */
        0xE5915040, /* 0x18 ldr r5, [r1, #0x40]    ; r5 = PLLLOCK again */
        0xE5814048, /* 0x1C str r4, [r1, #0x48]    ; write GATES1     */
        0xE5916048, /* 0x20 ldr r6, [r1, #0x48]    ; r6 = GATES1      */
        0xEAFFFFFE, /* 0x24 b   .                                      */
    };

    arm1176_t cpu = {0};
    soc_t soc;
    arm1176_reset(&cpu);
    soc_init(&soc, &cpu);
    soc_load_rom_words(&soc, program, sizeof(program) / sizeof(program[0]));

    uint32_t self_loop = SOC_SECUREROM_BASE + 0x24;
    int steps = 0;
    while (cpu.r[15] != self_loop && steps < 200 && !cpu.halted) { arm1176_step(&cpu); steps++; }

    int ok = !cpu.halted && (cpu.r[15] == self_loop)
           && (cpu.r[2] == 0x40000000u) && (cpu.r[3] == 0xFFFFFFFFu)
           && (cpu.r[5] == 0xFFFFFFFFu) && (cpu.r[6] == 0x99u);
    printf("[test 15, SoC: clock controller (CONFIRMED regs)]  "
           "config0=0x%08x plllock_before=0x%08x plllock_after_write=0x%08x gates1=0x%02x  -> %s\n",
           cpu.r[2], cpu.r[3], cpu.r[5], cpu.r[6], ok ? "PASS" : "FAIL");

    soc_destroy(&soc);
    return ok;
}

int main(void)
{
    int a = test_arithmetic_loop();
    int b = test_multiply_and_stack();
    int c = test_halfword_and_psr();
    int d = test_thumb_arithmetic_loop();
    int e = test_thumb_mul_and_stack();
    int f = test_mmu_sections_and_permission_fault();
    int g = test_tcm();
    int h = test_vfp_add();
    int i = test_vfp_cmp_and_branch();
    int j = test_vfp_disabled_trap();
    int k = test_jazelle_trivial_impl();
    int l = test_soc_boot_and_sdram();
    int m = test_soc_uart();
    int n = test_soc_timer_irq();
    int o = test_soc_clock();

    int all_ok = a && b && c && d && e && f && g && h && i && j && k && l && m && n && o;
    printf("\n%s\n", all_ok ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
    return all_ok ? 0 : 1;
}
