// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/enum.h"
#include "common/types.h"
#include "core/branch_targets.h"

// ============================================================================
// Windows static guest red-zone protection
// ============================================================================

enum class WindowsGuestRedZoneProtectionMode : u32 {
    Disabled,
    StaticPatching,
};

namespace Core::WindowsGuestRedZoneProtection {

void SetActiveMode(WindowsGuestRedZoneProtectionMode mode) noexcept;
WindowsGuestRedZoneProtectionMode GetActiveMode() noexcept;
bool IsStaticPatchingEnabled() noexcept;

} // namespace Core::WindowsGuestRedZoneProtection

// ============================================================================
// End Windows static guest red-zone protection
// ============================================================================

// ============================================================================
// Deterministic floating point for lockstep P2P titles
// ============================================================================

namespace Core::DeterministicFp {

/// Toggles replacement of the vendor-defined reciprocal and reciprocal-square-root
/// estimates. Declared here rather than in cpu_patches_fp.h so the settings layer can
/// reach it without pulling in Zydis and Xbyak.
void SetEnabled(bool enabled) noexcept;
bool IsEnabled() noexcept;

} // namespace Core::DeterministicFp

// ============================================================================
// End deterministic floating point
// ============================================================================

namespace Core {

/// Why a static ahead-of-time rewrite pass is running. The reasons share one function
/// walk, one relocation engine and one trampoline, so asking for both costs a single
/// pass rather than two.
enum class StaticRewriteReason : u32 {
    None = 0,
    GuestRedZone = 1 << 0,    ///< Windows guest red-zone protection
    DeterministicFp = 1 << 1, ///< lockstep-safe replacement of FP estimates
};
DECLARE_ENUM_FLAG_OPERATORS(StaticRewriteReason)

struct StaticRewriteResult {
    u64 function_count{};
    u64 instruction_count{};
    u64 red_zone_function_count{};
    u64 memory_instruction_count{};
    u64 short_memory_instruction_count{};
    u64 patched_memory_instruction_count{};
    u64 stack_dependent_memory_instruction_count{};
    u64 control_flow_memory_instruction_count{};
    u64 unrelocatable_memory_instruction_count{};
    u64 indirect_red_zone_function_count{};
    u64 cpu_patch_instruction_count{};
    u64 patched_cpu_patch_instruction_count{};
    u64 unsupported_cpu_patch_instruction_count{};

    // Deterministic floating point. A site that is found but not patched leaves a
    // vendor-defined estimate executing, and no runtime fallback can catch it later,
    // so the unsupported count is a correctness signal rather than a statistic.
    u64 fp_approximation_site_count{};
    u64 patched_fp_approximation_count{};
    u64 unsupported_fp_approximation_count{};

    // Why the unsupported ones were left behind. Closing the gap needs a different fix for
    // each, so they are separated rather than summed.
    u64 fp_unsupported_operand_shape_count{};   ///< no faithful replacement for the operands
    u64 fp_unsupported_indirect_branch_count{}; ///< function has an unresolvable branch table
    u64 fp_unsupported_unrelocatable_count{};   ///< no span or relay could reach the site

    /// Functions whose target set an external analysis vouched for, letting the pass
    /// borrow neighbouring instructions where it would otherwise have refused.
    u64 verified_target_function_count{};
};

// Windows static guest red-zone protection
using RedZonePatchResult = StaticRewriteResult;

/// Registers a module for patching, providing an area to generate trampoline code.
void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size);

/// Applies CPU patches that need to be done before beginning executions.
void PrePatchInstructions(u64 segment_addr, u64 segment_size);

/// Rewrites a loaded executable segment ahead of time, using the verified function
/// boundaries the caller decoded from the module's EH metadata. Instructions too short to
/// take a near jump are relocated along with enough neighbours to make room.
/// `verified` may be null, in which case the pass relies only on the targets it can
/// recover itself and refuses to borrow neighbours in any function with an indirect jump
/// it could not resolve.
StaticRewriteResult ApplyStaticRewrites(u64 segment_addr, u64 segment_size,
                                        std::span<const uintptr_t> function_starts,
                                        StaticRewriteReason reasons,
                                        const VerifiedBranchTargets* verified);

// Windows static guest red-zone protection
/// Keeps Windows exception dispatch outside live guest red zones at faultable memory accesses.
RedZonePatchResult PatchRedZoneMemoryInstructions(u64 segment_addr, u64 segment_size,
                                                  std::span<const uintptr_t> function_starts);

} // namespace Core
