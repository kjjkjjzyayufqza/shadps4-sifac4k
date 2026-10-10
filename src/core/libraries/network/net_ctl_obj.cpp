// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <vector>
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/network/net_ctl_codes.h"
#include "core/libraries/network/net_ctl_obj.h"
#include "core/tls.h"

namespace Libraries::NetCtl {

NetCtlInternal::NetCtlInternal() = default;

NetCtlInternal::~NetCtlInternal() = default;

s32 NetCtlInternal::RegisterCallback(OrbisNetCtlCallback func, void* arg) {
    std::scoped_lock lock{m_mutex};

    // Find the next available slot
    const auto it = std::ranges::find(callbacks, nullptr, &NetCtlCallback::func);
    if (it == callbacks.end()) {
        return ORBIS_NET_CTL_ERROR_CALLBACK_MAX;
    }

    const int next_id = std::distance(callbacks.begin(), it);
    callbacks[next_id].func = func;
    callbacks[next_id].arg = arg;
    // A fresh callback has been told nothing yet, so the next check reports the current state.
    callbacks[next_id].last_event.reset();
    return next_id;
}

s32 NetCtlInternal::RegisterNpToolkitCallback(OrbisNetCtlCallbackForNpToolkit func, void* arg) {
    std::scoped_lock lock{m_mutex};

    // Find the next available slot
    const auto it = std::ranges::find(nptool_callbacks, nullptr, &NetCtlCallbackForNpToolkit::func);
    if (it == nptool_callbacks.end()) {
        return ORBIS_NET_CTL_ERROR_CALLBACK_MAX;
    }

    const int next_id = std::distance(nptool_callbacks.begin(), it);
    nptool_callbacks[next_id].func = func;
    nptool_callbacks[next_id].arg = arg;
    // A fresh callback has been told nothing yet, so the next check reports the current state.
    nptool_callbacks[next_id].last_event.reset();
    return next_id;
}

s32 NetCtlInternal::LastEventError(int event_type) const {
    std::scoped_lock lock{m_mutex};
    const auto it = last_event_errors.find(event_type);
    return it == last_event_errors.end() ? 0 : it->second;
}

s32 NetCtlInternal::UnregisterCallback(int cid) {
    if (cid < 0 || static_cast<size_t>(cid) >= callbacks.size()) {
        return ORBIS_NET_CTL_ERROR_INVALID_ID;
    }
    std::scoped_lock lock{m_mutex};
    if (callbacks[cid].func == nullptr) {
        return ORBIS_NET_CTL_ERROR_ID_NOT_FOUND;
    }
    callbacks[cid] = NetCtlCallback{};
    LOG_DEBUG(Lib_NetCtl, "unregistered callback {}", cid);
    return ORBIS_OK;
}

s32 NetCtlInternal::UnregisterNpToolkitCallback(int cid) {
    if (cid < 0 || static_cast<size_t>(cid) >= nptool_callbacks.size()) {
        return ORBIS_NET_CTL_ERROR_INVALID_ID;
    }
    std::scoped_lock lock{m_mutex};
    if (nptool_callbacks[cid].func == nullptr) {
        return ORBIS_NET_CTL_ERROR_ID_NOT_FOUND;
    }
    nptool_callbacks[cid] = NetCtlCallbackForNpToolkit{};
    LOG_DEBUG(Lib_NetCtl, "unregistered NpToolkit callback {}", cid);
    return ORBIS_OK;
}

void NetCtlInternal::CheckCallback() {
    const auto event = EmulatorSettings.IsConnectedToNetwork()
                           ? ORBIS_NET_CTL_EVENT_TYPE_IPOBTAINED
                           : ORBIS_NET_CTL_EVENT_TYPE_DISCONNECTED;

    std::vector<NetCtlCallback> due;
    {
        std::scoped_lock lock{m_mutex};
        // A transition produced from host connectivity always succeeds; recording it keeps
        // sceNetCtlGetResult answering for the event the title is actually handling.
        last_event_errors[event] = 0;
        for (auto& cb : callbacks) {
            if (cb.func == nullptr || (cb.last_event && *cb.last_event == event)) {
                continue;
            }
            cb.last_event = event;
            due.push_back(cb);
        }
    }

    // Dispatched outside the lock: these run guest code, which is free to call back into netctl.
    for (const auto& cb : due) {
        LOG_DEBUG(Lib_NetCtl, "delivering event {} to callback {}", event, fmt::ptr(cb.arg));
        cb.func(event, cb.arg);
    }
}

void NetCtlInternal::CheckNpToolkitCallback() {
    const auto event = EmulatorSettings.IsConnectedToNetwork()
                           ? ORBIS_NET_CTL_EVENT_TYPE_IPOBTAINED
                           : ORBIS_NET_CTL_EVENT_TYPE_DISCONNECTED;

    std::vector<NetCtlCallbackForNpToolkit> due;
    {
        std::scoped_lock lock{m_mutex};
        last_event_errors[event] = 0;
        for (auto& cb : nptool_callbacks) {
            if (cb.func == nullptr || (cb.last_event && *cb.last_event == event)) {
                continue;
            }
            cb.last_event = event;
            due.push_back(cb);
        }
    }

    for (const auto& cb : due) {
        LOG_DEBUG(Lib_NetCtl, "delivering NpToolkit event {} to callback {}", event,
                  fmt::ptr(cb.arg));
        cb.func(event, cb.arg);
    }
}

} // namespace Libraries::NetCtl
