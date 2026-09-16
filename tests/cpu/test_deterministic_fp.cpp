// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Differential tests for the deterministic floating point replacements.
//
// Each case assembles the real approximation instruction, decodes it, asks the
// generator for a replacement, then executes both over identical register state and
// compares. Three properties are checked, and they are deliberately different in kind:
//
//   1. Lane merge and zeroing semantics must match the hardware instruction exactly.
//      Only lane 0's *value* is approximate; which lanes are written, preserved or
//      zeroed is fully specified, so it can be compared bit for bit.
//   2. Lane 0 must sit inside the error budget the ISA allows the estimate
//      (relative error below 1.5 * 2^-12), which proves the replacement computes the
//      right quantity without depending on either side's rounding.
//   3. Lane 0 must equal a software model of the replacement's documented semantics
//      bit for bit, which is what pins determinism: the model uses only correctly
//      rounded operations, so every host must land on the same bits.
//
// Registers the replacement is allowed to borrow as scratch are also checked for
// restoration, and the emitted code is disassembled to confirm no estimate survives.

#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include <xmmintrin.h>

#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

#include "common/types.h"
#include "core/cpu_patches_fp.h"

namespace {

namespace Fp = Core::DeterministicFp;

constexpr int TrackedRegisters = 4; // ymm0 - ymm3
constexpr int LanesPerRegister = 8;
constexpr int MemorySourceOffset = TrackedRegisters * LanesPerRegister * sizeof(u32);

/// The estimate instructions are allowed a relative error below 1.5 * 2^-12.
constexpr double EstimateErrorBudget = 1.5 / 4096.0;

/// MXCSR every guest thread carries, from thread.cpp: FTZ, DAZ, round-to-nearest, and all
/// exceptions masked.
constexpr u32 GuestMxcsr = 0x9fc0;

struct alignas(32) VectorState {
    u32 lane[TrackedRegisters][LanesPerRegister];
    u32 memory_source[LanesPerRegister];
};

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

/// The replacement's documented semantics for the VEX reciprocal-square-root forms.
/// volatile keeps each step a separately rounded float operation, matching the emitted
/// vdivss / vsqrtss pair rather than being folded at a wider precision.
float ModelSqrtOfReciprocal(float x) {
    volatile float reciprocal = 1.0f / x;
    return std::sqrt(static_cast<float>(reciprocal));
}

/// The replacement's documented semantics for the reciprocal forms.
float ModelReciprocal(float x) {
    volatile float reciprocal = 1.0f / x;
    return static_cast<float>(reciprocal);
}

/// The replacement's documented semantics for the legacy rsqrtss form, which keeps the
/// hardware's operation order.
float ModelReciprocalOfSqrt(float x) {
    volatile float root = std::sqrt(x);
    return 1.0f / static_cast<float>(root);
}

struct DecodedInstruction {
    std::vector<u8> bytes;
    ZydisDecodedInstruction info;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
};

DecodedInstruction Assemble(const std::function<void(Xbyak::CodeGenerator&)>& emit) {
    Xbyak::CodeGenerator gen(256);
    emit(gen);
    gen.ready();

    DecodedInstruction decoded{};
    decoded.bytes.assign(gen.getCode(), gen.getCode() + gen.getSize());

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    const auto status = ZydisDecoderDecodeFull(&decoder, decoded.bytes.data(),
                                               decoded.bytes.size(), &decoded.info,
                                               decoded.operands);
    EXPECT_TRUE(ZYAN_SUCCESS(status)) << "failed to decode the assembled instruction";
    return decoded;
}

/// Loads ymm0-ymm3 from the state, runs a body, and writes them back, so a replacement
/// that fails to restore its scratch register is visible in the result.
class Stub {
public:
    explicit Stub(const std::function<void(Xbyak::CodeGenerator&)>& body) : gen{BufferSize} {
        using namespace Xbyak::util;

        for (int i = 0; i < TrackedRegisters; ++i) {
            gen.vmovups(Xbyak::Ymm(i), ptr[ArgumentRegister() + i * 32]);
        }
        body(gen);
        for (int i = 0; i < TrackedRegisters; ++i) {
            gen.vmovups(ptr[ArgumentRegister() + i * 32], Xbyak::Ymm(i));
        }
        gen.vzeroupper();
        gen.ret();
        gen.ready();
    }

    ~Stub() {
        Fp::ForgetCodeGenerator(gen);
    }

    Stub(const Stub&) = delete;
    Stub& operator=(const Stub&) = delete;

    void Run(VectorState& state) {
        reinterpret_cast<void (*)(VectorState*)>(const_cast<u8*>(gen.getCode()))(&state);
    }

    /// Disassembles the emitted body and reports whether any estimate instruction
    /// survived. The constant pool sits inline but contains no 0x0F byte, so a linear
    /// sweep over it cannot manufacture one.
    [[nodiscard]] bool ContainsApproximation() const {
        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        const u8* code = gen.getCode();
        size_t offset = 0;
        while (offset < gen.getSize()) {
            ZydisDecodedInstruction info;
            // ZydisDecoderDecodeFull fills ZYDIS_MAX_OPERAND_COUNT entries, not just the
            // visible ones. Sizing this array by the visible count overruns the stack.
            ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
            const auto status = ZydisDecoderDecodeFull(&decoder, code + offset,
                                                       gen.getSize() - offset, &info, operands);
            if (!ZYAN_SUCCESS(status)) {
                offset += 1;
                continue;
            }
            if (Fp::IsApproximationMnemonic(info.mnemonic)) {
                return true;
            }
            offset += info.length;
        }
        return false;
    }

    static Xbyak::Reg64 ArgumentRegister() {
#ifdef _WIN32
        return Xbyak::util::rcx;
#else
        return Xbyak::util::rdi;
#endif
    }

private:
    // Xbyak must not grow its buffer: the constant pool is reached by an absolute
    // rip-relative displacement fixed at emit time, which a reallocation would
    // invalidate. The emulator's trampoline generator is likewise built over a fixed
    // pre-mapped area, so this matches production.
    static constexpr size_t BufferSize = 8192;

    Xbyak::CodeGenerator gen;
};

/// Keeps an assembled instruction at a live code address. A RIP-relative operand needs
/// this: its displacement only means anything relative to where it was assembled, so a
/// copy of the bytes elsewhere would point at unrelated memory.
class LiveInstruction {
public:
    explicit LiveInstruction(const std::function<void(Xbyak::CodeGenerator&)>& emit) : gen{256} {
        emit(gen);
        gen.ready();

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        const auto status =
            ZydisDecoderDecodeFull(&decoder, gen.getCode(), gen.getSize(), &info, operands);
        EXPECT_TRUE(ZYAN_SUCCESS(status)) << "failed to decode the assembled instruction";
    }

    LiveInstruction(const LiveInstruction&) = delete;
    LiveInstruction& operator=(const LiveInstruction&) = delete;

    [[nodiscard]] void* Address() const {
        return const_cast<u8*>(gen.getCode());
    }

    [[nodiscard]] const ZydisDecodedOperand* Operands() const {
        return operands;
    }

    [[nodiscard]] ZydisMnemonic Mnemonic() const {
        return info.mnemonic;
    }

private:
    Xbyak::CodeGenerator gen;
    ZydisDecodedInstruction info{};
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]{};
};

/// Installs the MXCSR guest threads carry (FTZ, DAZ, round-to-nearest, all exceptions
/// masked). Replacements execute under it in the emulator, so any case where it changes
/// the answer has to be exercised under it here too.
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

bool HostSupportsAvx() {
    static const Xbyak::util::Cpu cpu;
    return cpu.has(Xbyak::util::Cpu::tAVX);
}

/// Fills every tracked register with a distinct pattern so an unintended write shows up.
VectorState MakeState() {
    VectorState state{};
    for (int reg = 0; reg < TrackedRegisters; ++reg) {
        for (int lane = 0; lane < LanesPerRegister; ++lane) {
            state.lane[reg][lane] = 0xA0000000u | (reg << 8) | lane;
        }
    }
    for (int lane = 0; lane < LanesPerRegister; ++lane) {
        state.memory_source[lane] = 0xB0000000u | lane;
    }
    return state;
}

void ExpectWithinEstimateBudget(u32 hardware_bits, u32 replacement_bits, float input) {
    const float hardware = FromBits(hardware_bits);
    const float replacement = FromBits(replacement_bits);

    if (std::isnan(hardware) || std::isnan(replacement)) {
        EXPECT_TRUE(std::isnan(hardware) && std::isnan(replacement))
            << "NaN disagreement for input " << input;
        return;
    }
    if (std::isinf(hardware) || std::isinf(replacement) || hardware == 0.0f) {
        EXPECT_EQ(std::isinf(hardware), std::isinf(replacement))
            << "infinity disagreement for input " << input;
        return;
    }

    const double relative_error =
        std::abs(static_cast<double>(replacement) - hardware) / std::abs(hardware);
    EXPECT_LE(relative_error, EstimateErrorBudget)
        << "input " << input << " hardware " << hardware << " replacement " << replacement;
}

/// Finite positive inputs, where the estimate error budget is meaningful.
const std::vector<float> kFinitePositiveInputs = {
    1.0f,          4.0f,        0.25f,        2.0f,          1e-20f,       1e20f,
    3.14159265f,   1.17549435e-38f,           3.40282347e38f, 16777216.0f, 0.5f,
};

/// Values whose behaviour is specified rather than approximate.
const std::vector<u32> kSpecialInputBits = {
    0x00000000u, // +0
    0x80000000u, // -0
    0x7F800000u, // +inf
    0xFF800000u, // -inf
    0x7FC00000u, // quiet NaN
    0x7F800001u, // signalling NaN
    0xBF800000u, // -1.0
    0x00000001u, // smallest denormal
};

} // namespace

TEST(DeterministicFpMnemonics, RecognisesEveryEstimateForm) {
    EXPECT_TRUE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_RCPPS));
    EXPECT_TRUE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_RCPSS));
    EXPECT_TRUE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_RSQRTPS));
    EXPECT_TRUE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_RSQRTSS));
    EXPECT_TRUE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_VRCPPS));
    EXPECT_TRUE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_VRCPSS));
    EXPECT_TRUE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_VRSQRTPS));
    EXPECT_TRUE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_VRSQRTSS));
}

TEST(DeterministicFpMnemonics, LeavesExactOperationsAlone) {
    EXPECT_FALSE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_VSQRTPS));
    EXPECT_FALSE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_VDIVPS));
    EXPECT_FALSE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_SQRTSS));
    EXPECT_FALSE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_DIVSS));
    EXPECT_FALSE(Fp::IsApproximationMnemonic(ZYDIS_MNEMONIC_MULPS));
}

class DeterministicFpTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!HostSupportsAvx()) {
            GTEST_SKIP() << "host lacks AVX, which the titles being patched require anyway";
        }
    }
};

// vrsqrtss xmm1, xmm0, xmm0 is the shape that dominates Maxi Boost ON: 1152 of its
// 1258 estimate sites, and the destination aliases neither source.
TEST_F(DeterministicFpTest, VrsqrtssScalarMatchesLaneSemanticsAndStaysInBudget) {
    const auto original = Assemble([](Xbyak::CodeGenerator& c) {
        c.vrsqrtss(Xbyak::Xmm(1), Xbyak::Xmm(0), Xbyak::Xmm(0));
    });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_VRSQRTSS);

    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRSQRTSS(nullptr, original.operands, c);
    }};

    EXPECT_FALSE(replacement.ContainsApproximation());

    for (const float input : kFinitePositiveInputs) {
        VectorState hardware = MakeState();
        hardware.lane[0][0] = Bits(input);
        VectorState patched = hardware;

        reference.Run(hardware);
        replacement.Run(patched);

        // Lanes 1-3 come from src1, lanes 4-7 are zeroed by the VEX.128 form.
        for (int lane = 1; lane < 4; ++lane) {
            EXPECT_EQ(hardware.lane[1][lane], patched.lane[1][lane])
                << "merged lane " << lane << " for input " << input;
        }
        for (int lane = 4; lane < LanesPerRegister; ++lane) {
            EXPECT_EQ(0u, patched.lane[1][lane]) << "upper lane " << lane << " must be zeroed";
            EXPECT_EQ(hardware.lane[1][lane], patched.lane[1][lane]);
        }

        ExpectWithinEstimateBudget(hardware.lane[1][0], patched.lane[1][0], input);
        EXPECT_EQ(Bits(ModelSqrtOfReciprocal(input)), patched.lane[1][0])
            << "replacement must equal sqrt(1/x) bit for bit, input " << input;

        // The source and the untouched registers must come back unchanged.
        for (int reg : {0, 2, 3}) {
            for (int lane = 0; lane < LanesPerRegister; ++lane) {
                EXPECT_EQ(hardware.lane[reg][lane], patched.lane[reg][lane])
                    << "register ymm" << reg << " lane " << lane << " was clobbered";
            }
        }
    }
}

// +0, -0, infinities and NaNs. -0 is the one documented departure from hardware: the
// reordered form returns NaN where the estimate returns -inf.
TEST_F(DeterministicFpTest, VrsqrtssSpecialValuesFollowTheDocumentedModel) {
    const auto original = Assemble([](Xbyak::CodeGenerator& c) {
        c.vrsqrtss(Xbyak::Xmm(1), Xbyak::Xmm(0), Xbyak::Xmm(0));
    });
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRSQRTSS(nullptr, original.operands, c);
    }};

    for (const u32 input_bits : kSpecialInputBits) {
        VectorState state = MakeState();
        state.lane[0][0] = input_bits;
        replacement.Run(state);

        EXPECT_EQ(Bits(ModelSqrtOfReciprocal(FromBits(input_bits))), state.lane[1][0])
            << "special input " << std::hex << input_bits;
    }

    // Pin the -0 deviation explicitly so a future change cannot alter it silently.
    VectorState negative_zero = MakeState();
    negative_zero.lane[0][0] = 0x80000000u;
    replacement.Run(negative_zero);
    EXPECT_TRUE(std::isnan(FromBits(negative_zero.lane[1][0])))
        << "sqrt(1/-0) is NaN by construction; hardware rsqrtss would give -inf";

    VectorState positive_zero = MakeState();
    positive_zero.lane[0][0] = 0x00000000u;
    replacement.Run(positive_zero);
    EXPECT_EQ(0x7F800000u, positive_zero.lane[1][0]) << "+0 must still give +inf";
}

TEST_F(DeterministicFpTest, VrsqrtpsNarrowMatchesEveryLane) {
    const auto original = Assemble(
        [](Xbyak::CodeGenerator& c) { c.vrsqrtps(Xbyak::Xmm(1), Xbyak::Xmm(0)); });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_VRSQRTPS);

    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRSQRTPS(nullptr, original.operands, c);
    }};

    EXPECT_FALSE(replacement.ContainsApproximation());

    VectorState hardware = MakeState();
    for (int lane = 0; lane < 4; ++lane) {
        hardware.lane[0][lane] = Bits(kFinitePositiveInputs[lane]);
    }
    VectorState patched = hardware;

    reference.Run(hardware);
    replacement.Run(patched);

    for (int lane = 0; lane < 4; ++lane) {
        ExpectWithinEstimateBudget(hardware.lane[1][lane], patched.lane[1][lane],
                                   kFinitePositiveInputs[lane]);
        EXPECT_EQ(Bits(ModelSqrtOfReciprocal(kFinitePositiveInputs[lane])), patched.lane[1][lane]);
    }
    for (int lane = 4; lane < LanesPerRegister; ++lane) {
        EXPECT_EQ(0u, patched.lane[1][lane]) << "VEX.128 must zero the upper half";
    }
}

TEST_F(DeterministicFpTest, VrsqrtpsWideCoversAllEightLanes) {
    const auto original = Assemble(
        [](Xbyak::CodeGenerator& c) { c.vrsqrtps(Xbyak::Ymm(1), Xbyak::Ymm(0)); });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_VRSQRTPS);

    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRSQRTPS(nullptr, original.operands, c);
    }};

    VectorState hardware = MakeState();
    for (int lane = 0; lane < LanesPerRegister; ++lane) {
        hardware.lane[0][lane] = Bits(kFinitePositiveInputs[lane]);
    }
    VectorState patched = hardware;

    reference.Run(hardware);
    replacement.Run(patched);

    // The whole point of the wide case: a 128-bit replacement would zero lanes 4-7.
    for (int lane = 0; lane < LanesPerRegister; ++lane) {
        ExpectWithinEstimateBudget(hardware.lane[1][lane], patched.lane[1][lane],
                                   kFinitePositiveInputs[lane]);
        EXPECT_EQ(Bits(ModelSqrtOfReciprocal(kFinitePositiveInputs[lane])), patched.lane[1][lane])
            << "wide lane " << lane;
    }
}

TEST_F(DeterministicFpTest, VrcppsIsExactReciprocal) {
    const auto original =
        Assemble([](Xbyak::CodeGenerator& c) { c.vrcpps(Xbyak::Ymm(2), Xbyak::Ymm(0)); });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_VRCPPS);

    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRCPPS(nullptr, original.operands, c);
    }};

    EXPECT_FALSE(replacement.ContainsApproximation());

    VectorState hardware = MakeState();
    for (int lane = 0; lane < LanesPerRegister; ++lane) {
        hardware.lane[0][lane] = Bits(kFinitePositiveInputs[lane]);
    }
    VectorState patched = hardware;

    reference.Run(hardware);
    replacement.Run(patched);

    for (int lane = 0; lane < LanesPerRegister; ++lane) {
        ExpectWithinEstimateBudget(hardware.lane[2][lane], patched.lane[2][lane],
                                   kFinitePositiveInputs[lane]);
        EXPECT_EQ(Bits(ModelReciprocal(kFinitePositiveInputs[lane])), patched.lane[2][lane]);
    }
}

// The destination aliasing its source forces the generator down the scratch path, which
// has to spill below the guest red zone and restore what it borrowed.
TEST_F(DeterministicFpTest, VrsqrtpsWithAliasedOperandsRestoresScratch) {
    const auto original = Assemble(
        [](Xbyak::CodeGenerator& c) { c.vrsqrtps(Xbyak::Ymm(2), Xbyak::Ymm(2)); });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_VRSQRTPS);

    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRSQRTPS(nullptr, original.operands, c);
    }};

    VectorState hardware = MakeState();
    for (int lane = 0; lane < LanesPerRegister; ++lane) {
        hardware.lane[2][lane] = Bits(kFinitePositiveInputs[lane]);
    }
    VectorState patched = hardware;

    reference.Run(hardware);
    replacement.Run(patched);

    for (int lane = 0; lane < LanesPerRegister; ++lane) {
        ExpectWithinEstimateBudget(hardware.lane[2][lane], patched.lane[2][lane],
                                   kFinitePositiveInputs[lane]);
        EXPECT_EQ(Bits(ModelSqrtOfReciprocal(kFinitePositiveInputs[lane])), patched.lane[2][lane]);
    }
    for (int reg : {0, 1, 3}) {
        for (int lane = 0; lane < LanesPerRegister; ++lane) {
            EXPECT_EQ(hardware.lane[reg][lane], patched.lane[reg][lane])
                << "scratch register ymm" << reg << " was not restored";
        }
    }
}

// A memory source exercises the operand rebuild, including the base register the
// original encoding used.
TEST_F(DeterministicFpTest, VrsqrtpsWithMemorySourceRebuildsTheOperand) {
    const auto original = Assemble([](Xbyak::CodeGenerator& c) {
        c.vrsqrtps(Xbyak::Xmm(1), Xbyak::util::ptr[Stub::ArgumentRegister() + MemorySourceOffset]);
    });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_VRSQRTPS);
    ASSERT_EQ(original.operands[1].type, ZYDIS_OPERAND_TYPE_MEMORY);

    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRSQRTPS(nullptr, original.operands, c);
    }};

    VectorState hardware = MakeState();
    for (int lane = 0; lane < 4; ++lane) {
        hardware.memory_source[lane] = Bits(kFinitePositiveInputs[lane]);
    }
    VectorState patched = hardware;

    reference.Run(hardware);
    replacement.Run(patched);

    for (int lane = 0; lane < 4; ++lane) {
        ExpectWithinEstimateBudget(hardware.lane[1][lane], patched.lane[1][lane],
                                   kFinitePositiveInputs[lane]);
        EXPECT_EQ(Bits(ModelSqrtOfReciprocal(kFinitePositiveInputs[lane])), patched.lane[1][lane]);
    }
}

// Legacy scalar forms merge into the destination instead of overwriting it, and they
// leave the register's upper half alone. Both are easy to break and silent when broken.
TEST_F(DeterministicFpTest, LegacyRsqrtssPreservesTheDestinationUpperLanes) {
    const auto original =
        Assemble([](Xbyak::CodeGenerator& c) { c.rsqrtss(Xbyak::Xmm(1), Xbyak::Xmm(0)); });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_RSQRTSS);

    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateRSQRTSS(nullptr, original.operands, c);
    }};

    EXPECT_FALSE(replacement.ContainsApproximation());

    for (const float input : kFinitePositiveInputs) {
        VectorState hardware = MakeState();
        hardware.lane[0][0] = Bits(input);
        const VectorState before = hardware;
        VectorState patched = hardware;

        reference.Run(hardware);
        replacement.Run(patched);

        // Lanes 1-7 of the destination must survive untouched, unlike the VEX form.
        for (int lane = 1; lane < LanesPerRegister; ++lane) {
            EXPECT_EQ(before.lane[1][lane], patched.lane[1][lane])
                << "legacy form must not disturb destination lane " << lane;
            EXPECT_EQ(hardware.lane[1][lane], patched.lane[1][lane]);
        }

        ExpectWithinEstimateBudget(hardware.lane[1][0], patched.lane[1][0], input);
        EXPECT_EQ(Bits(ModelReciprocalOfSqrt(input)), patched.lane[1][0]);

        for (int reg : {0, 2, 3}) {
            for (int lane = 0; lane < LanesPerRegister; ++lane) {
                EXPECT_EQ(before.lane[reg][lane], patched.lane[reg][lane])
                    << "register ymm" << reg << " was clobbered";
            }
        }
    }
}

// The legacy ordering keeps the hardware's behaviour for -0, where the VEX reorder
// deliberately does not. Recording both makes the difference intentional.
TEST_F(DeterministicFpTest, LegacyRsqrtssKeepsHardwareSignedZeroBehaviour) {
    const auto original =
        Assemble([](Xbyak::CodeGenerator& c) { c.rsqrtss(Xbyak::Xmm(1), Xbyak::Xmm(0)); });
    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateRSQRTSS(nullptr, original.operands, c);
    }};

    VectorState hardware = MakeState();
    hardware.lane[0][0] = 0x80000000u; // -0
    VectorState patched = hardware;

    reference.Run(hardware);
    replacement.Run(patched);

    EXPECT_EQ(0xFF800000u, patched.lane[1][0]) << "1/sqrt(-0) must be -inf, matching hardware";
    EXPECT_EQ(hardware.lane[1][0], patched.lane[1][0]);
}

TEST_F(DeterministicFpTest, LegacyRcppsIsExactReciprocal) {
    const auto original =
        Assemble([](Xbyak::CodeGenerator& c) { c.rcpps(Xbyak::Xmm(1), Xbyak::Xmm(0)); });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_RCPPS);

    Stub reference{[&](Xbyak::CodeGenerator& c) {
        c.db(original.bytes.data(), original.bytes.size());
    }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateRCPPS(nullptr, original.operands, c);
    }};

    EXPECT_FALSE(replacement.ContainsApproximation());

    VectorState hardware = MakeState();
    for (int lane = 0; lane < 4; ++lane) {
        hardware.lane[0][lane] = Bits(kFinitePositiveInputs[lane]);
    }
    const VectorState before = hardware;
    VectorState patched = hardware;

    reference.Run(hardware);
    replacement.Run(patched);

    for (int lane = 0; lane < 4; ++lane) {
        ExpectWithinEstimateBudget(hardware.lane[1][lane], patched.lane[1][lane],
                                   kFinitePositiveInputs[lane]);
        EXPECT_EQ(Bits(ModelReciprocal(kFinitePositiveInputs[lane])), patched.lane[1][lane]);
    }
    // Legacy SSE writes leave the upper half of the register alone.
    for (int lane = 4; lane < LanesPerRegister; ++lane) {
        EXPECT_EQ(before.lane[1][lane], patched.lane[1][lane]);
    }
}

// vrcpss is the one VEX scalar form whose merge does not fall out of the arithmetic:
// vdivss keeps lanes 1-3 from its own first source, so they have to be blended back.
TEST_F(DeterministicFpTest, VrcpssMergesUpperLanesFromTheFirstSource) {
    const auto original = Assemble(
        [](Xbyak::CodeGenerator& c) { c.vrcpss(Xbyak::Xmm(1), Xbyak::Xmm(2), Xbyak::Xmm(0)); });
    ASSERT_EQ(original.info.mnemonic, ZYDIS_MNEMONIC_VRCPSS);

    Stub reference{
        [&](Xbyak::CodeGenerator& c) { c.db(original.bytes.data(), original.bytes.size()); }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRCPSS(const_cast<u8*>(original.bytes.data()), original.operands, c);
    }};

    EXPECT_FALSE(replacement.ContainsApproximation());

    for (const float input : kFinitePositiveInputs) {
        VectorState hardware = MakeState();
        hardware.lane[0][0] = Bits(input);
        VectorState patched = hardware;

        reference.Run(hardware);
        replacement.Run(patched);

        for (int lane = 1; lane < 4; ++lane) {
            EXPECT_EQ(hardware.lane[1][lane], patched.lane[1][lane])
                << "lane " << lane << " must come from src1 for input " << input;
        }
        for (int lane = 4; lane < LanesPerRegister; ++lane) {
            EXPECT_EQ(0u, patched.lane[1][lane]) << "VEX.128 must zero the upper half";
        }

        ExpectWithinEstimateBudget(hardware.lane[1][0], patched.lane[1][0], input);
        EXPECT_EQ(Bits(ModelReciprocal(input)), patched.lane[1][0]);

        for (int reg : {0, 2, 3}) {
            for (int lane = 0; lane < LanesPerRegister; ++lane) {
                EXPECT_EQ(hardware.lane[reg][lane], patched.lane[reg][lane])
                    << "register ymm" << reg << " lane " << lane << " was clobbered";
            }
        }
    }
}

// A destination that aliases src1 cannot hold the staged constant, so the generator takes
// its scratch path. That path must agree with the direct one bit for bit, or two sites
// meaning the same thing would compute different results within one title.
TEST_F(DeterministicFpTest, VrsqrtssWithAliasedFirstSourceMatchesTheDirectPath) {
    const auto aliased = Assemble(
        [](Xbyak::CodeGenerator& c) { c.vrsqrtss(Xbyak::Xmm(1), Xbyak::Xmm(1), Xbyak::Xmm(0)); });
    ASSERT_EQ(aliased.info.mnemonic, ZYDIS_MNEMONIC_VRSQRTSS);

    Stub reference{
        [&](Xbyak::CodeGenerator& c) { c.db(aliased.bytes.data(), aliased.bytes.size()); }};
    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRSQRTSS(const_cast<u8*>(aliased.bytes.data()), aliased.operands, c);
    }};

    EXPECT_FALSE(replacement.ContainsApproximation());

    for (const float input : kFinitePositiveInputs) {
        VectorState hardware = MakeState();
        hardware.lane[0][0] = Bits(input);
        VectorState patched = hardware;

        reference.Run(hardware);
        replacement.Run(patched);

        // src1 is the destination itself, so lanes 1-3 are its own previous contents.
        for (int lane = 1; lane < 4; ++lane) {
            EXPECT_EQ(hardware.lane[1][lane], patched.lane[1][lane]) << "merged lane " << lane;
        }
        EXPECT_EQ(Bits(ModelSqrtOfReciprocal(input)), patched.lane[1][0])
            << "scratch path must match the direct path for input " << input;

        for (int reg : {0, 2, 3}) {
            for (int lane = 0; lane < LanesPerRegister; ++lane) {
                EXPECT_EQ(hardware.lane[reg][lane], patched.lane[reg][lane])
                    << "scratch register ymm" << reg << " was not restored";
            }
        }
    }
}

// The nine eight-byte sites in Gundam Versus are estimates reading a RIP-relative
// constant. The replacement runs from the trampoline, where the original displacement
// points somewhere else entirely, so the target has to be resolved against the site.
TEST_F(DeterministicFpTest, RipRelativeSourceResolvesAgainstTheOriginalSite) {
    // The constant lives in the same buffer as the instruction, exactly as it does in a
    // real module, so the displacement stays inside the 32 bits the encoding allows.
    const LiveInstruction original{[](Xbyak::CodeGenerator& c) {
        Xbyak::Label source;
        c.vrsqrtps(Xbyak::Xmm(1), Xbyak::util::ptr[Xbyak::util::rip + source]);
        c.L(source);
        for (int lane = 0; lane < 4; ++lane) {
            c.dd(Bits(kFinitePositiveInputs[lane]));
        }
    }};
    ASSERT_EQ(original.Mnemonic(), ZYDIS_MNEMONIC_VRSQRTPS);
    ASSERT_EQ(original.Operands()[1].mem.base, ZYDIS_REGISTER_RIP);

    Stub replacement{[&](Xbyak::CodeGenerator& c) {
        Fp::GenerateVRSQRTPS(original.Address(), original.Operands(), c);
    }};

    VectorState state = MakeState();
    replacement.Run(state);

    for (int lane = 0; lane < 4; ++lane) {
        EXPECT_EQ(Bits(ModelSqrtOfReciprocal(kFinitePositiveInputs[lane])), state.lane[1][lane])
            << "lane " << lane << " did not read the RIP-relative target";
    }
}

// Resolving a RIP-relative source is impossible without knowing where the instruction was.
// Refusing loudly is the only safe answer: emitting the displacement unchanged would read
// an unrelated address and never fault.
TEST_F(DeterministicFpTest, RipRelativeSourceWithoutASiteAddressIsRefused) {
    const LiveInstruction original{[](Xbyak::CodeGenerator& c) {
        Xbyak::Label source;
        c.vrsqrtps(Xbyak::Xmm(1), Xbyak::util::ptr[Xbyak::util::rip + source]);
        c.L(source);
        c.dd(Bits(1.0f));
    }};

    Xbyak::CodeGenerator gen{256};
    EXPECT_THROW(Fp::GenerateVRSQRTPS(nullptr, original.Operands(), gen), Xbyak::Error);
    Fp::ForgetCodeGenerator(gen);
}

// Guest threads run with FTZ and DAZ set, which the reordered form is visible through:
// sqrt(1/x) flushes the intermediate reciprocal where hardware's 1/sqrt(x) never forms
// one. Both answers are the same on every host, so lockstep is unaffected, but the
// deviation from real hardware is real and must not change without being noticed.
TEST_F(DeterministicFpTest, ReorderedFormUnderflowsToZeroWhereHardwareDoesNot) {
    const auto original =
        Assemble([](Xbyak::CodeGenerator& c) { c.vrsqrtps(Xbyak::Xmm(1), Xbyak::Xmm(0)); });

    Stub reference{
        [&](Xbyak::CodeGenerator& c) { c.db(original.bytes.data(), original.bytes.size()); }};
    Stub replacement{
        [&](Xbyak::CodeGenerator& c) { Fp::GenerateVRSQRTPS(nullptr, original.operands, c); }};

    // 3.4e38, whose reciprocal is denormal and therefore flushed.
    constexpr u32 kLargestFinite = 0x7F7FFFFFu;

    VectorState hardware = MakeState();
    for (int lane = 0; lane < 4; ++lane) {
        hardware.lane[0][lane] = kLargestFinite;
    }
    VectorState patched = hardware;

    const GuestFpEnvironment guest_environment;
    reference.Run(hardware);
    replacement.Run(patched);

    for (int lane = 0; lane < 4; ++lane) {
        EXPECT_EQ(0u, patched.lane[1][lane])
            << "the reciprocal underflows and FTZ flushes it before the square root";
        EXPECT_NE(0u, hardware.lane[1][lane])
            << "hardware never forms the intermediate, so it still returns a value";
    }
}

TEST(DeterministicFpOperandShapes, AcceptsTheFormsTheTitlesActuallyUse) {
    const auto scalar = Assemble(
        [](Xbyak::CodeGenerator& c) { c.vrsqrtss(Xbyak::Xmm(1), Xbyak::Xmm(0), Xbyak::Xmm(0)); });
    EXPECT_TRUE(Fp::IsSupportedOperandShape(scalar.info.mnemonic, scalar.operands));

    const auto wide =
        Assemble([](Xbyak::CodeGenerator& c) { c.vrsqrtps(Xbyak::Ymm(4), Xbyak::Ymm(2)); });
    EXPECT_TRUE(Fp::IsSupportedOperandShape(wide.info.mnemonic, wide.operands));

    const auto indexed = Assemble([](Xbyak::CodeGenerator& c) {
        c.vrcpps(Xbyak::Xmm(1), Xbyak::util::ptr[Xbyak::util::rax + Xbyak::util::rdx * 4 + 16]);
    });
    EXPECT_TRUE(Fp::IsSupportedOperandShape(indexed.info.mnemonic, indexed.operands));

    // A stack source decodes with an SS segment rather than DS, and is perfectly ordinary.
    const auto stacked = Assemble([](Xbyak::CodeGenerator& c) {
        c.vrsqrtps(Xbyak::Xmm(1), Xbyak::util::ptr[Xbyak::util::rsp + 32]);
    });
    EXPECT_TRUE(Fp::IsSupportedOperandShape(stacked.info.mnemonic, stacked.operands));

    const auto legacy =
        Assemble([](Xbyak::CodeGenerator& c) { c.rsqrtss(Xbyak::Xmm(1), Xbyak::Xmm(0)); });
    EXPECT_TRUE(Fp::IsSupportedOperandShape(legacy.info.mnemonic, legacy.operands));
}

TEST(DeterministicFpOperandShapes, RejectsShapesNoReplacementCanExpress) {
    const auto segmented = Assemble([](Xbyak::CodeGenerator& c) {
        c.putSeg(Xbyak::util::gs);
        c.vrsqrtps(Xbyak::Xmm(1), Xbyak::util::ptr[Xbyak::util::rax]);
    });
    ASSERT_EQ(segmented.operands[1].mem.segment, ZYDIS_REGISTER_GS);
    EXPECT_FALSE(Fp::IsSupportedOperandShape(segmented.info.mnemonic, segmented.operands))
        << "a segment override would be dropped by the operand rebuild";

    const auto narrow_addressing = Assemble([](Xbyak::CodeGenerator& c) {
        c.vrsqrtps(Xbyak::Xmm(1), Xbyak::util::ptr[Xbyak::util::eax]);
    });
    ASSERT_EQ(narrow_addressing.operands[1].mem.base, ZYDIS_REGISTER_EAX);
    EXPECT_FALSE(
        Fp::IsSupportedOperandShape(narrow_addressing.info.mnemonic, narrow_addressing.operands))
        << "32-bit addressing truncates, and rebuilding it as 64-bit would not";
}
