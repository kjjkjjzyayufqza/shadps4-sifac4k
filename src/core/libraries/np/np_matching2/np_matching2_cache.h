// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include "common/types.h"
#include "core/libraries/np/np_matching2/np_matching2.h"
#include "core/libraries/np/np_types.h"

namespace Libraries::Np::NpMatching2 {

struct PeerInfo {
    u32 addr = 0;
    u16 port = 0;
    OrbisNpMatching2RoomMemberId member_id = 0;
    s32 conn_id = 0;
    s32 status = 0;
    Libraries::Np::OrbisNpOnlineId online_id{};
    bool handshake_started = false;
    bool sent_check = false;
    bool sent_established = false;
    u64 nonce = 0;
    u32 ping_us = 0;
    // Recent echo samples. The published ping is their median, so one slow handshake
    // round does not pin the room antenna on its worst bar.
    u32 ping_samples[5]{};
    u8 ping_sample_count = 0;
    u8 ping_sample_pos = 0;
    u32 packet_loss_pct = 0;
    u32 bandwidth_bps = 0;
    u64 ping_seq = 0;
    bool ping_pending = false;
    u32 pings_resolved = 0;
    u32 pings_lost = 0;
    bool relay_active = false;
    bool relay_warned = false;
    // Deadlines are measured from the first attempt, not from the last successful send: a peer
    // whose endpoint never resolves sends nothing at all, and must still be declared dead.
    std::chrono::steady_clock::time_point first_attempt{};
    std::chrono::steady_clock::time_point last_attempt{};
    std::chrono::steady_clock::time_point last_resolve_request{};
    std::chrono::steady_clock::time_point last_send{};
    std::chrono::steady_clock::time_point last_check_send{};
    std::chrono::steady_clock::time_point last_ping_send{};
    u32 handshake_sent = 0;
    u32 handshake_recv = 0;
    u32 resolve_failures = 0;
    s64 active_since_us = 0;
};

// Room-level binary attribute stored by value. The guest-facing struct holds a raw pointer,
// so the cache must not keep those pointers into a vector that grows on every update.
struct RoomBinCache {
    OrbisNpMatching2AttributeId id = 0;
    u64 update_date = 0;
    OrbisNpMatching2RoomMemberId member_id = 0;
    std::vector<u8> data;
};

struct MemberBinCache {
    OrbisNpMatching2AttributeId id = 0;
    u64 update_date = 0;
    std::vector<u8> data;
};

struct MemberCache {
    Libraries::Np::OrbisNpId np_id{};
    Libraries::Np::OrbisNpAccountId account_id = 0;
    Libraries::Np::OrbisNpPlatformType platform = Libraries::Np::OrbisNpPlatformType::None;
    u64 join_date = 0;
    OrbisNpMatching2RoomMemberId member_id = 0;
    OrbisNpMatching2TeamId team_id = 0;
    OrbisNpMatching2RoomGroupId group_id = 0;
    OrbisNpMatching2NatType nat_type = 0;
    OrbisNpMatching2Flags flag_attr = 0;
    u32 addr = 0;
    u16 port = 0;
    std::map<OrbisNpMatching2AttributeId, MemberBinCache> bins;
};

struct RoomCache {
    u32 num_slots = 0;
    u64 mask_password = 0;
    OrbisNpMatching2SignalingType signaling_type = ORBIS_NP_MATCHING2_SIGNALING_TYPE_MESH;
    OrbisNpMatching2RoomMemberId signaling_main_member = 0;
    OrbisNpMatching2ServerId server_id = 0;
    OrbisNpMatching2WorldId world_id = 0;
    OrbisNpMatching2LobbyId lobby_id = 0;
    OrbisNpMatching2RoomId room_id = 0;
    u16 max_slot = 0;
    u16 public_slots = 0;
    u16 private_slots = 0;
    u16 open_public_slots = 0;
    u16 open_private_slots = 0;
    u64 passwd_slot_mask = 0;
    u64 joined_slot_mask = 0;
    OrbisNpMatching2Flags flags = 0;
    std::vector<RoomBinCache> room_bins;
    std::vector<OrbisNpMatching2RoomBinAttrInternal> bin_attrs_internal;
    std::vector<std::vector<u8>> bin_buffers;
    std::map<OrbisNpMatching2RoomGroupId, OrbisNpMatching2RoomGroup> groups;
    std::map<OrbisNpMatching2RoomMemberId, MemberCache> members;
    bool owner = false;
    // Owner bookkeeping kept in sync with ROOM_EVENT_ROOM_OWNER_CHANGED so the local getters and
    // GetRoomDataInternal keep naming the member the server currently considers the owner.
    OrbisNpMatching2RoomMemberId owner_member_id = 0;
    // Room password as this client supplied it when creating or joining. The matching service
    // never echoes it back, so GetRoomPasswordLocal can only answer from what we sent.
    bool has_password = false;
    OrbisNpMatching2SessionPassword password{};
    // Signaling option parameters for the room, as last set through SetSignalingOptParam.
    OrbisNpMatching2SignalingOptParam signaling_opt_param{};
    bool has_signaling_opt_param = false;
};

struct Matching2ContextCache {
    mutable std::recursive_mutex mutex;
    std::map<OrbisNpMatching2RoomMemberId, PeerInfo> peers;
    std::map<OrbisNpMatching2RoomId, RoomCache> rooms;

    void Reset() {
        peers.clear();
        rooms.clear();
    }
};

} // namespace Libraries::Np::NpMatching2
