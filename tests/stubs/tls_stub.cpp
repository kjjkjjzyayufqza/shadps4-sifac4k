// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/tls.h"

namespace Core {

#ifdef _WIN32
// Only the TCB-access patch generators ask for the slot, and no test code reads the TCB.
u32 GetTcbKey() {
    return 0;
}
#endif

} // namespace Core
