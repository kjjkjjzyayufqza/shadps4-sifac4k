// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>
#include "common/types.h"

namespace ShadNet {

// Bound for one lookup. The system resolver is abandoned after this, even if it would
// answer later. A direct query runs beside it.
static constexpr u32 SHAD_RESOLVE_TIMEOUT_MS = 2500;
// Prefer the system resolver when it answers inside this window, so a hosts-file or
// split-DNS address is not replaced by the public answer.
static constexpr u32 SHAD_RESOLVE_SYSTEM_GRACE_MS = 400;

// IPv4 addresses in network byte order. A numeric host does not touch DNS.
// Empty when every lookup failed inside the budget.
std::vector<u32> ResolveIPv4(const std::string& host);

} // namespace ShadNet
