// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include "common/types.h"

namespace Libraries::NetCtl {

using OrbisNetCtlCallback = PS4_SYSV_ABI void (*)(int eventType, void* arg);
using OrbisNetCtlCallbackForNpToolkit = PS4_SYSV_ABI void (*)(int eventType, void* arg);

// netctl events are edge-triggered on hardware: the library reports a transition, not the
// current state. Each callback therefore remembers what it was last told, so a title that has
// already been handed IPOBTAINED is not handed it again on every check - titles drive their
// connection state machine off these events and re-arm it on each one.
struct NetCtlCallback {
    OrbisNetCtlCallback func;
    void* arg;
    std::optional<int> last_event;
};

struct NetCtlCallbackForNpToolkit {
    OrbisNetCtlCallbackForNpToolkit func;
    void* arg;
    std::optional<int> last_event;
};

class NetCtlInternal {
public:
    explicit NetCtlInternal();
    ~NetCtlInternal();

    s32 RegisterCallback(OrbisNetCtlCallback func, void* arg);
    s32 RegisterNpToolkitCallback(OrbisNetCtlCallbackForNpToolkit func, void* arg);
    void CheckCallback();
    void CheckNpToolkitCallback();

public:
    std::array<NetCtlCallbackForNpToolkit, 8> nptool_callbacks{};
    std::array<NetCtlCallback, 8> callbacks{};
    std::mutex m_mutex;
};
} // namespace Libraries::NetCtl
