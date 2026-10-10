// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/types.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_request_timeout.h"

namespace Libraries::Np::NpScore {

// Progressive transfer state for the game-data blob attached to a score - the replay file, for
// the two titles this matters to.
//
// Neither sceNpScoreRecordGameData nor sceNpScoreGetGameData takes an offset; the only size
// parameters are the total and the size of this one chunk. Resumption therefore has to be state
// on the request object, which is what a title is holding while it walks a multi-megabyte replay
// through a small staging buffer. Deleting the request drops the state with it.
struct ScoreGameDataXfer {
    // Upload: chunks are accumulated until the title has handed over totalSize bytes, and only
    // then submitted, because the server stores one blob per score and cannot append.
    bool sending = false;
    u32 sendBoardId = 0;
    s32 sendPcId = 0;
    s64 sendScore = 0;
    u64 sendTotal = 0;
    std::vector<u8> sendBuf;

    // Download: the blob is fetched once in full and handed out in recvSize-sized pieces, so a
    // second call continues where the first stopped instead of repeating the opening bytes.
    bool receiving = false;
    u32 recvBoardId = 0;
    s32 recvPcId = 0;
    std::string recvTarget; // online id, or the account id in decimal for the ByAccountId form
    u64 recvOffset = 0;
    std::vector<u8> recvBuf;

    bool SameSendTarget(u32 board, s32 pc, s64 score) const {
        return sending && sendBoardId == board && sendPcId == pc && sendScore == score;
    }
    void BeginSend(u32 board, s32 pc, s64 score, u64 total) {
        sending = true;
        sendBoardId = board;
        sendPcId = pc;
        sendScore = score;
        sendTotal = total;
        sendBuf.clear();
    }
    void EndSend() {
        sending = false;
        sendTotal = 0;
        sendBuf.clear();
        sendBuf.shrink_to_fit();
    }

    bool SameRecvTarget(u32 board, s32 pc, const std::string& target) const {
        return receiving && recvBoardId == board && recvPcId == pc && recvTarget == target;
    }
    void BeginRecv(u32 board, s32 pc, std::string target) {
        receiving = true;
        recvBoardId = board;
        recvPcId = pc;
        recvTarget = std::move(target);
        recvOffset = 0;
        recvBuf.clear();
    }
    void EndRecv() {
        receiving = false;
        recvOffset = 0;
        recvBuf.clear();
        recvBuf.shrink_to_fit();
        recvTarget.clear();
    }

    bool RecvExhausted() const {
        return receiving && recvOffset >= recvBuf.size();
    }

    // Copies the next slice of the cached blob into `out` and advances the read position.
    // Returns how many bytes were written.
    u64 ReadSlice(void* out, u64 outSize) {
        if (recvOffset >= recvBuf.size()) {
            return 0;
        }
        const u64 remaining = recvBuf.size() - recvOffset;
        const u64 n = outSize < remaining ? outSize : remaining;
        if (out != nullptr && n > 0) {
            std::memcpy(out, recvBuf.data() + recvOffset, static_cast<size_t>(n));
        }
        recvOffset += n;
        return n;
    }
};

// Per-request state shared between np_score.cpp (creator/waiter) and the
// handler that fulfils the request (np_handler.cpp)
struct ScoreRequestCtx {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<s32> result; // nullopt = still pending
    bool aborted = false;

    s32 titleCtxId = 0;
    s32 userId = -1;
    s32 pcId = 0;
    u32* tmpRankOut = nullptr;

    // Guarded by `mutex`. Only the guest thread that owns the request touches it, but the
    // download path also reads it from the handler thread when a reply lands.
    ScoreGameDataXfer xfer;

    // Clears the previous operation's outcome so PollAsync/WaitAsync report this call's result
    // rather than the last one's. A title walking a chunked transfer reuses one request object
    // for every chunk, so without this the second chunk would "complete" instantly with the
    // first chunk's result. Returns false once the request has been aborted, which is sticky.
    bool BeginOperation() {
        std::lock_guard lock(mutex);
        if (aborted) {
            return false;
        }
        result.reset();
        return true;
    }

    void Abort(s32 r) {
        {
            std::lock_guard lock(mutex);
            aborted = true;
            result = r;
        }
        cv.notify_all();
    }

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

    // Block until result is set. Used by the synced functions. Bounded so a reply that never
    // arrives fails the call instead of blocking the calling guest thread for the whole session.
    s32 Wait() {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, kNpRequestWaitTimeout, [this] { return result.has_value(); })) {
            result = ORBIS_NP_ERROR_TIMEOUT;
        }
        return *result;
    }
};

} // namespace Libraries::Np::NpScore
