// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "common/enum.h"
#include "common/types.h"
#include "core/branch_targets.h"
#include "core/loader/dwarf.h"

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
    /// Lies in a body's code but the walk never reached it - typically a switch case behind
    /// a jump table the pass cannot parse and no external analysis described.
    u64 fp_unsupported_undecoded_count{};

    /// Functions whose unwind extent was unknown, so the bytes the walk did not reach could
    /// not be searched for estimates. Any here means the counts above prove nothing.
    u64 fp_unmeasured_function_count{};

    /// Functions whose target set an external analysis vouched for, letting the pass
    /// borrow neighbouring instructions where it would otherwise have refused.
    u64 verified_target_function_count{};
    /// Of those, the ones where the vouched-for targets led the walk into code it had not
    /// reached from the entry alone.
    u64 verified_extended_function_count{};

    // Replaced sites that had to borrow bytes from the instructions around them, because
    // the estimate itself was too short to hold a near jump. Borrowing is only sound
    // where every address control flow can enter is known. The pass recovers targets
    // within a function body, but an entry from outside it - an exception landing pad,
    // say - is only known where an external analysis vouched for the body. The second
    // number is therefore the pass's exposure, not a statistic.
    u64 fp_borrowed_neighbour_count{};
    u64 fp_borrowed_unverified_count{};

    u64 inplace_cpu_patch_instruction_count{};
    u64 uncovered_inplace_cpu_patch_instruction_count{};
    u64 uncovered_unsupported_cpu_patch_instruction_count{};

    StaticRewriteResult& operator+=(const StaticRewriteResult& other) {
        function_count += other.function_count;
        instruction_count += other.instruction_count;
        red_zone_function_count += other.red_zone_function_count;
        memory_instruction_count += other.memory_instruction_count;
        short_memory_instruction_count += other.short_memory_instruction_count;
        patched_memory_instruction_count += other.patched_memory_instruction_count;
        stack_dependent_memory_instruction_count += other.stack_dependent_memory_instruction_count;
        control_flow_memory_instruction_count += other.control_flow_memory_instruction_count;
        unrelocatable_memory_instruction_count += other.unrelocatable_memory_instruction_count;
        indirect_red_zone_function_count += other.indirect_red_zone_function_count;
        cpu_patch_instruction_count += other.cpu_patch_instruction_count;
        patched_cpu_patch_instruction_count += other.patched_cpu_patch_instruction_count;
        unsupported_cpu_patch_instruction_count += other.unsupported_cpu_patch_instruction_count;
        fp_approximation_site_count += other.fp_approximation_site_count;
        patched_fp_approximation_count += other.patched_fp_approximation_count;
        unsupported_fp_approximation_count += other.unsupported_fp_approximation_count;
        fp_unsupported_operand_shape_count += other.fp_unsupported_operand_shape_count;
        fp_unsupported_indirect_branch_count += other.fp_unsupported_indirect_branch_count;
        fp_unsupported_unrelocatable_count += other.fp_unsupported_unrelocatable_count;
        fp_unsupported_undecoded_count += other.fp_unsupported_undecoded_count;
        fp_unmeasured_function_count += other.fp_unmeasured_function_count;
        verified_target_function_count += other.verified_target_function_count;
        verified_extended_function_count += other.verified_extended_function_count;
        fp_borrowed_neighbour_count += other.fp_borrowed_neighbour_count;
        fp_borrowed_unverified_count += other.fp_borrowed_unverified_count;
        inplace_cpu_patch_instruction_count += other.inplace_cpu_patch_instruction_count;
        uncovered_inplace_cpu_patch_instruction_count +=
            other.uncovered_inplace_cpu_patch_instruction_count;
        uncovered_unsupported_cpu_patch_instruction_count +=
            other.uncovered_unsupported_cpu_patch_instruction_count;
        return *this;
    }
};

// Windows static guest red-zone protection
using RedZonePatchResult = StaticRewriteResult;

/// Registers a module for patching, providing an area to generate trampoline code.
void RegisterPatchModule(void* module_ptr, u64 module_size, void* trampoline_area_ptr,
                         u64 trampoline_area_size);

/// Applies CPU patches that need to be done before beginning executions.
void PrePatchInstructions(u64 segment_addr, u64 segment_size);

/// Resets the Windows hung-window timer without dispatching messages. The static
/// rewrite pass and symbol load run on the loader thread long enough for QtLauncher
/// to ghost the window and delay IPC START.
void PumpLoaderThread();

/// Rewrites a loaded executable segment ahead of time, using the verified function
/// boundaries the caller decoded from the module's EH metadata. Instructions too short to
/// take a near jump are relocated along with enough neighbours to make room.
/// `verified` may be null, in which case the pass relies only on the targets it can
/// recover itself and refuses to borrow neighbours in any function with an indirect jump
/// it could not resolve.
/// `function_bodies` is the extent of each unwind-table body. Deterministic floating point
/// searches the bytes of each body the walk never reached, because an estimate there is
/// otherwise invisible; a function with no extent is counted as unmeasured.
StaticRewriteResult ApplyStaticRewrites(u64 segment_addr, u64 segment_size,
                                        std::span<const uintptr_t> function_starts,
                                        std::span<const Dwarf::FunctionBody> function_bodies,
                                        StaticRewriteReason reasons,
                                        const VerifiedBranchTargets* verified);

// Windows static guest red-zone protection
/// Keeps Windows exception dispatch outside live guest red zones at faultable memory accesses.
RedZonePatchResult PatchRedZoneMemoryInstructions(u64 segment_addr, u64 segment_size,
                                                  std::span<const uintptr_t> function_starts);

/// Applies CPU patches ahead of time to the functions listed in the EH frame search table. A patch
/// site too short for a jump is relocated together with its neighboring instructions. Between
/// those functions, only the SSE4a instructions long enough for a jump are patched.
RedZonePatchResult PatchCpuInstructionsStatically(u64 segment_addr, u64 segment_size,
                                                  std::span<const uintptr_t> function_starts);

} // namespace Core
