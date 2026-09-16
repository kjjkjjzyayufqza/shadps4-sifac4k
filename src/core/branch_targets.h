// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core {

// Branch targets recovered ahead of time by a disassembler that resolves switch tables.
//
// The static rewrite pass borrows bytes from the instructions next to a site that is too
// short to hold a jump. That is only safe where every address control flow can enter is
// known, because a branch landing inside a borrowed range would land in the middle of the
// jump that replaced it. The pass recovers targets itself, but gives up on a function
// whose indirect jump it cannot resolve, and then refuses to borrow anything there.
//
// It cannot do better on its own: a switch table holds offsets against a base the pass
// cannot identify, so scanning proves nothing about whether an address is a target. A
// disassembler that recovered the table does know, and this is how it says so.
//
// Produced by `scripts/export_branch_targets.py`. Anything the file does not mention keeps
// the pass's own conservative refusal, so a missing, partial or stale file can only cost
// coverage, never correctness.
class VerifiedBranchTargets {
public:
    struct Function {
        u64 start{}; ///< module-relative, inclusive
        u64 end{};   ///< module-relative, exclusive
        std::vector<u64> targets;
    };

    /// Reads the file for one module. A missing file yields an empty set, which is not an
    /// error: most modules have none and need none.
    [[nodiscard]] static VerifiedBranchTargets Load(const std::filesystem::path& path);

    /// Path convention: <user>/branch_targets/<title serial>/<module name>.txt. The serial
    /// is part of it because every title's main module is called eboot.bin, and applying
    /// one title's target set to another would be exactly the corruption this guards.
    [[nodiscard]] static std::filesystem::path PathForModule(std::string_view title_serial,
                                                             const std::string& module_name);

    [[nodiscard]] bool Empty() const {
        return functions.empty();
    }

    [[nodiscard]] size_t FunctionCount() const {
        return functions.size();
    }

    /// The entry whose range contains this module-relative offset, or nullptr when the
    /// file says nothing about it. Containment rather than an exact start, because a
    /// disassembler sometimes merges into one function what the module's unwind metadata
    /// splits in two: the larger analysis still enumerated every entry point in the
    /// smaller one's range, so it remains a valid answer for it.
    [[nodiscard]] const Function* FindContaining(u64 offset) const;

private:
    std::map<u64, Function> functions;
};

} // namespace Core
