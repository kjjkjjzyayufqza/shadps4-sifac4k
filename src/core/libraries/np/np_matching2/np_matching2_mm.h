// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "core/libraries/np/np_matching2/np_matching2.h"

namespace ShadNet {
class ShadNetClient;
enum class CommandType : u16;
enum class ErrorType : u8;
} // namespace ShadNet

namespace Libraries::Np::NpMatching2 {

enum class MmCommand : u16 {
    ContextStart = 100,
    CreateRoom = 101,
    JoinRoom = 102,
    LeaveRoom = 103,
    SearchRoom = 104,
    RequestSignalingInfos = 105,
    ContextStop = 106,
    SetUserInfo = 107,
    SetRoomDataInternal = 108,
    SetRoomDataExternal = 109,
    KickoutRoomMember = 110,
    GetWorldInfoList = 111,
    GetRoomDataExternalList = 112,
    GetUserInfoList = 113,
    GetRoomMemberDataExternalList = 114,
    SendRoomMessage = 115,
    GetLobbyInfoList = 116,
};

void SetMmShadNetClient(std::shared_ptr<ShadNet::ShadNetClient> client,
                        std::string_view server_host, u16 tcp_port);
// Tears the matching backend down once its last client is gone. Requests still waiting for a
// reply are completed with `error_code`, and started contexts are reported stopped with `cause`:
// the server session they belonged to no longer exists.
void ClearMmShadNetClient(OrbisNpMatching2EventCause cause, s32 error_code);
bool IsMmClientRunning();

// Completes any tracked matching request whose reply never arrived, so the title's callback
// still fires. Driven by the NP handler's worker thread.
void ExpireMatchingRequests();

void OnMatchingReply(ShadNet::CommandType cmd, u64 pkt_id, ShadNet::ErrorType error,
                     const std::vector<u8>& body);

void MmContextStart(OrbisNpMatching2ContextId ctx_id);
s32 MmContextStop(OrbisNpMatching2ContextId ctx_id);

s32 MmSubmitRequest(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                    OrbisNpMatching2Event req_event, MmCommand cmd, const std::vector<u8>& payload,
                    bool a_variant = false);

s32 MmCreateJoinRoom(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                     const OrbisNpMatching2CreateJoinRoomRequest& request);
s32 MmCreateJoinRoomA(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                      const OrbisNpMatching2CreateJoinRoomRequestA& request);
s32 MmJoinRoom(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
               const OrbisNpMatching2JoinRoomRequest& request);
s32 MmJoinRoomA(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                const OrbisNpMatching2JoinRoomRequestA& request);
s32 MmLeaveRoom(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                const OrbisNpMatching2LeaveRoomRequest& request);
s32 MmGetWorldInfoList(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                       const OrbisNpMatching2GetWorldInfoListRequest& request);
s32 MmGetLobbyInfoList(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                       const OrbisNpMatching2GetLobbyInfoListRequest& request);
s32 MmSearchRoom(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                 const OrbisNpMatching2SearchRoomRequest& request, bool a_variant = false);
s32 MmGetRoomDataExternalList(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                              const OrbisNpMatching2GetRoomDataExternalListRequest& request,
                              bool a_variant = false);
s32 MmGetRoomMemberDataExternalList(
    OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
    const OrbisNpMatching2GetRoomMemberDataExternalListRequest& request, bool a_variant = false);
s32 MmGetUserInfoList(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                      const OrbisNpMatching2GetUserInfoListRequest& request,
                      bool a_variant = false);
s32 MmSetUserInfo(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                  const OrbisNpMatching2SetUserInfoRequest& request);
s32 MmSendRoomMessage(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                      const OrbisNpMatching2SendRoomMessageRequest& request);
s32 MmSetRoomDataInternal(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                          const OrbisNpMatching2SetRoomDataInternalRequest& request);
s32 MmSetRoomDataExternal(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                          const OrbisNpMatching2SetRoomDataExternalRequest& request);
s32 MmKickoutRoomMember(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                        const OrbisNpMatching2KickoutRoomMemberRequest& request);

u32 GetMmServerAddr();
u16 GetMmServerUdpPort();

/// Asks the matching server for `target_online_id`'s STUN-observed endpoint. Returns true only
/// when the query was actually submitted; it is throttled per target. Never blocks: the reply is
/// handled on the ShadNet reader thread and lands in the cache below.
bool RequestSignalingInfoAsync(std::string_view target_online_id);

/// Reads a cached endpoint without touching the network. Returns false when nothing usable is
/// cached. `out_nat_type` is optional.
bool LookupSignalingInfo(std::string_view target_online_id, u32* out_addr, u16* out_port,
                         u32* out_nat_type);

/// Cache lookup plus a background refresh: returns true with the endpoint when one is known, and
/// otherwise schedules a query and returns false so the caller retries later. This is the only
/// resolver callers on the ShadNet reader thread may use - a blocking query there would wait on
/// a reply that only that same thread can deliver.
bool ResolveSignalingInfo(std::string_view target_online_id, u32* out_addr, u16* out_port);

/// True when the connected server forwards P2P datagrams for peers that cannot punch directly.
bool IsSignalingRelayEnabled();
/// Second STUN port advertised by the server for NAT classification, host order; 0 when absent.
u16 GetStunAltPort();

} // namespace Libraries::Np::NpMatching2
