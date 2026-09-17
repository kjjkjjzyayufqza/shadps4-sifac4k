// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <charconv>
#include <fstream>
#include <string_view>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/branch_targets.h"

namespace Core {

namespace {

constexpr u32 SupportedFormatVersion = 1;
constexpr std::string_view BranchTargetsDir = "branch_targets";

/// Parses "0x..." or a plain decimal. Returns false on anything else, which the caller
/// turns into a rejected file rather than a silently misread address.
bool ParseAddress(std::string_view text, u64& value) {
    int base = 10;
    if (text.starts_with("0x") || text.starts_with("0X")) {
        text.remove_prefix(2);
        base = 16;
    }
    if (text.empty()) {
        return false;
    }
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value, base);
    return result.ec == std::errc{} && result.ptr == last;
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

/// Splits a line into whitespace-separated fields. It stops one past the longest
/// directive, so a line with too many fields still fails the exact-count checks below
/// rather than having its tail silently dropped.
constexpr size_t MaxFields = 4;

std::vector<std::string_view> Split(std::string_view line) {
    std::vector<std::string_view> fields;
    size_t position = 0;
    while (position < line.size() && fields.size() < MaxFields) {
        while (position < line.size() && (line[position] == ' ' || line[position] == '\t')) {
            ++position;
        }
        const size_t begin = position;
        while (position < line.size() && line[position] != ' ' && line[position] != '\t') {
            ++position;
        }
        if (position > begin) {
            fields.push_back(line.substr(begin, position - begin));
        }
    }
    return fields;
}

} // namespace

std::filesystem::path VerifiedBranchTargets::PathForModule(std::string_view title_serial,
                                                           const std::string& module_name) {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / BranchTargetsDir /
           std::string{title_serial} / (module_name + ".txt");
}

const VerifiedBranchTargets::Function* VerifiedBranchTargets::FindContaining(u64 offset) const {
    auto entry = functions.upper_bound(offset);
    if (entry == functions.begin()) {
        return nullptr;
    }
    --entry;
    return entry->second.end > offset ? &entry->second : nullptr;
}

VerifiedBranchTargets VerifiedBranchTargets::Load(const std::filesystem::path& path) {
    VerifiedBranchTargets result;

    std::ifstream file{path};
    if (!file) {
        return result;
    }

    u32 version = 0;
    Function* current = nullptr;
    size_t line_number = 0;
    std::string line;

    const auto reject = [&](std::string_view reason) {
        LOG_ERROR(Core, "Branch target file {} rejected at line {}: {}", path.string(), line_number,
                  reason);
        return VerifiedBranchTargets{};
    };

    while (std::getline(file, line)) {
        ++line_number;
        const std::string_view trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }

        const auto fields = Split(trimmed);
        if (fields.empty()) {
            continue;
        }

        if (fields[0] == "version") {
            u64 parsed = 0;
            if (fields.size() != 2 || !ParseAddress(fields[1], parsed)) {
                return reject("malformed version");
            }
            version = static_cast<u32>(parsed);
            if (version != SupportedFormatVersion) {
                return reject("unsupported format version");
            }
            continue;
        }

        if (version != SupportedFormatVersion) {
            return reject("version must be declared before any entry");
        }

        if (fields[0] == "base") {
            // The exporter records the image base it subtracted. It is informational: the
            // addresses that follow are already module-relative.
            continue;
        }

        if (fields[0] == "func") {
            u64 start = 0;
            u64 end = 0;
            if (fields.size() != 3 || !ParseAddress(fields[1], start) ||
                !ParseAddress(fields[2], end) || end <= start) {
                return reject("malformed func entry");
            }
            const auto [entry, inserted] = result.functions.try_emplace(start);
            if (!inserted) {
                return reject("duplicate func entry");
            }
            entry->second.start = start;
            entry->second.end = end;
            current = &entry->second;
            continue;
        }

        if (fields[0] == "target") {
            u64 target = 0;
            if (current == nullptr || fields.size() != 2 || !ParseAddress(fields[1], target)) {
                return reject("malformed target entry");
            }
            if (target < current->start || target >= current->end) {
                return reject("target outside its function");
            }
            current->targets.push_back(target);
            continue;
        }

        return reject("unrecognised directive");
    }

    // Lookup is by containment, so overlapping entries would make the answer depend on
    // which one happened to be found. Two descriptions of one address is not a description.
    u64 previous_end = 0;
    for (const auto& [start, function] : result.functions) {
        if (start < previous_end) {
            return reject("overlapping func entries");
        }
        previous_end = function.end;
    }

    for (auto& [start, function] : result.functions) {
        std::ranges::sort(function.targets);
        const auto duplicates = std::ranges::unique(function.targets);
        function.targets.erase(duplicates.begin(), duplicates.end());
    }

    LOG_INFO(Core, "Loaded {} verified function target sets from {}", result.functions.size(),
             path.string());
    return result;
}

} // namespace Core
