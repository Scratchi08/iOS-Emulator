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
                          timer, CONFIRMED clock controller, GPIO stub (see soc.h — separate from
                          src/ since this is the board the CPU plugs into,
                          not part of the CPU die)
include/soc.h             SoC memory map and peripheral state. Its header
                          comment is required reading before trusting any
                          address in it — see "What's implemented" below
test/test_bus.c/.h       Flat-RAM bus used only for standalone CPU-only testing
test/main.c              Sixteen hand-assembled smoke-test programs (3 ARM,
                          2 Thumb, 2 CP15/MMU/TCM, 3 VFP, 1 Jazelle, 5 SoC)
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
  comment before trusting any address.** Every region is labeled with one
  of four confidence tiers: CONFIRMED (traced to a specific source for
  this chip), INFERRED (follows by reasoning from confirmed facts but no
  source states it), GROUNDED (a real, standard ARM peripheral layout used
  as a stand-in), or PLACEHOLDER (picked only to give a region a
  non-overlapping home). The S5L8900 has no public datasheet; sources are
  community reverse engineering plus Apple's own device tree as captured
  from real devices.
  - **Confirmed this round — the peripheral map.** Apple's device tree for
    the iPod touch 1G (an S5L8900X device; gist `dizimka/2922441`) gives
    an absolute address for every on-chip block, as an offset into an
    "arm-io" window at `0x38000000`: **VIC0 `0x38E00000`**, **VIC1
    `0x38E01000`** (node `compatible` is `vic,pl192`, so "PL192" is now
    Apple's own label), **GPIO `0x3E400000`**, and the **clock controller
    `0x3C500000`**, which independently matches the iDroid-notes source.
    **UART0 `0x3CC00000`** is corroborated by a real iPhone OS kernel boot
    log from the S5LBox emulator and by The iPhone Wiki's sibling-chip
    page. A second S5L8900 IOReg dump (iPod1,1) repeats the same
    addresses. Caveat: those are iPod touch dumps; the iPhone 2G shares
    the SoC, so the SoC's own blocks match, but board-level peripherals
    differ.
  - **This also corrected two earlier guesses.** The old placeholders put
    UART/VIC/timer/GPIO at `0x38000000`-`0x38004000`; in reality
    `0x38000000` is the SHA1 engine, so they were sitting on a real
    device. And the placeholder IRQ lines 1 and 2 for UART/timer collided
    with GPIO interrupt groups (IRQs 0-3 and 0x1f-0x21 per both the device
    tree and openiBoot's `gpio.c`); they are now 5 and 6 (still
    placeholder — see B2).
  - Memory regions: SecureROM/VROM at `0x20000000` (CONFIRMED; the CPU's
    reset PC points here via `soc_init()`, since real hardware does *not*
    start at address 0), SDRAM 128MiB at `0x08000000` (size CONFIRMED,
    base INFERRED: the framebuffer at `0x0F400000` and panic RAM at
    `0x0FFFC000` in the device tree must fit inside it), SRAM at
    `0x22000000` (INFERRED; size `0x2C000` is only a lower bound taken
    from a device-tree window there), and a read-only NOR window
    (PLACEHOLDER, and not how real NOR works — see B4).
  - **Clock/PLL controller — CONFIRMED**: register offsets and power-on-
    reset values matching openiBoot's `clock_reset()` (CONFIG0 resets to
    `0x40000000`, GATES1 to `0x35FE7`); test 15. PLLLOCK is read-only and
    always reports "locked" so firmware's lock-polling loops terminate
    instead of hanging. Real clock rates (24MHz crystal, 412MHz CPU, 103MHz
    bus, 51.5MHz peripheral, 6MHz timebase) are recorded in `soc.h` but not
    yet used (B5).
  - **Interrupt controller**: PL192-style VIC0 at its real address. Only
    VIC0 is modeled; VIC1's address is known but it isn't implemented (B2).
    IRQ/FIQ routing via `VICINTSELECT`, enable/clear, software interrupts,
    and a simplified (non-priority-stack) vector address readback.
  - **UART**: real base address, but the register *layout* is a PL011 stand-
    in that is likely wrong for this chip (Apple's driver is
    `AppleS5L8900XSerial`, and the Linux binding list has a separate
    `apple,s5l-uart` — a Samsung-family UART). It works for our tests
    because test and model agree with each other; real firmware would not.
    TX is captured to `soc_uart_t.tx_capture` and optionally echoed.
  - **Timer**: SP804-style stand-in at a PLACEHOLDER address (the one
    modeled peripheral with no known real address); the real timer's
    layout is also unverified. Test 14 proves the timer-to-VIC-to-CPU-IRQ
    path end to end.
  - **GPIO**: real address, but a plain read/write scratch array (test 16).
  - **Timing model**: `soc_tick()` — called once per `arm1176_step()` by
    the caller — charges one timer tick per CPU instruction. Deterministic,
    not cycle-accurate.

## What's explicitly NOT implemented yet

The to-do list is split into two independent parts so each can be worked
through to completion without drifting into the other. **Do Part A (the
ARM1176JZF-S core) first, then Part B (the S5L8900 SoC/board).** Within
each part, items are ordered by next logical milestone. IDs (A1, B3, ...)
are stable so commits and notes can refer to them.

Status right now: Part A is untouched; Part B1 is partly done (the
bases for VIC, UART0, GPIO and clock are now real; timer/WDT/I2C/SPI/NOR
and the UART/timer register layouts are not). B1 was started at the
maintainer's request ahead of Part A, since it's a research task that
doesn't conflict with core work.

### Part A — ARM1176JZF-S core (`src/`, `include/cp15.h`, `vfp.h`, `jazelle.h`)

- **A1. ARMv6 / ARMv5E instruction coverage (ARM and Thumb state).**
  The largest known correctness gap: these currently decode as silent
  no-ops rather than executing or trapping, so real ARMv6-compiled code
  will misbehave without any visible error. An audit of
  `arm1176_decode.c` found none of the following implemented:
  - ARM-state ARMv6 media instructions — REV/REV16/REVSH,
    SXTB/SXTH/UXTB/UXTH (and the SXTAB/UXTAB-style add forms),
    SXTB16/UXTB16, PKHBT/PKHTB, SSAT/USAT (and 16-bit forms), the
    parallel add/subtract families (SADD16, UQSUB8, ...), SEL, USAD8/
    USADA8, and the dual-multiply group (SMLAD, SMUAD, SMLSD, SMMLA, ...).
    The decoder currently returns early for this whole encoding space.
  - Exclusive load/store — LDREX/STREX (and byte/halfword/doubleword
    forms where ARM1176 has them), CLREX.
  - System/state instructions — CPS (ARM and Thumb), SETEND, SRS, RFE.
  - ARMv5E additions — LDRD/STRD, PLD (cache preload hint), the
    saturating adds (QADD/QSUB/QDADD/QDSUB), and the signed 16-bit
    multiplies (SMULxy, SMLAxy, SMULWy, SMLAWy, SMLALxy), plus the Q flag
    they set.
  - Thumb-state ARMv6 encodings that share opcode space with the classic
    19 formats: CPS, SETEND, REV/REV16/REVSH, SXTB/SXTH/UXTB/UXTH
    (flagged with a TODO in `arm1176_thumb.c`).
  - A proper BKPT (ARM and Thumb); the current ARM-side check sits in a
    branch that a real BKPT encoding would never reach.
- **A2. Deferred corner cases** (each flagged with a `TODO` in source):
  the PC-as-operand-reads-+12 quirk for register-shifted data
  processing, unaligned `LDR` word-rotate behavior (and ARMv6's
  unaligned-access mode, SCTLR.U), full privilege enforcement on MSR,
  and alignment-fault checking (SCTLR.A is stored but not enforced).
- **A3. CP15 refinements and verification** (see `cp15.h`): supersections
  (16MB); the ARMv6 extended AP[2]/APX permission model (SCTLR.XP=1);
  the feature-ID registers in c0 (ID_PFR0/ID_MMFR0/...) which currently
  read as zero; FCSE; and double-checking against the real TRM the two
  layouts that are best-effort today — the 64KB large-page descriptor
  fields, and the c9 TCM region-register bit positions (an emulator-
  internal convention for now).
- **A4. VFP gaps** (see `vfp.h`): half-precision, precise FPSCR
  cumulative exception flags, flush-to-zero and default-NaN modes,
  CPACR-based coprocessor access control (only FPEXC.EN is enforced
  today), and verifying the VMLA/VMLS/VNMLA/VNMLS bit-6 polarity that
  is currently best-effort. (Fused multiply-add and fixed-point VCVT are
  *not* gaps: they're VFPv3/v4 features ARM1176's VFPv2 doesn't have.)
- **A5. TrustZone** (Monitor mode / secure world). Real ARM1176 has it;
  whether it matters depends on how the boot ROM / iBoot chain uses it,
  which is only answerable once real firmware runs far enough to tell —
  so this one is gated on Part B progress.
- **A6. Cycle-accurate timing (core side).** Cycle counts from
  `arm1176_step()` are rough approximations, not modeled against the real
  pipeline — including CP15 page-table-walk/TLB timing and VFP latency
  (VDIV/VSQRT charge a flat cost, not the real data-dependent latency).
  Low priority until something timing-sensitive shows up in real boot
  code.
- **A7. A real Jazelle bytecode engine**, only if iPhone OS 1.0 (or
  something else run on this emulator) actually needs one — see
  `jazelle.h` for why that's considered unlikely enough to defer
  indefinitely.

### Part B — S5L8900 SoC/board (`board/`, `include/soc.h`)

- **B1. Remaining real S5L8900 register values** *(partly done)*. Done:
  VIC0/VIC1, UART0, GPIO and clock bases, SDRAM/SRAM bases (inferred), and
  a table of the other blocks' addresses (SHA1, DMAC0/1, USB OTG, CLCD,
  NAND controller, AES, SDIO, power) in `soc.h`. **Still open:**
  1. Timer base address (and the real timer's register layout).
  2. Watchdog, I2C, SPI, USB PHY and NOR-related bases — they come after
     the point where the device-tree dump was truncated by the fetch
     tool's size limit. The fix is to read the rest of that dump
     (`dizimka/2922441` or the iPod1,1 pastebin `VubPbsE0`); the middle of
     the file is a very large audio-controller blob, so a raw download
     would be easier than the fetch tool.
  3. The UART's real register layout (Samsung-style, per the Linux
     binding hint) — replace the PL011 stand-in once a source for offsets
     is found; openiBoot's UART driver is the likely one.
  4. Whether the iPhone 2G's map differs from the iPod touch's.
  Rule for this item: only addresses traceable to a specific source get
  promoted from PLACEHOLDER; reasoning-only results are labeled INFERRED.
- **B2. A second VIC instance, and real IRQ line assignments.** VIC1's
  address is now known (`0x38E01000`) but nothing models it, and only VIC0's
  32 lines exist. The device tree lists an interrupt number for every
  device (and shows GPIO on 0-3 and 0x1f-0x21, so >32 lines are certainly
  needed); those numbers were deliberately not transcribed yet and should
  be, with the source open, as part of this item. UART/timer IRQ lines are
  still placeholders (5 and 6).
- **B3. I2C controller + PMU emulation.** Real power management
  (battery/charger, RTC, power-off) is NOT memory-mapped — it's an
  external chip talked to over I2C (`plat-s5l8900/pmu.c`). Nothing here
  emulates an I2C controller or a PMU device yet; boot code that expects
  real PMU responses will stall or misbehave.
- **B4. SPI controller + real NOR, and NAND.** NOR is currently a flat
  read-only memory window (fine for boot-time code fetch, not for
  anything that modifies flash, and not how real hardware accesses it —
  the clock-gate table shows `spi0 (nor)`). NAND, where iPhone OS 1.0
  itself actually lives, isn't modeled at all.
- **B5. Real timing for peripherals.** `soc_tick()` charges one timer
  tick per CPU instruction — deterministic, but unrelated to a real clock
  rate. The real rates are now known (24MHz crystal, 6MHz timebase, 412MHz
  CPU, 103MHz bus, 51.5MHz peripheral; constants in `soc.h`), so the
  remaining work is deriving timer behavior from them via the clock
  controller's PLL configuration, which needs B1's timer layout first.
- **B6. Peripherals not yet modeled** (addresses are in `soc.h` for most):
  display controller (CLCD), NAND controller, DMA, SHA1/AES engines, SDIO,
  USB OTG, multitouch, watchdog, RTC, and real GPIO pin modeling. Actually
  booting real firmware needs boot-ROM/firmware images supplied by the user
  from their own device — those are Apple's and are not something this
  project bundles or should.

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
