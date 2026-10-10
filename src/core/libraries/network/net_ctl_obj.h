// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
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
    // Release a slot taken by the matching Register call. Titles unregister and re-register
    // around a reconnect; leaving the old entry in place makes the same callback fire twice for
    // every later event and eventually exhausts the table.
    s32 UnregisterCallback(int cid);
    s32 UnregisterNpToolkitCallback(int cid);
    void CheckCallback();
    void CheckNpToolkitCallback();

    // Error recorded for the most recent transition of `event_type`, which is what
    // sceNetCtlGetResult answers with. Every transition shadPS4 produces comes from the host
    // being connected or not and so carries no error, but a title branches on this value and
    // must see the code for the event it just handled rather than a constant.
    s32 LastEventError(int event_type) const;

public:
    std::array<NetCtlCallbackForNpToolkit, 8> nptool_callbacks{};
    std::array<NetCtlCallback, 8> callbacks{};
    std::map<int, s32> last_event_errors;
    mutable std::mutex m_mutex;
};
} // namespace Libraries::NetCtl
