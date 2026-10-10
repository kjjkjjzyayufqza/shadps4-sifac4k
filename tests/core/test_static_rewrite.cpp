// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The static rewrite pass decodes each function from its entry and follows only the edges it
// can prove. A switch whose jump table it cannot bound hides every case behind it, and an
// estimate in a hidden case was neither counted nor replaced: Maxi Boost ON reported full
// coverage while 15 estimates kept executing vendor-defined results, and an Intel and an AMD
// peer desynced. These tests drive the real pass over a hand-assembled switch of exactly that
// shape and check what it counts, what it rewrites, and what the rewritten code computes.

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "core/branch_targets.h"
#include "core/cpu_patches.h"
#include "core/loader/dwarf.h"

namespace {

using Core::StaticRewriteReason;
using Core::StaticRewriteResult;
using Core::VerifiedBranchTargets;

constexpr size_t ImageSize = 0x10000;
constexpr size_t TrampolineSize = 0x10000;
constexpr u8 NearJumpOpcode = 0xE9;

/// Readable, writable, executable memory. The pass registers a module for the life of the
/// process, so the image it describes is never released either.
u8* AllocateExecutable(size_t size) {
#ifdef _WIN32
    void* memory = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return memory != nullptr ? static_cast<u8*>(memory) : nullptr;
#else
    void* memory =
        mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory != MAP_FAILED ? static_cast<u8*>(memory) : nullptr;
#endif
}

/// A switch the pass cannot bound on its own. Copying the index before the table load is
/// ordinary compiler output, and it is enough to stop the pass's jump-table recogniser. Each
/// case holds a four-byte estimate followed by an instruction the replacement can borrow.
/// The fallback loads an immediate whose bytes encode an estimate, which is what a target in
/// the middle of an instruction would decode. After the last case sits an unreachable jump
/// into that immediate, the shape a walk takes when it follows a misread target. Past the
/// body, the table and then bytes that decode as an estimate: data, which a search bounded by
/// the body must never report.
struct SwitchModule {
    static constexpr u64 EstimateShapedImmediate = 0x90909090'C852FAC5ULL;
    static constexpr size_t ImmediateOffset = 2; // after the REX.W and B8 opcode bytes

    u8* image{};
    uintptr_t entry{};
    uintptr_t fallback{};
    uintptr_t rsqrt_case{};
    uintptr_t rcp_case{};
    uintptr_t jump_into_immediate{};
    uintptr_t body_end{};
    uintptr_t segment_end{};

    [[nodiscard]] u64 Offset(uintptr_t address) const {
        return address - reinterpret_cast<uintptr_t>(image);
    }

    [[nodiscard]] uintptr_t ImmediateAddress() const {
        return fallback + ImmediateOffset;
    }
};

SwitchModule AssembleSwitchModule() {
    using namespace Xbyak::util;

    SwitchModule module;
    module.image = AllocateExecutable(ImageSize + TrampolineSize);
    if (module.image == nullptr) {
        return module;
    }

    Xbyak::CodeGenerator c{ImageSize, module.image};
    Xbyak::Label rsqrt_case, rcp_case, fallback, table;
    c.cmp(edi, 1);
    c.ja(fallback, Xbyak::CodeGenerator::T_NEAR);
    c.mov(eax, edi);
    c.lea(rcx, ptr[rip + table]);
    c.movsxd(rax, dword[rcx + rax * 4]);
    c.add(rax, rcx);
    c.jmp(rax);
    c.L(fallback);
    c.mov(rax, SwitchModule::EstimateShapedImmediate);
    c.vxorps(xmm1, xmm1, xmm1);
    c.ret();
    c.L(rsqrt_case);
    c.vrsqrtss(xmm1, xmm0, xmm0);
    c.vmulss(xmm1, xmm1, xmm0);
    c.ret();
    c.L(rcp_case);
    c.vrcpps(xmm1, xmm0);
    c.vmulps(xmm1, xmm1, xmm0);
    c.ret();
    module.jump_into_immediate = reinterpret_cast<uintptr_t>(c.getCurr());
    c.jmp(reinterpret_cast<const void*>(fallback.getAddress() + SwitchModule::ImmediateOffset),
          Xbyak::CodeGenerator::T_NEAR);
    module.body_end = reinterpret_cast<uintptr_t>(c.getCurr());
    c.align(4);
    c.L(table);
    c.dd(0);
    c.dd(0);
    c.db(0xC5);
    c.db(0xFA);
    c.db(0x52);
    c.db(0xC8);
    c.ready();

    module.entry = reinterpret_cast<uintptr_t>(module.image);
    module.rsqrt_case = reinterpret_cast<uintptr_t>(rsqrt_case.getAddress());
    module.rcp_case = reinterpret_cast<uintptr_t>(rcp_case.getAddress());
    module.fallback = reinterpret_cast<uintptr_t>(fallback.getAddress());
    module.segment_end = reinterpret_cast<uintptr_t>(c.getCurr());

    const auto table_address = reinterpret_cast<uintptr_t>(table.getAddress());
    const std::array<s32, 2> offsets = {
        static_cast<s32>(module.rsqrt_case - table_address),
        static_cast<s32>(module.rcp_case - table_address),
    };
    std::memcpy(reinterpret_cast<void*>(table_address), offsets.data(), sizeof(offsets));

    Core::RegisterPatchModule(module.image, ImageSize, module.image + ImageSize, TrampolineSize);
    return module;
}

/// The exported description of the switch body: its extent and every address control flow
/// can enter, the two cases included, plus whatever a misread analysis added.
std::string DescribeBody(const SwitchModule& module, uintptr_t described_end,
                         std::vector<uintptr_t> extra_targets = {}) {
    std::string text = "version 1\nbase 0x0\nbodies 1\n";
    text += fmt::format("func {:#x} {:#x}\n", module.Offset(module.entry),
                        module.Offset(described_end));
    extra_targets.insert(extra_targets.end(),
                         {module.entry, module.fallback, module.rsqrt_case, module.rcp_case});
    for (const uintptr_t target : extra_targets) {
        if (target < described_end) {
            text += fmt::format("target {:#x}\n", module.Offset(target));
        }
    }
    return text;
}

VerifiedBranchTargets LoadTargets(const std::string& contents) {
    static int counter = 0;
    const auto directory = std::filesystem::temp_directory_path() /
                           ("shadps4_static_rewrite_test_" + std::to_string(++counter));
    std::filesystem::create_directories(directory);
    const auto path = directory / "eboot.bin.txt";
    {
        std::ofstream stream{path, std::ios::binary};
        stream << contents;
    }
    auto targets = VerifiedBranchTargets::Load(path, 1);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return targets;
}

StaticRewriteResult Rewrite(const SwitchModule& module, std::span<const Dwarf::FunctionBody> bodies,
                            const VerifiedBranchTargets* verified) {
    const std::array<uintptr_t, 1> starts = {module.entry};
    return Core::ApplyStaticRewrites(module.entry, module.segment_end - module.entry, starts,
                                     bodies, StaticRewriteReason::DeterministicFp, verified);
}

/// Calls the rewritten switch as a guest would: index in edi, input in xmm0, result in xmm1.
class GuestCaller {
public:
    explicit GuestCaller(uintptr_t entry) : gen{4096} {
        using namespace Xbyak::util;
#ifdef _WIN32
        const Xbyak::Reg32 index = ecx;
        const Xbyak::Reg64 input = rdx;
        const Xbyak::Reg64 output = r8;
#else
        const Xbyak::Reg32 index = edi;
        const Xbyak::Reg64 input = rsi;
        const Xbyak::Reg64 output = rdx;
#endif
        // rdi and rbx are preserved for the host; eight more bytes realign the stack.
        gen.push(rdi);
        gen.push(rbx);
        gen.sub(rsp, 8);
        gen.mov(rbx, output);
        gen.vmovss(xmm0, dword[input]);
        gen.mov(edi, index);
        gen.mov(rax, entry);
        gen.call(rax);
        gen.vmovss(dword[rbx], xmm1);
        gen.add(rsp, 8);
        gen.pop(rbx);
        gen.pop(rdi);
        gen.ret();
        gen.ready();
    }

    [[nodiscard]] float Call(u32 index, float input) const {
        float output = -1.0f;
        gen.getCode<void (*)(u32, const float*, float*)>()(index, &input, &output);
        return output;
    }

private:
    Xbyak::CodeGenerator gen;
};

class StaticRewriteTest : public ::testing::Test {
protected:
    void SetUp() override {
        const Xbyak::util::Cpu cpu;
        if (!cpu.has(Xbyak::util::Cpu::tAVX)) {
            GTEST_SKIP() << "host lacks AVX, which the titles being patched require anyway";
        }
        Core::DeterministicFp::SetEnabled(true);
        module = AssembleSwitchModule();
        ASSERT_NE(nullptr, module.image);
        bodies = {{.start = module.entry, .end = module.body_end}};
    }

    void TearDown() override {
        Core::DeterministicFp::SetEnabled(false);
    }

    [[nodiscard]] u8 FirstByteAt(uintptr_t address) const {
        return *reinterpret_cast<const u8*>(address);
    }

    SwitchModule module;
    std::vector<Dwarf::FunctionBody> bodies;
};

} // namespace

// Without an exported description the cases stay hidden from the walk. What changed is that
// they are no longer hidden from the count: both estimates are reported as undecoded, and the
// estimate-shaped data past the body is not.
TEST_F(StaticRewriteTest, EstimatesBehindAnUnboundedSwitchAreReportedWhenNothingDescribesIt) {
    const auto result = Rewrite(module, bodies, nullptr);

    EXPECT_EQ(2u, result.fp_approximation_site_count);
    EXPECT_EQ(0u, result.patched_fp_approximation_count);
    EXPECT_EQ(2u, result.unsupported_fp_approximation_count);
    EXPECT_EQ(2u, result.fp_unsupported_undecoded_count);
    EXPECT_EQ(0u, result.fp_unmeasured_function_count);
    EXPECT_EQ(0u, result.verified_target_function_count);
    EXPECT_NE(NearJumpOpcode, FirstByteAt(module.rsqrt_case));
    EXPECT_NE(NearJumpOpcode, FirstByteAt(module.rcp_case));
}

// The exported targets name both cases, so the walk starts from them too, finds both
// estimates, and borrows the instruction after each to make room for a near jump.
TEST_F(StaticRewriteTest, VerifiedTargetsLeadTheWalkIntoEveryCaseAndBothAreReplaced) {
    const auto verified = LoadTargets(DescribeBody(module, module.body_end));
    ASSERT_EQ(1u, verified.FunctionCount());

    const auto result = Rewrite(module, bodies, &verified);

    EXPECT_EQ(2u, result.fp_approximation_site_count);
    EXPECT_EQ(2u, result.patched_fp_approximation_count);
    EXPECT_EQ(0u, result.unsupported_fp_approximation_count);
    EXPECT_EQ(0u, result.fp_unsupported_undecoded_count);
    EXPECT_EQ(1u, result.verified_target_function_count);
    EXPECT_EQ(1u, result.verified_extended_function_count);
    EXPECT_EQ(2u, result.fp_borrowed_neighbour_count);
    EXPECT_EQ(0u, result.fp_borrowed_unverified_count);
    EXPECT_EQ(NearJumpOpcode, FirstByteAt(module.rsqrt_case));
    EXPECT_EQ(NearJumpOpcode, FirstByteAt(module.rcp_case));
}

// Replacing the bytes is only half of it: every path through the switch has to compute what
// the exact replacement defines, including the borrowed multiply, and return to the guest.
TEST_F(StaticRewriteTest, RewrittenSwitchComputesTheExactReplacementOnEveryPath) {
    const auto verified = LoadTargets(DescribeBody(module, module.body_end));
    const auto result = Rewrite(module, bodies, &verified);
    ASSERT_EQ(2u, result.patched_fp_approximation_count);

    const GuestCaller caller{module.entry};
    for (const float input : {0.5f, 2.0f, 3.0f, 7.25f, 1024.0f}) {
        SCOPED_TRACE(input);
        const float rsqrt_times_input = std::sqrt(1.0f / input) * input;
        const float rcp_times_input = (1.0f / input) * input;
        EXPECT_EQ(std::bit_cast<u32>(rsqrt_times_input), std::bit_cast<u32>(caller.Call(0, input)));
        EXPECT_EQ(std::bit_cast<u32>(rcp_times_input), std::bit_cast<u32>(caller.Call(1, input)));
        EXPECT_EQ(0u, std::bit_cast<u32>(caller.Call(2, input)));
    }
}

// A disassembler that misreads a table can name a target in the middle of an instruction.
// Decoding from there would find the estimate hidden in the fallback's immediate and "replace"
// it, rewriting the immediate. Only real instruction starts may lead the walk; the real cases
// are still found through the targets that are right.
TEST_F(StaticRewriteTest, TargetInsideAnInstructionDoesNotLeadTheWalk) {
    const auto verified =
        LoadTargets(DescribeBody(module, module.body_end, {module.ImmediateAddress()}));
    ASSERT_EQ(1u, verified.FunctionCount());

    const auto result = Rewrite(module, bodies, &verified);

    EXPECT_EQ(2u, result.fp_approximation_site_count);
    EXPECT_EQ(2u, result.patched_fp_approximation_count);
    EXPECT_EQ(0u, result.unsupported_fp_approximation_count);
    EXPECT_EQ(1u, result.verified_extended_function_count);
    u64 immediate;
    std::memcpy(&immediate, reinterpret_cast<const void*>(module.ImmediateAddress()),
                sizeof(immediate));
    EXPECT_EQ(SwitchModule::EstimateShapedImmediate, immediate);
}

// When the widened walk runs past what the description covers, the widening is discarded -
// but the entry-rooted walk was covered, and stays vouched for exactly as it was before the
// widening existed. The cases it cannot reach are reported, not silently dropped.
TEST_F(StaticRewriteTest, RefusedWideningLeavesTheEntryRootedWalkVouchedFor) {
    const auto verified = LoadTargets(DescribeBody(module, module.rcp_case + 4));
    ASSERT_EQ(1u, verified.FunctionCount());

    const auto result = Rewrite(module, bodies, &verified);

    EXPECT_EQ(1u, result.verified_target_function_count);
    EXPECT_EQ(0u, result.verified_extended_function_count);
    EXPECT_EQ(0u, result.patched_fp_approximation_count);
    EXPECT_EQ(2u, result.fp_unsupported_undecoded_count);
    EXPECT_NE(NearJumpOpcode, FirstByteAt(module.rsqrt_case));
}

// A starting point can be a real instruction start and the walk from it still reach bytes no
// instruction starts at - here a jump into the fallback's immediate. Anything the linear
// decode does not confirm discards the whole widening, rather than replacing an estimate that
// is really the middle of an immediate.
TEST_F(StaticRewriteTest, WideningThatReachesTheMiddleOfAnInstructionIsRefused) {
    const auto verified =
        LoadTargets(DescribeBody(module, module.body_end, {module.jump_into_immediate}));
    ASSERT_EQ(1u, verified.FunctionCount());

    const auto result = Rewrite(module, bodies, &verified);

    EXPECT_EQ(1u, result.verified_target_function_count);
    EXPECT_EQ(0u, result.verified_extended_function_count);
    EXPECT_EQ(2u, result.fp_approximation_site_count);
    EXPECT_EQ(0u, result.patched_fp_approximation_count);
    EXPECT_EQ(2u, result.fp_unsupported_undecoded_count);
    u64 immediate;
    std::memcpy(&immediate, reinterpret_cast<const void*>(module.ImmediateAddress()),
                sizeof(immediate));
    EXPECT_EQ(SwitchModule::EstimateShapedImmediate, immediate);
}

// A description that stops short of code the walk decoded vouches for part of the body only.
// Nothing it says is used then, and the cases are reported exactly as with no file at all.
TEST_F(StaticRewriteTest, DescriptionThatDoesNotCoverTheDecodedCodeIsNotUsed) {
    const auto verified = LoadTargets(DescribeBody(module, module.fallback));
    ASSERT_EQ(1u, verified.FunctionCount());

    const auto result = Rewrite(module, bodies, &verified);

    EXPECT_EQ(0u, result.verified_target_function_count);
    EXPECT_EQ(0u, result.patched_fp_approximation_count);
    EXPECT_EQ(2u, result.fp_unsupported_undecoded_count);
    EXPECT_NE(NearJumpOpcode, FirstByteAt(module.rsqrt_case));
}

// Without a body's extent there is no bound for the search, so the function is reported as
// unmeasured instead of being searched into whatever follows it.
TEST_F(StaticRewriteTest, FunctionWithoutAKnownExtentIsReportedAsUnmeasured) {
    const auto result = Rewrite(module, {}, nullptr);

    EXPECT_EQ(1u, result.fp_unmeasured_function_count);
    EXPECT_EQ(0u, result.fp_unsupported_undecoded_count);
    EXPECT_EQ(0u, result.fp_approximation_site_count);
}
