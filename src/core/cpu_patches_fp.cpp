// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <optional>
#include <unordered_map>

#include "common/types.h"
#include "core/cpu_patches_fp.h"

using namespace Xbyak::util;

namespace Core::DeterministicFp {

namespace {

// Guest code follows the SysV ABI and owns the 128 bytes below rsp. Any spill has to
// step past that window first, the same way GenerateEXTRQ does in cpu_patches.cpp.
constexpr int GuestRedZoneBytes = 128;

// One ymm register's worth of spill space. Scratch is always saved at full width so a
// VEX-128 replacement cannot zero an upper half the guest still holds live.
constexpr int ScratchSlotBytes = 32;

constexpr int SpillDisplacement = GuestRedZoneBytes + ScratchSlotBytes;

// Blend control taking lane 0 from the first source and lanes 1-3 from the second.
constexpr u8 BlendKeepUpperFromSecond = 0b1110;

// VEX can only reach the first sixteen vector registers, and guest code never uses more.
constexpr int EncodableVectorRegisters = 16;

// Constant pool, laid out once per trampoline generator:
//   [0]  eight 1.0f, serving both the xmm and the ymm form
//   [32] one 1.0 double, for the legacy cvtsd2ss trick that preserves dst[127:32]
constexpr size_t OnesPackedOffset = 0;
constexpr size_t OneDoubleOffset = 32;
constexpr u32 OneFloatBits = 0x3F800000;
constexpr u64 OneDoubleBits = 0x3FF0000000000000ULL;

std::mutex constant_pool_mutex;
std::unordered_map<const Xbyak::CodeGenerator*, const u8*> constant_pools;

// Emits the pool on first use for a generator and hands back its address. Emitting it
// per site instead would put a branch on a path that occurs over a thousand times in a
// single title. Patch modules live for the process, so the cache never dangles.
const u8* GetConstantPool(Xbyak::CodeGenerator& c) {
    std::scoped_lock lock{constant_pool_mutex};
    if (const auto it = constant_pools.find(&c); it != constant_pools.end()) {
        return it->second;
    }

    Xbyak::Label skip;
    c.jmp(skip, Xbyak::CodeGenerator::LabelType::T_NEAR);
    const u8* pool = c.getCurr();
    for (int i = 0; i < 8; ++i) {
        c.dd(OneFloatBits);
    }
    c.db(OneDoubleBits, sizeof(u64));
    c.L(skip);

    constant_pools.emplace(&c, pool);
    return pool;
}

Xbyak::Address PackedOnes(const u8* pool) {
    return ptr[rip + static_cast<const void*>(pool + OnesPackedOffset)];
}

Xbyak::Address OneAsDouble(const u8* pool) {
    return ptr[rip + static_cast<const void*>(pool + OneDoubleOffset)];
}

[[nodiscard]] bool IsWideRegister(ZydisRegister reg) {
    return reg >= ZYDIS_REGISTER_YMM0 && reg <= ZYDIS_REGISTER_YMM31;
}

[[nodiscard]] int VectorRegisterIndex(ZydisRegister reg) {
    if (reg >= ZYDIS_REGISTER_XMM0 && reg <= ZYDIS_REGISTER_XMM31) {
        return reg - ZYDIS_REGISTER_XMM0;
    }
    if (reg >= ZYDIS_REGISTER_YMM0 && reg <= ZYDIS_REGISTER_YMM31) {
        return reg - ZYDIS_REGISTER_YMM0;
    }
    return -1;
}

/// Index of a source operand's register, or -1 when it came from memory and so cannot
/// alias the destination.
[[nodiscard]] int SourceRegisterIndex(const ZydisDecodedOperand& operand) {
    return operand.type == ZYDIS_OPERAND_TYPE_REGISTER ? VectorRegisterIndex(operand.reg.value)
                                                       : -1;
}

[[nodiscard]] bool IsEncodableVectorRegister(ZydisRegister reg, bool wide) {
    const int index = VectorRegisterIndex(reg);
    return index >= 0 && index < EncodableVectorRegisters && IsWideRegister(reg) == wide;
}

[[nodiscard]] bool IsAddressableGpr(ZydisRegister reg) {
    return reg == ZYDIS_REGISTER_NONE || (reg >= ZYDIS_REGISTER_RAX && reg <= ZYDIS_REGISTER_R15);
}

/// Memory shapes a replacement can rebuild faithfully. A non-default segment or 32-bit
/// addressing would have to be re-encoded rather than reassembled from its parts, and
/// silently dropping either would read the wrong address at a site that never crashes.
[[nodiscard]] bool IsRebuildableMemoryOperand(const ZydisDecodedOperand& operand) {
    if (operand.mem.type != ZYDIS_MEMOP_TYPE_MEM) {
        return false;
    }
    if (operand.mem.segment != ZYDIS_REGISTER_DS && operand.mem.segment != ZYDIS_REGISTER_SS) {
        return false;
    }
    if (operand.mem.base == ZYDIS_REGISTER_RIP) {
        return operand.mem.index == ZYDIS_REGISTER_NONE;
    }
    if (!IsAddressableGpr(operand.mem.base) || !IsAddressableGpr(operand.mem.index)) {
        return false;
    }
    switch (operand.mem.scale) {
    case 0:
    case 1:
    case 2:
    case 4:
    case 8:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool IsUsableSource(const ZydisDecodedOperand& operand, bool wide) {
    if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
        return IsEncodableVectorRegister(operand.reg.value, wide);
    }
    return operand.type == ZYDIS_OPERAND_TYPE_MEMORY && IsRebuildableMemoryOperand(operand);
}

/// Absolute address a RIP-relative operand refers to. The encoded displacement is measured
/// from the end of the original instruction, and the replacement runs from the trampoline
/// instead, so the target has to be resolved here and the displacement re-derived by Xbyak
/// at wherever the replacement actually lands.
[[nodiscard]] uintptr_t ResolveRipTarget(const void* address, const ZydisDecodedOperand& operand) {
    if (address == nullptr) {
        // The generator contract requires the original site address for exactly this case.
        throw Xbyak::Error(Xbyak::ERR_BAD_PARAMETER);
    }

    static const ZydisDecoder decoder = [] {
        ZydisDecoder created;
        ZydisDecoderInit(&created, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        return created;
    }();

    ZydisDecodedInstruction instruction;
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr, address,
                                                    ZYDIS_MAX_INSTRUCTION_LENGTH, &instruction))) {
        throw Xbyak::Error(Xbyak::ERR_BAD_PARAMETER);
    }

    return reinterpret_cast<uintptr_t>(address) + instruction.length +
           static_cast<intptr_t>(operand.mem.disp.value);
}

// Rebuilds a decoded memory operand for Xbyak. `rsp_adjust` compensates for a spill
// that already moved rsp: an rsp-relative source decoded at the original site would
// otherwise read the wrong slot.
Xbyak::Address RebuildMemoryOperand(const ZydisDecodedOperand& operand, int rsp_adjust,
                                    const void* address) {
    if (operand.mem.base == ZYDIS_REGISTER_RIP) {
        return ptr[rip + reinterpret_cast<const void*>(ResolveRipTarget(address, operand))];
    }

    const auto to_gpr = [](ZydisRegister reg) {
        return Xbyak::Reg64(reg - ZYDIS_REGISTER_RAX + Xbyak::Operand::RAX);
    };

    Xbyak::RegExp expression{};
    bool uses_rsp = false;
    if (operand.mem.base != ZYDIS_REGISTER_NONE) {
        uses_rsp = uses_rsp || operand.mem.base == ZYDIS_REGISTER_RSP;
        expression = expression + to_gpr(operand.mem.base);
    }
    if (operand.mem.index != ZYDIS_REGISTER_NONE) {
        uses_rsp = uses_rsp || operand.mem.index == ZYDIS_REGISTER_RSP;
        const auto index = to_gpr(operand.mem.index);
        expression =
            operand.mem.scale != 0 ? expression + index * operand.mem.scale : expression + index;
    }

    int64_t displacement = operand.mem.disp.size != 0 ? operand.mem.disp.value : 0;
    if (uses_rsp) {
        displacement += rsp_adjust;
    }
    if (displacement != 0) {
        expression = expression + displacement;
    }
    return ptr[expression];
}

// A decoded source usable directly as an Xbyak operand, whether the original encoding
// held a register or a memory reference.
class SourceOperand {
public:
    SourceOperand(const ZydisDecodedOperand& operand, bool wide, int rsp_adjust,
                  const void* address) {
        if (operand.type != ZYDIS_OPERAND_TYPE_REGISTER) {
            memory.emplace(RebuildMemoryOperand(operand, rsp_adjust, address));
            return;
        }
        const int index = VectorRegisterIndex(operand.reg.value);
        if (wide) {
            ymm.emplace(index);
        } else {
            xmm.emplace(index);
        }
    }

    [[nodiscard]] const Xbyak::Operand& Ref() const {
        if (memory) {
            return *memory;
        }
        return ymm ? static_cast<const Xbyak::Operand&>(*ymm)
                   : static_cast<const Xbyak::Operand&>(*xmm);
    }

private:
    std::optional<Xbyak::Address> memory;
    std::optional<Xbyak::Xmm> xmm;
    std::optional<Xbyak::Ymm> ymm;
};

/// Lowest vector register colliding with none of the operands. A replacement touches at
/// most three registers, so xmm0-xmm3 always holds a free one.
[[nodiscard]] int PickScratchIndex(int a, int b, int c) {
    for (int candidate = 0; candidate < 4; ++candidate) {
        if (candidate != a && candidate != b && candidate != c) {
            return candidate;
        }
    }
    return 3;
}

void SpillScratchWide(Xbyak::CodeGenerator& c, const Xbyak::Ymm& scratch) {
    c.lea(rsp, ptr[rsp - SpillDisplacement]);
    c.vmovups(ptr[rsp], scratch);
}

void RestoreScratchWide(Xbyak::CodeGenerator& c, const Xbyak::Ymm& scratch) {
    c.vmovups(scratch, ptr[rsp]);
    c.lea(rsp, ptr[rsp + SpillDisplacement]);
}

// Legacy SSE replacements must not spill through VEX stores: a vmovups of an xmm zeroes
// the register's upper half, which a non-AVX guest running on an AVX host may still be
// holding live data in. Legacy movups leaves it alone.
void SpillScratchLegacy(Xbyak::CodeGenerator& c, const Xbyak::Xmm& scratch) {
    c.lea(rsp, ptr[rsp - SpillDisplacement]);
    c.movups(ptr[rsp], scratch);
}

void RestoreScratchLegacy(Xbyak::CodeGenerator& c, const Xbyak::Xmm& scratch) {
    c.movups(scratch, ptr[rsp]);
    c.lea(rsp, ptr[rsp + SpillDisplacement]);
}

// Shared body for the VEX packed forms at a single width. `take_sqrt` turns the
// reciprocal into a reciprocal square root.
template <typename Vec>
void EmitVexPackedReciprocal(Xbyak::CodeGenerator& c, const u8* pool, int dst_index,
                             const ZydisDecodedOperand& source, bool wide, bool take_sqrt,
                             const void* address) {
    const Vec dst{dst_index};
    const int source_index = SourceRegisterIndex(source);

    if (source_index != dst_index) {
        const SourceOperand src{source, wide, 0, address};
        c.vmovups(dst, PackedOnes(pool));
        c.vdivps(dst, dst, src.Ref());
        if (take_sqrt) {
            c.vsqrtps(dst, dst);
        }
        return;
    }

    // The destination aliases the source, so the ones cannot be staged in the
    // destination without destroying the input.
    const Xbyak::Ymm spill{PickScratchIndex(dst_index, source_index, -1)};
    const Vec scratch{spill.getIdx()};
    SpillScratchWide(c, spill);
    const SourceOperand src{source, wide, SpillDisplacement, address};
    c.vmovups(scratch, PackedOnes(pool));
    c.vdivps(dst, scratch, src.Ref());
    if (take_sqrt) {
        c.vsqrtps(dst, dst);
    }
    RestoreScratchWide(c, spill);
}

void EmitVexPackedReciprocalAtWidth(Xbyak::CodeGenerator& c, const ZydisDecodedOperand* operands,
                                    bool take_sqrt, const void* address) {
    const u8* pool = GetConstantPool(c);
    const bool wide = IsWideRegister(operands[0].reg.value);
    const int dst_index = VectorRegisterIndex(operands[0].reg.value);

    if (wide) {
        EmitVexPackedReciprocal<Xbyak::Ymm>(c, pool, dst_index, operands[1], wide, take_sqrt,
                                            address);
    } else {
        EmitVexPackedReciprocal<Xbyak::Xmm>(c, pool, dst_index, operands[1], wide, take_sqrt,
                                            address);
    }
}

// Shared body for the legacy packed forms.
void EmitLegacyPackedReciprocal(Xbyak::CodeGenerator& c, const ZydisDecodedOperand* operands,
                                bool take_sqrt, const void* address) {
    const u8* pool = GetConstantPool(c);
    const int dst_index = VectorRegisterIndex(operands[0].reg.value);
    const ZydisDecodedOperand& source = operands[1];
    const int source_index = SourceRegisterIndex(source);

    const Xbyak::Xmm dst{dst_index};

    if (source_index != dst_index) {
        const SourceOperand src{source, false, 0, address};
        c.movups(dst, PackedOnes(pool));
        c.divps(dst, src.Ref());
        if (take_sqrt) {
            c.sqrtps(dst, dst);
        }
        return;
    }

    const Xbyak::Xmm scratch{PickScratchIndex(dst_index, source_index, -1)};
    SpillScratchLegacy(c, scratch);
    const SourceOperand src{source, false, SpillDisplacement, address};
    c.movups(scratch, PackedOnes(pool));
    c.divps(scratch, src.Ref());
    if (take_sqrt) {
        c.sqrtps(dst, scratch);
    } else {
        c.movaps(dst, scratch);
    }
    RestoreScratchLegacy(c, scratch);
}

} // namespace

void ForgetCodeGenerator(const Xbyak::CodeGenerator& c) {
    std::scoped_lock lock{constant_pool_mutex};
    constant_pools.erase(&c);
}

bool IsApproximationMnemonic(ZydisMnemonic mnemonic) {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_RCPPS:
    case ZYDIS_MNEMONIC_RCPSS:
    case ZYDIS_MNEMONIC_RSQRTPS:
    case ZYDIS_MNEMONIC_RSQRTSS:
    case ZYDIS_MNEMONIC_VRCPPS:
    case ZYDIS_MNEMONIC_VRCPSS:
    case ZYDIS_MNEMONIC_VRSQRTPS:
    case ZYDIS_MNEMONIC_VRSQRTSS:
        return true;
    default:
        return false;
    }
}

bool IsSupportedOperandShape(ZydisMnemonic mnemonic, const ZydisDecodedOperand* operands) {
    if (operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER) {
        return false;
    }

    switch (mnemonic) {
    case ZYDIS_MNEMONIC_VRCPPS:
    case ZYDIS_MNEMONIC_VRSQRTPS: {
        const bool wide = IsWideRegister(operands[0].reg.value);
        return IsEncodableVectorRegister(operands[0].reg.value, wide) &&
               IsUsableSource(operands[1], wide);
    }
    case ZYDIS_MNEMONIC_VRCPSS:
    case ZYDIS_MNEMONIC_VRSQRTSS:
        // Scalar VEX forms merge lanes 1-3 from a register first source, never memory.
        return IsEncodableVectorRegister(operands[0].reg.value, false) &&
               operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
               IsEncodableVectorRegister(operands[1].reg.value, false) &&
               IsUsableSource(operands[2], false);
    case ZYDIS_MNEMONIC_RCPPS:
    case ZYDIS_MNEMONIC_RCPSS:
    case ZYDIS_MNEMONIC_RSQRTPS:
    case ZYDIS_MNEMONIC_RSQRTSS:
        return IsEncodableVectorRegister(operands[0].reg.value, false) &&
               IsUsableSource(operands[1], false);
    default:
        return false;
    }
}

// vrcpps dst, src  ->  dst = 1.0 / src, correctly rounded.
void GenerateVRCPPS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c) {
    EmitVexPackedReciprocalAtWidth(c, operands, false, address);
}

// vrsqrtps dst, src  ->  dst = sqrt(1.0 / src).
//
// Computing the reciprocal first and taking its square root needs no scratch register,
// and both steps are correctly rounded, so every host agrees bit for bit. Two inputs
// depart from the hardware estimate, and both are deterministic, so neither can desync a
// lockstep match - they only differ from a real PS4:
//
//   -0          gives NaN here rather than -inf, because sqrt(-inf) is NaN.
//   |x| > 2^127 gives +0, because the guest runs with FTZ set and the intermediate 1/x
//               is denormal, so it is flushed before the square root sees it.
//
// Normalisation feeds rsqrt a sum of squares, which can be +0 but never -0, and which
// reaches 2^127 only when the inputs have already overflowed. Restoring hardware's
// ordering would cost a scratch register and a red-zone-safe spill at every site.
void GenerateVRSQRTPS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c) {
    EmitVexPackedReciprocalAtWidth(c, operands, true, address);
}

// vrsqrtss dst, src1, src2
//   ->  dst = { sqrt(1.0 / src2[31:0]), src1[127:32] }, lanes [255:128] zeroed.
//
// The closing vsqrtss takes its upper lanes from src1, which is exactly the merge the
// instruction specifies, so the common case needs neither a blend nor a scratch.
void GenerateVRSQRTSS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c) {
    const u8* pool = GetConstantPool(c);
    const int dst_index = VectorRegisterIndex(operands[0].reg.value);
    const int src1_index = VectorRegisterIndex(operands[1].reg.value);
    const int src2_index = SourceRegisterIndex(operands[2]);

    const Xbyak::Xmm dst{dst_index};
    const Xbyak::Xmm src1{src1_index};

    if (dst_index != src1_index && dst_index != src2_index) {
        const SourceOperand src2{operands[2], false, 0, address};
        c.vmovups(dst, PackedOnes(pool));
        c.vdivss(dst, dst, src2.Ref());
        c.vsqrtss(dst, src1, dst);
        return;
    }

    // Staging the ones in dst would destroy an input. The arithmetic is deliberately
    // identical to the path above so both routes produce the same bits.
    const Xbyak::Ymm spill{PickScratchIndex(dst_index, src1_index, src2_index)};
    const Xbyak::Xmm scratch{spill.getIdx()};
    SpillScratchWide(c, spill);
    const SourceOperand src2{operands[2], false, SpillDisplacement, address};
    c.vmovups(scratch, PackedOnes(pool));
    c.vdivss(scratch, scratch, src2.Ref());
    c.vsqrtss(dst, src1, scratch);
    RestoreScratchWide(c, spill);
}

// vrcpss dst, src1, src2
//   ->  dst = { 1.0 / src2[31:0], src1[127:32] }, lanes [255:128] zeroed.
//
// vdivss keeps lanes 1-3 from its own first source, so src1's upper lanes have to be
// put back with a blend rather than arriving for free as they do for vrsqrtss.
void GenerateVRCPSS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c) {
    const u8* pool = GetConstantPool(c);
    const int dst_index = VectorRegisterIndex(operands[0].reg.value);
    const int src1_index = VectorRegisterIndex(operands[1].reg.value);
    const int src2_index = SourceRegisterIndex(operands[2]);

    const Xbyak::Xmm dst{dst_index};
    const Xbyak::Xmm src1{src1_index};

    if (dst_index != src1_index && dst_index != src2_index) {
        const SourceOperand src2{operands[2], false, 0, address};
        c.vmovups(dst, PackedOnes(pool));
        c.vdivss(dst, dst, src2.Ref());
        c.vblendps(dst, dst, src1, BlendKeepUpperFromSecond);
        return;
    }

    const Xbyak::Ymm spill{PickScratchIndex(dst_index, src1_index, src2_index)};
    const Xbyak::Xmm scratch{spill.getIdx()};
    SpillScratchWide(c, spill);
    const SourceOperand src2{operands[2], false, SpillDisplacement, address};
    c.vmovups(scratch, PackedOnes(pool));
    c.vdivss(scratch, scratch, src2.Ref());
    c.vblendps(dst, scratch, src1, BlendKeepUpperFromSecond);
    RestoreScratchWide(c, spill);
}

// rcpps dst, src  ->  dst = 1.0 / src. Legacy encoding, destination fully written.
void GenerateRCPPS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c) {
    EmitLegacyPackedReciprocal(c, operands, false, address);
}

// rsqrtps dst, src  ->  dst = sqrt(1.0 / src). Legacy encoding.
void GenerateRSQRTPS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c) {
    EmitLegacyPackedReciprocal(c, operands, true, address);
}

// rsqrtss dst, src  ->  dst[31:0] = 1.0 / sqrt(src[31:0]), dst[127:32] preserved.
//
// Legacy scalar forms merge into the destination, so the ones cannot simply be loaded
// over it. cvtsd2ss writes lane 0 from memory while leaving lanes 1-3 alone, which is
// what makes the constant reachable without disturbing the merge.
//
// Unlike the VEX path this keeps the hardware's operation order, so -0 yields -inf
// here exactly as rsqrtss does. Both orderings are deterministic across hosts; a given
// site only ever takes one of them, decided by its static encoding.
void GenerateRSQRTSS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c) {
    const u8* pool = GetConstantPool(c);
    const int dst_index = VectorRegisterIndex(operands[0].reg.value);

    const Xbyak::Xmm dst{dst_index};
    const Xbyak::Xmm scratch{
        PickScratchIndex(dst_index, SourceRegisterIndex(operands[1]), -1)};

    SpillScratchLegacy(c, scratch);
    const SourceOperand src{operands[1], false, SpillDisplacement, address};
    c.movaps(scratch, dst);
    c.sqrtss(scratch, src.Ref());
    c.cvtsd2ss(dst, OneAsDouble(pool));
    c.divss(dst, scratch);
    RestoreScratchLegacy(c, scratch);
}

// rcpss dst, src  ->  dst[31:0] = 1.0 / src[31:0], dst[127:32] preserved.
void GenerateRCPSS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c) {
    const u8* pool = GetConstantPool(c);
    const int dst_index = VectorRegisterIndex(operands[0].reg.value);

    const Xbyak::Xmm dst{dst_index};
    const Xbyak::Xmm scratch{
        PickScratchIndex(dst_index, SourceRegisterIndex(operands[1]), -1)};

    SpillScratchLegacy(c, scratch);
    const SourceOperand src{operands[1], false, SpillDisplacement, address};
    c.movss(scratch, src.Ref());
    c.cvtsd2ss(dst, OneAsDouble(pool));
    c.divss(dst, scratch);
    RestoreScratchLegacy(c, scratch);
}

} // namespace Core::DeterministicFp
