// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

#include "common/arch.h"
#ifdef ARCH_X86_64
#include <xmmintrin.h>
#endif

#include "common/logging/log.h"
#include "core/cpu_patches.h"
#include "core/cpu_patches_fp.h"
#include "core/deterministic_fp_report.h"

namespace Core::DeterministicFp {

namespace {

// Every diagnostic line carries this tag so a tester can extract the whole report from a
// long log with a single search, and so two reports can be diffed directly.
constexpr const char* Tag = "FPDIAG";

// Guest floating point environment, mirroring the values thread.cpp installs on every
// guest thread. Recorded because a host that fails to apply them diverges before any
// instruction patching matters.
constexpr u32 GuestMxcsr = 0x9fc0;
constexpr u32 GuestFpuControlWord = 0x037f;

constexpr u64 FnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr u64 FnvPrime = 0x100000001b3ULL;

void HashBits(u64& hash, u64 value) {
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8)) & 0xff;
        hash *= FnvPrime;
    }
}

u32 Bits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float FromBits(u32 bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// Probe inputs. Positive finite values where the estimate error budget is meaningful,
/// plus the specified cases whose handling is a common source of disagreement.
constexpr std::array<u32, 8> ProbeInputs = {
    0x3F800000, // 1.0
    0x40800000, // 4.0
    0x3E800000, // 0.25
    0x40490FDB, // pi
    0x00800000, // smallest normal
    0x7F7FFFFF, // largest finite
    0x00000000, // +0
    0x00000001, // smallest denormal
};

#ifdef ARCH_X86_64

/// Installs the guest's MXCSR for the duration of a probe. Without it the denormal cases
/// are measured under the host's defaults, and the report would describe rounding and
/// flush-to-zero behaviour the title never actually sees.
class GuestFpEnvironment {
public:
    GuestFpEnvironment() : saved{_mm_getcsr()} {
        _mm_setcsr(GuestMxcsr);
    }

    ~GuestFpEnvironment() {
        _mm_setcsr(saved);
    }

    GuestFpEnvironment(const GuestFpEnvironment&) = delete;
    GuestFpEnvironment& operator=(const GuestFpEnvironment&) = delete;

private:
    u32 saved;
};

/// Executes a generated body over one input vector and returns what it left in
/// xmm1 / ymm1. Built with Xbyak rather than intrinsics so the report can probe the exact
/// encodings the titles use, including the 256-bit and VEX forms, without needing the
/// emulator itself to be compiled for AVX.
class Probe {
public:
    Probe(const std::function<void(Xbyak::CodeGenerator&)>& body, bool avx, bool wide)
        : gen{BufferSize} {
        using namespace Xbyak::util;

        // Windows passes (out, in) in rcx/rdx, SysV in rdi/rsi.
#ifdef _WIN32
        const Xbyak::Reg64 out = rcx;
        const Xbyak::Reg64 in = rdx;
#else
        const Xbyak::Reg64 out = rdi;
        const Xbyak::Reg64 in = rsi;
#endif

        if (avx) {
            if (wide) {
                gen.vmovups(Xbyak::Ymm(0), ptr[in]);
            } else {
                gen.vmovups(Xbyak::Xmm(0), ptr[in]);
            }
        } else {
            gen.movups(Xbyak::Xmm(0), ptr[in]);
        }

        body(gen);

        if (avx) {
            if (wide) {
                gen.vmovups(ptr[out], Xbyak::Ymm(1));
            } else {
                gen.vmovups(ptr[out], Xbyak::Xmm(1));
            }
            gen.vzeroupper();
        } else {
            gen.movups(ptr[out], Xbyak::Xmm(1));
        }
        gen.ret();
        gen.ready();
    }

    ~Probe() {
        ForgetCodeGenerator(gen);
    }

    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    /// Runs the body over a single scalar input broadcast to every lane, and returns
    /// lane 0 of the result.
    [[nodiscard]] u32 Run(u32 input_bits) const {
        alignas(32) std::array<u32, 8> in{};
        alignas(32) std::array<u32, 8> out{};
        in.fill(input_bits);
        reinterpret_cast<void (*)(u32*, const u32*)>(const_cast<u8*>(gen.getCode()))(out.data(),
                                                                                     in.data());
        return out[0];
    }

private:
    // Fixed so the constant pool a generator emits stays reachable by the rip-relative
    // displacement it was given, exactly as in the trampoline area.
    static constexpr size_t BufferSize = 8192;

    Xbyak::CodeGenerator gen;
};

/// Assembles one instruction, decodes it, and hands the decoded operands to a generator,
/// so the replacement being reported is the one that actually ships rather than a
/// re-implementation of it.
void EmitReplacementFor(Xbyak::CodeGenerator& into,
                        const std::function<void(Xbyak::CodeGenerator&)>& assemble,
                        void (*generate)(void*, const ZydisDecodedOperand*,
                                         Xbyak::CodeGenerator&)) {
    Xbyak::CodeGenerator source{64};
    assemble(source);

    u8* code = const_cast<u8*>(source.getCode());

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(
            ZydisDecoderDecodeFull(&decoder, code, source.getSize(), &instruction, operands))) {
        LOG_ERROR(Core, "[{}] failed to decode a probe instruction", Tag);
        return;
    }

    // None of the probe forms carries a memory operand, so the generated replacement holds
    // no reference back into this buffer and stays valid once it is gone.
    generate(code, operands, into);
}

struct ProbeForm {
    const char* name;
    bool needs_avx;
    bool wide;
    std::function<void(Xbyak::CodeGenerator&)> assemble;
    void (*generate)(void*, const ZydisDecodedOperand*, Xbyak::CodeGenerator&);
};

std::vector<ProbeForm> BuildProbeForms() {
    using Xbyak::Xmm;
    using Xbyak::Ymm;
    return {
        {"rsqrtss", false, false, [](Xbyak::CodeGenerator& c) { c.rsqrtss(Xmm(1), Xmm(0)); },
         GenerateRSQRTSS},
        {"rcpss", false, false, [](Xbyak::CodeGenerator& c) { c.rcpss(Xmm(1), Xmm(0)); },
         GenerateRCPSS},
        {"rsqrtps", false, false, [](Xbyak::CodeGenerator& c) { c.rsqrtps(Xmm(1), Xmm(0)); },
         GenerateRSQRTPS},
        {"rcpps", false, false, [](Xbyak::CodeGenerator& c) { c.rcpps(Xmm(1), Xmm(0)); },
         GenerateRCPPS},
        {"vrsqrtss", true, false,
         [](Xbyak::CodeGenerator& c) { c.vrsqrtss(Xmm(1), Xmm(0), Xmm(0)); }, GenerateVRSQRTSS},
        {"vrcpss", true, false, [](Xbyak::CodeGenerator& c) { c.vrcpss(Xmm(1), Xmm(0), Xmm(0)); },
         GenerateVRCPSS},
        {"vrsqrtps.xmm", true, false, [](Xbyak::CodeGenerator& c) { c.vrsqrtps(Xmm(1), Xmm(0)); },
         GenerateVRSQRTPS},
        {"vrcpps.xmm", true, false, [](Xbyak::CodeGenerator& c) { c.vrcpps(Xmm(1), Xmm(0)); },
         GenerateVRCPPS},
        {"vrsqrtps.ymm", true, true, [](Xbyak::CodeGenerator& c) { c.vrsqrtps(Ymm(1), Ymm(0)); },
         GenerateVRSQRTPS},
        {"vrcpps.ymm", true, true, [](Xbyak::CodeGenerator& c) { c.vrcpps(Ymm(1), Ymm(0)); },
         GenerateVRCPPS},
    };
}

void LogCpuIdentity() {
    const Xbyak::util::Cpu cpu;

    std::array<u32, 4> registers{};
    Xbyak::util::Cpu::getCpuid(0, registers.data());
    char vendor[13]{};
    std::memcpy(vendor + 0, &registers[1], 4);
    std::memcpy(vendor + 4, &registers[3], 4);
    std::memcpy(vendor + 8, &registers[2], 4);

    char brand[49]{};
    for (u32 leaf = 0; leaf < 3; ++leaf) {
        Xbyak::util::Cpu::getCpuid(0x80000002 + leaf, registers.data());
        std::memcpy(brand + leaf * 16, registers.data(), 16);
    }

    LOG_INFO(Core, "[{}] host.cpu.vendor={} intel={} amd={}", Tag, vendor,
             cpu.has(Xbyak::util::Cpu::tINTEL), cpu.has(Xbyak::util::Cpu::tAMD));
    LOG_INFO(Core, "[{}] host.cpu.brand={}", Tag, brand);
    LOG_INFO(Core, "[{}] host.cpu.family={:#x} model={:#x}", Tag, cpu.displayFamily,
             cpu.displayModel);
    LOG_INFO(Core, "[{}] host.cpu.features sse4a={} avx={} avx2={} fma={} f16c={} avx512f={}", Tag,
             cpu.has(Xbyak::util::Cpu::tSSE4a), cpu.has(Xbyak::util::Cpu::tAVX),
             cpu.has(Xbyak::util::Cpu::tAVX2), cpu.has(Xbyak::util::Cpu::tFMA),
             cpu.has(Xbyak::util::Cpu::tF16C), cpu.has(Xbyak::util::Cpu::tAVX512F));
}

u64 LogInstructionProbes() {
    const Xbyak::util::Cpu cpu;
    const bool has_avx = cpu.has(Xbyak::util::Cpu::tAVX);
    if (!has_avx) {
        LOG_WARNING(Core,
                    "[{}] host lacks AVX; the VEX forms every patched title uses cannot be "
                    "probed and the title itself would not run",
                    Tag);
    }

    u64 estimate_fingerprint = FnvOffsetBasis;
    u64 replacement_fingerprint = FnvOffsetBasis;

    LOG_INFO(Core, "[{}] probes run with mxcsr={:#06x}, the value guest threads carry", Tag,
             GuestMxcsr);
    const GuestFpEnvironment guest_environment;

    for (const auto& form : BuildProbeForms()) {
        if (form.needs_avx && !has_avx) {
            continue;
        }

        const Probe hardware{[&](Xbyak::CodeGenerator& c) { form.assemble(c); }, form.needs_avx,
                             form.wide};
        const Probe replacement{
            [&](Xbyak::CodeGenerator& c) { EmitReplacementFor(c, form.assemble, form.generate); },
            form.needs_avx, form.wide};

        for (const u32 input : ProbeInputs) {
            const u32 hardware_bits = hardware.Run(input);
            const u32 replacement_bits = replacement.Run(input);
            HashBits(estimate_fingerprint, (static_cast<u64>(input) << 32) | hardware_bits);
            HashBits(replacement_fingerprint, (static_cast<u64>(input) << 32) | replacement_bits);

            LOG_INFO(Core, "[{}] probe {} in={:08X} hw={:08X} fixed={:08X} {}", Tag, form.name,
                     input, hardware_bits, replacement_bits,
                     hardware_bits == replacement_bits ? "same" : "differs");
        }
    }

    LOG_INFO(Core, "[{}] estimate.fingerprint={:016X} (expected to differ between Intel and AMD)",
             Tag, estimate_fingerprint);
    LOG_INFO(Core,
             "[{}] replacement.fingerprint={:016X} (MUST match on every host; a mismatch here "
             "is the bug)",
             Tag, replacement_fingerprint);
    return estimate_fingerprint;
}

#else

u64 LogInstructionProbes() {
    LOG_INFO(Core, "[{}] instruction probes are x86-64 only", Tag);
    return FnvOffsetBasis;
}

void LogCpuIdentity() {
    LOG_INFO(Core, "[{}] host.cpu identity is x86-64 only", Tag);
}

#endif

/// Calls one host math function through a volatile argument so the compiler cannot fold
/// the call and report its own answer instead of the runtime library's.
u32 CallHostLibm(float (*function)(float), u32 input_bits) {
    volatile float argument = FromBits(input_bits);
    return Bits(function(argument));
}

u32 CallHostLibm2(float (*function)(float, float), u32 first_bits, u32 second_bits) {
    volatile float first = FromBits(first_bits);
    volatile float second = FromBits(second_bits);
    return Bits(function(first, second));
}

u64 LogHostLibmProbes() {
    // The float entry points libSceLibcInternal forwards to the host runtime. Their results
    // depend on the C library build and, on glibc, on which implementation ifunc selected
    // for this CPU - which is a second, independent way for two peers to disagree.
    struct Unary {
        const char* name;
        float (*function)(float);
    };
    const std::array<Unary, 11> unary = {{
        {"sinf", sinf},
        {"cosf", cosf},
        {"tanf", tanf},
        {"asinf", asinf},
        {"acosf", acosf},
        {"atanf", atanf},
        {"expf", expf},
        {"exp2f", exp2f},
        {"logf", logf},
        {"log10f", log10f},
        // Correctly rounded by the ISA, so it must agree everywhere. Recorded as a
        // control: if this one differs, the disagreement is not about the math library.
        {"sqrtf", sqrtf},
    }};

    // Inputs inside every function's domain, so no probe reports a NaN that says nothing.
    constexpr std::array<u32, 4> inputs = {
        0x3E800000, // 0.25
        0x3F000000, // 0.5
        0x3F4CCCCD, // 0.8
        0x3F733333, // 0.95
    };

    u64 fingerprint = FnvOffsetBasis;
#ifdef ARCH_X86_64
    const GuestFpEnvironment guest_environment;
#endif
    for (const auto& entry : unary) {
        for (const u32 input : inputs) {
            const u32 result = CallHostLibm(entry.function, input);
            HashBits(fingerprint, (static_cast<u64>(input) << 32) | result);
            LOG_INFO(Core, "[{}] libm {} in={:08X} out={:08X}", Tag, entry.name, input, result);
        }
    }

    for (const u32 input : inputs) {
        const u32 pow_result = CallHostLibm2(powf, input, 0x40490FDB); // x ** pi
        const u32 atan2_result = CallHostLibm2(atan2f, input, 0x3F800000);
        HashBits(fingerprint, (static_cast<u64>(input) << 32) | pow_result);
        HashBits(fingerprint, (static_cast<u64>(input) << 32) | atan2_result);
        LOG_INFO(Core, "[{}] libm powf in={:08X} exp=40490FDB out={:08X}", Tag, input, pow_result);
        LOG_INFO(Core, "[{}] libm atan2f y={:08X} x=3F800000 out={:08X}", Tag, input, atan2_result);
    }

    LOG_INFO(Core,
             "[{}] libm.fingerprint={:016X} (peers whose values differ desync on the first "
             "transcendental, with or without instruction patching)",
             Tag, fingerprint);
    return fingerprint;
}

u64 cached_libm_fingerprint{};
u64 cached_estimate_fingerprint{};

} // namespace

u64 GetHostLibmFingerprint() {
    return cached_libm_fingerprint;
}

u64 GetHostEstimateFingerprint() {
    return cached_estimate_fingerprint;
}

void LogHostEnvironmentReport() {
    LOG_INFO(Core, "[{}] ===== floating point determinism report begin =====", Tag);
    LOG_INFO(Core, "[{}] setting.deterministic_fp={}", Tag, IsEnabled() ? "enabled" : "disabled");
    LOG_INFO(Core, "[{}] guest.mxcsr={:#06x} guest.fpucw={:#06x}", Tag, GuestMxcsr,
             GuestFpuControlWord);

    LogCpuIdentity();
    cached_estimate_fingerprint = LogInstructionProbes();
    cached_libm_fingerprint = LogHostLibmProbes();

    LOG_INFO(Core,
             "[{}] compare two logs from different machines: replacement.fingerprint must be "
             "identical, libm.fingerprint must be identical, estimate.fingerprint may differ",
             Tag);
    LOG_INFO(Core, "[{}] ===== floating point determinism report end =====", Tag);
}

} // namespace Core::DeterministicFp
