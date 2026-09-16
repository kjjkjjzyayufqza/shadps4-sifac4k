// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The static rewrite pass borrows bytes from the instructions around a patch site, and it
// only does that in functions this file vouches for. A entry read wrongly would licence
// borrowing across a real branch target, which corrupts guest code at a site that never
// faults. So every malformed input has to produce *nothing* rather than a partial set:
// saying nothing leaves the pass's own conservative refusal in place, which is safe.

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/branch_targets.h"

namespace {

using Core::VerifiedBranchTargets;

/// Writes a file into a unique temporary directory and removes it afterwards.
class TemporaryFile {
public:
    explicit TemporaryFile(std::string_view contents) {
        directory = std::filesystem::temp_directory_path() /
                    ("shadps4_branch_targets_test_" + std::to_string(++counter));
        std::filesystem::create_directories(directory);
        path = directory / "eboot.bin.txt";
        std::ofstream stream{path, std::ios::binary};
        stream << contents;
    }

    ~TemporaryFile() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const {
        return path;
    }

private:
    static inline int counter = 0;

    std::filesystem::path directory;
    std::filesystem::path path;
};

VerifiedBranchTargets LoadFrom(std::string_view contents) {
    const TemporaryFile file{contents};
    return VerifiedBranchTargets::Load(file.Path());
}

constexpr std::string_view kValid = R"(# a comment
# another

version 1
base 0x400000
func 0x1000 0x1100
target 0x1020
target 0x10f0
target 0x1020
func 0x2000 0x2010
)";

} // namespace

TEST(VerifiedBranchTargets, AbsentFileIsEmptyAndNotAnError) {
    const auto targets = VerifiedBranchTargets::Load(
        std::filesystem::temp_directory_path() / "shadps4_branch_targets_does_not_exist.txt");
    EXPECT_TRUE(targets.Empty());
    EXPECT_EQ(0u, targets.FunctionCount());
}

TEST(VerifiedBranchTargets, ParsesFunctionsTargetsCommentsAndBlankLines) {
    const auto targets = LoadFrom(kValid);
    ASSERT_EQ(2u, targets.FunctionCount());

    const auto* first = targets.FindContaining(0x1000);
    ASSERT_NE(nullptr, first);
    EXPECT_EQ(0x1000u, first->start);
    EXPECT_EQ(0x1100u, first->end);
    // Sorted and deduplicated, so a lookup never depends on the file's ordering.
    ASSERT_EQ(2u, first->targets.size());
    EXPECT_EQ(0x1020u, first->targets[0]);
    EXPECT_EQ(0x10f0u, first->targets[1]);

    const auto* second = targets.FindContaining(0x2000);
    ASSERT_NE(nullptr, second);
    EXPECT_TRUE(second->targets.empty()) << "a function with no internal targets is ordinary";

    // Lookup is by containment, so an address inside an entry finds it. That is what lets
    // an entry cover a function the module's unwind metadata splits out of a larger one.
    EXPECT_EQ(first, targets.FindContaining(0x1020));
    EXPECT_EQ(first, targets.FindContaining(0x10ff));
    EXPECT_EQ(nullptr, targets.FindContaining(0x1100)) << "the end is exclusive";
    EXPECT_EQ(nullptr, targets.FindContaining(0x3000));
    EXPECT_EQ(nullptr, targets.FindContaining(0x0fff));
}

TEST(VerifiedBranchTargets, PathIsScopedByTitleSerial) {
    // Every title's main module is called eboot.bin. Applying one title's target set to
    // another would licence borrowing at addresses that mean something else entirely.
    const auto first = VerifiedBranchTargets::PathForModule("CUSA15006", "eboot.bin");
    const auto second = VerifiedBranchTargets::PathForModule("CUSA08379", "eboot.bin");
    EXPECT_NE(first, second);
    EXPECT_EQ("eboot.bin.txt", first.filename().string());
    EXPECT_EQ("CUSA15006", first.parent_path().filename().string());
}

class BranchTargetRejection : public ::testing::TestWithParam<std::string_view> {};

TEST_P(BranchTargetRejection, MalformedInputYieldsNothing) {
    const auto targets = LoadFrom(GetParam());
    EXPECT_TRUE(targets.Empty())
        << "a file that cannot be trusted whole must not be trusted in part";
}

INSTANTIATE_TEST_SUITE_P(
    Cases, BranchTargetRejection,
    ::testing::Values(
        // A version this build does not understand may mean anything at all.
        std::string_view{"version 2\nfunc 0x1000 0x1100\n"},
        // Entries before the version line could have been written by any format.
        std::string_view{"func 0x1000 0x1100\nversion 1\n"},
        // A target outside its function points at a neighbour's code.
        std::string_view{"version 1\nfunc 0x1000 0x1100\ntarget 0x1100\n"},
        std::string_view{"version 1\nfunc 0x1000 0x1100\ntarget 0xfff\n"},
        // A target with no function to belong to.
        std::string_view{"version 1\ntarget 0x1020\n"},
        // Two entries for one function: which one describes it?
        std::string_view{"version 1\nfunc 0x1000 0x1100\nfunc 0x1000 0x1200\n"},
        // An empty or inverted range describes nothing.
        std::string_view{"version 1\nfunc 0x1100 0x1000\n"},
        std::string_view{"version 1\nfunc 0x1000 0x1000\n"},
        // Addresses that are not addresses.
        std::string_view{"version 1\nfunc 0x1000 zzzz\n"},
        std::string_view{"version 1\nfunc 0x1000 0x1100\ntarget 0x10g0\n"},
        std::string_view{"version 1\nfunc 0x1000\n"},
        std::string_view{"version 1\nfunc 0x1000 0x1100 0x1200\n"},
        // Overlapping entries: lookup is by containment, so which one describes 0x1050?
        std::string_view{"version 1\nfunc 0x1000 0x1100\nfunc 0x1050 0x1200\n"},
        // A directive from some other format.
        std::string_view{"version 1\nfunc 0x1000 0x1100\nsection .text\n"}));
