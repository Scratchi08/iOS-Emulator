# iphone2g-emu — ARM1176JZF-S core + S5L8900 SoC scaffold

An iPhone 2G / iPhone OS 1.0 emulator, built bottom-up: a complete
ARM1176JZF-S CPU core (integer ARM+Thumb, CP15/MMU, VFP11, Jazelle),
now paired with a first-cut S5L8900 SoC/board layer (memory map, UART
console, interrupt controller, timer) so there's an actual physical
address space to point the CPU at instead of hand-built test buses.

## Layout

```
include/arm1176.h        Public API: CPU struct, bus interface, lifecycle
src/arm1176_internal.h   Shared internals (not part of the public API):
                          register access (reg_read/reg_write/write_pc_bx),
                          add-with-carry, raw physical-bus wrappers, and the
                          MMU/TCM-aware bus_*/fetch_* wrappers that both
                          decoders actually call
src/arm1176_cpu.c        Reset, register banking/mode switches, condition
                          codes, barrel shifter, exception entry
src/arm1176_decode.c     ARM (32-bit) instruction decode + execute, fetch loop
src/arm1176_thumb.c      Thumb (16-bit) instruction decode + execute
src/cp15.c               CP15 system control coprocessor: MMU/TLB, cache-op
                          stubs, TCM
src/vfp.c                VFP11 (VFPv2) floating-point coprocessor
src/jazelle.c            Jazelle extension (trivial implementation — see
                          jazelle.h)
include/cp15.h            CP15 register state and public API (see its header
                          comment for the detailed fidelity/scope notes)
include/vfp.h             VFP register state and public API (ditto)
include/jazelle.h         Jazelle scope notes and public API (ditto)
board/soc.c              S5L8900 SoC/board layer: memory-map dispatch,
                          PL192-style VIC, PL011-style UART, SP804-style
                          timer, clock/GPIO stubs (see soc.h — separate from
                          src/ since this is the board the CPU plugs into,
                          not part of the CPU die)
include/soc.h             SoC memory map and peripheral state. Its header
                          comment is required reading before trusting any
                          address in it — see "What's implemented" below
test/test_bus.c/.h       Flat-RAM bus used only for standalone CPU-only testing
test/main.c              Fourteen hand-assembled smoke-test programs (3 ARM,
                          2 Thumb, 2 CP15/MMU/TCM, 3 VFP, 1 Jazelle, 3 SoC)
Makefile                 `make`, `make run`, `make clean`
```

Build & run:
```
make run
```

## What's implemented

- All 7 legacy CPU modes (usr/fiq/irq/svc/abt/und/sys) with correct
  register banking (r8-r12 banked for FIQ only; r13/r14 banked per mode;
  SPSR per privileged mode).
- Full condition-code evaluation (all 16 conditions).
- Barrel shifter: LSL/LSR/ASR/ROR, including the LSR #32 / ASR #32 / RRX
  special cases for immediate shifts.
- **ARM state:**
  - Data processing: AND/EOR/SUB/RSB/ADD/ADC/SBC/RSC/TST/TEQ/CMP/CMN/ORR/
    MOV/BIC/MVN, with immediate, shift-by-immediate, and shift-by-register
    operand2 forms, correct N/Z/C/V flag updates, and the `MOVS pc, lr`
    style exception-return idiom (S=1, Rd=15 restores CPSR from SPSR).
  - MRS / MSR (register and immediate, CPSR and SPSR, field masking).
  - MUL / MLA, and UMULL / UMLAL / SMULL / SMLAL.
  - SWP / SWPB.
  - Single data transfer: LDR/STR, word and byte, immediate and
    register(+shift) offsets, pre/post-index, writeback.
  - Halfword and signed transfers: LDRH/STRH/LDRSB/LDRSH.
  - Block data transfer: LDM/STM, all four addressing modes (IA/IB/DA/DB),
    writeback, and the S-bit behaviors for user-bank access and
    CPSR-restore-on-PC-load (covers the common `push {..,lr}` /
    `pop {..,pc}` idiom, verified in test 2).
  - B/BL, BX/BLX(register), BLX(immediate, ARM→Thumb only), CLZ.
  - SWI, and exception entry for reset/undef/swi/prefetch-abort/
    data-abort/irq/fiq (vectors assumed at 0x00000000, i.e. "low
    vectors" — iPhone OS 1.0's iBoot/kernel setup will need to confirm
    this against the real S5L8900 boot ROM behavior).
- **Thumb state** (all 19 classic Thumb-1 formats, plus BLX):
  - Format 1: move shifted register (LSL/LSR/ASR #imm5)
  - Format 2: add/subtract register or #imm3
  - Format 3: MOV/CMP/ADD/SUB Rd, #imm8
  - Format 4: ALU ops (AND/EOR/LSL/LSR/ASR/ADC/SBC/ROR/TST/NEG/CMP/CMN/
    ORR/MUL/BIC/MVN), all shift-by-register amount taken from Rs
  - Format 5: hi-register ADD/CMP/MOV, and BX/BLX(register) — the
    interworking branch that can switch back to ARM state
  - Format 6: PC-relative load
  - Formats 7/8: load/store with register offset (word/byte,
    halfword, sign-extended byte/halfword)
  - Format 9: load/store with immediate offset (word/byte)
  - Format 10: load/store halfword with immediate offset
  - Format 11: SP-relative load/store
  - Format 12: load address (ADD Rd, PC/SP, #imm8*4)
  - Format 13: ADD/SUB SP, #imm7*4
  - Format 14: PUSH/POP, including POP-with-PC interworking (verified
    in test 5, alongside a full two-halfword `BL`)
  - Format 15: STMIA/LDMIA Rb!, {Rlist}
  - Format 16: conditional branch
  - Format 17: SWI
  - Format 18: unconditional branch
  - Format 19: BL (two-halfword prefix/suffix sequence, using LR as
    scratch space exactly as real hardware does), and BLX(suffix)
    switching to ARM state
- External IRQ/FIQ lines sampled once per instruction, respecting the
  I/F mask bits, in both ARM and Thumb state.
- **CP15 (system control coprocessor)** — see `cp15.h`'s header comment
  for the full fidelity/scope discussion; summary:
  - Identification registers (MIDR, CTR, TCMTR, TLBTR) with the
    documented ARM1176JZF-S r0p7 values.
  - **MMU**: VMSAv6 short-descriptor page table walk — sections (1MB)
    and small pages (4KB) with high-confidence bit layouts (the small
    page layout matches the hardware PTE format used by Linux's
    short-descriptor headers); large pages (64KB) use a best-effort
    reconstructed layout, flagged in the source. 16-entry domain access
    control (client/manager/no-access), classic 2-bit AP permission
    checks, XN execute-never enforcement, and a simple direct-mapped
    TLB with proper invalidation on context/table/domain changes.
    TTBR0/TTBR1 split via TTBCR.N is implemented; supersections are not
    (treated as a translation fault).
  - Precise Data/Prefetch Abort delivery: on a fault, DFSR/DFAR or
    IFSR/IFAR are set with the standard FS[3:0] codes (translation/
    domain/permission, section or page granularity) plus the WnR bit,
    and multi-access instructions (LDM/STM, push/pop) correctly stop
    issuing further accesses the moment a fault redirects the CPU mid-
    instruction, rather than continuing to run with stale banked
    registers (see `cpu->abort_taken` in `arm1176.h`).
  - **Cache maintenance (c7)**: accepted and treated as completed no-ops
    — this emulator has no real cache model, so memory is always
    coherent and there's nothing to actually clean/invalidate. Barriers
    (DSB/DMB) and the prefetch-buffer flush are included in this.
  - **TLB maintenance (c8)**: invalidate-all, invalidate-by-VA, and
    invalidate-by-ASID (the latter falls back to invalidate-all, since
    the TLB doesn't tag entries by ASID).
  - **TCM**: configurable ITCM/DTCM via c9 region registers, checked
    ahead of the MMU for instruction fetches / data accesses
    respectively, with the backing store lazily allocated/resized to
    match the programmed size. The exact region-register bit layout is
    an emulator-internal convention (documented in `cp15.c`) rather
    than a verified match to real ARM1176 hardware — it's internally
    consistent for our own board code, but flagged in case real iBoot
    TCM setup code ever needs bit-exact compatibility.
  - c13 (FCSEIDR/CONTEXTIDR/TPIDR*) are stored but FCSE itself isn't
    implemented — CONTEXTIDR writes just invalidate the TLB as a safe
    approximation of an address-space switch.
- **VFP11 (VFPv2 floating point)** — see `vfp.h`'s header comment for
  the full fidelity/scope discussion; summary:
  - 32 single-precision registers aliased as 16 double-precision
    registers, matching real hardware layout.
  - Data processing: VADD, VSUB, VMUL, VNMUL, VMLA, VMLS, VNMLA, VNMLS,
    VDIV, VABS, VNEG, VSQRT, VMOV (register copy), VCMP/VCMPE (register
    and #0.0 forms), VCVT (single<->double, and int<->float in both
    directions with round-toward-zero or FPSCR rounding mode).
  - Register transfer: VMOV (core register <-> single-precision reg),
    VMOV (two core registers <-> double-precision reg), VMRS/VMSR
    (FPSID/FPSCR/FPEXC, including the special `VMRS APSR_nzcv, FPSCR`
    form compiled `if (float_a > float_b)` code actually uses).
  - Memory: VLDR/VSTR (single register, immediate offset, including
    PC-relative literal loads) and VLDM/VSTM (register list, IA/DB,
    with/without writeback — this is what VPUSH/VPOP compile to).
  - **FPEXC.EN gating**: a VFP instruction executed while the unit is
    disabled raises Undefined Instruction, matching the trap real OSes
    use for lazy FPU context switching (verified in test 10).
  - Key encodings (VMOV core<->single, VMOV two-core<->double, VMRS/
    VMSR, VLDR/VSTR, VLDM/VSTM, VADD, VNEG, VCMP/VCMPE, VCVT single<->
    double, VCVT int<->float) were checked against the ARM Architecture
    Reference Manual's actual encoding tables rather than reconstructed
    from memory. The remaining arithmetic family (VSUB/VMUL/VNMUL/VMLA/
    VMLS/VNMLA/VNMLS) shares a well-documented "family opcode + bit6
    picks the pair" pattern cross-checked from two confirmed data
    points (VADD, VDIV); the VMLA/VMLS/VNMLA/VNMLS bit6 *polarity*
    (which of each pair maps to which bit value) is lower-confidence
    and flagged with a comment at the point of use in `vfp.c`.
  - Not modeled: half-precision, fused multiply-add (VFMA/VFNMA are
    VFPv4+), fixed-point<->float VCVT (VFPv3+), precise FPSCR cumulative
    exception flags (IOC/DZC/OFC/UFC/IXC/IDC are only approximately
    set), flush-to-zero mode, and CPACR-based coprocessor access
    control (only the finer-grained FPEXC.EN check is enforced).
- **Jazelle** — the ARM-sanctioned "trivial implementation" (see
  `jazelle.h`): JIDR, JMCR, and JOSCR (CP14) are Read-As-Zero and
  ignore writes, and `BXJ` always falls back to plain `BX` semantics
  (including Thumb interworking), since JIDR reporting no acceleration
  means real Jazelle-state entry can never succeed. This is a genuine,
  spec-compliant implementation choice, not a shortcut — real hardware
  is permitted to work exactly this way, and it's what any real-world
  code that merely probes for Jazelle before falling back to a JVM
  interpreter needs to see. Not a real Java bytecode engine, but iPhone
  OS 1.0 shipped without one anyway.
- **S5L8900 SoC/board layer** (`soc.h`/`soc.c`) — the memory map and
  peripherals the CPU is actually plugged into. **Read `soc.h`'s header
  comment before trusting any address**: it labels every region/base
  address as CONFIRMED (sourced from a specific community reference for
  this exact chip — currently just the SecureROM base and SDRAM
  capacity), GROUNDED (a real, standard, publicly-documented ARM
  PrimeCell register layout that this SoC family is confirmed to
  actually use, at an unconfirmed base address), or PLACEHOLDER (picked
  purely to give every region a non-overlapping home). The S5L8900 has
  no public datasheet; this is as far as free-text search tools get
  before the remaining detail lives only in decompiled/reverse-engineered
  binaries (openiBoot's `plat-s5l8900` source tree is the next place to
  look for real values).
  - Memory regions: SecureROM/VROM (boot ROM, CPU reset PC points here
    via `soc_init()` — real hardware does *not* start at address 0 the
    way the generic ARM convention assumes), SRAM, SDRAM (128MiB,
    confirmed capacity), and a read-only-for-now NOR flash window.
  - **Interrupt controller**: PL192-style VIC (confirmed as the right
    family — openiBoot's own `interrupt.c` names `VICINTENABLE`,
    `VICINTENCLEAR`, `VICINTSELECT`, `VICSWPRIORITYMASK`,
    `VICVECTADDRS`, stock PL192 register names). IRQ/FIQ routing via
    `VICINTSELECT`, enable/clear, software-forced interrupts, and a
    simplified (non-priority-stack) vector address readback.
  - **UART**: PL011-style register set (data register, flag register,
    baud-rate divisors, line/control registers, masked/raw interrupt
    status). Transmitted bytes are captured to a growable buffer
    (`soc_uart_t.tx_capture`, what test 13 checks) and optionally echoed
    live to stdout (`tx_echo`).
  - **Timer**: SP804-style single-channel down-counter with periodic/
    one-shot modes and an IRQ-on-underflow, wired into the VIC. Test 14
    proves the full path — timer underflow to VIC to CPU IRQ exception
    — end to end.
  - **Clock/power and GPIO**: intentionally trivial stubs, not modeled
    in any real detail (entirely undocumented for this chip). Clock
    registers default to reading back all-ones until written, so
    firmware's inevitable "spin until the PLL reports locked" loops
    terminate instead of hanging forever; GPIO is a plain read/write
    scratch array with no pin modeling.
  - **Timing model**: `soc_tick()` — called once per `arm1176_step()` by
    the caller, not automatically — charges exactly one timer tick per
    CPU instruction. Not remotely cycle-accurate, but simple,
    deterministic, and enough for interrupts and delay loops to actually
    terminate in a bring-up/test setting.

## What's explicitly NOT implemented yet

These are the natural next milestones, ordered by what unblocks the
most (getting to an actual boot attempt) rather than strictly by
subsystem:

1. **Real S5L8900 register values**: replace the PLACEHOLDER base
   addresses in `soc.h` with real ones — most productively by pulling
   openiBoot's actual `plat-s5l8900` hardware headers (confirmed to
   exist and to be the right source; see the SoC section above) rather
   than continuing to guess. This is the highest-leverage remaining
   item: it's what makes real Apple-signed firmware images (rather than
   hand-assembled test programs) a viable test target.
2. **NAND flash + NOR write/erase**: NOR is currently read-only from the
   CPU's side (fine for boot-time code fetch, not for anything that
   modifies flash), and NAND (where iPhone OS 1.0 itself actually lives,
   as opposed to just the early boot chain) isn't modeled at all yet.
3. **A second VIC instance, and real IRQ line assignments.** Only VIC0
   (32 lines) exists right now with placeholder IRQ numbers for UART0/
   the timer; real hardware likely needs more lines than that once more
   peripherals (I2C/PMU, GPIO interrupts, NAND controller, etc.) show up.
4. **TrustZone** (Monitor mode / secure world). Whether this matters
   depends on how the S5L8900 boot ROM / iBoot chain actually uses it;
   worth revisiting once real firmware is running far enough to tell.
5. **Cycle-accurate timing.** Cycle counts returned by `arm1176_step()`
   are rough approximations, not modeled against the real pipeline —
   including CP15 page-table-walk/TLB timing and VFP op latency
   (VDIV/VSQRT are charged a flat approximate cost, not the real
   data-dependent latency), and now also the SoC's one-tick-per-
   instruction timer model, which is deterministic but not remotely
   tied to a real clock rate. Low priority until something timing-
   sensitive (e.g. a busy-wait calibrated against a real clock) shows
   up in actual boot code.
6. Supersections (16MB) and the ARMv6 extended AP[2]/APX permission
   model (SCTLR.XP=1) — see `cp15.h` for why the classic 2-bit AP model
   was chosen instead as the practical default.
7. A handful of ARMv6-specific Thumb encodings that share opcode space
   with the classic 19 formats but aren't decoded yet: CPS, SETEND,
   REV/REV16/REVSH, SXTB/SXTH/UXTB/UXTH (all currently fall through to a
   silent no-op rather than executing or halting — flagged with a TODO
   comment at the relevant dispatch point in `arm1176_thumb.c`).
8. VFP gaps noted in `vfp.h`: half-precision, fused multiply-add,
   fixed-point convert, precise FPSCR exception flags, flush-to-zero,
   and CPACR-based access control (see the VFP11 section above).
9. A real Jazelle bytecode engine, if it ever turns out iPhone OS 1.0
   (or something else run on this emulator) actually needs one — see
   `jazelle.h` for why that's considered unlikely enough to defer
   indefinitely rather than a near-term milestone.
10. Some rarer-but-real corner cases are intentionally deferred and
    flagged with `TODO` comments in the source: PC-as-operand+12 quirk
    for register-shifted data processing, unaligned LDR word-rotate
    behavior, full privilege enforcement on MSR, BKPT (ARM and Thumb),
    alignment-fault checking (SCTLR.A is stored but not enforced).

## Design notes

- The core never touches memory directly — it only calls through the
  `arm_bus_t` function-pointer table (`read8/16/32`, `write8/16/32`).
  `board/soc.c` is what plugs in here now: a real memory map (SecureROM/
  SRAM/SDRAM/NOR plus UART/VIC/timer/clock/GPIO) instead of the flat
  test-only RAM `test/test_bus.c` provides. There's a layer in between
  on the CPU side: `bus_read*/write*` and `fetch_read*` (in
  `arm1176_internal.h`, what the decoders actually call) go through
  CP15's MMU/TCM logic first, which only calls the raw `phys_read*/
  write*` functions (talking to `arm_bus_t`, i.e. ultimately `soc.c`)
  once a virtual address has been resolved to physical — or
  immediately, if the MMU is disabled.
- The SoC layer's interrupt/timer state doesn't advance on its own —
  `soc_tick()` must be called once per `arm1176_step()` by whatever is
  driving the emulation (see the timing-model note above). This keeps
  `arm1176_step()` itself free of any board-specific knowledge, which
  matters since the CPU core is meant to be reusable independent of
  which board it's plugged into.
- Registers are stored as a flat `r[16]` "currently visible" file, with
  banked copies swapped in/out by `arm1176_switch_mode()` on every mode
  change — this keeps the hot path (`reg_read`/`reg_write` during
  execute) a simple array index instead of a mode-dependent lookup.
- `reg_read(cpu, 15)` returns `r[15] + 4` to implement the ARM
  "PC reads as current instruction + 8" rule, since `r[15]` itself is
  kept at `+4` (the address of the next sequential instruction) once
  fetch has occurred.
