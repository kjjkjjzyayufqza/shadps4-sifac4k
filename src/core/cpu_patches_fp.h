// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>

// Deterministic floating point for lockstep P2P titles.
//
// The reciprocal and reciprocal-square-root *estimate* instructions are the only
// x86-64 floating point operations whose results are implementation-defined: the
// ISA only requires a relative error below 1.5 * 2^-12 and leaves the lookup
// tables to the vendor. Intel, AMD and the PS4's own Jaguar core all differ.
//
// Titles such as Gundam Versus and Gundam Extreme VS Maxi Boost ON run a lockstep
// simulation and exchange only inputs over P2P, so a single differing bit desyncs
// the match permanently. Replacing every estimate with exact IEEE-754 division and
// square root makes the result bit-identical on every host, which is the property
// lockstep actually requires (matching real hardware is not).
//
// See documents/deterministic-fp-netplay-plan.md for the full design.

namespace Core::DeterministicFp {

/// True when the mnemonic is a reciprocal or reciprocal-square-root estimate, in
/// either legacy SSE or VEX encoding.
[[nodiscard]] bool IsApproximationMnemonic(ZydisMnemonic mnemonic);

/// True when the operands of an approximation instruction have a shape a generator can
/// replace exactly. Sites that fail this are left executing the original estimate, so the
/// caller must count them as unpatched rather than treating them as absent: for a lockstep
/// title an unreplaced site is a correctness hole, not a missed optimisation.
///
/// Rejected shapes are the ones no replacement can express faithfully: a non-default
/// segment, 32-bit addressing, an extended register a VEX replacement cannot encode, or a
/// source whose width disagrees with the destination.
[[nodiscard]] bool IsSupportedOperandShape(ZydisMnemonic mnemonic,
                                           const ZydisDecodedOperand* operands);

/// Emits an exact replacement for the instruction the operands were decoded from.
/// The signature matches the patch table's InstructionGenerator.
///
/// `address` is the address the original instruction was decoded from. It is required:
/// a RIP-relative source is resolved against it, because the replacement executes from
/// the trampoline where the original displacement no longer points anywhere useful.
///
/// Every generator preserves the destination's merge and zeroing semantics: VEX
/// scalar forms take lanes [127:32] from the first source and zero [255:128],
/// legacy scalar forms leave [127:32] untouched, and packed forms follow the
/// operand width.
///
/// Operands must satisfy IsSupportedOperandShape; anything else throws Xbyak::Error.
void GenerateVRCPPS(void* address, const ZydisDecodedOperand* operands,
                    Xbyak::CodeGenerator& c);
void GenerateVRCPSS(void* address, const ZydisDecodedOperand* operands,
                    Xbyak::CodeGenerator& c);
void GenerateVRSQRTPS(void* address, const ZydisDecodedOperand* operands,
                      Xbyak::CodeGenerator& c);
void GenerateVRSQRTSS(void* address, const ZydisDecodedOperand* operands,
                      Xbyak::CodeGenerator& c);
void GenerateRCPPS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c);
void GenerateRCPSS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c);
void GenerateRSQRTPS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c);
void GenerateRSQRTSS(void* address, const ZydisDecodedOperand* operands, Xbyak::CodeGenerator& c);

/// Drops the cached constant pool belonging to a generator. Patch modules live for the
/// process, so the emulator never needs this; a caller that destroys and recreates
/// generators must call it, or a later generator reusing the same address would be
/// handed a pool that no longer exists.
void ForgetCodeGenerator(const Xbyak::CodeGenerator& c);

} // namespace Core::DeterministicFp
