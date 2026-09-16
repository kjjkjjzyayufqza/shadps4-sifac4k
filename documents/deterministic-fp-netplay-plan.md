# Deterministic Floating Point for Lockstep P2P Titles (GVS / MBON)

Status: layers A and B are implemented, tested, and at full instruction coverage on both
titles; the diagnostics are implemented; C, D and E are not.
See section 11 for what was measured on real hardware and what remains.
Scope: `src/core/cpu_patches.*`, `src/core/module.cpp`,
       `src/core/libraries/libc_internal/libc_internal_math.cpp`,
       `src/shadnet/*`, `tests/`

---

## 1. Problem

Gundam Versus (GVS) and Gundam Extreme VS Maxi Boost ON (MBON, CUSA15006) run a
**lockstep** netplay model: shadNet only performs matchmaking and NAT traversal
(`CommandType::CreateRoom/JoinRoom/RequestSignalingInfos`, `src/shadnet/client.h`),
and the actual match runs peer-to-peer over the NP P2P UDP path
(`src/core/libraries/network/p2p_sockets.cpp`, `p2p_port.cpp`). Only **inputs**
cross the wire. Each peer advances an identical simulation locally.

Therefore any single-bit difference in simulation state between the two peers is
permanent and compounding. Symptoms are either an explicit desync error raised by
the title, or a silent divergence where both players see different worlds.

shadPS4 executes guest code **natively** - there is no JIT and no interpreter
(`src/core/module.cpp` maps `PT_LOAD` and jumps in). Consequently the guest's own
instruction stream, including its non-deterministic instructions, executes directly
on whatever host CPU the player owns.

### 1.1 What actually diverges on x86-64

IEEE-754 arithmetic (`ADD/SUB/MUL/DIV/SQRT`, scalar and packed, SSE and AVX, plus
FMA) is correctly rounded and therefore **bit-identical on every x86-64 CPU**. It is
not a divergence source.

Only two instruction classes are implementation-defined:

| Class | Instructions | Why they differ |
|---|---|---|
| Reciprocal / reciprocal-sqrt estimates | `RCPPS`, `RCPSS`, `RSQRTPS`, `RSQRTSS` and their VEX forms | Spec only guarantees relative error <= 1.5 * 2^-12. The lookup tables are vendor-specific: Intel, AMD and the PS4's Jaguar core all differ. |
| x87 transcendentals | `FSIN`, `FCOS`, `FPTAN`, `FPATAN`, `F2XM1`, `FYL2X`, `FYL2XP1`, `FXTRACT`, `FSCALE` | Microcoded, vendor-specific. |

Plus two *state* sources: MXCSR rounding mode / FTZ / DAZ, and the x87 control word.

GPU vendor differences (NVIDIA vs AMD) do **not** desync a lockstep title, because
GPU results never re-enter the simulation. This must still be positively verified
once (see 8.3), but it is not the mechanism being fixed here.

---

## 2. Measured evidence

Method: parse `.eh_frame_hdr` FDE search table to obtain authoritative function
boundaries, then disassemble each function individually (no linear sweep, so no
drift into `.rodata` and no false positives).

### MBON - CUSA15006 (46,888 functions, 3,039,154 instructions, .text 0x80-0xb8f1d0)

| Instruction | Count |
|---|---:|
| `vrsqrtss` | 1152 |
| `vrsqrtps` | 61 |
| `vrcpps` | 45 |
| **Total** | **1258 sites in 472 functions** |
| x87 transcendentals | 0 |

Encoded length distribution: **4 bytes x 1243, 5 bytes x 15**.

### GVS - Gundam Versus (71,985 functions, 9,375,294 instructions, .text 0xa0-0x2717870)

| Instruction | Count |
|---|---:|
| `vrsqrtps` | 1709 |
| `vrcpps` | 966 |
| `vrsqrtss` | 51 |
| **Total** | **2726 sites in 678 functions** |
| x87 transcendentals | 0 |

Encoded length distribution: **4 bytes x 2665, 5 bytes x 52, 8 bytes x 9**.

### Cross-check against the known-good reference

EXVS2-POC hardcodes the equivalent site list for the arcade builds:

| Target | Sites |
|---|---:|
| `EXVS2-POC/cpu/vs2/Vs2V29.h` | 1256 |
| `EXVS2-POC/cpu/xb/XbV27.h` | 1284 |
| `EXVS2-POC/cpu/over/ObV27.h` | 1284 |

MBON's 1258 against EXVS2 v29's 1256 confirms the same engine and math library
lineage, and confirms the expected order of magnitude. GVS's distribution is
inverted (packed-heavy) because that engine generation is more aggressively
vectorised.

### Host libm surface (shadPS4-specific, absent from EXVS2-POC)

`src/core/libraries/libc_internal/libc_internal_math.cpp` forwards
`libSceLibcInternal` math to the **host** C runtime. Cross-referencing the
registered NIDs against each title's import table:

| Title | Host libm functions actually imported |
|---|---|
| MBON | `sinf cosf tanf asinf acosf atanf atan2f expf exp2f powf logf log10f` (12) |
| GVS | `asinf acosf atanf atan2f expf exp2f powf tanf` (8) |

---

## 3. What is already correct - do not touch

| Item | Location | State |
|---|---|---|
| Guest MXCSR (`0x9fc0` = FTZ + DAZ + all exceptions masked + round-to-nearest) | `src/core/thread.cpp:22` | Correct |
| Guest x87 control word (`0x037f`) | `src/core/thread.cpp:23` | Correct |
| Applied on every guest pthread | `src/core/libraries/kernel/threads/pthread.cpp:263` | Correct |
| Preserved across fibers | `src/core/libraries/fiber/fiber.cpp:105,131,312` | Correct |
| Trampoline area per module (8 MB) | `src/core/module.cpp:109` | Sufficient for 2726 sites |

---

## 4. Obstacles and how each is resolved

### O1 - The reference implementation does not handle VEX encoding

`ArgoGulskii/determinize` (the library EXVS2-POC uses) matches only three
mnemonics in `ShouldDeterminize()`:

```cpp
case ZYDIS_MNEMONIC_RCPPS:
case ZYDIS_MNEMONIC_RSQRTPS:
case ZYDIS_MNEMONIC_RSQRTSS:
```

Zydis assigns **separate mnemonics** to VEX forms
(`externals/zydis/include/Zydis/Generated/EnumMnemonic.h:1660,1691,1693` ->
`ZYDIS_MNEMONIC_VRCPPS`, `VRSQRTPS`, `VRSQRTSS`). Every single site in GVS and MBON
is VEX-encoded, so none of the three cases ever fire.

`determinize` additionally rejects anything that is not two-operand:

```cpp
if (disasm.info.operand_count != 2) { ... return {}; }
```

`vrsqrtss dst, src1, src2` is three-operand, so even after adding the mnemonics it
would bail at every site.

**Resolution:** write new VEX-aware generators (section 5.2). Treat `determinize`
as a semantic reference for the legacy forms only.

### O2 - 98% of sites are 4 bytes, shorter than a near jump

`TryPatch` (`src/core/cpu_patches.cpp:613`) refuses any trampoline patch whose
instruction is shorter than `NearJumpSize` (5, defined at line 529):

```cpp
if (needs_trampoline && instruction.length < NearJumpSize) {
    // Trampoline is needed but instruction is too short to patch.
    return std::make_pair(false, instruction.length);
}
```

`vrsqrtss xmm1, xmm0, xmm0` encodes as `C5 FA 52 C8` - 4 bytes. This covers
1243/1258 MBON sites and 2665/2726 GVS sites.

**Resolution - already present in this fork.** `PatchRedZoneMemoryInstructions`
(`src/core/cpu_patches.cpp`, ~line 1596 onward) implements a complete
instruction-relocation pass that solves exactly this:

- `DecodeFunction(function_start, function_end, ...)` gives verified instruction
  boundaries per function, derived from `.eh_frame` (`src/core/loader/dwarf.cpp`).
- `FindMatchingPatch(decoded)` looks the instruction up in the `Patches` table, and
  when `TryPatch` declines (too short) the site is queued:
  `rewrite_sites.emplace(address, InstructionRewrite{.cpu_patch = matching_patch})`.
- `collect_forward_span()` / `collect_backward_span()` absorb neighbouring
  instructions until the patch region reaches `NearJumpSize`, guarded against
  stealing an instruction that is a branch target
  (`function.branch_targets.contains(...)`), against crossing a control-flow
  terminator (`IsControlFlowTerminator`), against functions with indirect branches
  (`can_relocate_neighbors = !function.has_indirect_branch`), and against
  overlapping an already-patched span (`covered_until`).
- `emit_span()` re-encodes the span into the trampoline, invoking
  `rewrite->second.cpu_patch->generator(...)` for the patched instruction and
  `EncodeRelocatedInstruction(...)` for the stolen neighbours, then
  `jmp continuation`.
- When no 5-byte span can be formed, a two-stage relay is used: a 2-byte
  `jmp rel8` (`ShortJumpSize`, line 951) at the site into a `relay_slot` - leftover
  space inside an already-patched span within +/-127 bytes - which then performs the
  5-byte near jump. `find_host()` can synthesise a relay host if none exists.
- Failures are counted in `RedZonePatchResult::unsupported_cpu_patch_instruction_count`.

So the machinery exists and is production-grade. The problem is **where it lives**,
not whether it works (see O4).

### O3 - There is no fault to hook, so the lazy path cannot be the fallback

The comment in `TryPatch` says short instructions are "handled at runtime". That
fallback is `TryPatchJit` driven by `PatchesAccessViolationHandler`
(`src/core/cpu_patches.cpp:2131,2151`). It works for SSE4a because `EXTRQ` /
`INSERTQ` raise `#UD` on an Intel host.

`vrsqrtss` raises nothing. It executes correctly on every host and simply produces
a host-specific value. **No exception is ever delivered, so the lazy path can never
fire for this patch class.**

**Resolution:** deterministic-FP patching must be 100% AOT. Any site the AOT pass
cannot patch is a permanent correctness hole and must be counted and reported, not
silently deferred. This makes the coverage assertion in 8.2 mandatory rather than
optional.

### O4 - The relocation pass is Windows-only, red-zone-specific, and disabled by default

- Gated by `WindowsGuestRedZoneProtection::IsStaticPatchingEnabled()`.
- Wrapped in `#if defined(ARCH_X86_64) && defined(_WIN32)` at both the call site
  (`src/core/module.cpp:181-186, 209-217`) and the `.eh_frame` table decode
  (`Dwarf::DecodeEHHdrTable` is only invoked under the same guard).
- Default is `WindowsGuestRedZoneProtectionMode::Disabled`
  (`src/core/emulator_settings.h:413-415`).
- `rewrite_sites` are only collected for functions reached by the red-zone walk, and
  the whole pass returns early when `function_starts.empty()`.

**Resolution:** generalise the pass into a platform-independent static AOT rewrite
pass with two independent *reasons* - red-zone protection and deterministic FP -
that share one function walk, one relocation engine and one trampoline. See 5.1.

### O5 - Spilling a scratch register would corrupt the guest red zone

`determinize` allocates scratch space with `sub rsp, 16` /
`movups [rsp], scratch`. That is safe for EXVS2 because the arcade binary is a
Windows MS-ABI executable, which has no red zone.

**Guest PS4 code is SysV ABI and has a 128-byte red zone.** Writing at `[rsp-16]`
clobbers live guest data. Copying `determinize`'s prologue verbatim would introduce
exactly the class of silent state corruption that this fork's red-zone pass exists
to prevent, and it would do so at 1258-2726 sites.

**Resolution:** prefer replacement sequences that need **no scratch register and no
stack at all** (section 5.2). Where a scratch is unavoidable, displace past the red
zone first, matching the existing idiom
`module->trampoline_gen.lea(rsp, ptr[rsp - GuestRedZoneSize])`
(`GuestRedZoneSize = 128`, line 950).

### O6 - Upper-lane and vector-width semantics are easy to get silently wrong

- `vrsqrtss dst, src1, src2` writes `dst[31:0] = est(src2[31:0])`,
  `dst[127:32] = src1[127:32]`, `dst[255:128] = 0`. Dropping the `src1` merge
  corrupts three lanes.
- Legacy `rsqrtss dst, src` **preserves** `dst[127:32]`, so `dst` is not dead on
  entry. `determinize` handles this with `movaps scratch, dst` before `sqrtss`.
- `vrsqrtps` exists in a 256-bit form (`vrsqrtps ymm4, ymm2` occurs in both titles).
  Emitting the 128-bit replacement silently zeroes `ymm[255:128]`.

A bug here does not crash. It manifests as rare, input-dependent misbehaviour in
effects or physics - the hardest possible thing to debug.

**Resolution:** width and lane semantics are encoded per-generator and pinned by the
differential unit tests in 8.1, which compare against the real instruction across the
full special-value set.

### O7 - The host libm HLE is a second, independent divergence source

```cpp
float PS4_SYSV_ABI internal_sinf(float x) { return sinf(x); }   // host libm
float PS4_SYSV_ABI internal_powf(float x, float y) { return powf(x, y); }
```

Results differ across: Windows UCRT vs glibc vs macOS libm; glibc versions; UCRT
versions; and **Intel vs AMD on the same glibc**, because glibc selects
`sinf`/`powf`/`expf` implementations by ifunc on CPUID (FMA/AVX2 variants versus
SSE2 variants).

EXVS2-POC has no equivalent exposure: the arcade binary statically links its own
libm, so every player executes identical bytes. shadPS4 removed that property.

**If O7 is not fixed, fixing O1-O6 does not make netplay deterministic.**

**Resolution:** vendor a deterministic libm (5.3).

### O8 - Both peers must run the same patch set, or the situation gets worse

A patched client and an unpatched client desync *differently* than two unpatched
clients. Silent mismatch must be impossible.

**Resolution:** advertise a determinism profile ID and refuse mismatched
matchmaking (5.4).

### O9 - Other divergence sources are hypothesised but not yet proven absent

Static analysis proves what *can* diverge; it does not prove the enumerated list is
*complete*. Still unverified:

- **Variable timestep.** Both titles import `sceKernelGetTscFrequency`; MBON also
  imports `sceKernelGettimeofday`. If the simulation uses a wall-clock-derived `dt`,
  host frame-rate differences desync regardless of any FP work.
- **GPU readback into simulation state.** Considered very unlikely for this engine
  generation, but not excluded by static analysis.
- **SSE4a software emulation.** On Intel hosts `EXTRQ`/`INSERTQ` run through
  `GenerateEXTRQ`/`GenerateINSERTQ`; on AMD hosts they execute natively. These are
  integer bitfield operations and are exactly emulable, but the equivalence has not
  been proven by test.
- **Thread scheduling order** affecting simulation-visible state.

**Resolution:** build the divergence harness (5.5) **first** and use it to identify
the actual first divergent frame before landing any rewrite work. This converts the
whole effort from speculative to evidence-driven, and it is the single highest-value
item in this plan.

---

## 5. Design

Five layers. Layer E is built and run first.

### 5.1 Layer A - generalised static AOT rewrite pass

Refactor `PatchRedZoneMemoryInstructions` into a reason-agnostic pass.

**New public surface (`src/core/cpu_patches.h`):**

```cpp
enum class StaticRewriteReason : u32 {
    None             = 0,
    GuestRedZone     = 1 << 0,  // Windows guest red-zone protection
    DeterministicFp  = 1 << 1,  // lockstep-safe FP approximation replacement
};
DECLARE_ENUM_FLAG_OPERATORS(StaticRewriteReason)

struct StaticRewriteResult {
    // existing RedZonePatchResult fields, retained verbatim
    u64 function_count{};
    u64 instruction_count{};
    // ...
    // deterministic FP accounting
    u64 fp_approximation_site_count{};      // sites found
    u64 patched_fp_approximation_count{};   // sites successfully rewritten
    u64 unsupported_fp_approximation_count{};
};

StaticRewriteResult ApplyStaticRewrites(u64 segment_addr, u64 segment_size,
                                        std::span<const uintptr_t> function_starts,
                                        StaticRewriteReason reasons);
```

`PatchRedZoneMemoryInstructions` becomes a thin wrapper passing
`StaticRewriteReason::GuestRedZone`, so the existing Windows path is unchanged.

**Changes inside the pass:**

- The `for (auto& [address, decoded] : function.instructions)` loop that populates
  `rewrite_sites` gains a deterministic-FP branch. When
  `reasons & DeterministicFp` and the mnemonic is in the approximation set, the site
  is queued as a `cpu_patch` rewrite exactly like the existing too-short path, and
  `fp_approximation_site_count` is incremented.
- The `if (function.uses_red_zone)` block stays gated on
  `reasons & GuestRedZone`, so a non-Windows build performs no red-zone analysis.
- `AnalyzeRedZoneLiveness(function)` is skipped entirely unless
  `reasons & GuestRedZone` - it is the expensive part of the walk and is irrelevant
  to FP rewriting.
- Everything downstream - `RelocationSpan`, `collect_forward_span`,
  `collect_backward_span`, `emit_span`, `relay_slots`, `short_relay_slots`,
  `find_host`, `covered_until`, `unresolved_sites` - is reused **unmodified**. This
  is the whole point of the refactor.

**Platform gating (`src/core/module.cpp`):**

- Move `std::vector<uintptr_t> function_starts;` and the `executable_segments`
  vector out of `#if defined(ARCH_X86_64) && defined(_WIN32)` into
  `#if defined(ARCH_X86_64)`.
- Always run `Dwarf::DecodeEHHdrTable` when any static rewrite reason is active, not
  only under the Windows red-zone toggle.
- Compute the reason mask once:

```cpp
StaticRewriteReason reasons = StaticRewriteReason::None;
#if defined(_WIN32)
if (WindowsGuestRedZoneProtection::IsStaticPatchingEnabled()) {
    reasons |= StaticRewriteReason::GuestRedZone;
}
#endif
if (Config::deterministicFloatingPoint()) {
    reasons |= StaticRewriteReason::DeterministicFp;
}
```

**Failure policy.** If a title has `DeterministicFp` enabled and
`unsupported_fp_approximation_count != 0`, log at `LOG_CRITICAL` and surface it to
the netplay layer (5.4). Partial coverage is not acceptable for lockstep and must
never be reported as success.

**Ordering constraint.** `PrePatchInstructions` (the existing blind AOT linear
sweep, `src/core/cpu_patches.cpp:2194`) runs before the function-boundary pass and
may already have rewritten SSE4a sites in place. The FP pass re-decodes instructions
after `TryPatch` in the existing loop, so this is already handled; do not reorder.

### 5.2 Layer B - replacement generators

Register in the `Patches` table (`src/core/cpu_patches.cpp:537`):

```cpp
// Deterministic FP. The reciprocal / reciprocal-sqrt estimates are
// implementation-defined: Intel, AMD and Jaguar each use different tables, so two
// peers running a lockstep title diverge. Replace them with exact IEEE-754
// div/sqrt, which is bit-identical on every x86-64 host.
{ZYDIS_MNEMONIC_VRCPPS,   {{FilterDeterministicFp, GenerateVRCPPS,   true}}},
{ZYDIS_MNEMONIC_VRSQRTPS, {{FilterDeterministicFp, GenerateVRSQRTPS, true}}},
{ZYDIS_MNEMONIC_VRSQRTSS, {{FilterDeterministicFp, GenerateVRSQRTSS, true}}},
{ZYDIS_MNEMONIC_VRCPSS,   {{FilterDeterministicFp, GenerateVRCPSS,   true}}},
{ZYDIS_MNEMONIC_RCPPS,    {{FilterDeterministicFp, GenerateRCPPS,    true}}},
{ZYDIS_MNEMONIC_RSQRTPS,  {{FilterDeterministicFp, GenerateRSQRTPS,  true}}},
{ZYDIS_MNEMONIC_RSQRTSS,  {{FilterDeterministicFp, GenerateRSQRTSS,  true}}},
{ZYDIS_MNEMONIC_RCPSS,    {{FilterDeterministicFp, GenerateRCPSS,    true}}},
```

`FilterDeterministicFp` returns the config flag. Note this is the **opposite**
polarity to `FilterNoSSE4a`: SSE4a patching depends on host capability, whereas
determinism patching must be applied uniformly regardless of host, precisely so all
hosts agree.

**Constant pool.** Emit one 32-byte-aligned `{1.0f x 8}` block per module into the
trampoline area on first use and address it rip-relative from every trampoline. One
constant serves all 1258-2726 sites and both 128- and 256-bit forms.

#### Preferred sequences - zero scratch registers, zero stack

Using `1/sqrt(x) == sqrt(1/x)`. Both `vdivps` and `vsqrtps` are correctly rounded,
so the composed result is bit-identical on every host, which is the only property
required. `dst` is fully overwritten by all VEX forms, so it is dead on entry and
free to use as working space.

```
; vrcpps dst, src                     (width follows operands: xmm or ymm)
vmovaps dst, [rip + ones]             ; dst = {1.0, ...}
vdivps  dst, dst, src                 ; dst = 1/src          exact, single rounding

; vrsqrtps dst, src
vmovaps dst, [rip + ones]
vdivps  dst, dst, src                 ; dst = 1/src
vsqrtps dst, dst                      ; dst = sqrt(1/src)

; vrsqrtss dst, src1, src2
vmovaps dst, [rip + ones]             ; dst = {1.0, 1.0, 1.0, 1.0}
vdivss  dst, dst, src2                ; dst = {1/src2, 1.0, 1.0, 1.0}
vsqrtss dst, src1, dst                ; dst = {sqrt(1/src2), src1[127:32]}, upper zeroed
```

The last line is the key detail: `vsqrtss dst, src1, dst` takes the upper lanes from
`src1`, which is exactly the merge semantics `vrsqrtss` requires. No blend, no
scratch, no stack.

**Edge-case behaviour of the reordered form:**

| Input | `rsqrt` hardware | `sqrt(1/x)` | Match |
|---|---|---|---|
| `+0` | `+inf` | `sqrt(+inf) = +inf` | yes |
| `+inf` | `+0` | `sqrt(+0) = +0` | yes |
| `x < 0` | `NaN` | `sqrt(negative) = NaN` | yes |
| `NaN` | `NaN` | `NaN` | yes |
| `-0` | `-inf` | `sqrt(-inf) = NaN` | **no** |

`-0` is the only deviation. The dominant use is vector normalisation, where the
argument is a sum of squares and can only ever be `+0`, never `-0`. DAZ does not
affect `-0` (it is not denormal). The deviation is still deterministic across hosts,
so it cannot desync; it can only differ from real hardware. This must be covered by
an explicit unit test, and if a title is ever found to depend on it, that generator
falls back to the scratch form below.

#### Fallback sequences - one scratch register

Required for `VRCPSS`/`RCPSS` (no zero-scratch form exists) and for legacy
`RSQRTSS` (SSE preserves `dst[127:32]`, so `dst` is not dead on entry).

```
; vrsqrtss dst, src1, src2   -- exact ordering, correct -0
vsqrtss  scratch, src1, src2          ; {sqrt(src2), src1[127:32]}
vmovaps  dst, [rip + ones]
vdivss   dst, dst, scratch            ; {1/sqrt, 1.0, 1.0, 1.0}
vblendps dst, dst, scratch, 0b1110    ; lane 0 from dst, lanes 1-3 from scratch

; rsqrtss dst, src            -- legacy, dst[127:32] must survive
movaps   scratch, dst
sqrtss   scratch, src
cvtsd2ss dst, [rip + one_f64]
divss    dst, scratch
```

The legacy form above is `determinize`'s sequence and is semantically correct as
written; only its scratch *allocation* must change.

**Scratch allocation rule.** Pick the lowest-numbered XMM register not equal to
`dst`, `src1` or `src2` (same idea as `determinize`'s `XmmScratch`). Spill it with
the red-zone-safe idiom, never bare `sub rsp`:

```cpp
gen.lea(rsp, ptr[rsp - GuestRedZoneSize - 32]);
gen.vmovups(ptr[rsp], scratch);
// ... replacement body ...
gen.vmovups(scratch, ptr[rsp]);
gen.lea(rsp, ptr[rsp + GuestRedZoneSize + 32]);
```

Note `vmovups`, not `movups`: mixing legacy SSE and VEX encodings on the same
registers triggers AVX-SSE transition penalties, which is measurable across 1258+
hot sites.

**VEX2 / VEX3 selection.** Let Xbyak choose the encoding. Generators write into the
trampoline, where length is unconstrained, so there is no need to match the original
instruction's length.

### 5.3 Layer C - deterministic libm

Three options, evaluated:

| Option | Description | Verdict |
|---|---|---|
| **A. Vendored deterministic libm** | Import the FreeBSD `msun` float routines (`sinf`, `cosf`, `tanf`, `asinf`, `acosf`, `atanf`, `atan2f`, `expf`, `exp2f`, `powf`, `logf`, `log10f`), compile with `-ffp-contract=off` and no fast-math. | **Recommended.** PS4's libc is FreeBSD-derived, so this is also the closest behavioural match to real hardware. Results depend only on vendored source, not on the host. |
| B. LLE the real module | Load the title's / firmware `libSceLibcInternal.sprx`. | Most faithful, but sysmodule coverage and stability is a separate project. |
| C. Pin host ifunc dispatch | Force glibc onto SSE2 paths. | Fragile, no Windows equivalent. Rejected. |

Implementation notes for option A:

- New directory `src/core/libraries/libc_internal/deterministic_math/`, compiled as
  its own target so that global optimisation flags cannot alter its semantics.
- Verify the build does not contract `a*b+c` into FMA: FMA changes results and the
  host compiler may or may not apply it. `-ffp-contract=off` on GCC/Clang,
  `/fp:precise` on MSVC.
- Only the 12 functions MBON imports plus the 8 GVS imports need deterministic
  implementations; the rest may continue to forward to the host until needed.
  Track which are covered.
- Add a load-bearing comment at the top of `libc_internal_math.cpp`:

```cpp
// These must NOT forward to the host libm. Host results differ between
// glibc/UCRT versions and between Intel and AMD (glibc selects sinf/powf/expf
// implementations by ifunc on CPUID), which desyncs lockstep P2P titles such
// as GVS and MBON. See documents/deterministic-fp-netplay-plan.md.
```

- Gate on the same config flag as Layer A so single-player performance is unaffected
  and so a "determinism profile" is a single coherent thing (5.4).

### 5.4 Layer D - netplay determinism profile negotiation

A determinism profile is the tuple: instruction rewrite set version + libm version +
MXCSR policy. Compute a stable `u32` ID from it at build time.

- Send it via the existing `CommandType::SetClientVersion` (13) and read server
  capability through `GetServerFeatures` (12) - `src/shadnet/client.h:106-108`.
- The server must refuse to place clients with mismatched profile IDs into the same
  room (`CreateRoom` / `JoinRoom` / `SearchRoom` filtering).
- If `unsupported_fp_approximation_count != 0` for the loaded module, the client
  reports a **degraded** profile that matches nothing. Failing closed is mandatory:
  a partially patched client is worse than an unpatched one.
- Surface the profile in the UI so cross-platform expectations are explicit.

### 5.5 Layer E - divergence diagnosis harness (build this first)

Without this, every other layer is a guess about the mechanism.

**Input recording / replay.** Capture the guest pad state per simulation tick and
replay it deterministically. Hook at the pad library boundary
(`src/core/libraries/pad/`), not at the host input layer.

**State hashing.** CRC64 over the simulation state block each tick, logged with the
tick index. Locating the state block is the hard part; a practical proxy that
requires no reverse engineering is to hash the guest's writable `PT_LOAD` segment at
a fixed cadence. It is noisy but sufficient to bisect to a first divergent tick, and
from there to the owning function via a write watchpoint.

**Comparison.** Run the same recording on two hosts (one Intel, one AMD; one NVIDIA,
one AMD GPU) and diff the hash streams. The first divergent tick index identifies
the responsible subsystem far faster than observing visual desync.

This harness also resolves O9 directly:

- Run the same recording twice on one machine with different GPUs or a software
  Vulkan device. Identical hashes exclude GPU readback.
- Run twice on one machine at locked 60 fps and at unlocked frame rate. Identical
  hashes exclude variable-timestep dependence.
- Run once on an Intel host and once on an AMD host with deterministic FP **off**,
  and confirm the first divergent tick lands in a function that contains a known
  approximation site. That is the direct proof that the enumerated mechanism is the
  real one.

---

## 6. Change list by file

| File | Change |
|---|---|
| `src/core/cpu_patches.h` | Add `StaticRewriteReason`, `StaticRewriteResult`, `ApplyStaticRewrites`. Keep `PatchRedZoneMemoryInstructions` as a wrapper. |
| `src/core/cpu_patches.cpp:537` | Add 8 entries to the `Patches` table. |
| `src/core/cpu_patches.cpp` (new) | `FilterDeterministicFp`, `GenerateVRCPPS`, `GenerateVRSQRTPS`, `GenerateVRSQRTSS`, `GenerateVRCPSS`, `GenerateRCPPS`, `GenerateRSQRTPS`, `GenerateRSQRTSS`, `GenerateRCPSS`; module-local ones-constant emitter; red-zone-safe scratch spill helper. |
| `src/core/cpu_patches.cpp` (~1596+) | Parameterise the pass by reason; add the FP site-collection branch; skip `AnalyzeRedZoneLiveness` when red-zone is not requested; add FP counters. |
| `src/core/module.cpp:181-217` | Lift `function_starts` / `executable_segments` / `DecodeEHHdrTable` out of the Windows-only guard; compute and pass the reason mask; log coverage. |
| `src/core/emulator_settings.h` | Add `deterministic_floating_point` setting, default off, exposed as an overrideable per-title field. |
| `src/core/libraries/libc_internal/libc_internal_math.cpp` | Route the 12 float entry points to the vendored implementations when the flag is on; add the load-bearing comment. |
| `src/core/libraries/libc_internal/deterministic_math/` (new) | Vendored FreeBSD `msun` float routines, own compile target, `-ffp-contract=off`. |
| `src/shadnet/client.cpp/.h` | Determinism profile ID in `SetClientVersion`; consume `GetServerFeatures`; fail closed on degraded coverage. |
| `tests/cpu/test_deterministic_fp.cpp` (new) | Differential tests, section 8.1. |
| `tests/CMakeLists.txt` | Register the new test target. |

---

## 7. Reference material

| Source | Use |
|---|---|
| `github.com/asesidaa/EXVS2-POC` - `EXVS2-POC/dllmain.cpp` | Proof that this exact engine family requires the fix; shows the call is made once at startup over a precomputed site list. |
| `EXVS2-POC/cpu/{vs2,xb,over}/*.h` | Independent site-count corroboration (1256 / 1284 / 1284). |
| `github.com/ArgoGulskii/determinize` - `src/replacements.cpp` | Semantic reference for legacy SSE replacement, including the `movaps scratch, dst` trick that preserves `dst[127:32]` on `rsqrtss`. |
| `github.com/ArgoGulskii/determinize` - `src/determinize.cpp` | Reference for span collection past the jump site, and for the `ShouldDeterminize` instruction set. **Its mnemonic list and 2-operand assumption must not be copied.** |
| Intel SDM, `RSQRTSS` / `RCPPS` entries | Normative statement that the estimate tables are implementation-defined (relative error <= 1.5 * 2^-12). |
| Bruce Dawson, "Floating-Point Determinism" | Background on which x86 operations are and are not reproducible. |
| This fork, `PatchRedZoneMemoryInstructions` | The relocation engine being reused. Read `collect_forward_span` / `collect_backward_span` / `find_host` before touching anything. |

**Do not copy from `determinize`:** its `sub rsp, 16` scratch prologue (O5), its
mnemonic set (O1), or its 2-operand assumption (O1).

---

## 8. Validation

### 8.1 Differential instruction tests - mandatory, no networking required

For each generator, execute the original instruction and the replacement sequence in
the same process and compare full register state bit-for-bit.

Input set per lane:
`+0`, `-0`, `+inf`, `-inf`, quiet NaN, signalling NaN, smallest and largest
normals, denormals (verifying DAZ behaviour), `1.0`, `4.0`, `2^-126`, `2^127`,
plus 10^6 random bit patterns.

Assertions:

1. The replacement produces **bit-identical results on Intel and AMD**. This is the
   property being bought. The original instruction will differ - that difference is
   what proves the test is meaningful.
2. Lanes `[127:32]` (and `[255:128]` for VEX) match the documented merge/zero
   semantics of the instruction being replaced.
3. Documented deviations (the `-0` case of the reordered form) are asserted
   explicitly, so a future change cannot alter them silently.
4. `MXCSR` exception flags after the replacement do not acquire bits that would be
   visible to the guest beyond what the original would set.

Place in `tests/cpu/`, alongside the existing `tests/network/` style.

### 8.2 Coverage assertion

After the AOT pass, log:

```cpp
LOG_INFO(Core, "Deterministic FP: {} sites found, {} patched, {} unsupported in {}",
         result.fp_approximation_site_count, result.patched_fp_approximation_count,
         result.unsupported_fp_approximation_count, name);
```

Expected values - these are measured ground truth, not estimates:

| Title | Sites | Required |
|---|---:|---|
| MBON CUSA15006 | 1258 | patched == 1258, unsupported == 0 |
| GVS | 2726 | patched == 2726, unsupported == 0 |

Any other number is a regression. This is the cheapest possible guard and it is the
only thing that catches a silently degraded pass, because O3 means there is no
runtime fallback.

### 8.3 Hash-stream equivalence

Using Layer E, on an Intel host and an AMD host:

1. Deterministic FP off -> hash streams diverge (confirms the mechanism).
2. Deterministic FP on, host libm still in use -> may still diverge (confirms O7
   independently).
3. Deterministic FP on, vendored libm on -> hash streams identical for the full
   recording.

Only step 3 passing constitutes a fix.

### 8.4 SSE4a emulation equivalence

Separate differential test for `GenerateEXTRQ` / `GenerateINSERTQ` against native
execution on an AMD host, closing the O9 sub-item.

---

## 9. Risks and accepted trade-offs

| Risk | Assessment |
|---|---|
| **Performance** | `vrsqrtss` (~5 cycles) becomes 3 instructions (~25-30 cycles). GVS's 2675 packed sites become `vdivps` + `vsqrtps` on `ymm` (~30-40 cycles). Must be a toggle, enforced only for netplay. Measure before and after on a GPU-unbound scene. |
| **Trampoline pressure** | 8 MB per module (`src/core/module.cpp:109`) against 2726 sites; ample, but `trampoline_exhausted` must be treated as a hard failure under `DeterministicFp`, not a silent skip. |
| **Divergence from real hardware** | Results become more accurate than a real PS4. Irrelevant for lockstep, but save data and replays produced under this mode are not hardware-comparable. Document it. |
| **Relocation correctness** | Stealing neighbouring instructions is inherently risky. The existing guards (branch targets, control-flow terminators, indirect-branch functions, `covered_until`) are the mitigation; do not weaken them to raise coverage. If coverage is short, fix the relay path, never the guards. |
| **Cross-platform play before Layer C lands** | Windows and Linux peers will desync. Must be stated in the UI, not left for players to discover. |
| **Mixed-version play** | Prevented by Layer D failing closed. |
| **Unproven completeness** | O9 remains open until Layer E reports identical hash streams end to end. Until then, this plan fixes a proven mechanism, not necessarily the only one. |

---

## 10. Execution order

1. **Layer E** - recording, replay, hash streams. Identify the real first divergent
   tick. Do not skip; everything else is speculative without it.
2. **Layer B generators + 8.1 differential tests.** Testable standalone, no
   integration needed.
3. **Layer A refactor**, red-zone path unchanged, plus the 8.2 coverage assertion.
4. **Layer C** vendored libm.
5. **Layer D** profile negotiation, failing closed.
6. **8.3 end-to-end** on an Intel/AMD host pair.


---

## 11. Implementation status and measurements

### 11.1 What landed

| Layer | State | Where |
|---|---|---|
| B - replacement generators | Done, 19 differential tests | `src/core/cpu_patches_fp.{h,cpp}`, `tests/cpu/test_deterministic_fp.cpp` |
| A - generalised static AOT rewrite pass | Done | `ApplyStaticRewrites` in `src/core/cpu_patches.cpp`, `src/core/module.cpp` |
| Setting | Done, per-title override, default off | `DeterministicFpSettings` in `src/core/emulator_settings.h`, JSON group `DeterministicFp` |
| Startup diagnostics | Done, always on in release | `src/core/deterministic_fp_report.{h,cpp}` |
| Verified branch targets | Done, takes MBON to full coverage | `src/core/branch_targets.{h,cpp}`, `scripts/export_branch_targets.py` |
| C - deterministic libm | Not started | - |
| D - profile negotiation | Not started | - |
| E - divergence harness | Not started | - |

The pass is no longer Windows-only: `PatchRedZoneMemoryInstructions` is a wrapper over
`ApplyStaticRewrites`, which takes a `StaticRewriteReason` mask, skips
`AnalyzeRedZoneLiveness` unless the red-zone reason is set, and shares one function walk,
one relocation engine and one trampoline between the reasons.

FP patch entries are marked `verified_boundaries_only`, so `TryPatch` refuses them from
the blind linear sweep in `PrePatchInstructions` (Linux) and accepts them only from the
`.eh_frame`-driven walk. A linear sweep can start mid-instruction; replacing an estimate
at a phantom site would corrupt whatever really lives at those bytes.

### 11.2 Measured coverage - Windows, Core i7-12700K

Both titles were booted with the setting on. Neither exhausted its trampoline and neither
logged a patch failure.

| Title | 8.2 predicted | Found at runtime | Replaced | Unsupported |
|---|---:|---:|---:|---:|
| MBON CUSA15006 | 1258 | 1243 | 1162 | 81 (6.5%) |
| GVS CUSA08379 | 2726 | 2535 | 2314 | 221 (8.7%) |

```
[FPDIAG] coverage eboot.bin: 1243 estimate sites found, 1162 replaced, 81 left unsupported
[FPDIAG] shortfall for eboot.bin: 0 unexpressible operand shapes, 81 in functions with
         branch tables, 0 with no relocatable span or relay

[FPDIAG] coverage eboot.bin: 2535 estimate sites found, 2314 replaced, 221 left unsupported
[FPDIAG] shortfall for eboot.bin: 0 unexpressible operand shapes, 221 in functions with
         branch tables, 0 with no relocatable span or relay
```

GVS's 221 are 150 `vrsqrtps`, 61 `vrcpps`, 10 `vrsqrtss`.

Both runtime counts come in under the static prediction, which counted every estimate in
the file; the runtime walk only sees functions the `.eh_frame` search table lists and that
`DecodeFunction` reaches. Every single failure in both titles has the same cause: a 4-byte
VEX encoding inside a function whose indirect jump `ResolveBoundedJumpTable` could not
resolve, so `can_relocate_neighbors` is false and no 5-byte span can be formed. Neither
title produced a single operand-shape refusal.

GVS booting at all is also the evidence that the 256-bit path works: a generator that
emitted a 128-bit replacement for a `ymm` operand throws `ERR_BAD_COMBINATION`, which the
pass rethrows rather than swallowing, so the module would have failed to load.

Each unpatched site is logged individually with its module-relative address, mnemonic,
encoding bytes and owning function, so the list can be worked through from a log alone.

Two candidate fixes, neither implemented:

- Narrow the guard. A *single-instruction* span is safe even in a function with an
  unresolved indirect jump, because the only instruction boundary inside it is its own
  start, which the near jump preserves. Only multi-instruction spans can be entered in
  the middle. This would need `find_host` restricted to single-instruction hosts, which
  are rare, so the yield is probably small.
- Trap the residue. Replace the 4-byte estimate in place with `ud2` plus two nops and
  emulate it in the illegal-instruction handler, reusing the machinery
  `TryExecuteIllegalInstruction` and `GetXmmPointer` already provide for 4-byte
  `EXTRQ`/`INSERTQ`. Always correct, but costs an exception per execution, so it is only
  acceptable if these 81 sites are cold. The per-site log identifies them for measurement.

### 11.3 Deviation found by the hardware probe, not predicted by section 5.2

Guest threads run with MXCSR `0x9fc0`, so FTZ is set. The reordered form computes `1/x`
first, and for `|x| > 2^127` that intermediate is denormal and is flushed before the
square root sees it:

```
probe rsqrtps in=7F7FFFFF hw=1F800800 fixed=00000000 differs
probe rsqrtss in=7F7FFFFF hw=1F800800 fixed=1F800001 differs
```

`rsqrtss` keeps hardware's ordering and so never forms the intermediate, which is why the
two replacements disagree with each other on this input. Both are deterministic across
hosts, so neither can desync a match; they differ only from a real PS4. Normalisation
feeds rsqrt a sum of squares, which reaches `2^127` only when its inputs have already
overflowed. Pinned by `ReorderedFormUnderflowsToZeroWhereHardwareDoesNot`.

This is also why the probes install the guest MXCSR: under host defaults the report
describes flush and denormal behaviour the title never sees.

### 11.4 Cross-vendor verification

8.3 cannot be run locally - only Intel hardware is available. The startup report exists to
close that gap remotely: it records the host CPU identity, what the host's estimate
instructions return, what the replacement returns, and what the host libm returns, all
under the guest FP environment, and reduces each to a fingerprint.

Ask an AMD tester for the lines tagged `FPDIAG`. Then:

| Fingerprint | Expected | Meaning if it differs |
|---|---|---|
| `replacement.fingerprint` | identical on every host | the replacement itself is not deterministic - this is the bug |
| `libm.fingerprint` | identical on every host | O7 is live: the peers desync on the first transcendental regardless of instruction patching |
| `estimate.fingerprint` | differs between vendors | confirms the mechanism this work removes; identical values would mean the premise is wrong |



### 11.5 Closing the shortfall: verified branch targets

Every unpatched site in both titles had one cause: a 4-byte encoding inside a function
whose indirect jump `ResolveBoundedJumpTable` could not resolve, so the pass refused to
borrow neighbouring bytes anywhere in that function.

The refusal is not something the pass can lift on its own. A switch table holds offsets
against a base it cannot identify, so no amount of scanning proves that an address is
*not* a target. A disassembler that recovered the table does know.

Measured in IDA against `CUSA15006_mapped.elf` (image base `0x400000`, so an IDA address
is the module-relative offset plus that):

- The 81 sites live in **18 functions**; GVS's 221 live in **35**.
- `sub_CDCF90` (34 sites) and `sub_CE4F40` (12 sites) each contain a `jmp reg` that IDA
  resolves to a fully enumerated switch. `sub_BA5400` (10 sites) contains
  `jmp qword ptr [rax+70h]`, a dispatch through a vtable slot, which has no edge back
  into the function at all.
- Every one of the 81 sites is followed by an instruction with exactly one code xref:
  the fall-through from the site itself. Not one is a branch target. The sites sit in
  straight-line Newton-Raphson refinement blocks, so a forward span of site plus one
  neighbour is 8 bytes where 5 are needed.

So the ceiling was full coverage, and only the blanket refusal stood in the way.

`scripts/export_branch_targets.py` runs under IDA and writes, for each function whose
indirect jumps are all accounted for, the complete set of addresses control flow can
enter. `VerifiedBranchTargets` reads it from
`<user>/branch_targets/<title serial>/<module>.txt`. The pass adopts a set only when it
covers every instruction it decoded for that function, and then treats the function as
relocatable. The serial is in the path because every title's main module is `eboot.bin`.

Nothing about this can cost correctness. A missing, partial, stale or malformed file
leaves the pass's own refusal in place; `tests/core/test_branch_targets.cpp` pins that a
file which cannot be trusted whole is not trusted in part.

Result on MBON:

```
Loaded 1646 verified function target sets from .../CUSA15006/eboot.bin.txt
[FPDIAG] coverage eboot.bin: 1243 estimate sites found, 1243 replaced, 0 left unsupported
         (827 functions used verified branch targets of 1646 in file)
```

An undisturbed boot with full coverage reaches the same milestones as a known-good run
from before any of this work: 5502 log lines against 5180, 199 `Render.Vulkan` against
195, and identical `Lib.VideoOut`, `Lib.AudioOut` and `Lib.Vdec2` counts, so the intro
video and audio path both run.



### 11.6 Full coverage on both titles

| Title | Estimate sites | Replaced | Unsupported | Bodies vouched for |
|---|---:|---:|---:|---|
| MBON CUSA15006 | 1243 | **1243** | **0** | 1429 of 1429 used |
| GVS CUSA08379 | 2535 | **2535** | **0** | 3385 of 3385 used |

Getting there took three corrections to the exporter, each found by measuring rather than
reasoning:

1. **Partition by `.eh_frame_hdr`, not by the disassembler's functions.** The pass walks
   bodies delimited by the unwind table; a body spanning two of IDA's functions held code
   no single entry covered and was refused. Parsing the same table the emulator parses
   made every exported entry usable - the `used` and `in file` counts above are equal,
   where before only 1502 of 2086 matched. The parse is corroborated by the body counts:
   46,888 for MBON and 71,985 for GVS, exactly the figures section 2 measured statically.
2. **Identified data inside a body is ordinary.** Jump tables and constant pools sit
   between the code of one body and the next. Refusing a body for containing them cost
   1690 of 3330 candidates. Only bytes the disassembler left *unexplored* are a risk,
   because those could be code holding a jump the export would then deny exists.
3. **Define the bodies auto-analysis never reached.** Analysis follows flow, so a body
   entered only through an unresolved indirect jump stays undefined - 7448 of them in
   MBON, 9421 in GVS. The unwind table says they are functions on the compiler's own
   record, so defining them is not a guess.

### 11.7 Recognising a tail call

After those three, 25 sites remained in five bodies, each holding a `jmp reg` IDA could
not resolve. Reading them settled what they were:

```
0xFD55F3   lea rsp, [rbp-28h] / pop rbx,r12,r13,r14,r15,rbp / jmp rax
0xA4F2E9   add rsp, 188h      / pop rbx,r12,r13,r14,r15,rbp / jmp rax
0xFF8AAA   mov rax, [rdi] / mov rax, [rax+28h]              / jmp rax
```

Every one is a tail call, and none is a jump table. Two signatures say so, and a jump
table can show neither:

- **A stack frame teardown right before the jump.** A switch case runs with the frame
  intact: it is still inside the function and still uses its locals. Popping the
  callee-saved registers and unwinding `rsp` first only makes sense when the jump leaves.
- **The jump register loaded from a single memory slot**, as a vtable dispatch loads the
  object's table and then a slot from it. Reading a jump table needs an index and a
  scale, which is the operand shape the exporter already refused.

`leaves_body` in the exporter encodes both, walking back at most twelve instructions and
stopping at any label or control transfer so that what it reads is the one path that
actually reaches the jump. That took GVS from 25 unsupported sites to none, and raised the
bodies it can vouch for from 1890 to 3385 while dropping refusals from 1628 to 133.

### 11.8 Handing a build to an AMD tester

Everything needed to compare two machines is in the log, so a tester needs no tooling.

Install, per title:

```
<user>/custom_configs/<serial>.json          {"DeterministicFp": {"deterministic_floating_point": true}}
<user>/branch_targets/<serial>/eboot.bin.txt  the exported target set
```

Boot the title once and send back the lines containing `FPDIAG`. Two things are being
read from them:

- `coverage eboot.bin: N found, N replaced, 0 left unsupported`. Anything else means the
  side-car did not match that build of the title, and the run says nothing about
  determinism.
- The three fingerprints, compared against an Intel log:

| Fingerprint | Expected | Meaning if it differs |
|---|---|---|
| `replacement.fingerprint` | identical | the replacement itself is not deterministic - this is the bug |
| `libm.fingerprint` | identical | O7 is live: the peers desync on the first transcendental whatever the instruction patching does |
| `estimate.fingerprint` | differs | confirms the mechanism this work removes; identical values would mean the premise is wrong |

Intel reference, Core i7-12700K, Windows: `estimate.fingerprint=3CEFC9F88621AF41`,
`replacement.fingerprint=59B2788E530449CF`, `libm.fingerprint=F85085DB4ECB276D`.

