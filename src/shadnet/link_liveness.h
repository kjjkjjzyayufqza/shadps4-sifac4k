// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>
#include <vector>
#include "shadnet/client.h"

namespace ShadNet {

/// Enables dead-peer detection on a connected shadNet TCP socket: keepalive probing of an idle
/// link and a bound on unacknowledged data (see SHAD_KEEPALIVE_* in client.h). Returns the names
/// of the socket options the platform refused; empty when every option was applied.
std::vector<std::string_view> ApplyLinkLiveness(ShadSocketHandle sock);

} // namespace ShadNet
