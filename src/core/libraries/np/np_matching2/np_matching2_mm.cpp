// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <fmt/format.h>

#include "common/logging/log.h"
#include "core/libraries/net/net.h"
#include "core/libraries/net/net_p2p.h"
#include "core/libraries/net/net_upnp.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_handler/np_handler.h"
#include "core/libraries/np/np_matching2/np_matching2_internal.h"
#include "core/libraries/np/np_matching2/np_matching2_mm.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_request_timeout.h"
#include "core/libraries/np/np_signaling/np_signaling_transport.h"
#include "core/libraries/np/signaling_handler.h"
#include "shadnet.pb.h"
#include "shadnet/client.h"

namespace Libraries::Np::NpMatching2 {

void StartMatching2HandshakeThread();
void StopMatching2HandshakeThread();

namespace {

struct PendingRequest {
    OrbisNpMatching2ContextId ctx_id = 0;
    OrbisNpMatching2RequestId req_id = 0;
    OrbisNpMatching2Event req_event{};
    bool a_variant = false;
    OrbisNpMatching2RequestCallback request_cb = nullptr;
    void* request_cb_arg = nullptr;
    // When the expiry sweep gives up on this request and completes it with a timeout.
    std::chrono::steady_clock::time_point deadline{};
};

// A STUN-derived endpoint stays usable for this long; past it the peer may sit behind a new
// NAT binding and the cached value would send the handshake into a black hole.
constexpr auto kSignalingInfoTtl = std::chrono::seconds(60);
// Older than this and the entry is refreshed in the background while still being served, so a
// long-lived room never falls off the cliff above.
constexpr auto kSignalingInfoRefresh = std::chrono::seconds(15);
// One query per target per interval. Without it an unresolved peer turns the handshake retry
// loop into a request flood against the matching server.
constexpr auto kSignalingInfoQueryInterval = std::chrono::milliseconds(1000);

// Endpoint the matching server last reported for one online id.
struct SignalingInfo {
    u32 addr = 0; // network byte order
    u16 port = 0; // network byte order
    u32 nat_type = 0;
    std::chrono::steady_clock::time_point resolved_at{};
};

struct MmClientState {
    std::shared_ptr<ShadNet::ShadNetClient> client;
    std::mutex mutex;
    u32 server_addr = 0;
    u16 server_udp_port = 0;
    bool matching2_enabled = false;
    bool relay_enabled = false;
    u16 stun_alt_port = 0;

    std::mutex pending_mutex;
    std::map<u64, PendingRequest> pending;

    // Signaling-endpoint lookups are resolved asynchronously: the reply arrives on the ShadNet
    // reader thread, which is also the thread that runs the room callbacks asking for an
    // endpoint. A blocking query there would wait on a packet only it can deliver.
    std::mutex sig_mutex;
    std::map<std::string, SignalingInfo> sig_cache;
    std::map<std::string, std::chrono::steady_clock::time_point> sig_last_request;
    struct SignalingQuery {
        std::string target;
        std::chrono::steady_clock::time_point deadline;
    };
    std::map<u64, SignalingQuery> sig_inflight;
};

MmClientState g_mm;

u32 IpStringToAddr(std::string_view ip) {
    u32 a, b, c, d;
    if (std::sscanf(std::string(ip).c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0;
    }
    return a | (b << 8) | (c << 16) | (d << 24);
}

bool IsMatching2BackendDisabled() {
    std::lock_guard lock(g_mm.mutex);
    return !g_mm.matching2_enabled;
}

bool IsContextLifecycleEvent(OrbisNpMatching2Event event) {
    return event == ORBIS_NP_MATCHING2_CONTEXT_EVENT_STARTED ||
           event == ORBIS_NP_MATCHING2_CONTEXT_EVENT_STOPPED;
}

template <typename T>
std::vector<u8> MakeProtoPayload(const T& msg) {
    std::string bytes;
    (void)msg.SerializeToString(&bytes);
    const u32 len = static_cast<u32>(bytes.size());
    std::vector<u8> out;
    out.reserve(4 + bytes.size());
    out.push_back(static_cast<u8>(len));
    out.push_back(static_cast<u8>(len >> 8));
    out.push_back(static_cast<u8>(len >> 16));
    out.push_back(static_cast<u8>(len >> 24));
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

std::string OnlineIdToString(const Libraries::Np::OrbisNpOnlineId& online_id) {
    char buf[ORBIS_NP_ONLINEID_MAX_LENGTH + 1]{};
    std::memcpy(buf, online_id.data, ORBIS_NP_ONLINEID_MAX_LENGTH);
    return std::string(buf);
}

std::string HexPreview(const void* data, u32 size, u32 max_bytes = 96) {
    if (!data || size == 0) {
        return {};
    }

    const auto* bytes = static_cast<const u8*>(data);
    const u32 count = std::min(size, max_bytes);
    std::string out;
    out.reserve(count * 3 + 16);
    char byte_text[4]{};
    for (u32 i = 0; i < count; ++i) {
        std::snprintf(byte_text, sizeof(byte_text), "%02x", bytes[i]);
        if (!out.empty()) {
            out.push_back(' ');
        }
        out += byte_text;
    }
    if (count < size) {
        out += " ...";
    }
    return out;
}

void LogBinAttr(std::string_view prefix, u64 index, const OrbisNpMatching2BinAttr& attr) {
    LOG_INFO(Lib_NpMatching2, "{}[{}]: id={:#x} size={} data={}", prefix, index, attr.id,
             attr.dataSize, HexPreview(attr.data, attr.dataSize));
}

void LogIntAttr(std::string_view prefix, u64 index, const OrbisNpMatching2IntAttr& attr) {
    LOG_INFO(Lib_NpMatching2, "{}[{}]: id={:#x} value={}", prefix, index, attr.id, attr.num);
}

void AppendBinAttr(shadnet::MatchingBinAttr* dst, const OrbisNpMatching2BinAttr& src) {
    dst->set_attr_id(src.id);
    if (src.data && src.dataSize > 0) {
        dst->set_data(src.data, src.dataSize);
    }
}

void AppendCreateRoomBinAttrById(shadnet::CreateRoomRequest& req,
                                 const OrbisNpMatching2BinAttr& src, std::string_view source) {
    switch (src.id) {
    case ORBIS_NP_MATCHING2_ROOM_BIN_ATTR_INTERNAL_1_ID:
    case ORBIS_NP_MATCHING2_ROOM_BIN_ATTR_INTERNAL_2_ID:
        AppendBinAttr(req.add_internal_bin_attrs(), src);
        LOG_INFO(Lib_NpMatching2,
                 "CreateJoinRoom: routed bin attr id={:#x} from {} to internal size={}", src.id,
                 source, src.dataSize);
        break;
    case ORBIS_NP_MATCHING2_ROOM_BIN_ATTR_EXTERNAL_1_ID:
    case ORBIS_NP_MATCHING2_ROOM_BIN_ATTR_EXTERNAL_2_ID:
        AppendBinAttr(req.add_external_bin_attrs(), src);
        LOG_INFO(Lib_NpMatching2,
                 "CreateJoinRoom: routed bin attr id={:#x} from {} to external size={}", src.id,
                 source, src.dataSize);
        break;
    case ORBIS_NP_MATCHING2_ROOM_SEARCHABLE_BIN_ATTR_EXTERNAL_1_ID:
        AppendBinAttr(req.add_external_search_bin_attrs(), src);
        LOG_INFO(Lib_NpMatching2,
                 "CreateJoinRoom: routed bin attr id={:#x} from {} to search external size={}",
                 src.id, source, src.dataSize);
        break;
    default:
        LOG_WARNING(Lib_NpMatching2, "CreateJoinRoom: unexpected bin attr id={:#x} from {} size={}",
                    src.id, source, src.dataSize);
        break;
    }
}

void AppendIntAttr(shadnet::MatchingIntAttr* dst, const OrbisNpMatching2IntAttr& src) {
    dst->set_attr_id(src.id);
    dst->set_attr_value(src.num);
}

// The room password has to travel with the request: the server compares the joiner's bytes with
// the owner's, and a request that only says "a password was given" leaves the room open.
template <typename Request>
void SetRoomPassword(Request& req, const OrbisNpMatching2SessionPassword* password) {
    req.set_room_password_present(password != nullptr);
    if (password != nullptr) {
        req.set_room_password(password->data, ORBIS_NP_MATCHING2_SESSION_PASSWORD_SIZE);
    }
}

// Remembers the password used for the create/join now in flight. The matching service treats it
// as write-only, so this copy is the only thing GetRoomPasswordLocal can answer with once the
// room exists.
void StashPendingRoomPassword(OrbisNpMatching2ContextId ctx_id, const void* password) {
    auto ctx = NpHandler::GetInstance().GetMatching2ContextManager().Get(ctx_id);
    if (!ctx) {
        return;
    }
    ctx->pending_room_password_set = password != nullptr;
    ctx->pending_room_password =
        password != nullptr ? *static_cast<const OrbisNpMatching2SessionPassword*>(password)
                            : OrbisNpMatching2SessionPassword{};
}

// Room searches only match rooms created in the same world with matching searchable
// attributes, so both sides of a failed match have to be visible in the log.
void LogCreateRoomRequest(const shadnet::CreateRoomRequest& req) {
    LOG_INFO(Lib_NpMatching2,
             "MmCreateJoinRoom: world={} lobby={} maxSlot={} flags={:#x} groups={} intAttrN={} "
             "binAttrN={} extBinN={} memberBinN={}",
             req.world_id(), req.lobby_id(), req.max_slots(), req.flags(), req.group_config_count(),
             req.external_search_int_attrs_size(), req.external_search_bin_attrs_size(),
             req.external_bin_attrs_size(), req.member_bin_attrs_size());
    for (const auto& attr : req.external_search_int_attrs()) {
        LOG_INFO(Lib_NpMatching2, "  searchIntAttr id={:#x} num={}", attr.attr_id(),
                 attr.attr_value());
    }
    for (const auto& attr : req.external_search_bin_attrs()) {
        LOG_INFO(Lib_NpMatching2, "  searchBinAttr id={:#x} size={}", attr.attr_id(),
                 attr.data().size());
    }
}

void LogJoinRoomRequest(const shadnet::JoinRoomRequest& req) {
    LOG_INFO(Lib_NpMatching2, "MmJoinRoom: room={} team={} flags={:#x} memberBinN={}",
             req.room_id(), req.team_id(), req.join_flags(), req.member_bin_attrs_size());
}

void AppendRoomGroupConfig(shadnet::MatchingRoomGroupConfig* dst,
                           const OrbisNpMatching2RoomGroupConfig& src) {
    dst->set_slot_count(src.slots);
    dst->set_has_label(src.hasLabel);
    dst->set_has_passwd(src.hasPassword);
    if (src.hasLabel) {
        dst->set_label(src.label.data, sizeof(src.label.data));
    }
}

void AppendCreateJoinRoomCommon(shadnet::CreateRoomRequest& req, const void* room_passwd,
                                const OrbisNpMatching2RoomPasswordSlotMask* passwd_slot_mask,
                                const void* group_config, u64 group_configs,
                                const void* join_group_label) {
    if (room_passwd) {
        const auto* password = static_cast<const OrbisNpMatching2SessionPassword*>(room_passwd);
        req.set_room_password(password->data, sizeof(password->data));
    }
    if (passwd_slot_mask) {
        req.set_has_passwd_slot_mask(true);
        req.set_passwd_slot_mask(*passwd_slot_mask);
    }
    if (group_config) {
        const auto* groups = static_cast<const OrbisNpMatching2RoomGroupConfig*>(group_config);
        for (u64 i = 0; i < group_configs; ++i) {
            AppendRoomGroupConfig(req.add_group_configs(), groups[i]);
        }
    }
    if (join_group_label) {
        const auto* label = static_cast<const OrbisNpMatching2GroupLabel*>(join_group_label);
        req.set_join_group_label(label->data, sizeof(label->data));
    }
}

std::string ExtractProtoBytes(const std::vector<u8>& payload, size_t offset = 0) {
    if (payload.size() < offset + 4) {
        return {};
    }
    const u32 len = static_cast<u32>(payload[offset]) |
                    (static_cast<u32>(payload[offset + 1]) << 8) |
                    (static_cast<u32>(payload[offset + 2]) << 16) |
                    (static_cast<u32>(payload[offset + 3]) << 24);
    if (payload.size() < offset + 4 + len) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(payload.data() + offset + 4), len);
}

// A start the matching service could not complete returns the context to stopped and tells the
// title to start it over, which is how the library reports a failed start. Leaving the context
// marked started would make every retry fail with CONTEXT_ALREADY_STARTED.
void FailContextStart(OrbisNpMatching2ContextId ctx_id, s32 error_code) {
    if (!NpHandler::GetInstance().GetMatching2ContextManager().AbortStart(ctx_id)) {
        return;
    }
    PendingEvent ev{};
    ev.type = PendingEvent::CONTEXT_CB;
    ev.ctx_id = ctx_id;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.ctx_event = ORBIS_NP_MATCHING2_CONTEXT_EVENT_START_OVER;
    ev.ctx_event_cause = ORBIS_NP_MATCHING2_EVENT_CAUSE_CONTEXT_ERROR;
    ev.error_code = error_code;
    ScheduleEvent(std::move(ev));
}

// A request whose reply can no longer arrive must still complete: the title is waiting on this
// callback and has no other way to learn the request is dead.
void DispatchRequestFailure(const PendingRequest& pr, s32 error_code) {
    Matching2StateLock state_lock{"DispatchRequestFailure"};
    if (NpHandler::GetInstance().GetMatching2ContextManager().Get(pr.ctx_id) == nullptr) {
        return;
    }
    if (pr.req_event == ORBIS_NP_MATCHING2_CONTEXT_EVENT_STARTED) {
        FailContextStart(pr.ctx_id, error_code);
        return;
    }

    PendingEvent ev{};
    ev.ctx_id = pr.ctx_id;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.error_code = error_code;
    if (pr.req_event == ORBIS_NP_MATCHING2_CONTEXT_EVENT_STOPPED) {
        ev.type = PendingEvent::CONTEXT_CB;
        ev.ctx_event = pr.req_event;
        ev.ctx_event_cause = ORBIS_NP_MATCHING2_EVENT_CAUSE_CONTEXT_ACTION;
    } else {
        ev.type = PendingEvent::REQUEST_CB;
        ev.req_id = pr.req_id;
        ev.req_event = pr.req_event;
        ev.request_cb = pr.request_cb;
        ev.request_cb_arg = pr.request_cb_arg;
    }
    ScheduleEvent(std::move(ev));
}

void DispatchRequestComplete(const PendingRequest& pr, ShadNet::ErrorType error,
                             const std::vector<u8>& body) {
    Matching2StateLock state_lock{"DispatchRequestComplete"};
    auto ctx = NpHandler::GetInstance().GetMatching2ContextManager().Get(pr.ctx_id);
    if (!ctx) {
        return;
    }

    const s32 error_code = (error == ShadNet::ErrorType::NoError) ? 0 : static_cast<s32>(error);

    if (pr.req_event == ORBIS_NP_MATCHING2_CONTEXT_EVENT_STARTED && error_code != 0) {
        FailContextStart(pr.ctx_id, error_code);
        return;
    }
    if (pr.req_event == ORBIS_NP_MATCHING2_CONTEXT_EVENT_STARTED ||
        pr.req_event == ORBIS_NP_MATCHING2_CONTEXT_EVENT_STOPPED) {
        PendingEvent ev{};
        ev.type = PendingEvent::CONTEXT_CB;
        ev.ctx_id = pr.ctx_id;
        ev.context_owner = ctx;
        ev.fire_at = std::chrono::steady_clock::now();
        ev.ctx_event = pr.req_event;
        ev.ctx_event_cause = ORBIS_NP_MATCHING2_EVENT_CAUSE_CONTEXT_ACTION;
        ev.error_code = error_code;
        ScheduleEvent(std::move(ev));
        return;
    }

    void* request_data = nullptr;
    std::shared_ptr<CallbackPayload> request_payload_owner;

    if (error_code == 0) {
        request_payload_owner = std::make_shared<CallbackPayload>();
        auto& payload = *request_payload_owner;
        const std::string proto = ExtractProtoBytes(body);
        if (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM ||
            pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM_A) {
            shadnet::CreateRoomReply reply;
            if (reply.ParseFromString(proto) && reply.has_details()) {
                request_data = pr.a_variant
                                   ? BuildCreateJoinRoomPayloadA(*ctx, payload, reply.details())
                                   : BuildCreateJoinRoomPayload(*ctx, payload, reply.details());
            }
        } else if (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_JOIN_ROOM ||
                   pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_JOIN_ROOM_A) {
            shadnet::JoinRoomReply reply;
            if (reply.ParseFromString(proto) && reply.has_details()) {
                request_data = pr.a_variant
                                   ? BuildCreateJoinRoomPayloadA(*ctx, payload, reply.details())
                                   : BuildCreateJoinRoomPayload(*ctx, payload, reply.details());
            }
        } else if (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_LEAVE_ROOM) {
            shadnet::LeaveRoomReply reply;
            if (reply.ParseFromString(proto)) {
                QueueMatching2DeadForRoomPeers(
                    *ctx, static_cast<OrbisNpMatching2RoomId>(reply.room_id()),
                    ORBIS_NP_MATCHING2_SIGNALING_ERROR_TERMINATED_BY_MYSELF);
                request_data = BuildLeaveRoomPayload(*ctx, payload, reply);
            }
        } else if (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_WORLD_INFO_LIST) {
            shadnet::GetWorldInfoListReply reply;
            if (reply.ParseFromString(proto)) {
                request_data = BuildGetWorldInfoListPayload(*ctx, payload, reply);
            }
        } else if (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_LOBBY_INFO_LIST) {
            shadnet::GetLobbyInfoListReply reply;
            if (reply.ParseFromString(proto)) {
                request_data = BuildGetLobbyInfoListPayload(*ctx, payload, reply);
            }
        } else if (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_SEARCH_ROOM ||
                   pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_SEARCH_ROOM_A) {
            shadnet::SearchRoomReply reply;
            if (reply.ParseFromString(proto)) {
                LOG_INFO(Lib_NpMatching2,
                         "SearchRoom reply: ctx={} reqId={} rangeStart={} total={} returned={}",
                         pr.ctx_id, pr.req_id, reply.range_start(), reply.range_total(),
                         reply.rooms_size());
                for (const auto& room : reply.rooms()) {
                    LOG_INFO(Lib_NpMatching2,
                             "  room={} world={} lobby={} members={}/{} flags={:#x}",
                             room.room_id(), room.world_id(), room.lobby_id(), room.cur_members(),
                             room.max_slot(), room.flags());
                }
                request_data = pr.a_variant ? BuildSearchRoomPayloadA(*ctx, payload, reply)
                                            : BuildSearchRoomPayload(*ctx, payload, reply);
            }
        } else if (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_DATA_EXTERNAL_LIST ||
                   pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_DATA_EXTERNAL_LIST_A) {
            shadnet::GetRoomDataExternalListReply reply;
            if (reply.ParseFromString(proto)) {
                request_data = pr.a_variant
                                   ? BuildGetRoomDataExternalListPayloadA(*ctx, payload, reply)
                                   : BuildGetRoomDataExternalListPayload(*ctx, payload, reply);
            }
        } else if (pr.req_event ==
                       ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_MEMBER_DATA_EXTERNAL_LIST ||
                   pr.req_event ==
                       ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_MEMBER_DATA_EXTERNAL_LIST_A) {
            shadnet::GetRoomMemberDataExternalListReply reply;
            if (reply.ParseFromString(proto)) {
                request_data =
                    pr.a_variant ? BuildGetRoomMemberDataExternalListPayloadA(*ctx, payload, reply)
                                 : BuildGetRoomMemberDataExternalListPayload(*ctx, payload, reply);
            }
        } else if (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_USER_INFO_LIST) {
            shadnet::GetUserInfoListReply reply;
            if (reply.ParseFromString(proto)) {
                request_data = pr.a_variant ? BuildGetUserInfoListPayloadA(*ctx, payload, reply)
                                            : BuildGetUserInfoListPayload(*ctx, payload, reply);
            }
        }
    }

    PendingEvent ev{};
    ev.type = PendingEvent::REQUEST_CB;
    ev.ctx_id = pr.ctx_id;
    ev.context_owner = ctx;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.req_id = pr.req_id;
    ev.req_event = pr.req_event;
    ev.error_code = error_code;
    ev.request_cb = pr.request_cb;
    ev.request_cb_arg = pr.request_cb_arg;
    ev.request_data = request_data;
    ev.payload_owner = std::move(request_payload_owner);
    ScheduleEvent(std::move(ev));

    if (error_code == 0 && (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_JOIN_ROOM ||
                            pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_JOIN_ROOM_A)) {
        SignalingHandler::StartMatching2(*ctx);
    } else if (error_code == 0 &&
               (pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM ||
                pr.req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM_A)) {
        SignalingHandler::StartMatching2(*ctx);
    }
}

void AppendEventMemberBinAttrs(CallbackPayload& p, const MemberCache& mc) {
    p.member_bin_attrs.resize(mc.bins.size());
    p.bin_buffers.reserve(mc.bins.size());
    size_t i = 0;
    for (const auto& [attr_id, bin] : mc.bins) {
        p.bin_buffers.emplace_back(bin.data.begin(), bin.data.end());
        auto& buf = p.bin_buffers.back();
        auto& dst = p.member_bin_attrs[i++];
        dst = OrbisNpMatching2RoomMemberBinAttrInternal{};
        dst.lastUpdate.tick = bin.update_date;
        dst.binAttr.id = bin.id;
        dst.binAttr.data = buf.empty() ? nullptr : buf.data();
        dst.binAttr.dataSize = buf.size();
    }
}

void BuildMemberData(CallbackPayload& p, const RoomCache& rc, const MemberCache& mc,
                     bool a_variant) {
    if (mc.group_id != 0) {
        auto grp_it = rc.groups.find(mc.group_id);
        if (grp_it != rc.groups.end()) {
            p.room_groups.resize(1);
            p.room_groups[0] = grp_it->second;
        }
    }

    AppendEventMemberBinAttrs(p, mc);

    if (a_variant) {
        p.event_member_a = std::make_unique<OrbisNpMatching2RoomMemberDataInternalA>();
        OrbisNpMatching2RoomMemberDataInternalA& m = *p.event_member_a;
        m = OrbisNpMatching2RoomMemberDataInternalA{};
        m.memberId = mc.member_id;
        m.teamId = mc.team_id;
        m.natType = mc.nat_type;
        m.flags = mc.flag_attr;
        m.joinDateTicks.tick = mc.join_date;
        m.user.accountId = mc.account_id;
        m.user.platform = mc.platform;
        m.onlineId = mc.np_id.handle;
        m.roomGroup = p.room_groups.empty() ? nullptr : p.room_groups.data();
        m.roomMemberInternalBinAttr =
            p.member_bin_attrs.empty() ? nullptr : p.member_bin_attrs.data();
        m.roomMemberInternalBinAttrs = p.member_bin_attrs.size();

        return;
    }

    p.event_member = std::make_unique<OrbisNpMatching2RoomMemberDataInternal>();
    OrbisNpMatching2RoomMemberDataInternal& m = *p.event_member;
    m = OrbisNpMatching2RoomMemberDataInternal{};
    m.memberId = mc.member_id;
    m.teamId = mc.team_id;
    m.natType = mc.nat_type;
    m.flagAttr = mc.flag_attr;
    m.joinDate = mc.join_date;
    m.npId = mc.np_id;
    m.roomGroup = p.room_groups.empty() ? nullptr : p.room_groups.data();
    m.roomMemberBinAttrInternal = p.member_bin_attrs.empty() ? nullptr : p.member_bin_attrs.data();
    m.roomMemberBinAttrInternalNum = p.member_bin_attrs.size();
}

void BuildMemberUpdate(CallbackPayload& p, const RoomCache& rc, const MemberCache& mc,
                       OrbisNpMatching2EventCause cause, bool a_variant) {
    BuildMemberData(p, rc, mc, a_variant);

    if (a_variant) {
        p.room_member_update_a = std::make_unique<OrbisNpMatching2RoomMemberUpdateA>();
        auto& u = *p.room_member_update_a;
        u = {};
        u.roomMemberDataInternal = p.event_member_a.get();
        u.eventCause = cause;
        p.room_event_data = p.room_member_update_a.get();
        return;
    }

    p.room_member_update = std::make_unique<OrbisNpMatching2RoomMemberUpdate>();
    auto& u = *p.room_member_update;
    u = {};
    u.roomMemberDataInternal = p.event_member.get();
    u.eventCause = cause;
    p.room_event_data = p.room_member_update.get();
}

// Destroyed after Matching2StateLock when declared first. Room events used to log while the
// state mutex was still held, so a guest poll waited on the synchronous write.
struct LogAfterUnlock {
    std::string info;
    std::string warning;
    ~LogAfterUnlock() {
        if (!warning.empty()) {
            LOG_WARNING(Lib_NpMatching2, "{}", warning);
        }
        if (!info.empty()) {
            LOG_INFO(Lib_NpMatching2, "{}", info);
        }
    }
};

void HandleRoomEvent(const ShadNet::NotifyRoomEvent& n) {
    LogAfterUnlock after_unlock;
    Matching2StateLock state_lock{"HandleRoomEvent"};
    auto ctx = NpHandler::GetInstance().GetMatching2ContextManager().Get(
        static_cast<OrbisNpMatching2ContextId>(n.ctx_id));
    if (!ctx) {
        return;
    }
    auto cache = NpHandler::GetInstance().LockMatching2Cache(ctx->ctx_id);
    const auto event = static_cast<OrbisNpMatching2Event>(n.event);
    const auto cause = static_cast<OrbisNpMatching2EventCause>(n.event_cause);
    const auto room_id = static_cast<OrbisNpMatching2RoomId>(n.room_id);
    const auto member_id = static_cast<OrbisNpMatching2RoomMemberId>(n.member_id);

    auto payload_owner = std::make_shared<CallbackPayload>();
    CallbackPayload& p = *payload_owner;

    switch (event) {
    case ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_JOINED: {
        auto room_it = cache->rooms.find(room_id);
        if (room_it == cache->rooms.end()) {
            return;
        }
        const bool first_seen =
            room_it->second.members.find(member_id) == room_it->second.members.end();
        MemberCache& mc = room_it->second.members[member_id];
        mc = MemberCache{};
        mc.member_id = member_id;
        mc.team_id = static_cast<OrbisNpMatching2TeamId>(n.member_team_id);
        mc.nat_type = static_cast<OrbisNpMatching2NatType>(n.member_nat_type);
        mc.flag_attr = n.member_flag_attr;
        mc.group_id = static_cast<OrbisNpMatching2RoomGroupId>(n.member_group_id);
        if (first_seen && mc.group_id != 0) {
            auto grp_it = room_it->second.groups.find(mc.group_id);
            if (grp_it != room_it->second.groups.end()) {
                ++grp_it->second.groupMembers;
            }
        }
        mc.join_date = n.member_join_date;
        mc.addr = IpStringToAddr(n.member_addr);
        mc.port = Libraries::Net::sceNetHtons(static_cast<u16>(n.member_port));
        SetNpId(mc.np_id, n.member_npid);
        mc.account_id = static_cast<Libraries::Np::OrbisNpAccountId>(n.member_account_id);
        mc.platform = static_cast<Libraries::Np::OrbisNpPlatformType>(n.member_platform);
        for (const auto& a : n.member_bin_attrs) {
            MemberBinCache& b = mc.bins[a.attr_id];
            b.id = static_cast<OrbisNpMatching2AttributeId>(a.attr_id);
            b.update_date = a.update_date;
            b.data = a.data;
        }

        // Keep a record that is already there. A room of three or more delivers this event again
        // for a member whose connection is already up, and replacing the record restarts the
        // handshake from zero. Endpoints are filled only into an empty record: a live one holds
        // the address the handshake actually succeeded on.
        const auto existing_peer = cache->peers.find(member_id);
        if (existing_peer == cache->peers.end()) {
            PeerInfo pi{};
            pi.member_id = member_id;
            pi.addr = mc.addr;
            pi.port = mc.port;
            SetNpOnlineId(pi.online_id, n.member_npid);
            cache->peers.emplace(member_id, pi);
        } else {
            if (existing_peer->second.addr == 0) {
                existing_peer->second.addr = mc.addr;
            }
            if (existing_peer->second.port == 0) {
                existing_peer->second.port = mc.port;
            }
            SetNpOnlineId(existing_peer->second.online_id, n.member_npid);
        }
        // The host used to wait for the joiner's offer. Punching from this side as well is
        // what gets an echo RTT into the room antenna for a member who was not here when
        // we created or joined.
        StartMatching2PeerHandshake(*ctx, room_id, member_id);

        BuildMemberUpdate(p, room_it->second, mc, cause, ctx->a_variant);
        break;
    }
    case ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_LEFT: {
        auto room_it = cache->rooms.find(room_id);
        if (room_it == cache->rooms.end()) {
            return;
        }
        auto mem_it = room_it->second.members.find(member_id);
        if (mem_it == room_it->second.members.end()) {
            return;
        }
        // Drop the seat before the callback copies the group. The leave payload is what the
        // title reads to decide the team still has room; leaving the old count in place makes a
        // 2v2 look full after someone has already gone.
        if (mem_it->second.group_id != 0) {
            auto grp_it = room_it->second.groups.find(mem_it->second.group_id);
            if (grp_it != room_it->second.groups.end() && grp_it->second.groupMembers > 0) {
                --grp_it->second.groupMembers;
            }
        }
        BuildMemberUpdate(p, room_it->second, mem_it->second, cause, ctx->a_variant);
        QueueMatching2SignalingEvent(*ctx, room_id, member_id,
                                     ORBIS_NP_MATCHING2_SIGNALING_EVENT_DEAD,
                                     ORBIS_NP_MATCHING2_SIGNALING_ERROR_TERMINATED_BY_PEER);
        room_it->second.members.erase(mem_it);
        cache->peers.erase(member_id);
        break;
    }
    case ORBIS_NP_MATCHING2_ROOM_EVENT_UPDATED_ROOM_MEMBER_DATA_INTERNAL: {
        auto room_it = cache->rooms.find(room_id);
        if (room_it == cache->rooms.end()) {
            after_unlock.warning = fmt::format("0x1107 for uncached room {}", room_id);
            return;
        }
        auto mem_it = room_it->second.members.find(member_id);
        if (mem_it == room_it->second.members.end()) {
            after_unlock.warning =
                fmt::format("0x1107 for unknown member {} in room {}", member_id, room_id);
            return;
        }
        MemberCache& mc = mem_it->second;
        mc.team_id = static_cast<OrbisNpMatching2TeamId>(n.member_team_id);
        mc.flag_attr = n.member_flag_attr;
        // The notification carries the member's full attribute set, so replacing rather than
        // merging keeps a cleared attribute from lingering in the cache.
        mc.bins.clear();
        u64 bytes = 0;
        for (const auto& a : n.member_bin_attrs) {
            MemberBinCache& b = mc.bins[a.attr_id];
            b.id = static_cast<OrbisNpMatching2AttributeId>(a.attr_id);
            b.update_date = a.update_date;
            b.data = a.data;
            bytes += a.data.size();
        }
        after_unlock.info = fmt::format(
            "RoomEvent 0x1107: room={} member={} team={} flagAttr={:#x} attrs={} bytes={}", room_id,
            member_id, mc.team_id, mc.flag_attr, mc.bins.size(), bytes);
        BuildMemberUpdate(p, room_it->second, mc, cause, ctx->a_variant);
        break;
    }
    case ORBIS_NP_MATCHING2_ROOM_EVENT_ROOM_OWNER_CHANGED: {
        auto room_it = cache->rooms.find(room_id);
        if (room_it == cache->rooms.end()) {
            after_unlock.warning = fmt::format("0x1105 for uncached room {}", room_id);
            return;
        }
        RoomCache& rc = room_it->second;
        const OrbisNpMatching2RoomMemberId prev_owner = rc.owner_member_id;
        // Ownership is a single flag bit: clearing it everywhere first keeps two members from
        // both claiming it if a notification is ever replayed.
        for (auto& [mid, mc] : rc.members) {
            if (mid == member_id) {
                mc.flag_attr |= ORBIS_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER;
            } else {
                mc.flag_attr &= ~ORBIS_NP_MATCHING2_ROOMMEMBER_FLAG_ATTR_OWNER;
            }
        }
        rc.owner_member_id = member_id;
        rc.owner = (member_id == ctx->my_member_id);
        if (room_id == ctx->room_id) {
            ctx->is_room_owner = rc.owner;
        }
        after_unlock.info =
            fmt::format("RoomEvent 0x1105: room={} owner {} -> {} (self={}) cause={}", room_id,
                        prev_owner, member_id, rc.owner, static_cast<u32>(cause));

        p.event_room_password = std::make_unique<OrbisNpMatching2SessionPassword>();
        *p.event_room_password = rc.has_password ? rc.password : OrbisNpMatching2SessionPassword{};
        p.room_owner_update = std::make_unique<OrbisNpMatching2RoomOwnerUpdateInfo>();
        OrbisNpMatching2RoomOwnerUpdateInfo& u = *p.room_owner_update;
        u = OrbisNpMatching2RoomOwnerUpdateInfo{};
        u.prevOwner = prev_owner;
        u.newOwner = member_id;
        u.eventCause = cause;
        u.roomPassword = p.event_room_password.get();
        p.room_event_data = p.room_owner_update.get();
        break;
    }
    case ORBIS_NP_MATCHING2_ROOM_EVENT_UPDATED_SIGNALING_OPT_PARAM: {
        auto room_it = cache->rooms.find(room_id);
        if (room_it == cache->rooms.end()) {
            after_unlock.warning = fmt::format("0x1108 for uncached room {}", room_id);
            return;
        }
        RoomCache& rc = room_it->second;
        rc.signaling_opt_param.type = static_cast<OrbisNpMatching2SignalingType>(n.signaling_type);
        rc.signaling_opt_param.flag = static_cast<OrbisNpMatching2SignalingFlag>(n.signaling_flag);
        rc.signaling_opt_param.memberId =
            static_cast<OrbisNpMatching2RoomMemberId>(n.signaling_hub_member_id);
        rc.has_signaling_opt_param = true;
        rc.signaling_type = rc.signaling_opt_param.type;
        rc.signaling_main_member = rc.signaling_opt_param.memberId;
        // The cache update is the whole point: GetSignalingOptParamLocal now answers with the
        // owner's choice. The guest callback is deliberately not raised - the payload structure
        // this event carries was not recoverable from libSceNpMatching2, and handing a title a
        // structure we guessed at is worse than handing it nothing.
        after_unlock.info = fmt::format(
            "RoomEvent 0x1108: room={} type={} flag={} hub={} (cache only, no callback)", room_id,
            static_cast<u32>(rc.signaling_opt_param.type),
            static_cast<u32>(rc.signaling_opt_param.flag), rc.signaling_opt_param.memberId);
        // Members already in the room were skipped while the type was still the default.
        // Start whatever this update now allows; an active peer is left alone.
        StartMatching2SignalingForRoomPeers(*ctx, room_id);
        return;
    }
    case ORBIS_NP_MATCHING2_ROOM_EVENT_KICKEDOUT: {
        p.room_update = std::make_unique<OrbisNpMatching2RoomUpdate>();
        OrbisNpMatching2RoomUpdate& u = *p.room_update;
        u = OrbisNpMatching2RoomUpdate{};
        u.eventCause = cause;
        u.errorCode = n.error_code;
        p.room_event_data = p.room_update.get();
        break;
    }
    case ORBIS_NP_MATCHING2_ROOM_EVENT_UPDATED_ROOM_DATA_INTERNAL: {
        auto room_it = cache->rooms.find(room_id);
        if (room_it == cache->rooms.end()) {
            return;
        }
        RoomCache& rc = room_it->second;
        const auto prev_flags = static_cast<OrbisNpMatching2FlagAttr>(rc.flags);
        const auto prev_passwd_slot_mask = rc.passwd_slot_mask;
        rc.flags = n.flags;
        if (n.has_passwd_mask) {
            rc.passwd_slot_mask = n.passwd_slot_mask;
        }

        std::vector<RoomBinCache> changed_attrs;
        changed_attrs.reserve(n.bin_attrs.size());
        for (const auto& a : n.bin_attrs) {
            // Replace the owned bytes in place. Appending into a side buffer and keeping a
            // pointer to it freed every earlier blob the next time the vector grew, which is
            // what took a 2v2 room down after it had been publishing state for a while.
            RoomBinCache updated;
            updated.id = static_cast<OrbisNpMatching2AttributeId>(a.attr_id);
            updated.update_date = a.update_date;
            updated.member_id = static_cast<OrbisNpMatching2RoomMemberId>(a.update_member_id);
            updated.data = a.data;

            bool replaced = false;
            for (auto& existing : rc.room_bins) {
                if (existing.id == updated.id) {
                    existing = updated;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                rc.room_bins.push_back(updated);
            }
            changed_attrs.push_back(std::move(updated));
        }

        p.room_groups.reserve(rc.groups.size());
        for (const auto& [gid, group] : rc.groups) {
            p.room_groups.push_back(group);
        }
        p.room_group_ptrs.reserve(p.room_groups.size());
        for (auto& group : p.room_groups) {
            p.room_group_ptrs.push_back(&group);
        }

        p.room_bin_attrs.resize(changed_attrs.size());
        p.bin_buffers.clear();
        p.bin_buffers.reserve(changed_attrs.size());
        for (size_t i = 0; i < changed_attrs.size(); ++i) {
            const auto& src = changed_attrs[i];
            p.bin_buffers.push_back(src.data);
            auto& buf = p.bin_buffers.back();
            auto& dst = p.room_bin_attrs[i];
            dst = OrbisNpMatching2RoomBinAttrInternal{};
            dst.lastUpdate.tick = src.update_date;
            dst.memberId = src.member_id;
            dst.binAttr.id = src.id;
            dst.binAttr.data = buf.empty() ? nullptr : buf.data();
            dst.binAttr.dataSize = buf.size();
        }
        p.room_bin_attr_ptrs.reserve(p.room_bin_attrs.size());
        for (auto& attr : p.room_bin_attrs) {
            p.room_bin_attr_ptrs.push_back(&attr);
        }

        p.room_data = std::make_unique<OrbisNpMatching2RoomDataInternal>();
        auto& room = *p.room_data;
        room = {};
        room.publicSlots = rc.public_slots;
        room.privateSlots = rc.private_slots;
        room.openPublicSlots = rc.open_public_slots;
        room.openPrivateSlots = rc.open_private_slots;
        room.maxSlot = rc.max_slot;
        room.serverId = rc.server_id;
        room.worldId = rc.world_id;
        room.lobbyId = rc.lobby_id;
        room.roomId = rc.room_id;
        room.passwdSlotMask = rc.passwd_slot_mask;
        room.joinedSlotMask = rc.joined_slot_mask;
        room.roomGroup = p.room_groups.empty() ? nullptr : p.room_groups.data();
        room.roomGroups = p.room_groups.size();
        room.flags = rc.flags;
        room.internalBinAttr = p.room_bin_attrs.empty() ? nullptr : p.room_bin_attrs.data();
        room.internalBinAttrs = p.room_bin_attrs.size();

        p.event_chg_flag_attr = std::make_unique<OrbisNpMatching2FlagAttr>(
            static_cast<OrbisNpMatching2FlagAttr>(rc.flags));
        p.event_prev_flag_attr = std::make_unique<OrbisNpMatching2FlagAttr>(prev_flags);
        p.event_chg_passwd_slot_mask =
            std::make_unique<OrbisNpMatching2RoomPasswordSlotMask>(rc.passwd_slot_mask);
        p.event_prev_passwd_slot_mask =
            std::make_unique<OrbisNpMatching2RoomPasswordSlotMask>(prev_passwd_slot_mask);

        p.room_data_internal_update = std::make_unique<OrbisNpMatching2RoomDataInternalUpdate>();
        auto& u = *p.room_data_internal_update;
        u = {};
        u.chgRoomDataInternal = p.room_data.get();
        u.chgFlagAttr = p.event_chg_flag_attr.get();
        u.prevFlagAttr = p.event_prev_flag_attr.get();
        u.chgRoomPasswordSlotMask = p.event_chg_passwd_slot_mask.get();
        u.prevRoomPasswordSlotMask = p.event_prev_passwd_slot_mask.get();
        u.chgRoomGroup = p.room_group_ptrs.empty() ? nullptr : p.room_group_ptrs.data();
        u.chgRoomGroupNum = p.room_group_ptrs.size();
        u.chgRoomBinAttrInternal =
            p.room_bin_attr_ptrs.empty() ? nullptr : p.room_bin_attr_ptrs.data();
        u.chgRoomBinAttrInternalNum = p.room_bin_attr_ptrs.size();
        p.room_event_data = p.room_data_internal_update.get();
        break;
    }
    default:
        after_unlock.warning = fmt::format("unhandled room event {:#x}", n.event);
        return;
    }

    LOG_DEBUG(Lib_NpMatching2, "RoomEvent ctx={} room={} event={:#x} cause={}", n.ctx_id, n.room_id,
              n.event, n.event_cause);

    OrbisNpMatching2Event fired_event = event;
    if (ctx->a_variant) {
        switch (event) {
        case ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_JOINED:
            fired_event = ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_JOINED_A;
            break;
        case ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_LEFT:
            fired_event = ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_LEFT_A;
            break;
        case ORBIS_NP_MATCHING2_ROOM_EVENT_UPDATED_ROOM_MEMBER_DATA_INTERNAL:
            fired_event = ORBIS_NP_MATCHING2_ROOM_EVENT_UPDATED_ROOM_MEMBER_DATA_INTERNAL_A;
            break;
        default:
            break;
        }
    }

    PendingEvent ev{};
    ev.type = PendingEvent::ROOM_EVENT_CB;
    ev.ctx_id = ctx->ctx_id;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.room_id = room_id;
    ev.room_event = fired_event;
    ev.room_event_data = p.room_event_data;
    ev.context_owner = ctx;
    ev.payload_owner = std::move(payload_owner);
    ScheduleEvent(std::move(ev));

    if (event == ORBIS_NP_MATCHING2_ROOM_EVENT_MEMBER_JOINED) {
        StartMatching2PeerHandshake(*ctx, room_id, member_id);
        SignalingHandler::StartMatching2(*ctx);
    }
}

void HandleRoomMessage(const ShadNet::NotifyRoomMessage& n) {
    Matching2StateLock state_lock{"HandleRoomMessage"};
    auto ctx = NpHandler::GetInstance().GetMatching2ContextManager().Get(
        static_cast<OrbisNpMatching2ContextId>(n.ctx_id));
    if (!ctx) {
        return;
    }

    const auto room_id = static_cast<OrbisNpMatching2RoomId>(n.room_id);
    const auto src_member_id = static_cast<OrbisNpMatching2RoomMemberId>(n.src_member_id);
    MemberCache src_member_copy{};
    bool have_src_member = false;
    {
        auto cache = NpHandler::GetInstance().LockMatching2Cache(ctx->ctx_id);
        const auto room_it = cache->rooms.find(room_id);
        if (room_it != cache->rooms.end()) {
            const auto member_it = room_it->second.members.find(src_member_id);
            if (member_it != room_it->second.members.end()) {
                src_member_copy = member_it->second;
                have_src_member = true;
            }
        }
    }

    MemberCache notification_src{};
    const MemberCache* src_member = &src_member_copy;
    if (!have_src_member) {
        notification_src.member_id = src_member_id;
        SetNpId(notification_src.np_id, n.src_npid);
        notification_src.account_id =
            static_cast<Libraries::Np::OrbisNpAccountId>(n.src_account_id);
        notification_src.platform = static_cast<Libraries::Np::OrbisNpPlatformType>(n.src_platform);
        src_member = &notification_src;
    }

    std::vector<OrbisNpMatching2RoomMemberId> dst_members;
    dst_members.reserve(n.dst_member_ids.size());
    for (const u32 member_id : n.dst_member_ids) {
        dst_members.push_back(static_cast<OrbisNpMatching2RoomMemberId>(member_id));
    }

    auto payload_owner = std::make_shared<CallbackPayload>();
    void* data = BuildRoomMessagePayload(*payload_owner, ctx->a_variant,
                                         static_cast<OrbisNpMatching2CastType>(n.cast_type),
                                         dst_members, src_member, n.msg);

    PendingEvent ev{};
    ev.type = PendingEvent::ROOM_MESSAGE_CB;
    ev.ctx_id = ctx->ctx_id;
    ev.context_owner = ctx;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.room_id = room_id;
    ev.src_member_id = src_member_id;
    ev.msg_event = ctx->a_variant ? ORBIS_NP_MATCHING2_ROOM_MSG_EVENT_MESSAGE_A
                                  : ORBIS_NP_MATCHING2_ROOM_MSG_EVENT_MESSAGE;
    ev.message_data = data;
    ev.payload_owner = std::move(payload_owner);
    ScheduleEvent(std::move(ev));
}

// Runs on the ShadNet reader thread. It only fills the cache; whoever asked for the endpoint
// picks it up on its next attempt, so nothing ever waits here.
void HandleSignalingInfosReply(u64 pkt_id, ShadNet::ErrorType error, const std::vector<u8>& body) {
    std::string target;
    {
        std::lock_guard lock(g_mm.sig_mutex);
        const auto it = g_mm.sig_inflight.find(pkt_id);
        if (it == g_mm.sig_inflight.end()) {
            LOG_WARNING(Lib_NpMatching2, "signaling info reply pkt_id={} has no pending query",
                        pkt_id);
            return;
        }
        target = it->second.target;
        g_mm.sig_inflight.erase(it);
    }

    if (error != ShadNet::ErrorType::NoError) {
        LOG_WARNING(Lib_NpMatching2,
                    "signaling info '{}' refused by server: error={} (peer has not registered a "
                    "STUN endpoint yet, or its UDP path to the server is blocked)",
                    target, static_cast<int>(error));
        return;
    }

    shadnet::RequestSignalingInfosReply rep;
    const std::string proto = ExtractProtoBytes(body);
    if (proto.empty() || !rep.ParseFromString(proto)) {
        LOG_ERROR(Lib_NpMatching2, "signaling info '{}' reply is not parseable ({} bytes)", target,
                  body.size());
        return;
    }

    const std::string addr_str = rep.target_ip();
    const u16 port_host = static_cast<u16>(rep.target_port());
    u32 addr_nbo = 0;
    if (addr_str.empty() || port_host == 0 ||
        Libraries::Net::sceNetInetPton(Net::ORBIS_NET_AF_INET, addr_str.c_str(), &addr_nbo) <= 0) {
        LOG_WARNING(Lib_NpMatching2,
                    "signaling info '{}' is unusable: ip='{}' port={} - the server has no STUN "
                    "record for this peer",
                    target, addr_str, port_host);
        return;
    }

    {
        std::lock_guard lock(g_mm.sig_mutex);
        SignalingInfo& info = g_mm.sig_cache[target];
        info.addr = addr_nbo;
        info.port = Libraries::Net::sceNetHtons(port_host);
        info.nat_type = rep.target_nat_type();
        info.resolved_at = std::chrono::steady_clock::now();
    }
    LOG_INFO(Lib_NpMatching2, "signaling info '{}' -> {}:{} memberId={} natType={}", target,
             addr_str, port_host, rep.target_member_id(), rep.target_nat_type());
}

} // namespace

void OnMatchingReply(ShadNet::CommandType cmd, u64 pkt_id, ShadNet::ErrorType error,
                     const std::vector<u8>& body) {
    if (cmd == ShadNet::CommandType::RequestSignalingInfos) {
        HandleSignalingInfosReply(pkt_id, error, body);
        return;
    }

    PendingRequest pr;
    {
        std::lock_guard lock(g_mm.pending_mutex);
        auto it = g_mm.pending.find(pkt_id);
        if (it == g_mm.pending.end()) {
            LOG_WARNING(Lib_NpMatching2, "reply pkt_id={} cmd={} has no pending entry (have {})",
                        pkt_id, static_cast<u16>(cmd), g_mm.pending.size());
            return;
        }
        pr = it->second;
        g_mm.pending.erase(it);
    }
    LOG_DEBUG(Lib_NpMatching2, "reply pkt_id={} cmd={} -> ctx={} reqId={} event={:#x}", pkt_id,
              static_cast<u16>(cmd), pr.ctx_id, pr.req_id, static_cast<u16>(pr.req_event));
    DispatchRequestComplete(pr, error, body);
}

void ExpireMatchingRequests() {
    const auto now = std::chrono::steady_clock::now();

    std::vector<PendingRequest> expired;
    {
        std::lock_guard lock(g_mm.pending_mutex);
        for (auto it = g_mm.pending.begin(); it != g_mm.pending.end();) {
            if (now < it->second.deadline) {
                ++it;
                continue;
            }
            LOG_ERROR(Lib_NpMatching2,
                      "request pkt_id={} ctx={} reqId={} event={:#x} had no reply after {}s; "
                      "completing it as timed out",
                      it->first, it->second.ctx_id, it->second.req_id,
                      static_cast<u16>(it->second.req_event), kNpRequestTimeout.count());
            expired.push_back(it->second);
            it = g_mm.pending.erase(it);
        }
    }
    // Dispatched outside the lock: scheduling an event takes the event queue's own lock.
    for (const PendingRequest& pr : expired) {
        DispatchRequestFailure(pr, ORBIS_NP_MATCHING2_ERROR_REQUEST_TIMEOUT);
    }

    // Signaling queries only populate a cache, so a lost reply needs no completion - but the
    // entry must still go, or the map grows for the rest of the session.
    std::lock_guard lock(g_mm.sig_mutex);
    for (auto it = g_mm.sig_inflight.begin(); it != g_mm.sig_inflight.end();) {
        if (now < it->second.deadline) {
            ++it;
            continue;
        }
        LOG_WARNING(Lib_NpMatching2, "signaling info query pkt_id={} target='{}' got no reply",
                    it->first, it->second.target);
        it = g_mm.sig_inflight.erase(it);
    }
}

void SetMmShadNetClient(std::shared_ptr<ShadNet::ShadNetClient> client,
                        std::string_view server_host, u16 tcp_port) {
    u32 server_addr = 0;
    u16 server_udp_port = 0;
    bool matching2_enabled = false;
    bool relay_enabled = false;
    u16 stun_alt_port = 0;
    {
        std::lock_guard lock(g_mm.mutex);
        g_mm.client = client;
        g_mm.matching2_enabled = client ? client->IsMatching2Enabled() : false;
        g_mm.relay_enabled = client ? client->IsSignalingRelayEnabled() : false;
        g_mm.stun_alt_port = client ? client->GetStunAltPort() : 0;
        matching2_enabled = g_mm.matching2_enabled;
        relay_enabled = g_mm.relay_enabled;
        stun_alt_port = g_mm.stun_alt_port;
        g_mm.server_addr = client ? client->GetAddrServer() : 0;
        if (g_mm.server_addr == 0) {
            g_mm.server_addr = IpStringToAddr(server_host);
        }
        g_mm.server_udp_port =
            matching2_enabled ? Libraries::Net::sceNetHtons(static_cast<u16>(tcp_port + 1)) : 0;
        server_addr = g_mm.server_addr;
        server_udp_port = g_mm.server_udp_port;
    }
    // One place to read the whole online setup from when a session will not connect: what the
    // server offers, which UDP endpoints this client will use, and what it will advertise.
    LOG_INFO(Lib_NpMatching2,
             "ShadNet online setup: server={}:{} matching2={} relay={} stunPort={} stunAltPort={}",
             server_host, tcp_port, matching2_enabled, relay_enabled,
             Libraries::Net::sceNetNtohs(server_udp_port), stun_alt_port);
    Net::UPnPClient::Instance().SetP2PFeaturesEnabled(matching2_enabled);
    if (matching2_enabled && Net::EnsureP2PTransport()) {
        LOG_INFO(Lib_NpMatching2,
                 "P2P transport: port={} advertising={:#010x}. Peers reach this console on that "
                 "UDP port; forward it or keep UPnP enabled when behind a strict NAT.",
                 Net::GetP2PConfiguredPort(),
                 Libraries::Net::sceNetNtohl(Net::GetP2PAdvertisedAddr()));
    }

    if (!client) {
        NpSignaling::Transport::SetTransportHooks({});
        NpSignaling::Transport::SetPeerResolver(nullptr);
        NpSignaling::Transport::SetMatching2Enabled(false);
        NpSignaling::Transport::SetMmServerEndpoint(0, 0);
        StopMatching2SignalingRuntime();
        return;
    }

    NpSignaling::Transport::SetTransportHooks({
        .signaling_send = Net::P2PSignalingSendTo,
        .signaling_recv = Net::P2PSignalingRecvFrom,
        .control_send = Net::P2PControlSendTo,
        .control_recv = Net::P2PControlRecvFrom,
        .transport_ready = Net::P2PTransportIsReady,
        .configured_port = Net::GetP2PAdvertisedPort, // the bound port once running
        .advertised_addr = Net::GetP2PAdvertisedAddr,
        .ensure_transport = Net::EnsureP2PTransport,
    });
    NpSignaling::Transport::SetPeerResolver(matching2_enabled ? ResolveSignalingInfo : nullptr);
    NpSignaling::Transport::SetMatching2Enabled(matching2_enabled);
    NpSignaling::Transport::SetMmServerEndpoint(server_addr, server_udp_port);
    if (matching2_enabled) {
        StartMatching2HandshakeThread();
    } else {
        StopMatching2HandshakeThread();
        StopMatching2SignalingRuntime();
    }
    client->onRoomEvent = [](const ShadNet::NotifyRoomEvent& n) { HandleRoomEvent(n); };
    client->onRoomMessage = [](const ShadNet::NotifyRoomMessage& n) { HandleRoomMessage(n); };
}

void ClearMmShadNetClient(OrbisNpMatching2EventCause cause, s32 error_code) {
    std::shared_ptr<ShadNet::ShadNetClient> old_client;
    {
        std::lock_guard lock(g_mm.mutex);
        old_client = std::move(g_mm.client);
        g_mm.server_addr = 0;
        g_mm.server_udp_port = 0;
        g_mm.matching2_enabled = false;
        g_mm.relay_enabled = false;
        g_mm.stun_alt_port = 0;
    }
    Net::UPnPClient::Instance().SetP2PFeaturesEnabled(false);
    if (old_client) {
        old_client->onRoomEvent = nullptr;
        old_client->onRoomMessage = nullptr;
    }
    NpSignaling::Transport::SetTransportHooks({});
    NpSignaling::Transport::SetPeerResolver(nullptr);
    NpSignaling::Transport::SetMatching2Enabled(false);
    NpSignaling::Transport::SetMmServerEndpoint(0, 0);
    StopMatching2HandshakeThread();
    StopMatching2SignalingRuntime();
    Net::ClearP2PRelayedPeers();
    Net::ReleaseP2PPortMapping();
    std::vector<PendingRequest> orphaned;
    {
        std::lock_guard lock(g_mm.pending_mutex);
        orphaned.reserve(g_mm.pending.size());
        for (const auto& entry : g_mm.pending) {
            orphaned.push_back(entry.second);
        }
        g_mm.pending.clear();
    }
    {
        std::lock_guard lock(g_mm.sig_mutex);
        g_mm.sig_cache.clear();
        g_mm.sig_last_request.clear();
        g_mm.sig_inflight.clear();
    }

    // The replies these requests were waiting for can no longer arrive, and the server session the
    // started contexts belonged to is gone. Dropping them left the title waiting on callbacks that
    // nothing would ever deliver. Requests go first, so a context whose start was still in flight
    // is reported as a failed start rather than as stopped.
    for (const PendingRequest& pr : orphaned) {
        DispatchRequestFailure(pr, error_code);
    }
    const std::vector<OrbisNpMatching2ContextId> stopped =
        NpHandler::GetInstance().GetMatching2ContextManager().StopAllStarted();
    for (const OrbisNpMatching2ContextId ctx_id : stopped) {
        PendingEvent ev{};
        ev.type = PendingEvent::CONTEXT_CB;
        ev.ctx_id = ctx_id;
        ev.fire_at = std::chrono::steady_clock::now();
        ev.ctx_event = ORBIS_NP_MATCHING2_CONTEXT_EVENT_STOPPED;
        ev.ctx_event_cause = cause;
        ev.error_code = error_code;
        ScheduleEvent(std::move(ev));
    }
    if (!orphaned.empty() || !stopped.empty()) {
        LOG_INFO(Lib_NpMatching2,
                 "matching backend torn down: failed {} pending request(s), stopped {} "
                 "context(s) (cause={}, error={:#x})",
                 orphaned.size(), stopped.size(), static_cast<u8>(cause),
                 static_cast<u32>(error_code));
    }
}

bool IsMmClientRunning() {
    std::lock_guard lock(g_mm.mutex);
    return g_mm.client && g_mm.client->IsAuthenticated();
}

void MmContextStart(OrbisNpMatching2ContextId ctx_id) {
    shadnet::ContextStartRequest req;
    req.set_ctx_id(ctx_id);
    if (MmSubmitRequest(ctx_id, 0, ORBIS_NP_MATCHING2_CONTEXT_EVENT_STARTED,
                        MmCommand::ContextStart, MakeProtoPayload(req)) != ORBIS_OK) {
        // sceNpMatching2ContextStart has already returned success, so the context callback is the
        // only way left to report the failure; without it the title waits for a STARTED event
        // that nothing will deliver.
        FailContextStart(ctx_id, ORBIS_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE);
        return;
    }
    if (auto ctx = NpHandler::GetInstance().GetMatching2ContextManager().Get(ctx_id)) {
        SendMatching2StunPing(*ctx);
    }
}

s32 MmContextStop(OrbisNpMatching2ContextId ctx_id) {
    shadnet::ContextStopRequest req;
    req.set_ctx_id(ctx_id);
    return MmSubmitRequest(ctx_id, 0, ORBIS_NP_MATCHING2_CONTEXT_EVENT_STOPPED,
                           MmCommand::ContextStop, MakeProtoPayload(req));
}

s32 MmSubmitRequest(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                    OrbisNpMatching2Event req_event, MmCommand cmd, const std::vector<u8>& payload,
                    bool a_variant) {
    std::shared_ptr<ShadNet::ShadNetClient> client;
    {
        std::lock_guard lock(g_mm.mutex);
        client = g_mm.client;
    }
    if (!client || !client->IsAuthenticated()) {
        LOG_ERROR(Lib_NpMatching2, "cmd={} not connected", static_cast<u16>(cmd));
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }

    auto ctx = NpHandler::GetInstance().GetMatching2ContextManager().Get(ctx_id);
    if (!ctx) {
        LOG_ERROR(Lib_NpMatching2, "cmd={} invalid ctx={}", static_cast<u16>(cmd), ctx_id);
        return ORBIS_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID;
    }
    const RequestCallbackInfo request_cb = ConsumeRequestCallback(ctx);
    if (IsMatching2BackendDisabled() && !IsContextLifecycleEvent(req_event)) {
        LOG_INFO(Lib_NpMatching2,
                 "matching2 backend disabled; failing ctx={} reqId={} "
                 "event={:#x} cmd={}",
                 ctx_id, req_id, static_cast<u16>(req_event), static_cast<u16>(cmd));

        s32 error_code = ORBIS_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
        void* request_data = nullptr;
        std::shared_ptr<CallbackPayload> request_payload_owner;
        if (req_event == ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_WORLD_INFO_LIST) {
            LOG_INFO(Lib_NpMatching2, "matching2 backend disabled; returning empty world");
            shadnet::GetWorldInfoListReply reply;
            auto* world = reply.add_worlds();
            world->set_world_id(1);
            world->set_lobbies_num(0);
            world->set_max_lobby_members(0);
            world->set_lobby_members_num(0);
            world->set_rooms_num(0);
            world->set_room_members_num(0);
            request_payload_owner = std::make_shared<CallbackPayload>();
            request_data = BuildGetWorldInfoListPayload(*ctx, *request_payload_owner, reply);
            error_code = ORBIS_OK;
        }

        PendingEvent ev{};
        ev.type = PendingEvent::REQUEST_CB;
        ev.ctx_id = ctx_id;
        ev.context_owner = ctx;
        ev.fire_at = std::chrono::steady_clock::now();
        ev.req_id = req_id;
        ev.req_event = req_event;
        ev.error_code = error_code;
        ev.request_cb = request_cb.callback;
        ev.request_cb_arg = request_cb.arg;
        ev.request_data = request_data;
        ev.payload_owner = std::move(request_payload_owner);
        ScheduleEvent(std::move(ev));
        return ORBIS_OK;
    }

    LOG_INFO(Lib_NpMatching2,
             "ctx={} reqId={} event={:#x} cmd={} aVariant={} callback={:#x} "
             "arg={}",
             ctx_id, req_id, static_cast<u16>(req_event), static_cast<u16>(cmd), a_variant,
             reinterpret_cast<std::uintptr_t>(request_cb.callback), fmt::ptr(request_cb.arg));

    PendingRequest pending{ctx_id,        req_id, req_event, a_variant, request_cb.callback,
                           request_cb.arg};
    pending.deadline = std::chrono::steady_clock::now() + kNpRequestTimeout;
    const u64 pkt_id =
        client->SubmitRequest(static_cast<ShadNet::CommandType>(cmd), payload, [&pending](u64 id) {
            std::lock_guard lock(g_mm.pending_mutex);
            g_mm.pending[id] = pending;
        });
    LOG_DEBUG(Lib_NpMatching2, "submit cmd={} pkt_id={} ctx={} reqId={}", static_cast<u16>(cmd),
              pkt_id, ctx_id, req_id);
    return ORBIS_OK;
}

s32 MmCreateJoinRoom(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                     const OrbisNpMatching2CreateJoinRoomRequest& request) {
    shadnet::CreateRoomRequest req;
    req.set_req_id(req_id);
    req.set_max_slots(request.maxSlot);
    req.set_team_id(request.teamId);
    req.set_world_id(request.worldId);
    req.set_lobby_id(static_cast<u32>(request.lobbyId));
    req.set_flags(request.flags);
    req.set_group_config_count(static_cast<u32>(request.groupConfigs));
    req.set_allowed_user_count(static_cast<u32>(request.allowedUsers));
    req.set_blocked_user_count(static_cast<u32>(request.blockedUsers));
    req.set_internal_bin_attr_count(static_cast<u32>(request.internalBinAttrs));
    StashPendingRoomPassword(ctx_id, request.roomPasswd);
    LOG_INFO(Lib_NpMatching2,
             "ctx={} reqId={} maxSlot={} world={} lobby={} flags={:#x} "
             "internalBin={} searchInt={} searchBin={} extBin={} memberBin={}",
             ctx_id, req_id, request.maxSlot, request.worldId, request.lobbyId, request.flags,
             request.internalBinAttrs, request.externalSearchIntAttrs,
             request.externalSearchBinAttrs, request.externalBinAttrs,
             request.memberInternalBinAttrs);
    AppendCreateJoinRoomCommon(req, request.roomPasswd, request.passwdSlotMask, request.groupConfig,
                               request.groupConfigs, request.joinGroupLabel);
    req.set_user_id_kind(shadnet::MATCHING_USER_ID_ONLINE_ID);
    if (request.allowedUser) {
        for (u64 i = 0; i < request.allowedUsers; ++i) {
            req.add_allowed_online_ids(OnlineIdToString(request.allowedUser[i]));
        }
    }
    if (request.blockedUser) {
        for (u64 i = 0; i < request.blockedUsers; ++i) {
            req.add_blocked_online_ids(OnlineIdToString(request.blockedUser[i]));
        }
    }
    for (u64 i = 0; i < request.internalBinAttrs; ++i) {
        LogBinAttr("  createInternalBin", i, request.internalBinAttr[i]);
        AppendCreateRoomBinAttrById(req, request.internalBinAttr[i], "internalBinAttr");
    }
    for (u64 i = 0; i < request.externalSearchIntAttrs; ++i) {
        LogIntAttr("  createExternalSearchInt", i, request.externalSearchIntAttr[i]);
        AppendIntAttr(req.add_external_search_int_attrs(), request.externalSearchIntAttr[i]);
    }
    for (u64 i = 0; i < request.externalSearchBinAttrs; ++i) {
        LogBinAttr("  createExternalSearchBin", i, request.externalSearchBinAttr[i]);
        AppendCreateRoomBinAttrById(req, request.externalSearchBinAttr[i], "externalSearchBinAttr");
    }
    for (u64 i = 0; i < request.externalBinAttrs; ++i) {
        LogBinAttr("  createExternalBin", i, request.externalBinAttr[i]);
        AppendCreateRoomBinAttrById(req, request.externalBinAttr[i], "externalBinAttr");
    }
    for (u64 i = 0; i < request.memberInternalBinAttrs; ++i) {
        LogBinAttr("  createMemberInternalBin", i, request.memberInternalBinAttr[i]);
        AppendBinAttr(req.add_member_bin_attrs(), request.memberInternalBinAttr[i]);
    }
    req.set_join_group_label_present(request.joinGroupLabel != nullptr);
    StashPendingRoomPassword(ctx_id, request.roomPasswd);
    SetRoomPassword(req, request.roomPasswd);
    if (request.signalingParam) {
        req.set_sig_type(request.signalingParam->type);
        req.set_sig_flag(request.signalingParam->flag);
        req.set_sig_main_member(request.signalingParam->memberId);
    }
    LogCreateRoomRequest(req);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM,
                           MmCommand::CreateRoom, MakeProtoPayload(req));
}

s32 MmCreateJoinRoomA(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                      const OrbisNpMatching2CreateJoinRoomRequestA& request) {
    shadnet::CreateRoomRequest req;
    req.set_req_id(req_id);
    req.set_max_slots(request.maxSlot);
    req.set_team_id(request.teamId);
    req.set_world_id(request.worldId);
    req.set_lobby_id(static_cast<u32>(request.lobbyId));
    req.set_flags(request.flags);
    req.set_group_config_count(static_cast<u32>(request.groupConfigs));
    req.set_allowed_user_count(static_cast<u32>(request.allowedUsers));
    req.set_blocked_user_count(static_cast<u32>(request.blockedUsers));
    req.set_internal_bin_attr_count(static_cast<u32>(request.internalBinAttrs));
    StashPendingRoomPassword(ctx_id, request.roomPasswd);
    LOG_INFO(Lib_NpMatching2,
             "ctx={} reqId={} maxSlot={} world={} lobby={} flags={:#x} "
             "internalBin={} searchInt={} searchBin={} extBin={} memberBin={}",
             ctx_id, req_id, request.maxSlot, request.worldId, request.lobbyId, request.flags,
             request.internalBinAttrs, request.externalSearchIntAttrs,
             request.externalSearchBinAttrs, request.externalBinAttrs,
             request.memberInternalBinAttrs);
    AppendCreateJoinRoomCommon(req, request.roomPasswd, request.passwdSlotMask, request.groupConfig,
                               request.groupConfigs, request.joinGroupLabel);
    req.set_user_id_kind(shadnet::MATCHING_USER_ID_ACCOUNT_ID);
    if (request.allowedUser) {
        for (u64 i = 0; i < request.allowedUsers; ++i) {
            req.add_allowed_account_ids(request.allowedUser[i]);
        }
    }
    if (request.blockedUser) {
        for (u64 i = 0; i < request.blockedUsers; ++i) {
            req.add_blocked_account_ids(request.blockedUser[i]);
        }
    }
    for (u64 i = 0; i < request.internalBinAttrs; ++i) {
        LogBinAttr("  createInternalBinA", i, request.internalBinAttr[i]);
        AppendCreateRoomBinAttrById(req, request.internalBinAttr[i], "internalBinAttrA");
    }
    for (u64 i = 0; i < request.externalSearchIntAttrs; ++i) {
        LogIntAttr("  createExternalSearchIntA", i, request.externalSearchIntAttr[i]);
        AppendIntAttr(req.add_external_search_int_attrs(), request.externalSearchIntAttr[i]);
    }
    for (u64 i = 0; i < request.externalSearchBinAttrs; ++i) {
        LogBinAttr("  createExternalSearchBinA", i, request.externalSearchBinAttr[i]);
        AppendCreateRoomBinAttrById(req, request.externalSearchBinAttr[i],
                                    "externalSearchBinAttrA");
    }
    for (u64 i = 0; i < request.externalBinAttrs; ++i) {
        LogBinAttr("  createExternalBinA", i, request.externalBinAttr[i]);
        AppendCreateRoomBinAttrById(req, request.externalBinAttr[i], "externalBinAttrA");
    }
    for (u64 i = 0; i < request.memberInternalBinAttrs; ++i) {
        LogBinAttr("  createMemberInternalBinA", i, request.memberInternalBinAttr[i]);
        AppendBinAttr(req.add_member_bin_attrs(), request.memberInternalBinAttr[i]);
    }
    req.set_join_group_label_present(request.joinGroupLabel != nullptr);
    StashPendingRoomPassword(ctx_id, request.roomPasswd);
    SetRoomPassword(req, request.roomPasswd);
    if (request.signalingParam) {
        req.set_sig_type(request.signalingParam->type);
        req.set_sig_flag(request.signalingParam->flag);
        req.set_sig_main_member(request.signalingParam->memberId);
    }
    LogCreateRoomRequest(req);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_CREATE_JOIN_ROOM_A,
                           MmCommand::CreateRoom, MakeProtoPayload(req), true);
}

s32 MmJoinRoom(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
               const OrbisNpMatching2JoinRoomRequest& request) {
    shadnet::JoinRoomRequest req;
    req.set_room_id(request.roomId);
    req.set_req_id(req_id);
    req.set_team_id(request.teamId);
    req.set_join_flags(request.flags);
    req.set_blocked_user_count(static_cast<u32>(request.blockedUsers));
    req.set_user_id_kind(shadnet::MATCHING_USER_ID_ONLINE_ID);
    if (request.blockedUser) {
        for (u64 i = 0; i < request.blockedUsers; ++i) {
            req.add_blocked_online_ids(OnlineIdToString(request.blockedUser[i]));
        }
    }
    for (u64 i = 0; i < request.roomMemberBinInternalAttrNum; ++i) {
        AppendBinAttr(req.add_member_bin_attrs(), request.roomMemberBinInternalAttr[i]);
    }
    StashPendingRoomPassword(ctx_id, request.roomPasswd);
    SetRoomPassword(req, request.roomPasswd);
    req.set_join_group_label_present(request.joinGroupLabel != nullptr);
    if (request.joinGroupLabel != nullptr) {
        req.set_join_group_label(request.joinGroupLabel->data,
                                 sizeof(request.joinGroupLabel->data));
    }
    LogJoinRoomRequest(req);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_JOIN_ROOM,
                           MmCommand::JoinRoom, MakeProtoPayload(req));
}

s32 MmJoinRoomA(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                const OrbisNpMatching2JoinRoomRequestA& request) {
    shadnet::JoinRoomRequest req;
    req.set_room_id(request.roomId);
    req.set_req_id(req_id);
    req.set_team_id(request.teamId);
    req.set_join_flags(request.flags);
    req.set_blocked_user_count(static_cast<u32>(request.blockedUsers));
    req.set_user_id_kind(shadnet::MATCHING_USER_ID_ACCOUNT_ID);
    if (request.blockedUser) {
        for (u64 i = 0; i < request.blockedUsers; ++i) {
            req.add_blocked_account_ids(request.blockedUser[i]);
        }
    }
    for (u64 i = 0; i < request.roomMemberBinInternalAttrNum; ++i) {
        AppendBinAttr(req.add_member_bin_attrs(), request.roomMemberBinInternalAttr[i]);
    }
    StashPendingRoomPassword(ctx_id, request.roomPasswd);
    SetRoomPassword(req, request.roomPasswd);
    req.set_join_group_label_present(request.joinGroupLabel != nullptr);
    if (request.joinGroupLabel != nullptr) {
        req.set_join_group_label(request.joinGroupLabel->data,
                                 sizeof(request.joinGroupLabel->data));
    }
    LogJoinRoomRequest(req);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_JOIN_ROOM_A,
                           MmCommand::JoinRoom, MakeProtoPayload(req), true);
}

s32 MmLeaveRoom(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                const OrbisNpMatching2LeaveRoomRequest& request) {
    shadnet::LeaveRoomRequest req;
    req.set_room_id(request.roomId);
    req.set_req_id(req_id);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_LEAVE_ROOM,
                           MmCommand::LeaveRoom, MakeProtoPayload(req));
}

s32 MmGetWorldInfoList(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                       const OrbisNpMatching2GetWorldInfoListRequest& request) {
    shadnet::GetWorldInfoListRequest req;
    req.set_server_id(request.serverId);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_WORLD_INFO_LIST,
                           MmCommand::GetWorldInfoList, MakeProtoPayload(req));
}

s32 MmGetLobbyInfoList(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                       const OrbisNpMatching2GetLobbyInfoListRequest& request) {
    shadnet::GetLobbyInfoListRequest req;
    req.set_world_id(request.worldId);
    req.set_range_filter_start(request.rangeFilter.start);
    req.set_range_filter_max(request.rangeFilter.max);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_LOBBY_INFO_LIST,
                           MmCommand::GetLobbyInfoList, MakeProtoPayload(req));
}

s32 MmSearchRoom(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                 const OrbisNpMatching2SearchRoomRequest& request, bool a_variant) {
    shadnet::SearchRoomRequest req;
    req.set_world_id(request.worldId);
    req.set_lobby_id(request.lobbyId);
    req.set_range_filter_start(request.rangeFilter.start);
    req.set_range_filter_max(request.rangeFilter.max);
    req.set_flag_filter(request.flags1);
    req.set_flag_attr(request.flags2);
    // A mode that "finds nobody" is decided by exactly three things: the world/lobby the search
    // is scoped to, the filters it carries, and the attributes the rooms were created with.
    // They are logged at INFO on both sides so a failed search can be read straight out of a
    // player's log without a special build.
    LOG_INFO(Lib_NpMatching2,
             "MmSearchRoom: ctx={} world={} lobby={} range={}+{} flags={:#x}/{:#x} intN={} "
             "binN={} attrN={}",
             ctx_id, request.worldId, request.lobbyId, request.rangeFilter.start,
             request.rangeFilter.max, request.flags1, request.flags2, request.intFilters,
             request.binFilters, request.attrs);
    for (u64 i = 0; i < request.intFilters; ++i) {
        const auto& f = request.intFilter[i];
        LOG_INFO(Lib_NpMatching2, "  intFilter[{}] op={} id={:#x} num={}", i, f.searchOperator,
                 f.attr.id, f.attr.num);
        auto* dst = req.add_int_filters();
        dst->set_op(f.searchOperator);
        dst->set_attr_id(f.attr.id);
        dst->set_attr_value(f.attr.num);
    }
    for (u64 i = 0; i < request.binFilters; ++i) {
        const auto& f = request.binFilter[i];
        LOG_INFO(Lib_NpMatching2, "  binFilter[{}] op={} id={:#x} size={}", i, f.searchOperator,
                 f.attr.id, f.attr.dataSize);
        auto* dst = req.add_bin_filters();
        dst->set_op(f.searchOperator);
        dst->set_attr_id(f.attr.id);
        if (f.attr.data && f.attr.dataSize > 0) {
            dst->set_data(f.attr.data, f.attr.dataSize);
        }
    }
    for (u64 i = 0; i < request.attrs; ++i) {
        req.add_attr_ids(request.attr[i]);
    }
    return MmSubmitRequest(ctx_id, req_id,
                           a_variant ? ORBIS_NP_MATCHING2_REQUEST_EVENT_SEARCH_ROOM_A
                                     : ORBIS_NP_MATCHING2_REQUEST_EVENT_SEARCH_ROOM,
                           MmCommand::SearchRoom, MakeProtoPayload(req), a_variant);
}

s32 MmGetRoomDataExternalList(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                              const OrbisNpMatching2GetRoomDataExternalListRequest& request,
                              bool a_variant) {
    shadnet::GetRoomDataExternalListRequest req;
    for (u64 i = 0; i < request.roomIdNum; ++i) {
        req.add_room_ids(request.roomId[i]);
    }
    for (u64 i = 0; i < request.attrIdNum; ++i) {
        req.add_attr_ids(request.attrId[i]);
    }
    LOG_INFO(Lib_NpMatching2, "getRoomDataExternalList roomN={} attrN={}", request.roomIdNum,
             request.attrIdNum);
    for (u64 i = 0; i < request.roomIdNum; ++i) {
        LOG_INFO(Lib_NpMatching2, "  getRoomDataExternalList roomId[{}]={}", i, request.roomId[i]);
    }
    for (u64 i = 0; i < request.attrIdNum; ++i) {
        LOG_INFO(Lib_NpMatching2, "  getRoomDataExternalList attrId[{}]={:#x}", i,
                 request.attrId[i]);
    }
    return MmSubmitRequest(ctx_id, req_id,
                           a_variant
                               ? ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_DATA_EXTERNAL_LIST_A
                               : ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_DATA_EXTERNAL_LIST,
                           MmCommand::GetRoomDataExternalList, MakeProtoPayload(req), a_variant);
}

s32 MmGetRoomMemberDataExternalList(
    OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
    const OrbisNpMatching2GetRoomMemberDataExternalListRequest& request, bool a_variant) {
    shadnet::GetRoomMemberDataExternalListRequest req;
    req.set_room_id(request.roomId);
    LOG_DEBUG(Lib_NpMatching2, "getRoomMemberDataExternalList room={}", request.roomId);
    return MmSubmitRequest(
        ctx_id, req_id,
        a_variant ? ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_MEMBER_DATA_EXTERNAL_LIST_A
                  : ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_ROOM_MEMBER_DATA_EXTERNAL_LIST,
        MmCommand::GetRoomMemberDataExternalList, MakeProtoPayload(req), a_variant);
}

s32 MmGetUserInfoList(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                      const OrbisNpMatching2GetUserInfoListRequest& request, bool a_variant) {
    shadnet::GetUserInfoListRequest req;
    for (u64 i = 0; i < request.npIdNum; ++i) {
        req.add_npids(request.npId[i].handle.data);
    }
    for (u64 i = 0; i < request.attrIdNum; ++i) {
        req.add_attr_ids(request.attrId[i]);
    }
    req.set_option(request.option);
    LOG_DEBUG(Lib_NpMatching2, "getUserInfoList npN={} attrN={}", request.npIdNum,
              request.attrIdNum);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_GET_USER_INFO_LIST,
                           MmCommand::GetUserInfoList, MakeProtoPayload(req), a_variant);
}

s32 MmSetUserInfo(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                  const OrbisNpMatching2SetUserInfoRequest& request) {
    shadnet::SetUserInfoRequest req;
    req.set_server_id(request.serverId);
    for (u64 i = 0; request.userBinAttr && i < request.userBinAttrs; ++i) {
        AppendBinAttr(req.add_user_bin_attrs(), request.userBinAttr[i]);
    }
    LOG_DEBUG(Lib_NpMatching2, "setUserInfo server={} binAttrs={}", request.serverId,
              request.userBinAttrs);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_USER_INFO,
                           MmCommand::SetUserInfo, MakeProtoPayload(req));
}

namespace {

shadnet::SendRoomMessageRequest BuildRoomMessageRequest(
    const OrbisNpMatching2SendRoomMessageRequest& request, bool is_chat) {
    shadnet::SendRoomMessageRequest req;
    req.set_room_id(request.roomId);
    req.set_cast_type(request.castType);
    req.set_option(request.option);
    req.set_is_chat(is_chat);

    if (request.castType == ORBIS_NP_MATCHING2_CASTTYPE_UNICAST) {
        req.add_dst_member_ids(request.dst.unicastTarget);
    } else if (request.castType == ORBIS_NP_MATCHING2_CASTTYPE_MULTICAST &&
               request.dst.multicastTarget.memberId) {
        for (u64 i = 0; i < request.dst.multicastTarget.memberIdNum; ++i) {
            req.add_dst_member_ids(request.dst.multicastTarget.memberId[i]);
        }
    }

    if (request.msg && request.msgLen > 0) {
        req.set_msg(request.msg, request.msgLen);
    }
    return req;
}

} // namespace

s32 MmSendRoomMessage(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                      const OrbisNpMatching2SendRoomMessageRequest& request) {
    const auto req = BuildRoomMessageRequest(request, false);
    LOG_INFO(Lib_NpMatching2, "MmSendRoomMessage: ctx={} room={} cast={} dst={} len={}", ctx_id,
             request.roomId, request.castType, req.dst_member_ids_size(), request.msgLen);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_SEND_ROOM_MESSAGE,
                           MmCommand::SendRoomMessage, MakeProtoPayload(req));
}

s32 MmSendRoomChatMessage(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                          const OrbisNpMatching2SendRoomMessageRequest& request) {
    const auto req = BuildRoomMessageRequest(request, true);
    LOG_INFO(Lib_NpMatching2, "MmSendRoomChatMessage: ctx={} room={} cast={} dst={} len={}", ctx_id,
             request.roomId, request.castType, req.dst_member_ids_size(), request.msgLen);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_SEND_ROOM_CHAT_MESSAGE,
                           MmCommand::SendRoomMessage, MakeProtoPayload(req));
}

s32 MmSetRoomMemberDataInternal(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                                const OrbisNpMatching2SetRoomMemberDataInternalRequest& request) {
    shadnet::SetRoomMemberDataInternalRequest req;
    req.set_req_id(req_id);
    req.set_room_id(request.roomId);
    req.set_member_id(request.memberId);
    req.set_flag_filter(request.flagFilter);
    req.set_flag_attr(request.flagAttr);
    // teamId 0 means "leave the team alone"; only a non-zero value is an assignment, so the
    // server must be able to tell the two apart instead of writing 0 over a real team.
    if (request.teamId != 0) {
        req.set_has_team_id(true);
        req.set_team_id(request.teamId);
    }
    u64 attr_bytes = 0;
    for (u64 i = 0; i < request.roomMemberBinAttrInternalNum; ++i) {
        AppendBinAttr(req.add_bin_attrs(), request.roomMemberBinAttrInternal[i]);
        attr_bytes += request.roomMemberBinAttrInternal[i].dataSize;
    }
    LOG_INFO(Lib_NpMatching2,
             "MmSetRoomMemberDataInternal: ctx={} room={} member={} team={} flagFilter={:#x} "
             "flagAttr={:#x} attrs={} bytes={}",
             ctx_id, request.roomId, request.memberId, request.teamId, request.flagFilter,
             request.flagAttr, request.roomMemberBinAttrInternalNum, attr_bytes);
    return MmSubmitRequest(ctx_id, req_id,
                           ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_ROOM_MEMBER_DATA_INTERNAL,
                           MmCommand::SetRoomMemberDataInternal, MakeProtoPayload(req));
}

s32 MmSetSignalingOptParam(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                           const OrbisNpMatching2SetSignalingOptParamRequest& request) {
    shadnet::SetSignalingOptParamRequest req;
    req.set_req_id(req_id);
    req.set_room_id(request.roomId);
    req.set_signaling_type(request.sigOptParam.type);
    req.set_signaling_flag(request.sigOptParam.flag);
    req.set_hub_member_id(request.sigOptParam.memberId);
    LOG_INFO(Lib_NpMatching2, "MmSetSignalingOptParam: ctx={} room={} type={} flag={} hub={}",
             ctx_id, request.roomId, static_cast<u32>(request.sigOptParam.type),
             static_cast<u32>(request.sigOptParam.flag), request.sigOptParam.memberId);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_SIGNALING_OPT_PARAM,
                           MmCommand::SetSignalingOptParam, MakeProtoPayload(req));
}

s32 MmSetRoomDataInternal(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                          const OrbisNpMatching2SetRoomDataInternalRequest& request) {
    shadnet::SetRoomDataInternalRequest req;
    req.set_req_id(req_id);
    req.set_room_id(request.roomId);
    req.set_flag_filter(request.flagFilter);
    req.set_flag_attr(request.flagAttr);
    LOG_INFO(Lib_NpMatching2,
             "ctx={} reqId={} room={} flags={:#x}/{:#x} binAttrs={} "
             "passwdMask={}",
             ctx_id, req_id, request.roomId, request.flagFilter, request.flagAttr,
             request.roomBinAttrInternalNum, fmt::ptr(request.passwordSlotMask));
    for (u64 i = 0; i < request.roomBinAttrInternalNum; ++i) {
        LogBinAttr("  internalBin", i, request.roomBinAttrInternal[i]);
        AppendBinAttr(req.add_bin_attrs(), request.roomBinAttrInternal[i]);
    }
    if (request.passwordSlotMask) {
        req.set_has_passwd_mask(true);
        req.set_passwd_slot_mask(*request.passwordSlotMask);
    }
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_ROOM_DATA_INTERNAL,
                           MmCommand::SetRoomDataInternal, MakeProtoPayload(req));
}

s32 MmSetRoomDataExternal(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                          const OrbisNpMatching2SetRoomDataExternalRequest& request) {
    shadnet::SetRoomDataExternalRequest req;
    req.set_req_id(req_id);
    req.set_room_id(request.roomId);
    LOG_INFO(Lib_NpMatching2,
             "MmSetRoomDataExternal: ctx={} reqId={} room={} searchInt={} searchBin={} extBin={}",
             ctx_id, req_id, request.roomId, request.roomSearchableIntAttrExternalNum,
             request.roomSearchableBinAttrExternalNum, request.roomBinAttrExternalNum);
    for (u64 i = 0; i < request.roomSearchableIntAttrExternalNum; ++i) {
        LogIntAttr("  externalSearchInt", i, request.roomSearchableIntAttrExternal[i]);
        AppendIntAttr(req.add_search_int_attrs(), request.roomSearchableIntAttrExternal[i]);
    }
    for (u64 i = 0; i < request.roomSearchableBinAttrExternalNum; ++i) {
        LogBinAttr("  externalSearchBin", i, request.roomSearchableBinAttrExternal[i]);
        AppendBinAttr(req.add_search_bin_attrs(), request.roomSearchableBinAttrExternal[i]);
    }
    for (u64 i = 0; i < request.roomBinAttrExternalNum; ++i) {
        LogBinAttr("  externalBin", i, request.roomBinAttrExternal[i]);
        AppendBinAttr(req.add_ext_bin_attrs(), request.roomBinAttrExternal[i]);
    }
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_SET_ROOM_DATA_EXTERNAL,
                           MmCommand::SetRoomDataExternal, MakeProtoPayload(req));
}

s32 MmKickoutRoomMember(OrbisNpMatching2ContextId ctx_id, OrbisNpMatching2RequestId req_id,
                        const OrbisNpMatching2KickoutRoomMemberRequest& request) {
    shadnet::KickoutRoomMemberRequest req;
    req.set_req_id(req_id);
    req.set_room_id(request.roomId);
    req.set_target_member_id(request.memberId);
    req.set_block_kick_flag(request.blockKickFlag);
    return MmSubmitRequest(ctx_id, req_id, ORBIS_NP_MATCHING2_REQUEST_EVENT_KICKOUT_ROOM_MEMBER,
                           MmCommand::KickoutRoomMember, MakeProtoPayload(req));
}

u32 GetMmServerAddr() {
    std::lock_guard lock(g_mm.mutex);
    return g_mm.server_addr;
}

u16 GetMmServerUdpPort() {
    std::lock_guard lock(g_mm.mutex);
    return g_mm.server_udp_port;
}

bool RequestSignalingInfoAsync(std::string_view target_online_id) {
    if (target_online_id.empty() || IsMatching2BackendDisabled()) {
        return false;
    }
    std::shared_ptr<ShadNet::ShadNetClient> client;
    {
        std::lock_guard lock(g_mm.mutex);
        client = g_mm.client;
    }
    if (!client || !client->IsAuthenticated()) {
        return false;
    }
    // Room events and replies are handled on the reader thread, which is the one that would
    // read this reply. Waiting there always times out, so leave it to the handshake thread.
    if (ShadNet::ShadNetClient::OnReaderThread()) {
        LOG_DEBUG(Lib_NpMatching2, "'{}': deferred to the handshake thread", target_online_id);
        return false;
    }

    const std::string target(target_online_id);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(g_mm.sig_mutex);
        const auto it = g_mm.sig_last_request.find(target);
        if (it != g_mm.sig_last_request.end() && now - it->second < kSignalingInfoQueryInterval) {
            return false;
        }
        g_mm.sig_last_request[target] = now;
    }

    shadnet::RequestSignalingInfosRequest req;
    req.set_target_npid(target);
    const u64 pkt_id = client->SubmitRequest(
        ShadNet::CommandType::RequestSignalingInfos, MakeProtoPayload(req), [&target](u64 id) {
            std::lock_guard lock(g_mm.sig_mutex);
            g_mm.sig_inflight[id] = {std::string(target),
                                     std::chrono::steady_clock::now() + kNpRequestTimeout};
        });
    LOG_DEBUG(Lib_NpMatching2, "signaling info query pkt_id={} target='{}'", pkt_id, target);
    return true;
}

bool LookupSignalingInfo(std::string_view target_online_id, u32* out_addr, u16* out_port,
                         u32* out_nat_type) {
    if (target_online_id.empty() || !out_addr || !out_port) {
        return false;
    }
    const std::string target(target_online_id);
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard lock(g_mm.sig_mutex);
    const auto it = g_mm.sig_cache.find(target);
    if (it == g_mm.sig_cache.end() || now - it->second.resolved_at > kSignalingInfoTtl) {
        return false;
    }
    *out_addr = it->second.addr;
    *out_port = it->second.port;
    if (out_nat_type) {
        *out_nat_type = it->second.nat_type;
    }
    return it->second.addr != 0 && it->second.port != 0;
}

bool ResolveSignalingInfo(std::string_view target_online_id, u32* out_addr, u16* out_port) {
    if (LookupSignalingInfo(target_online_id, out_addr, out_port, nullptr)) {
        bool stale = false;
        {
            std::lock_guard lock(g_mm.sig_mutex);
            const auto it = g_mm.sig_cache.find(std::string(target_online_id));
            stale =
                it != g_mm.sig_cache.end() &&
                std::chrono::steady_clock::now() - it->second.resolved_at > kSignalingInfoRefresh;
        }
        if (stale) {
            RequestSignalingInfoAsync(target_online_id);
        }
        return true;
    }
    RequestSignalingInfoAsync(target_online_id);
    return false;
}

bool IsSignalingRelayEnabled() {
    std::lock_guard lock(g_mm.mutex);
    return g_mm.relay_enabled;
}

u16 GetStunAltPort() {
    std::lock_guard lock(g_mm.mutex);
    return g_mm.stun_alt_port;
}

} // namespace Libraries::Np::NpMatching2
