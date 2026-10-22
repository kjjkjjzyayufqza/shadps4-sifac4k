// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <mutex>
#include <set>
#include <vector>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_handler/np_handler.h"
#include "core/libraries/np/np_ingame_message.h"

namespace Libraries::Np::NpInGameMessage {

// In-game messages are PSN's server-relayed, title-defined short messages: a handle is created,
// a target list is prepared, and one payload is sent to those targets. They never touch the P2P
// mesh, which is the point - the targets are players who are not in the sender's room (invites,
// join-in requests, cross-room notices).
//
// Until now shadPS4 had no implementation at all: the seven entry points resolved to the generic
// AeroLib stub, which returns 0. A title reading that gets "sent successfully" for a message that
// was never transmitted, so anything built on this channel failed silently with nothing in the
// log to show for it.
//
// What the retail library does, read out of libSceNpManager.sprx:
//   * CreateHandle takes no arguments and returns the handle id (module offset 0x2520).
//   * PrepareA records the target list on the library context, not on the handle: the thunks at
//     0x2130/0x2140 feed the same worker either an online-id list or an account-id list plus the
//     count, and 0x38E0 writes the pointer, the count and which kind it was onto the context
//     object. Only the A forms are registered here, because those are the ones both titles
//     import.
//   * SendDataA takes no handle id: it reads the prepared flag and the target list back off that
//     same context (0x2690), answering NOT_PREPARED when Prepare never ran, and rejects a payload
//     outside 1..512 bytes. A second PrepareA therefore replaces the first rather than adding to
//     it.
//   * The send itself goes out through the WebApi layer with the data type "inGameMessage"
//     (0x34B0). There is no receive entry point in the library at all, so reception is the
//     title's own WebApi push-event callback - which shadPS4 already delivers.
// The first parameter of PrepareA/SendDataA is the library context id, and the retail
// implementation ignores it: it resolves everything through a single process-wide context. This
// implementation does the same rather than inventing a stricter rule than the hardware has.

namespace {

std::mutex g_mutex;
bool g_initialized = false;
s32 g_lib_ctx_id = 0;
s32 g_next_lib_ctx_id = 1;
s32 g_next_handle_id = 1;
// Live handle ids. A handle is only an identity the send path validates against; the recipients
// live on the library context, not on it (see g_targets).
std::set<s32> g_handles;
// Recipients recorded by the last PrepareA, and whether one has happened. This is context-wide
// rather than per-handle because that is where the retail library keeps it: its Prepare worker
// writes the list and the prepared flag onto the single process-wide context object
// (libSceNpManager offsets 0x38E0 and 0x34B0), and SendDataA - which takes no handle id - reads
// them back from there. So a second PrepareA replaces the first rather than adding to it.
std::vector<u64> g_targets;
bool g_prepared = false;

} // namespace

s32 PS4_SYSV_ABI sceNpInGameMessageInitialize(const void* param) {
    std::lock_guard lock(g_mutex);
    if (g_initialized) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: already initialized (ctx={})", g_lib_ctx_id);
        return ORBIS_NP_ERROR_ALREADY_INITIALIZED;
    }
    g_initialized = true;
    g_lib_ctx_id = g_next_lib_ctx_id++;
    g_handles.clear();
    g_targets.clear();
    g_prepared = false;
    LOG_INFO(Lib_NpManager, "InGameMessage: initialized, libCtxId={} param={}", g_lib_ctx_id,
             fmt::ptr(param));
    return g_lib_ctx_id;
}

s32 PS4_SYSV_ABI sceNpInGameMessageTerminate(s32 libCtxId) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: terminate while not initialized");
        return ORBIS_NP_ERROR_NOT_INITIALIZED;
    }
    LOG_INFO(Lib_NpManager, "InGameMessage: terminated libCtxId={} ({} handle(s) dropped)",
             libCtxId, g_handles.size());
    g_initialized = false;
    g_lib_ctx_id = 0;
    g_handles.clear();
    g_targets.clear();
    g_prepared = false;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpInGameMessageCreateHandle() {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: CreateHandle before Initialize");
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
    }
    const s32 handle_id = g_next_handle_id++;
    g_handles.insert(handle_id);
    LOG_INFO(Lib_NpManager, "InGameMessage: CreateHandle -> {}", handle_id);
    return handle_id;
}

s32 PS4_SYSV_ABI sceNpInGameMessageDeleteHandle(s32 handleId) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
    }
    if (g_handles.erase(handleId) == 0) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: DeleteHandle unknown handle {}", handleId);
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_HANDLE_NOT_FOUND;
    }
    LOG_INFO(Lib_NpManager, "InGameMessage: DeleteHandle {}", handleId);
    return ORBIS_OK;
}

// Records the recipients for the next send. `reserved` is the parameter the retail thunks zero
// before calling their worker, so it carries nothing.
static s32 PrepareCommon(s32 libCtxId, s32 handleId, std::vector<u64> targets) {
    std::lock_guard lock(g_mutex);
    if (!g_initialized) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: Prepare before Initialize");
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
    }
    if (!g_handles.contains(handleId)) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: Prepare unknown handle {}", handleId);
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_HANDLE_NOT_FOUND;
    }
    if (targets.empty()) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: Prepare with no targets (handle {})", handleId);
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT;
    }
    LOG_INFO(Lib_NpManager, "InGameMessage: PrepareA libCtx={} handle={} targets={}", libCtxId,
             handleId, targets.size());
    for (const u64 account_id : targets) {
        LOG_INFO(Lib_NpManager, "InGameMessage:   target accountId={}", account_id);
    }
    g_targets = std::move(targets);
    g_prepared = true;
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpInGameMessagePrepareA(s32 libCtxId, s32 handleId, u64 reserved,
                                            const OrbisNpAccountId* to, u64 toNum) {
    if (to == nullptr || toNum == 0) {
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT;
    }
    std::vector<u64> targets(to, to + toNum);
    return PrepareCommon(libCtxId, handleId, std::move(targets));
}

// The recipients come from the handle the matching PrepareA call filled in, so the two pointer
// arguments naming identifiers are never interpreted here. They are still validated the way the
// retail thunk does - it refuses a null for any of the three before reaching the worker - so a
// title that passes a bad pointer gets the same error it would on hardware.
static s32 SendDataCommon(s32 libCtxId, const void* to, const void* from,
                          const OrbisNpInGameMessageData* msg) {
    if (to == nullptr || from == nullptr || msg == nullptr) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: SendDataA null argument");
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT;
    }
    if (msg->dataSize == 0 || msg->dataSize > ORBIS_NP_IN_GAME_MESSAGE_DATA_SIZE_MAX) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: SendDataA dataSize {} outside 1..{}",
                  msg->dataSize, ORBIS_NP_IN_GAME_MESSAGE_DATA_SIZE_MAX);
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT;
    }

    std::vector<u64> targets;
    {
        std::lock_guard lock(g_mutex);
        if (!g_initialized) {
            return ORBIS_NP_IN_GAME_MESSAGE_ERROR_LIB_CONTEXT_NOT_FOUND;
        }
        if (!g_prepared) {
            // The same answer the retail library gives when SendData runs before Prepare.
            LOG_ERROR(Lib_NpManager, "InGameMessage: SendDataA before PrepareA");
            return ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_PREPARED;
        }
        targets = g_targets;
    }
    if (targets.empty()) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: SendDataA with an empty target list");
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_PREPARED;
    }

    const s32 user_id = NpHandler::GetInstance().GetFirstSignedInUserId();
    if (user_id < 0) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: SendDataA while signed out");
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_SIGNED_IN;
    }

    LOG_INFO(Lib_NpManager, "InGameMessage: SendDataA libCtx={} targets={} bytes={}", libCtxId,
             targets.size(), msg->dataSize);
    const s32 rc = NpHandler::GetInstance().SendInGameMessage(user_id, targets, msg->data,
                                                              static_cast<size_t>(msg->dataSize),
                                                              /*message_type=*/0);
    if (rc != ORBIS_OK) {
        LOG_ERROR(Lib_NpManager, "InGameMessage: SendDataA relay failed ({:#x})", rc);
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_NOT_SIGNED_IN;
    }
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpInGameMessageSendDataA(s32 libCtxId, const void* to, const void* from,
                                             const OrbisNpInGameMessageData* msg) {
    return SendDataCommon(libCtxId, to, from, msg);
}

s32 PS4_SYSV_ABI sceNpInGameMessageGetMemoryPoolStatistics(s32 libCtxId, void* stats) {
    if (stats == nullptr) {
        return ORBIS_NP_IN_GAME_MESSAGE_ERROR_INVALID_ARGUMENT;
    }
    // shadPS4 relays these messages from the host heap, so there is no pool to measure. The three
    // counters the caller expects are zeroed rather than left as whatever was on its stack.
    std::memset(stats, 0, sizeof(u64) * 3);
    LOG_INFO(Lib_NpManager, "InGameMessage: GetMemoryPoolStatistics libCtx={} (no pool in use)",
             libCtxId);
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("GFhVUpRmbHE", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageInitialize);
    LIB_FUNCTION("bMG3cVmUmuk", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageTerminate);
    LIB_FUNCTION("s4UEa5iBJdc", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageCreateHandle);
    LIB_FUNCTION("+anuSx2avHQ", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageDeleteHandle);
    LIB_FUNCTION("IkL62FMpIpo", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessagePrepareA);
    LIB_FUNCTION("PQDFxcnqxtw", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageSendDataA);
    LIB_FUNCTION("Ubv+fP58W1U", "libSceNpManager", 1, "libSceNpManager",
                 sceNpInGameMessageGetMemoryPoolStatistics);
}

} // namespace Libraries::Np::NpInGameMessage
