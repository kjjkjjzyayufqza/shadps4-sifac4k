// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include "common/types.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_request_timeout.h"

namespace Libraries::Np::NpTus {

struct TusRequestCtx {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<s32> result;

    void SetResult(s32 r) {
        {
            std::lock_guard lock(mutex);
            if (result.has_value()) {
                return;
            }
            result = r;
        }
        cv.notify_all();
    }

    // True once the operation has completed (or been aborted).
    bool HasResult() {
        std::lock_guard lock(mutex);
        return result.has_value();
    }

    // Block until the handler (or an abort/delete) sets the result. Bounded so a reply that
    // never arrives fails the call instead of blocking the calling guest thread forever.
    s32 Wait() {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, kNpRequestWaitTimeout, [this] { return result.has_value(); })) {
            result = ORBIS_NP_ERROR_TIMEOUT;
        }
        return *result;
    }
};

} // namespace Libraries::Np::NpTus
