// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>

namespace Libraries::Np {

// A shadNet request whose reply never arrives must not strand the title. Nothing below the
// handler enforces a deadline: a dropped, mis-routed or unanswered reply would otherwise leave
// the pending entry in place forever, and every synchronous NP entry point waiting on it blocked
// for the rest of the session. The handler's worker sweeps pending requests older than this and
// fails them, which wakes those waiters with an error the title can act on.
constexpr std::chrono::seconds kNpRequestTimeout{30};

// Synchronous NP entry points wait slightly longer than the sweep deadline, so the sweep - which
// reports a meaningful error and releases the pending entry - is what normally completes a
// request. This is only a backstop for a context that never reached a pending map at all.
constexpr std::chrono::seconds kNpRequestWaitTimeout{35};

} // namespace Libraries::Np
