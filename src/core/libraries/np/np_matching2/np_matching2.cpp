// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstring>
#include <memory>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/libraries/error_codes.h"
#include "core/libraries/libs.h"
#include "core/libraries/network/net.h"
#include "core/libraries/network/net_util.h"
#include "core/libraries/network/netctl.h"
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_handler.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_matching2/np_matching2.h"
#include "core/libraries/np/np_matching2/np_matching2_internal.h"
#include "core/libraries/np/np_matching2/np_matching2_mm.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_types.h"
#include "core/libraries/system/userservice.h"

namespace Libraries::Np::NpMatching2 {

int PS4_SYSV_ABI sceNpMatching2CreateContext(const OrbisNpMatching2CreateContextParameter* param,
                                             OrbisNpMatching2ContextId* ctxId) {
    LOG_INFO(Lib_NpMatching2, "called, npId = {}, serviceLabel = {}, size = {}",
             param->npId->handle.data, param->serviceLabel, param->size);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!param || param->size != 0x28 || !ctxId) {
        LOG_ERROR(Lib_NpMatching2, "param null, invalid size, or ctxId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    return ContextManager::Instance().CreateContext(param->npId, param->serviceLabel, ctxId);
}

int PS4_SYSV_ABI sceNpMatching2CreateContextA(const OrbisNpMatching2CreateContextParameterA* param,
                                              OrbisNpMatching2ContextId* ctxId) {
    LOG_INFO(Lib_NpMatching2, "called, userId = {}, serviceLabel = {}, size = {}", param->userId,
             param->serviceLabel, param->size);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!param || param->size != 0x10 || !ctxId) {
        LOG_ERROR(Lib_NpMatching2, "param null, invalid size, or ctxId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    const Libraries::Np::OrbisNpId np_id =
        Libraries::Np::NpHandler::GetInstance().GetNpId(param->userId);

    return ContextManager::Instance().CreateContext(&np_id, param->serviceLabel, ctxId, true);
}

int PS4_SYSV_ABI sceNpMatching2CreateJoinRoom(OrbisNpMatching2ContextId ctxId,
                                              OrbisNpMatching2CreateJoinRoomRequest* request,
                                              OrbisNpMatching2RequestOptParam* requestOpt,
                                              OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmCreateJoinRoom(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2JoinRoom(OrbisNpMatching2ContextId ctxId,
                                        OrbisNpMatching2JoinRoomRequest* request,
                                        OrbisNpMatching2RequestOptParam* requestOpt,
                                        OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmJoinRoom(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2CreateJoinRoomA(OrbisNpMatching2ContextId ctxId,
                                               OrbisNpMatching2CreateJoinRoomRequestA* request,
                                               OrbisNpMatching2RequestOptParam* requestOpt,
                                               OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmCreateJoinRoomA(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2RegisterContextCallback(OrbisNpMatching2ContextCallback callback,
                                                       void* userdata) {
    LOG_INFO(Lib_NpMatching2, "called, userdata = {}", userdata);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    ContextManager::Instance().ApplyContextCallback(callback, userdata);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2RegisterLobbyEventCallback(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2LobbyEventCallback callback, void* userdata) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    ctx->lobby_event_callback = callback;
    ctx->lobby_event_callback_arg = userdata;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2RegisterRoomEventCallback(OrbisNpMatching2ContextId ctxId,
                                                         OrbisNpMatching2RoomEventCallback callback,
                                                         void* userdata) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    ctx->room_event_callback = callback;
    ctx->room_event_callback_arg = userdata;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2RegisterSignalingCallback(OrbisNpMatching2ContextId ctxId,
                                                         OrbisNpMatching2SignalingCallback callback,
                                                         void* userdata) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    ctx->signaling_callback = callback;
    ctx->signaling_callback_arg = userdata;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2ContextStart(OrbisNpMatching2ContextId ctxId, u64 timeout) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, timeout = {}", ctxId, timeout);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!EmulatorSettings.IsConnectedToNetwork() || !EmulatorSettings.IsShadNetEnabled()) {
        // error confirmed with a real console disconnected from the internet
        constexpr int ORBIS_NET_ERROR_RESOLVER_ETIMEDOUT = 0x804101e2;
        LOG_ERROR(Lib_NpMatching2, "not connected to network");
        return ORBIS_NET_ERROR_RESOLVER_ETIMEDOUT;
    }

    const s32 rc = ContextManager::Instance().Start(ctxId);
    if (rc != ORBIS_OK) {
        return rc;
    }

    MmContextStart(ctxId);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2ContextStop(OrbisNpMatching2ContextId ctxId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    const s32 rc = ContextManager::Instance().Stop(ctxId);
    if (rc != ORBIS_OK) {
        return rc;
    }

    const s32 submit_rc = MmContextStop(ctxId);
    if (submit_rc != ORBIS_OK) {
        ContextManager::Instance().CompleteStop(ctxId);
        return submit_rc;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2Initialize(OrbisNpMatching2InitializeParameter* param) {
    LOG_INFO(Lib_NpMatching2, "called");

    if (IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "already initialized");
        return ORBIS_NP_MATCHING2_ERROR_ALREADY_INITIALIZED;
    }

    if (!param || (param->size != 0x30 && param->size != 0x28)) {
        LOG_ERROR(Lib_NpMatching2, "param null or invalid size");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    LOG_INFO(Lib_NpMatching2, "poolSize={:#x} stackSize={:#x} priority={} size={:#x}",
             param->poolSize, param->stackSize, param->priority, param->size);
    g_state.pool_size = param->poolSize;
    g_state.ssl_pool_size = 0;
    if (param->size == 0x30) {
        LOG_INFO(Lib_NpMatching2, "sslPoolSize={:#x}", param->sslPoolSize);
        g_state.ssl_pool_size = param->sslPoolSize;
    }

    InitEventDispatcher();

    SetInitialized(true);
    g_state.initialized.store(true);

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2Terminate() {
    LOG_INFO(Lib_NpMatching2, "called");

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    SetInitialized(false);
    g_state.initialized.store(false);

    TermEventDispatcher();
    ContextManager::Instance().Reset();

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetDefaultRequestOptParam(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2RequestOptParam* requestOpt) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!requestOpt) {
        LOG_ERROR(Lib_NpMatching2, "requestOpt null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    ctx->default_request_callback = requestOpt->callback;
    ctx->default_request_callback_arg = requestOpt->arg;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetServerId(OrbisNpMatching2ContextId ctxId,
                                           OrbisNpMatching2ServerId* serverId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!serverId) {
        LOG_ERROR(Lib_NpMatching2, "serverId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    *serverId = 1;

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetWorldInfoList(OrbisNpMatching2ContextId ctxId,
                                                OrbisNpMatching2GetWorldInfoListRequest* request,
                                                OrbisNpMatching2RequestOptParam* requestOpt,
                                                OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, request.serverId = {}, requestOpt = {}", ctxId,
             request ? request->serverId : 0xFFFF, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmGetWorldInfoList(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2LeaveRoom(OrbisNpMatching2ContextId ctxId,
                                         OrbisNpMatching2LeaveRoomRequest* request,
                                         OrbisNpMatching2RequestOptParam* requestOpt,
                                         OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmLeaveRoom(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SearchRoom(OrbisNpMatching2ContextId ctxId,
                                          OrbisNpMatching2SearchRoomRequest* request,
                                          OrbisNpMatching2RequestOptParam* requestOpt,
                                          OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmSearchRoom(ctxId, reqId, *request, ctx->a_variant);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetUserInfo(OrbisNpMatching2ContextId ctxId,
                                           OrbisNpMatching2SetUserInfoRequest* request,
                                           OrbisNpMatching2RequestOptParam* requestOpt,
                                           OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmSetUserInfo(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SendRoomMessage(OrbisNpMatching2ContextId ctxId, void* request,
                                               OrbisNpMatching2RequestOptParam* requestOpt,
                                               OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    return MmSendRoomMessage(ctxId, reqId,
                             *static_cast<OrbisNpMatching2SendRoomMessageRequest*>(request));
}

int PS4_SYSV_ABI sceNpMatching2SetRoomDataExternal(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2SetRoomDataExternalRequest* request,
    OrbisNpMatching2RequestOptParam* requestOpt, OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmSetRoomDataExternal(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetRoomDataInternal(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2SetRoomDataInternalRequest* request,
    OrbisNpMatching2RequestOptParam* requestOpt, OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmSetRoomDataInternal(ctxId, reqId, *request);
    return ORBIS_OK;
}

s32 PS4_SYSV_ABI sceNpMatching2SignalingGetConnectionStatus(OrbisNpMatching2ContextId ctxId,
                                                            OrbisNpMatching2RoomId roomId,
                                                            OrbisNpMatching2RoomMemberId memberId,
                                                            s32* connStatus, void* peerAddr,
                                                            u16* peerPort) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, roomId = {}, memberId = {}", ctxId, roomId,
             memberId);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    constexpr s32 kInactive = 0;
    constexpr s32 kPending = 1;
    constexpr s32 kActive = 2;
    // The title polls this once per peer per frame while a room is filling. The handshake thread
    // and the ShadNet reader mutate the same maps.
    std::lock_guard state_lock(Matching2StateMutex());
    const auto room_it = ctx->room_cache.find(roomId);
    const bool in_room = room_it != ctx->room_cache.end() &&
                         room_it->second.members.find(memberId) != room_it->second.members.end();

    if (!in_room) {
        if (connStatus) {
            *connStatus = kInactive;
        }
        LOG_INFO(Lib_NpMatching2, "member={} is not in room={}, status=inactive", memberId, roomId);
        return ORBIS_OK;
    }

    auto peer_it = ctx->peers.find(memberId);
    if (peer_it == ctx->peers.end()) {
        PeerInfo pi{};
        pi.member_id = memberId;
        pi.status = kPending;
        const auto member_it = room_it->second.members.find(memberId);
        if (member_it != room_it->second.members.end()) {
            pi.addr = member_it->second.addr;
            pi.port = member_it->second.port;
            pi.online_id = member_it->second.np_id.handle;
        }
        peer_it = ctx->peers.emplace(memberId, pi).first;
    }

    const s32 status = peer_it->second.status == kActive ? kActive : kPending;
    if (connStatus) {
        *connStatus = status;
    }
    if (peerAddr) {
        *reinterpret_cast<u32*>(peerAddr) = peer_it->second.addr;
    }
    if (peerPort) {
        *peerPort = peer_it->second.port;
    }

    LOG_INFO(Lib_NpMatching2, "member={} is in room={}, status={} addr={:#x}:{}", memberId, roomId,
             status, peer_it->second.addr, Libraries::Net::sceNetNtohs(peer_it->second.port));
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2AbortContextStart(OrbisNpMatching2ContextId ctxId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2DestroyContext(OrbisNpMatching2ContextId ctxId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!ContextManager::Instance().Destroy(ctxId)) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2RegisterLobbyMessageCallback(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2LobbyMessageCallback callback,
    void* userdata) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    ctx->lobby_message_callback = callback;
    ctx->lobby_message_callback_arg = userdata;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2RegisterRoomMessageCallback(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2RoomMessageCallback callback, void* userdata) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, userdata = {}", ctxId, userdata);

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    ctx->room_message_callback = callback;
    ctx->room_message_callback_arg = userdata;
    return ORBIS_OK;
}

// Reports the library's own allocator accounting. shadPS4 satisfies matching2 allocations from
// the host heap, so there is no pool to measure; what the title must get back is the budget it
// handed to Initialize with a usage figure that never exceeds it.
static int GetMemoryInfoCommon(OrbisNpMatching2MemoryInfo* memInfo, u64 total_size,
                               const char* what) {
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!memInfo) {
        LOG_ERROR(Lib_NpMatching2, "memInfo null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    *memInfo = OrbisNpMatching2MemoryInfo{};
    memInfo->totalMemSize = total_size;
    memInfo->curMemUsage = 0;
    memInfo->maxMemUsage = 0;
    LOG_INFO(Lib_NpMatching2, "{}: total={:#x} cur=0 max=0", what, total_size);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetMemoryInfo(OrbisNpMatching2MemoryInfo* memInfo) {
    return GetMemoryInfoCommon(memInfo, g_state.pool_size, "GetMemoryInfo");
}

int PS4_SYSV_ABI sceNpMatching2GetSslMemoryInfo(OrbisNpMatching2MemoryInfo* memInfo) {
    return GetMemoryInfoCommon(memInfo, g_state.ssl_pool_size, "GetSslMemoryInfo");
}

int PS4_SYSV_ABI sceNpMatching2GetRoomMemberIdListLocal(OrbisNpMatching2ContextId ctxId,
                                                        OrbisNpMatching2RoomId roomId,
                                                        OrbisNpMatching2NatType natTypeFilter,
                                                        OrbisNpMatching2RoomMemberId* memberId,
                                                        u32 memberIdNum) {
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (roomId == 0) {
        LOG_ERROR(Lib_NpMatching2, "GetRoomMemberIdListLocal: roomId 0");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ROOM_ID;
    }
    if (memberIdNum != 0 && memberId == nullptr) {
        LOG_ERROR(Lib_NpMatching2, "GetRoomMemberIdListLocal: buffer null with memberIdNum={}",
                  memberIdNum);
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }
    std::lock_guard state_lock(Matching2StateMutex());
    const auto rc_it = ctx->room_cache.find(roomId);
    if (rc_it == ctx->room_cache.end()) {
        LOG_ERROR(Lib_NpMatching2, "GetRoomMemberIdListLocal: room {} not cached", roomId);
        return ORBIS_NP_MATCHING2_ERROR_ROOM_NOT_FOUND;
    }

    // A zero filter means "every member"; otherwise only members whose NAT type is at least as
    // good as the requested one are listed, which is how the caller asks for peers it can
    // actually reach directly.
    u32 written = 0;
    u32 matched = 0;
    for (const auto& [mid, mc] : rc_it->second.members) {
        if (natTypeFilter != 0 && mc.nat_type > natTypeFilter) {
            continue;
        }
        ++matched;
        if (written < memberIdNum) {
            memberId[written++] = mid;
        }
    }
    LOG_INFO(Lib_NpMatching2,
             "GetRoomMemberIdListLocal: ctx={} room={} natFilter={} matched={} written={} cap={}",
             ctxId, roomId, natTypeFilter, matched, written, memberIdNum);
    // The library returns how many ids it stored, not how many members exist, so a caller that
    // sized its buffer from a previous call is not told to grow it forever.
    return static_cast<int>(written);
}

int PS4_SYSV_ABI sceNpMatching2GetRoomPasswordLocal(OrbisNpMatching2ContextId ctxId,
                                                    OrbisNpMatching2RoomId roomId,
                                                    bool* withPassword,
                                                    OrbisNpMatching2SessionPassword* password) {
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!withPassword || !password) {
        LOG_ERROR(Lib_NpMatching2, "GetRoomPasswordLocal: out parameter null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }
    std::lock_guard state_lock(Matching2StateMutex());
    const auto rc_it = ctx->room_cache.find(roomId);
    if (rc_it == ctx->room_cache.end() || !rc_it->second.has_password) {
        *withPassword = false;
        LOG_INFO(Lib_NpMatching2, "GetRoomPasswordLocal: ctx={} room={} no password", ctxId,
                 roomId);
        return ORBIS_OK;
    }
    *withPassword = true;
    *password = rc_it->second.password;
    LOG_INFO(Lib_NpMatching2, "GetRoomPasswordLocal: ctx={} room={} password returned", ctxId,
             roomId);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetSignalingOptParamLocal(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2RoomId roomId,
    OrbisNpMatching2SignalingOptParam* signalingOptParam) {
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!signalingOptParam) {
        LOG_ERROR(Lib_NpMatching2, "GetSignalingOptParamLocal: out parameter null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }
    std::lock_guard state_lock(Matching2StateMutex());
    *signalingOptParam = OrbisNpMatching2SignalingOptParam{};
    const auto rc_it = ctx->room_cache.find(roomId);
    if (rc_it != ctx->room_cache.end() && rc_it->second.has_signaling_opt_param) {
        *signalingOptParam = rc_it->second.signaling_opt_param;
    }
    LOG_INFO(Lib_NpMatching2, "GetSignalingOptParamLocal: ctx={} room={} type={} flag={} hub={}",
             ctxId, roomId, static_cast<u32>(signalingOptParam->type),
             static_cast<u32>(signalingOptParam->flag), signalingOptParam->memberId);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetLobbyInfoList(OrbisNpMatching2ContextId ctxId,
                                                OrbisNpMatching2GetLobbyInfoListRequest* request,
                                                OrbisNpMatching2RequestOptParam* requestOpt,
                                                OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    // libSceNpMatching2 validates the request in this order and leaves requestId optional.
    if (!request) {
        LOG_ERROR(Lib_NpMatching2, "request null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    if (request->worldId == 0) {
        LOG_ERROR(Lib_NpMatching2, "worldId is zero");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_MATCHING_SPACE;
    }
    if (request->rangeFilter.max > ORBIS_NP_MATCHING2_RANGE_FILTER_MAX) {
        LOG_ERROR(Lib_NpMatching2, "rangeFilter.max {} exceeds {}", request->rangeFilter.max,
                  ORBIS_NP_MATCHING2_RANGE_FILTER_MAX);
        return ORBIS_NP_MATCHING2_ERROR_RANGE_FILTER_MAX;
    }
    if (request->rangeFilter.start == 0) {
        LOG_ERROR(Lib_NpMatching2, "rangeFilter.start is zero");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_REQUEST_PARAMETER;
    }
    for (u8 reserved : request->reserved) {
        if (reserved != 0) {
            LOG_ERROR(Lib_NpMatching2, "reserved bytes are not zero");
            return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
        }
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    if (requestId) {
        *requestId = reqId;
    }
    MmGetLobbyInfoList(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetLobbyMemberDataInternal() {
    LOG_INFO(Lib_NpMatching2, "called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetLobbyMemberDataInternalList() {
    LOG_INFO(Lib_NpMatching2, "called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetRoomDataExternalList(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2GetRoomDataExternalListRequest* request,
    OrbisNpMatching2RequestOptParam* requestOpt, OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmGetRoomDataExternalList(ctxId, reqId, *request, ctx->a_variant);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetRoomDataInternal(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2GetRoomDataInternalRequest* request,
    OrbisNpMatching2RequestOptParam* requestOpt, OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    // Lock first so the payload's destructor still runs while it is held. This request is
    // answered locally and the title polls it every frame once a third member is in the room.
    // Built into the context's one shared slot, the next poll's Reset() deleted the member list
    // before Matching2:Dispatch invoked the callback, and the guest faulted on that freed list.
    std::lock_guard state_lock(Matching2StateMutex());
    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;

    ScopedRequestPayload payload(*ctx);
    void* request_data = BuildGetRoomDataInternalPayload(*ctx, request->roomId);

    PendingEvent ev{};
    ev.type = PendingEvent::REQUEST_CB;
    ev.ctx_id = ctxId;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.req_id = reqId;
    ev.req_event = ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_DATA_INTERNAL;
    ev.error_code = 0;
    const RequestCallbackInfo request_cb = ConsumeRequestCallback(ctx);
    ev.request_cb = request_cb.callback;
    ev.request_cb_arg = request_cb.arg;
    ev.request_data = request_data;
    ev.request_payload_owner = payload.Owner();
    ScheduleEvent(std::move(ev));
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetRoomMemberDataExternalList(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2GetRoomMemberDataExternalListRequest* request,
    OrbisNpMatching2RequestOptParam* requestOpt, OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmGetRoomMemberDataExternalList(ctxId, reqId, *request, ctx->a_variant);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetRoomMemberDataInternal(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2GetRoomMemberDataInternalRequest* request,
    OrbisNpMatching2RequestOptParam* requestOpt, OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    // Validation order and error codes mirror libSceNpMatching2's own validator (module offset
    // 0x5BD30): argument, then room id, then member id, then the reserved bytes.
    if (!request) {
        LOG_ERROR(Lib_NpMatching2, "request null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    if (request->roomId == 0) {
        LOG_ERROR(Lib_NpMatching2, "GetRoomMemberDataInternal: roomId 0");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ROOM_ID;
    }
    if (request->memberId < ORBIS_NP_MATCHING2_ROOM_MEMBER_ID_MIN ||
        request->memberId > ORBIS_NP_MATCHING2_ROOM_MEMBER_ID_MAX ||
        (request->memberId & 0xF) == 0) {
        LOG_ERROR(Lib_NpMatching2, "GetRoomMemberDataInternal: invalid memberId {}",
                  request->memberId);
        return ORBIS_NP_MATCHING2_ERROR_INVALID_MEMBER_ID;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    // Same lifetime as GetRoomDataInternal: a later poll must not free the member record this
    // callback still has to read.
    std::lock_guard state_lock(Matching2StateMutex());
    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    if (requestId) {
        *requestId = reqId;
    }

    // Answered from the room cache the 0x1101/0x1102/0x1107 room events keep current, the same
    // way GetRoomDataInternal is: the matching server has no separate per-member read command,
    // and a round trip would only echo state we were already pushed.
    ScopedRequestPayload payload(*ctx);
    void* request_data =
        BuildGetRoomMemberDataInternalPayload(*ctx, request->roomId, request->memberId);
    const s32 error_code = request_data ? 0 : ORBIS_NP_MATCHING2_ERROR_ROOM_MEMBER_NOT_FOUND;
    LOG_INFO(Lib_NpMatching2, "GetRoomMemberDataInternal: ctx={} room={} member={} reqId={} err={}",
             ctxId, request->roomId, request->memberId, reqId, error_code);

    PendingEvent ev{};
    ev.type = PendingEvent::REQUEST_CB;
    ev.ctx_id = ctxId;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.req_id = reqId;
    ev.req_event = ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_MEMBER_DATA_INTERNAL;
    ev.error_code = error_code;
    const RequestCallbackInfo request_cb = ConsumeRequestCallback(ctx);
    ev.request_cb = request_cb.callback;
    ev.request_cb_arg = request_cb.arg;
    ev.request_data = request_data;
    ev.request_payload_owner = payload.Owner();
    ScheduleEvent(std::move(ev));
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetUserInfoListA(OrbisNpMatching2ContextId ctxId,
                                                OrbisNpMatching2GetUserInfoListRequest* request,
                                                OrbisNpMatching2RequestOptParam* requestOpt,
                                                OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmGetUserInfoList(ctxId, reqId, *request, true);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GetUserInfoList(OrbisNpMatching2ContextId ctxId,
                                               OrbisNpMatching2GetUserInfoListRequest* request,
                                               OrbisNpMatching2RequestOptParam* requestOpt,
                                               OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmGetUserInfoList(ctxId, reqId, *request, ctx->a_variant);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2GrantRoomOwner(OrbisNpMatching2ContextId ctxId, void* reqParam,
                                              void* optParam, s32* reqId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2JoinLobby(OrbisNpMatching2ContextId ctxId, void* reqParam,
                                         void* optParam, s32* reqId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2JoinRoomA(OrbisNpMatching2ContextId ctxId,
                                         OrbisNpMatching2JoinRoomRequestA* request,
                                         OrbisNpMatching2RequestOptParam* requestOpt,
                                         OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmJoinRoomA(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2KickoutRoomMember(OrbisNpMatching2ContextId ctxId,
                                                 OrbisNpMatching2KickoutRoomMemberRequest* request,
                                                 OrbisNpMatching2RequestOptParam* requestOpt,
                                                 OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!request || !requestId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    *requestId = reqId;
    MmKickoutRoomMember(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2LeaveLobby() {
    LOG_INFO(Lib_NpMatching2, "called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SendLobbyChatMessage() {
    LOG_INFO(Lib_NpMatching2, "called");
    return ORBIS_OK;
}

// Shared by SendRoomMessage and SendRoomChatMessage: libSceNpMatching2 validates both through the
// same routine (module offset 0x5BF80) and both carry the same request structure.
static int ValidateSendRoomMessageRequest(const OrbisNpMatching2SendRoomMessageRequest* request) {
    if (!request) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    if (request->roomId == 0) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ROOM_ID;
    }
    // The library accepts cast types 1..4. Only 1..3 are named in the SDK headers shadPS4
    // mirrors; 4 exists on the wire and is passed through rather than rejected.
    constexpr OrbisNpMatching2CastType kMaxCastType = 4;
    if (request->castType < ORBIS_NP_MATCHING2_CASTTYPE_BROADCAST ||
        request->castType > kMaxCastType) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CASTTYPE;
    }
    const auto member_id_ok = [](OrbisNpMatching2RoomMemberId id) {
        return id >= ORBIS_NP_MATCHING2_ROOM_MEMBER_ID_MIN &&
               id <= ORBIS_NP_MATCHING2_ROOM_MEMBER_ID_MAX && (id & 0xF) != 0;
    };
    if (request->castType == ORBIS_NP_MATCHING2_CASTTYPE_UNICAST) {
        if (!member_id_ok(request->dst.unicastTarget)) {
            return ORBIS_NP_MATCHING2_ERROR_INVALID_MEMBER_ID;
        }
    } else if (request->castType == ORBIS_NP_MATCHING2_CASTTYPE_MULTICAST) {
        if (!request->dst.multicastTarget.memberId ||
            request->dst.multicastTarget.memberIdNum == 0) {
            return ORBIS_NP_MATCHING2_ERROR_INVALID_MESSAGE_TARGET;
        }
        for (u64 i = 0; i < request->dst.multicastTarget.memberIdNum; ++i) {
            if (!member_id_ok(request->dst.multicastTarget.memberId[i])) {
                return ORBIS_NP_MATCHING2_ERROR_INVALID_MEMBER_ID;
            }
        }
    }
    if (!request->msg || request->msgLen == 0) {
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SendRoomChatMessage(OrbisNpMatching2ContextId ctxId,
                                                   OrbisNpMatching2SendRoomMessageRequest* request,
                                                   OrbisNpMatching2RequestOptParam* requestOpt,
                                                   OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (const int rc = ValidateSendRoomMessageRequest(request); rc != ORBIS_OK) {
        LOG_ERROR(Lib_NpMatching2, "SendRoomChatMessage: invalid request ({:#x})", rc);
        return rc;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    if (requestId) {
        *requestId = reqId;
    }
    MmSendRoomChatMessage(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetLobbyMemberDataInternal() {
    LOG_INFO(Lib_NpMatching2, "called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetRoomMemberDataInternal(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2SetRoomMemberDataInternalRequest* request,
    OrbisNpMatching2RequestOptParam* requestOpt, OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    // Validation order copied from libSceNpMatching2's validator (module offset 0x5BD90).
    if (!request) {
        LOG_ERROR(Lib_NpMatching2, "request null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    if (request->roomId == 0) {
        LOG_ERROR(Lib_NpMatching2, "SetRoomMemberDataInternal: roomId 0");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ROOM_ID;
    }
    const auto* attrs = request->roomMemberBinAttrInternal;
    const u64 attr_num = request->roomMemberBinAttrInternalNum;
    // With no team change requested the call must carry a blob, otherwise it asks for nothing.
    if (request->teamId == 0 && (attrs == nullptr || attr_num == 0)) {
        LOG_ERROR(Lib_NpMatching2, "SetRoomMemberDataInternal: no team id and no attribute");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    if (attrs != nullptr) {
        if (attrs->dataSize > ORBIS_NP_MATCHING2_ROOMMEMBER_BIN_ATTR_INTERNAL_MAX_SIZE) {
            LOG_ERROR(Lib_NpMatching2, "SetRoomMemberDataInternal: attribute too large ({})",
                      attrs->dataSize);
            return ORBIS_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_SIZE;
        }
        if (attrs->id != ORBIS_NP_MATCHING2_ROOMMEMBER_BIN_ATTR_INTERNAL_1_ID) {
            LOG_ERROR(Lib_NpMatching2, "SetRoomMemberDataInternal: invalid attribute id {:#x}",
                      attrs->id);
            return ORBIS_NP_MATCHING2_ERROR_INVALID_ATTRIBUTE_ID;
        }
    }
    // The matching service carries exactly one internal blob per member.
    if (attr_num > 1) {
        LOG_ERROR(Lib_NpMatching2, "SetRoomMemberDataInternal: attribute count {} > 1", attr_num);
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    if (requestId) {
        *requestId = reqId;
    }
    MmSetRoomMemberDataInternal(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SetSignalingOptParam(
    OrbisNpMatching2ContextId ctxId, OrbisNpMatching2SetSignalingOptParamRequest* request,
    OrbisNpMatching2RequestOptParam* requestOpt, OrbisNpMatching2RequestId* requestId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, requestOpt = {}", ctxId, fmt::ptr(requestOpt));

    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    // Validator at module offset 0x5BB80: argument, then room id.
    if (!request) {
        LOG_ERROR(Lib_NpMatching2, "request null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    if (request->roomId == 0) {
        LOG_ERROR(Lib_NpMatching2, "SetSignalingOptParam: roomId 0");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ROOM_ID;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    // Applied locally up front so GetSignalingOptParamLocal answers correctly even before the
    // server acknowledges; the server copy is what later joiners and the other members read.
    // Only an already-cached room is touched - inserting one here would make GetRoomDataInternal
    // start answering for a room this client never joined.
    if (const auto rc_it = ctx->room_cache.find(request->roomId); rc_it != ctx->room_cache.end()) {
        rc_it->second.signaling_opt_param = request->sigOptParam;
        rc_it->second.has_signaling_opt_param = true;
        rc_it->second.signaling_type = request->sigOptParam.type;
        rc_it->second.signaling_main_member = request->sigOptParam.memberId;
        // The server echo of this request arrives later. Peers who joined before the
        // option was known still need a handshake, or their antenna stays on lv_load.
        StartMatching2SignalingForRoomPeers(*ctx, request->roomId);
    } else {
        LOG_WARNING(Lib_NpMatching2, "SetSignalingOptParam: room {} not cached locally",
                    request->roomId);
    }

    StoreRequestCallback(ctx, requestOpt);
    const OrbisNpMatching2RequestId reqId = AllocRequestId();
    if (requestId) {
        *requestId = reqId;
    }
    MmSetSignalingOptParam(ctxId, reqId, *request);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SignalingGetPeerNetInfo() {
    LOG_INFO(Lib_NpMatching2, "called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SignalingGetPingInfo(OrbisNpMatching2ContextId ctxId,
                                                    const void* reqParam, const void* optParam,
                                                    s32* reqId) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}", ctxId);
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (!reqParam || !reqId) {
        LOG_ERROR(Lib_NpMatching2, "request or requestId null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    const auto* request = static_cast<const OrbisNpMatching2SignalingGetPingInfoRequest*>(reqParam);
    std::lock_guard state_lock(Matching2StateMutex());
    StoreRequestCallback(ctx, static_cast<const OrbisNpMatching2RequestOptParam*>(optParam));
    const OrbisNpMatching2RequestId request_id = AllocRequestId();
    *reqId = request_id;

    ScopedRequestPayload payload(*ctx);
    void* request_data = BuildSignalingGetPingInfoPayload(*ctx, request->roomId);

    PendingEvent ev{};
    ev.type = PendingEvent::REQUEST_CB;
    ev.ctx_id = ctxId;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.req_id = request_id;
    ev.req_event = ORBIS_NP_MATCHING2_REQUEST_EVENT_SIGNALING_GET_PING_INFO;
    ev.error_code = ORBIS_OK;
    const RequestCallbackInfo request_cb = ConsumeRequestCallback(ctx);
    ev.request_cb = request_cb.callback;
    ev.request_cb_arg = request_cb.arg;
    ev.request_data = request_data;
    ev.request_payload_owner = payload.Owner();
    ScheduleEvent(std::move(ev));
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SignalingCancelPeerNetInfo() {
    LOG_INFO(Lib_NpMatching2, "called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SignalingGetConnectionInfoA(OrbisNpMatching2ContextId ctxId,
                                                           OrbisNpMatching2RoomId roomId,
                                                           OrbisNpMatching2RoomMemberId memberId,
                                                           u32 infoType, void* connInfo) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, roomId = {}, memberId = {}", ctxId, roomId,
             memberId);
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    return FillMatching2ConnectionInfo(*ctx, roomId, memberId, infoType, connInfo, true);
}

int PS4_SYSV_ABI sceNpMatching2SignalingGetConnectionInfo(OrbisNpMatching2ContextId ctxId,
                                                          OrbisNpMatching2RoomId roomId,
                                                          OrbisNpMatching2RoomMemberId memberId,
                                                          u32 infoType, void* connInfo) {
    LOG_INFO(Lib_NpMatching2, "called, ctxId = {}, roomId = {}, memberId = {}", ctxId, roomId,
             memberId);
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    ContextObject* ctx = ContextManager::Instance().Get(ctxId);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "invalid context id");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    return FillMatching2ConnectionInfo(*ctx, roomId, memberId, infoType, connInfo, false);
}

// The SDK entry point takes only the output block: SceNpMatching2SignalingNetInfo shares the
// layout of NpSignaling::OrbisNpSignalingNetInfo, and there is no context argument.
int PS4_SYSV_ABI
sceNpMatching2SignalingGetLocalNetInfo(NpSignaling::OrbisNpSignalingNetInfo* netInfo) {
    if (!IsInitialized()) {
        LOG_ERROR(Lib_NpMatching2, "not initialized");
        return ORBIS_NP_MATCHING2_ERROR_NOT_INITIALIZED;
    }
    if (netInfo == nullptr || netInfo->size != sizeof(NpSignaling::OrbisNpSignalingNetInfo)) {
        LOG_ERROR(Lib_NpMatching2, "invalid netInfo");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }
    // Titles bind their P2P sockets to localAddr. That must stay a real local NIC (or the
    // explicit bind), never the STUN-mapped address advertised to peers.
    if (!Net::EnsureP2PTransport()) {
        LOG_ERROR(Lib_NpMatching2, "P2P transport unavailable");
        return ORBIS_NP_MATCHING2_SIGNALING_ERROR_NETINFO_NOT_AVAILABLE;
    }
    netInfo->localAddr = Net::GetP2PLocalAddr();

    NetCtl::OrbisNetCtlNatInfo nat_info{};
    nat_info.size = sizeof(nat_info);
    netInfo->mappedAddr = 0;
    netInfo->natStatus = 0;
    if (NetCtl::sceNetCtlGetNatInfo(&nat_info) >= 0) {
        netInfo->mappedAddr = nat_info.mapped_addr;
        netInfo->natStatus = nat_info.nat_type;
    }
    if (netInfo->mappedAddr == 0) {
        const u32 external =
            Common::Singleton<NetUtil::NetUtilInternal>::Instance()->GetExternalIp();
        netInfo->mappedAddr = external != 0 ? external : netInfo->localAddr;
    }
    netInfo->_pad_14 = 0;

    LOG_INFO(Lib_NpMatching2, "localAddr={:#x} mappedAddr={:#x} natStatus={}", netInfo->localAddr,
             netInfo->mappedAddr, netInfo->natStatus);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceNpMatching2SignalingGetPeerNetInfoResult() {
    LOG_INFO(Lib_NpMatching2, "called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("10t3e5+JPnU", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2Initialize);
    LIB_FUNCTION("Mqp3lJ+sjy4", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2Terminate);
    LIB_FUNCTION("YfmpW719rMo", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateContext);
    LIB_FUNCTION("ajvzc8e2upo", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateContextA);
    LIB_FUNCTION("V6KSpKv9XJE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateJoinRoomA);
    LIB_FUNCTION("zCWZmXXN600", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2CreateJoinRoom);
    LIB_FUNCTION("CSIMDsVjs-g", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2JoinRoom);
    LIB_FUNCTION("fQQfP87I7hs", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterContextCallback);
    LIB_FUNCTION("4Nj7u5B5yCA", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterLobbyEventCallback);
    LIB_FUNCTION("p+2EnxmaAMM", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterRoomEventCallback);
    LIB_FUNCTION("0UMeWRGnZKA", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterSignalingCallback);
    LIB_FUNCTION("7vjNQ6Z1op0", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2ContextStart);
    LIB_FUNCTION("-f6M4caNe8k", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2ContextStop);
    LIB_FUNCTION("LhCPctIICxQ", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetServerId);
    LIB_FUNCTION("rJNPJqDCpiI", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetWorldInfoList);
    LIB_FUNCTION("BD6kfx442Do", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2LeaveRoom);
    LIB_FUNCTION("+8e7wXLmjds", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetDefaultRequestOptParam);
    LIB_FUNCTION("VqZX7POg2Mk", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SearchRoom);
    LIB_FUNCTION("Iw2h0Jrrb5U", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SendRoomMessage);
    LIB_FUNCTION("meEjIdbjAA0", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetUserInfo);
    LIB_FUNCTION("q7GK98-nYSE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetRoomDataExternal);
    LIB_FUNCTION("S9D8JSYIrjE", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetRoomDataInternal);
    LIB_FUNCTION("tHD5FPFXtu4", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetConnectionStatus);
    LIB_FUNCTION("pFzhpCMlJXQ", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2AbortContextStart);
    LIB_FUNCTION("Nz-ZE7ur32I", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2DestroyContext);
    LIB_FUNCTION("DnPUsBAe8oI", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterLobbyMessageCallback);
    LIB_FUNCTION("uBESzz4CQws", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2RegisterRoomMessageCallback);
    LIB_FUNCTION("gpSAvdheZ0Q", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetMemoryInfo);
    LIB_FUNCTION("8btynvj0KNA", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetSslMemoryInfo);
    LIB_FUNCTION("KC+GnHzrK2o", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetRoomMemberIdListLocal);
    LIB_FUNCTION("vbtWT3lZBOM", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetRoomPasswordLocal);
    LIB_FUNCTION("cgQhq3E0eGo", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetSignalingOptParamLocal);
    LIB_FUNCTION("wyvlEgZ-55w", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetLobbyInfoList);
    LIB_FUNCTION("1JtbJ0kxm3E", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetLobbyMemberDataInternal);
    LIB_FUNCTION("1Z4Xxumgm+Y", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetLobbyMemberDataInternalList);
    LIB_FUNCTION("26vWrPAWJfM", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetRoomDataExternalList);
    LIB_FUNCTION("Jraxifmoet4", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetRoomDataInternal);
    LIB_FUNCTION("dMQ+xGvTdqM", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetRoomMemberDataExternalList);
    LIB_FUNCTION("5lhvOqheFBA", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetRoomMemberDataInternal);
    LIB_FUNCTION("GyI2f9yDUXM", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetUserInfoListA);
    LIB_FUNCTION("qeF-q5KDtAc", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GetUserInfoList);
    LIB_FUNCTION("NCP3bLGPt+o", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2GrantRoomOwner);
    LIB_FUNCTION("n5JmImxTiZU", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2JoinLobby);
    LIB_FUNCTION("gQ6cUriNpgs", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2JoinRoomA);
    LIB_FUNCTION("AUVfU6byg3c", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2KickoutRoomMember);
    LIB_FUNCTION("BBbJ92uUdCg", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2LeaveLobby);
    LIB_FUNCTION("K+KtxhPsMZ4", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SendLobbyChatMessage);
    LIB_FUNCTION("opDpl74pi2E", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SendRoomChatMessage);
    LIB_FUNCTION("ir2CzSs9K-g", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetLobbyMemberDataInternal);
    LIB_FUNCTION("HoqTrkS9c5Q", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetRoomMemberDataInternal);
    LIB_FUNCTION("ES3UMUWWj9U", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SetSignalingOptParam);
    LIB_FUNCTION("8CqniKDzjvg", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetPeerNetInfo);
    LIB_FUNCTION("wUmwXZHaX1w", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetPingInfo);
    LIB_FUNCTION("GNSN5849fjU", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingCancelPeerNetInfo);
    LIB_FUNCTION("nNeC3F8-g+4", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetConnectionInfoA);
    LIB_FUNCTION("twVupeaYYrk", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetConnectionInfo);
    LIB_FUNCTION("380EWm2DrVg", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetLocalNetInfo);
    LIB_FUNCTION("CTy4PBhpWDw", "libSceNpMatching2", 1, "libSceNpMatching2",
                 sceNpMatching2SignalingGetPeerNetInfoResult);
};

} // namespace Libraries::Np::NpMatching2
