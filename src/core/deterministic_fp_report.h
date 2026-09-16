// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// Startup diagnostics for lockstep floating point determinism.
//
// A desync between two peers is caused by their hosts disagreeing about a result, so it
// cannot be diagnosed from one machine. This report makes a single log file from either
// peer enough to compare: it records what the host CPU is, what its estimate instructions
// actually return, what the replacement returns, and what the host math library returns.
//
// Everything here runs unconditionally in release builds. It costs a few microseconds once
// at startup, and the whole point is that any tester's log is usable evidence.
//
// See documents/deterministic-fp-netplay-plan.md.

namespace Core::DeterministicFp {

/// Emits the host environment report. Call once, after settings are loaded so the report
/// can state whether the replacement is active for this run.
void LogHostEnvironmentReport();

/// Fingerprint of the host math library, derived from what it actually computes rather
/// than from its version. Two peers whose libm fingerprints differ will desync on the
/// first transcendental the simulation evaluates, regardless of instruction patching.
[[nodiscard]] u64 GetHostLibmFingerprint();

/// Fingerprint of the host's estimate instructions. Expected to differ between vendors -
/// that difference is the bug this whole effort exists to remove, so it is recorded rather
/// than hidden.
[[nodiscard]] u64 GetHostEstimateFingerprint();

} // namespace Core::DeterministicFp
