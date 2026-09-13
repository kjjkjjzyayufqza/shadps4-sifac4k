// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "common/logging/log.h"
#include "core/libraries/network/net.h"
#include "core/libraries/network/sockets.h"
#include "core/libraries/np/np_error.h"
#include "core/libraries/np/np_matching2/np_matching2_internal.h"
#include "core/libraries/np/np_matching2/np_matching2_mm.h"
#include "core/libraries/np/np_matching2/np_matching2_signaling.h"
#include "core/libraries/np/np_signaling/np_signaling_stubs.h"
#include "core/libraries/np/np_utility/np_utility.h"

namespace Libraries::Np::NpMatching2 {

namespace {

constexpr s32 kMatching2ConnInactive = 0;
constexpr s32 kMatching2ConnPending = 1;
constexpr s32 kMatching2ConnActive = 2;
constexpr auto kMatching2HandshakeRetry = std::chrono::milliseconds(350);
// Measured from the first attempt, not the last send: a peer whose endpoint never resolves sends
// nothing at all and still has to be declared dead, or the title waits for an event that never
// comes and its "joining room" screen never ends.
constexpr auto kMatching2HandshakeTimeout = std::chrono::seconds(20);
// Two NATs that were going to open for a direct punch have done so long before this. Past it the
// handshake keeps retrying but through the server relay, which is the only path left for a peer
// behind a symmetric NAT or CGNAT.
constexpr auto kMatching2RelayAfter = std::chrono::seconds(6);
constexpr auto kMatching2StunPingInterval = std::chrono::seconds(5);
constexpr auto kMatching2PingInterval = std::chrono::seconds(1);
// One line per interval describing every peer, so a session that will not connect can be
// diagnosed from the log alone instead of from a rebuild with extra prints.
constexpr auto kMatching2StatusInterval = std::chrono::seconds(5);
constexpr u32 kMatching2LossWindow = 32;
// Cap so a flooded socket cannot starve the periodic handshake and ping work below.
constexpr u32 kMatching2MaxDrainPerTick = 64;

std::atomic<bool> g_matching2_stop{false};
std::mutex g_matching2_thread_mutex;
std::thread g_matching2_thread;

enum class Matching2HandshakeKind : u8 {
    Offer = 1,
    Accept = 2,
    Check = 3,
    CheckAck = 4,
    Ping = 5,
    Pong = 6,
};

#pragma pack(push, 1)
struct Matching2HandshakePacket {
    u8 magic[4] = {'S', 'H', 'A', 'D'};
    u8 type = 0x21;
    u8 kind = 0;
    u64 room_id = 0;
    u16 from_member_id = 0;
    u16 to_member_id = 0;
    u8 online_id_from[16]{};
    u32 mapped_addr = 0;
    u16 mapped_port = 0;
    // libSceNpMatching2 does not measure the peer link itself: connection info type 2 returns a
    // value the peer advertised in signaling, so carry it here and report it back verbatim.
    u32 bandwidth_bps = 0;
    u64 nonce = 0;
};
#pragma pack(pop)
static_assert(sizeof(Matching2HandshakePacket) == 0x34);

// 0x01 asks the primary STUN endpoint, 0x03 the alternate one; the alternate answers with a
// tagged echo so one socket can tell the two replies apart. Mirrors shadNet's stun_server.cpp.
constexpr u8 kMatching2StunPingCmd = 0x01;
constexpr u8 kMatching2StunAltPingCmd = 0x03;

#pragma pack(push, 1)
struct Matching2StunPing {
    u8 cmd = kMatching2StunPingCmd;
    u8 online_id[ORBIS_NP_ONLINEID_MAX_LENGTH]{};
    u32 local_ip = 0;
};
#pragma pack(pop)
static_assert(sizeof(Matching2StunPing) == 21);

bool HasMatching2Magic(const Matching2HandshakePacket& pkt) {
    return pkt.magic[0] == 'S' && pkt.magic[1] == 'H' && pkt.magic[2] == 'A' &&
           pkt.magic[3] == 'D' && pkt.type == 0x21;
}

std::string OnlineIdToString(const Libraries::Np::OrbisNpOnlineId& online_id) {
    char buf[ORBIS_NP_ONLINEID_MAX_LENGTH + 1]{};
    std::memcpy(buf, online_id.data, ORBIS_NP_ONLINEID_MAX_LENGTH);
    return std::string(buf);
}

bool ShouldConnectToPeer(const RoomCache& room, OrbisNpMatching2RoomMemberId self,
                         OrbisNpMatching2RoomMemberId peer) {
    if (peer == 0 || peer == self) {
        return false;
    }
    if (room.signaling_type == ORBIS_NP_MATCHING2_SIGNALING_TYPE_NONE) {
        return false;
    }
    if (room.signaling_type == ORBIS_NP_MATCHING2_SIGNALING_TYPE_STAR &&
        room.signaling_main_member != 0) {
        return self == room.signaling_main_member || peer == room.signaling_main_member;
    }
    return true;
}

const char* Matching2ConnStateName(s32 status) {
    switch (status) {
    case kMatching2ConnInactive:
        return "inactive";
    case kMatching2ConnPending:
        return "pending";
    case kMatching2ConnActive:
        return "active";
    default:
        return "unknown";
    }
}

// Never blocks. Room callbacks reach this from the ShadNet reader thread, and that thread is the
// only one that can deliver a signaling-info reply, so asking the server here and waiting for the
// answer would deadlock. An unknown endpoint schedules the query instead and the handshake thread
// picks the answer up on a later retry.
bool ResolvePeerEndpoint(const MemberCache& member, PeerInfo& peer) {
    if (peer.online_id.data[0] == 0) {
        peer.online_id = member.np_id.handle;
    }
    if (peer.addr != 0 && peer.port != 0) {
        return true;
    }
    if (member.addr != 0 && member.port != 0) {
        peer.addr = member.addr;
        peer.port = member.port;
        LOG_INFO(Lib_NpMatching2, "Matching2 peer {} endpoint from room data: {:#010x}:{}",
                 member.member_id, Libraries::Net::sceNetNtohl(peer.addr),
                 Libraries::Net::sceNetNtohs(peer.port));
        return true;
    }

    const std::string online_id(member.np_id.handle.data);
    if (online_id.empty()) {
        return false;
    }
    u32 resolved_addr = 0;
    u16 resolved_port = 0;
    if (ResolveSignalingInfo(online_id, &resolved_addr, &resolved_port)) {
        peer.addr = resolved_addr;
        peer.port = resolved_port;
        LOG_INFO(Lib_NpMatching2, "Matching2 peer {} ('{}') endpoint from server: {:#010x}:{}",
                 member.member_id, online_id, Libraries::Net::sceNetNtohl(peer.addr),
                 Libraries::Net::sceNetNtohs(peer.port));
        return true;
    }

    ++peer.resolve_failures;
    peer.last_resolve_request = std::chrono::steady_clock::now();
    if (peer.resolve_failures == 1) {
        LOG_WARNING(Lib_NpMatching2,
                    "Matching2 peer {} ('{}') has no endpoint yet: the room record carries no "
                    "port and the server has no STUN entry. Waiting on the lookup.",
                    member.member_id, online_id);
    }
    return false;
}

void MarkMatching2PeerActive(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                             OrbisNpMatching2RoomMemberId member_id, u32 addr, u16 port) {
    if (member_id == 0 || member_id == ctx.my_member_id) {
        return;
    }

    PeerInfo& peer = ctx.peers[member_id];
    peer.member_id = member_id;
    if (addr != 0) {
        peer.addr = addr;
    }
    if (port != 0) {
        peer.port = port;
    }
    const bool first_active = peer.status != kMatching2ConnActive;
    peer.status = kMatching2ConnActive;
    peer.handshake_started = true;

    if (first_active && !peer.sent_established) {
        peer.sent_established = true;
        QueueMatching2SignalingEvent(ctx, room_id, member_id,
                                     ORBIS_NP_MATCHING2_SIGNALING_EVENT_ESTABLISHED, ORBIS_OK);
        const auto elapsed_ms = peer.first_attempt.time_since_epoch().count() == 0
                                    ? 0
                                    : std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - peer.first_attempt)
                                          .count();
        LOG_INFO(Lib_NpMatching2,
                 "Matching2 signaling established: ctx={} room={} member={} addr={:#010x}:{} "
                 "path={} after={}ms sent={} recv={}",
                 ctx.ctx_id, room_id, member_id, Libraries::Net::sceNetNtohl(peer.addr),
                 Libraries::Net::sceNetNtohs(peer.port), peer.relay_active ? "relay" : "direct",
                 elapsed_ms, peer.handshake_sent, peer.handshake_recv);
    }
}

bool SendMatching2Handshake(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                            OrbisNpMatching2RoomMemberId member_id, Matching2HandshakeKind kind,
                            u64 nonce) {
    auto room_it = ctx.room_cache.find(room_id);
    if (room_it == ctx.room_cache.end()) {
        return false;
    }
    auto member_it = room_it->second.members.find(member_id);
    if (member_it == room_it->second.members.end()) {
        return false;
    }

    PeerInfo& peer = ctx.peers[member_id];
    peer.member_id = member_id;
    const auto attempt_now = std::chrono::steady_clock::now();
    // Stamped before the endpoint is known. The retry gate and the dead-peer deadline both read
    // these, so an attempt that cannot even be addressed still has to advance them.
    if (peer.first_attempt.time_since_epoch().count() == 0) {
        peer.first_attempt = attempt_now;
    }
    peer.last_attempt = attempt_now;
    if (!ResolvePeerEndpoint(member_it->second, peer)) {
        LOG_DEBUG(Lib_NpMatching2,
                  "Matching2 signaling: unresolved endpoint room={} member={} attempts={}", room_id,
                  member_id, peer.resolve_failures);
        return false;
    }

    Matching2HandshakePacket pkt{};
    pkt.kind = static_cast<u8>(kind);
    pkt.room_id = room_id;
    pkt.from_member_id = ctx.my_member_id;
    pkt.to_member_id = member_id;
    std::memcpy(pkt.online_id_from, ctx.online_id.data, ORBIS_NP_ONLINEID_MAX_LENGTH);
    pkt.mapped_addr = Net::GetP2PAdvertisedAddr();
    pkt.mapped_port = Net::GetP2PConfiguredPort() != 0
                          ? Libraries::Net::sceNetHtons(Net::GetP2PConfiguredPort())
                          : 0;
    pkt.bandwidth_bps = NpUtility::GetLastMeasuredUploadBps();
    pkt.nonce = nonce;

    const int rc = Net::P2PMatching2SendTo(&pkt, sizeof(pkt), peer.addr, peer.port);
    peer.last_send = attempt_now;
    ++peer.handshake_sent;
    if (kind == Matching2HandshakeKind::Check) {
        peer.last_check_send = attempt_now;
    }
    if (rc < 0) {
        LOG_WARNING(Lib_NpMatching2,
                    "Matching2 handshake send failed kind={} ctx={} room={} {}->{} "
                    "dst={:#010x}:{} path={}",
                    static_cast<u8>(kind), ctx.ctx_id, room_id, ctx.my_member_id, member_id,
                    Libraries::Net::sceNetNtohl(peer.addr), Libraries::Net::sceNetNtohs(peer.port),
                    peer.relay_active ? "relay" : "direct");
        return false;
    }
    LOG_DEBUG(Lib_NpMatching2,
              "Matching2 handshake send kind={} ctx={} room={} {}->{} dst={:#010x}:{} path={}",
              static_cast<u8>(kind), ctx.ctx_id, room_id, ctx.my_member_id, member_id,
              Libraries::Net::sceNetNtohl(peer.addr), Libraries::Net::sceNetNtohs(peer.port),
              peer.relay_active ? "relay" : "direct");
    return true;
}

// Connection quality is reported per resolved ping: a pong answers its ping, and a ping still
// unanswered when the next one falls due is a lost packet. The counters halve once the window is
// full so the rate follows the current link instead of the whole session.
void UpdateMatching2PacketLoss(PeerInfo& peer, bool lost) {
    ++peer.pings_resolved;
    if (lost) {
        ++peer.pings_lost;
    }
    peer.packet_loss_pct = (peer.pings_lost * 100) / peer.pings_resolved;
    if (peer.pings_resolved >= kMatching2LossWindow) {
        peer.pings_resolved /= 2;
        peer.pings_lost /= 2;
    }
}

// The handshake only ever produces one RTT sample, but titles poll GetConnectionInfo for as long
// as the connection lives and show the link quality from it, so an established peer keeps being
// measured.
void MaybeSendMatching2Ping(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                            OrbisNpMatching2RoomMemberId member_id, PeerInfo& peer,
                            std::chrono::steady_clock::time_point now) {
    if (peer.last_ping_send.time_since_epoch().count() != 0 &&
        now - peer.last_ping_send < kMatching2PingInterval) {
        return;
    }
    if (peer.ping_pending) {
        UpdateMatching2PacketLoss(peer, true);
    }
    ++peer.ping_seq;
    peer.ping_pending = true;
    peer.last_ping_send = now;
    SendMatching2Handshake(ctx, room_id, member_id, Matching2HandshakeKind::Ping, peer.ping_seq);
}

ContextObject* FindContextForMatching2Packet(const Matching2HandshakePacket& pkt) {
    for (u32 id = 1; id <= ContextManager::kMaxContexts; ++id) {
        ContextObject* ctx =
            ContextManager::Instance().Get(static_cast<OrbisNpMatching2ContextId>(id));
        if (!ctx || ctx->room_id != pkt.room_id || ctx->my_member_id != pkt.to_member_id) {
            continue;
        }
        if (ctx->room_cache.find(static_cast<OrbisNpMatching2RoomId>(pkt.room_id)) !=
            ctx->room_cache.end()) {
            return ctx;
        }
    }
    return nullptr;
}

void HandleMatching2HandshakePacket(u32 from_addr, u16 from_port, bool relayed,
                                    const Matching2HandshakePacket& pkt) {
    if (!HasMatching2Magic(pkt)) {
        return;
    }

    ContextObject* ctx = FindContextForMatching2Packet(pkt);
    if (!ctx) {
        LOG_DEBUG(Lib_NpMatching2, "Matching2 handshake: no ctx for room={} to_member={}",
                  pkt.room_id, pkt.to_member_id);
        return;
    }

    const auto room_id = static_cast<OrbisNpMatching2RoomId>(pkt.room_id);
    const auto member_id = static_cast<OrbisNpMatching2RoomMemberId>(pkt.from_member_id);
    auto room_it = ctx->room_cache.find(room_id);
    if (room_it == ctx->room_cache.end() ||
        !ShouldConnectToPeer(room_it->second, ctx->my_member_id, member_id)) {
        return;
    }

    PeerInfo& peer = ctx->peers[member_id];
    peer.member_id = member_id;
    ++peer.handshake_recv;
    // Answer the observed source: it is the path this packet already travelled. The sender's
    // mapped address is its own interface address, which a wildcard or NATed bind cannot be
    // reached on from the peer.
    if ((peer.addr != from_addr || peer.port != from_port) && peer.relay_active && peer.addr != 0 &&
        peer.port != 0) {
        // The relay marking is keyed on the endpoint, so it has to move with it.
        Net::SetP2PPeerRelayed(peer.addr, peer.port, false);
        peer.relay_active = false;
    }
    peer.addr = from_addr;
    peer.port = from_port;
    if (relayed && !peer.relay_active) {
        // The peer had to give up on the direct path; answering it directly would go nowhere, so
        // this side follows it onto the relay instead of waiting out its own timer.
        peer.relay_active = true;
        Net::SetP2PPeerRelayed(peer.addr, peer.port, true);
        LOG_WARNING(Lib_NpMatching2,
                    "Matching2 peer {} reached us through the relay; switching this direction to "
                    "the relay as well",
                    member_id);
    }
    peer.status =
        peer.status == kMatching2ConnActive ? kMatching2ConnActive : kMatching2ConnPending;
    peer.handshake_started = true;
    SetNpOnlineId(peer.online_id,
                  std::string_view(reinterpret_cast<const char*>(pkt.online_id_from),
                                   ORBIS_NP_ONLINEID_MAX_LENGTH));

    peer.bandwidth_bps = pkt.bandwidth_bps;

    const auto kind = static_cast<Matching2HandshakeKind>(pkt.kind);
    switch (kind) {
    case Matching2HandshakeKind::Offer:
        SendMatching2Handshake(*ctx, room_id, member_id, Matching2HandshakeKind::Accept, 0);
        break;
    case Matching2HandshakeKind::Accept:
        peer.sent_check = true;
        SendMatching2Handshake(*ctx, room_id, member_id, Matching2HandshakeKind::Check, peer.nonce);
        break;
    case Matching2HandshakeKind::Check:
        SendMatching2Handshake(*ctx, room_id, member_id, Matching2HandshakeKind::CheckAck,
                               pkt.nonce);
        MarkMatching2PeerActive(*ctx, room_id, member_id, peer.addr, peer.port);
        break;
    case Matching2HandshakeKind::CheckAck:
        if (pkt.nonce == 0 || pkt.nonce == peer.nonce) {
            if (peer.last_check_send.time_since_epoch().count() != 0) {
                const auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - peer.last_check_send);
                peer.ping_us =
                    static_cast<u32>(std::min<u64>(static_cast<u64>(std::max<s64>(0, rtt.count())),
                                                   std::numeric_limits<u32>::max()));
                peer.last_check_send = {};
            }
            MarkMatching2PeerActive(*ctx, room_id, member_id, peer.addr, peer.port);
        }
        break;
    case Matching2HandshakeKind::Ping:
        SendMatching2Handshake(*ctx, room_id, member_id, Matching2HandshakeKind::Pong, pkt.nonce);
        break;
    case Matching2HandshakeKind::Pong:
        if (peer.ping_pending && pkt.nonce == peer.ping_seq) {
            const auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - peer.last_ping_send);
            const u32 sample = static_cast<u32>(std::min<u64>(
                static_cast<u64>(std::max<s64>(0, rtt.count())), std::numeric_limits<u32>::max()));
            // A single sample carries the scheduling jitter of both ends; smooth it the way a
            // signaling stack does so the reported value is stable between frames.
            peer.ping_us = peer.ping_us == 0 ? sample : (peer.ping_us * 3 + sample) / 4;
            peer.ping_pending = false;
            UpdateMatching2PacketLoss(peer, false);
        }
        break;
    default:
        break;
    }
}

// Escalates one stalled peer onto the server relay. Direct punching has had its chance by the
// time this runs, so the alternative is not connecting at all.
void MaybeEscalateToRelay(OrbisNpMatching2RoomMemberId member_id, PeerInfo& peer,
                          std::chrono::steady_clock::time_point now) {
    if (peer.relay_active || peer.addr == 0 || peer.port == 0) {
        return;
    }
    if (peer.first_attempt.time_since_epoch().count() == 0 ||
        now - peer.first_attempt < kMatching2RelayAfter) {
        return;
    }
    if (!IsSignalingRelayEnabled()) {
        if (!peer.relay_warned) {
            peer.relay_warned = true;
            LOG_WARNING(Lib_NpMatching2,
                        "Matching2 peer {} has not answered a direct punch and this server offers "
                        "no relay; the connection will time out",
                        member_id);
        }
        return;
    }
    peer.relay_active = true;
    Net::SetP2PPeerRelayed(peer.addr, peer.port, true);
    LOG_WARNING(Lib_NpMatching2,
                "Matching2 peer {} did not answer a direct punch in {} s; retrying through the "
                "server relay at {:#010x}:{}",
                member_id, kMatching2RelayAfter.count(), Libraries::Net::sceNetNtohl(peer.addr),
                Libraries::Net::sceNetNtohs(peer.port));
}

void LogMatching2PeerStatus(const ContextObject& ctx) {
    if (ctx.peers.empty()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const Libraries::Net::P2PPortStats stats = Net::GetP2PTransportStats();
    LOG_INFO(Lib_NpMatching2,
             "Matching2 status: ctx={} room={} self={} peers={} port={} advertised={:#010x} "
             "tx(direct={} relay={} failed={}) rx(socket={} relay={} rejected={})",
             ctx.ctx_id, ctx.room_id, ctx.my_member_id, ctx.peers.size(),
             Net::GetP2PConfiguredPort(), Libraries::Net::sceNetNtohl(Net::GetP2PAdvertisedAddr()),
             stats.sent_direct, stats.sent_relayed, stats.send_failed, stats.recv_socket,
             stats.recv_relayed, stats.recv_relay_rejected);
    for (const auto& [member_id, peer] : ctx.peers) {
        const auto age_ms =
            peer.first_attempt.time_since_epoch().count() == 0
                ? 0
                : std::chrono::duration_cast<std::chrono::milliseconds>(now - peer.first_attempt)
                      .count();
        LOG_INFO(Lib_NpMatching2,
                 "  peer {} '{}' state={} path={} endpoint={:#010x}:{} sent={} recv={} "
                 "unresolved={} age={}ms rtt={}us loss={}%",
                 member_id, OnlineIdToString(peer.online_id), Matching2ConnStateName(peer.status),
                 peer.relay_active ? "relay" : "direct", Libraries::Net::sceNetNtohl(peer.addr),
                 Libraries::Net::sceNetNtohs(peer.port), peer.handshake_sent, peer.handshake_recv,
                 peer.resolve_failures, age_ms, peer.ping_us, peer.packet_loss_pct);
    }
}

void Matching2HandshakeThreadMain() {
    auto last_stun_ping = std::chrono::steady_clock::time_point{};
    auto last_status_log = std::chrono::steady_clock::time_point{};
    while (!g_matching2_stop.load(std::memory_order_relaxed)) {
        for (u32 drained = 0; drained < kMatching2MaxDrainPerTick; ++drained) {
            Matching2HandshakePacket pkt{};
            u32 from_addr = 0;
            u16 from_port = 0;
            bool relayed = false;
            const int rc =
                Net::P2PMatching2RecvFrom(&pkt, sizeof(pkt), &from_addr, &from_port, &relayed);
            if (rc != sizeof(pkt)) {
                break;
            }
            HandleMatching2HandshakePacket(from_addr, from_port, relayed, pkt);
        }

        const auto now = std::chrono::steady_clock::now();
        const bool should_stun_ping = last_stun_ping.time_since_epoch().count() == 0 ||
                                      now - last_stun_ping >= kMatching2StunPingInterval;
        const bool should_log_status = last_status_log.time_since_epoch().count() == 0 ||
                                       now - last_status_log >= kMatching2StatusInterval;
        if (should_stun_ping) {
            // The relay lives on the matching server's STUN endpoint; keeping this in step with
            // the ping means it is set as soon as the server address is known and cleared again
            // when the client disconnects.
            Net::SetP2PRelayEndpoint(NpSignaling::Stubs::MmServerAddr(),
                                     NpSignaling::Stubs::MmServerUdpPort());
        }
        for (u32 id = 1; id <= ContextManager::kMaxContexts; ++id) {
            ContextObject* ctx =
                ContextManager::Instance().Get(static_cast<OrbisNpMatching2ContextId>(id));
            if (!ctx) {
                continue;
            }
            if (should_stun_ping) {
                SendMatching2StunPing(*ctx);
            }
            if (ctx->room_id == 0) {
                continue;
            }
            auto room_it = ctx->room_cache.find(ctx->room_id);
            if (room_it == ctx->room_cache.end()) {
                continue;
            }
            if (should_log_status) {
                LogMatching2PeerStatus(*ctx);
            }
            for (auto& [member_id, peer] : ctx->peers) {
                if (peer.status == kMatching2ConnActive) {
                    MaybeSendMatching2Ping(*ctx, ctx->room_id, member_id, peer, now);
                    continue;
                }
                if (peer.status != kMatching2ConnPending || !peer.handshake_started) {
                    continue;
                }
                const bool started = peer.first_attempt.time_since_epoch().count() != 0;
                if (started && now - peer.first_attempt > kMatching2HandshakeTimeout) {
                    peer.status = kMatching2ConnInactive;
                    if (peer.relay_active && peer.addr != 0 && peer.port != 0) {
                        Net::SetP2PPeerRelayed(peer.addr, peer.port, false);
                        peer.relay_active = false;
                    }
                    LOG_ERROR(Lib_NpMatching2,
                              "Matching2 peer {} gave up after {} s: endpoint={:#010x}:{} sent={} "
                              "recv={} unresolvedAttempts={}. {}",
                              member_id, kMatching2HandshakeTimeout.count(),
                              Libraries::Net::sceNetNtohl(peer.addr),
                              Libraries::Net::sceNetNtohs(peer.port), peer.handshake_sent,
                              peer.handshake_recv, peer.resolve_failures,
                              peer.addr == 0 ? "The server never reported an endpoint for it - its "
                                               "UDP path to the STUN port is probably blocked."
                                             : "Its endpoint was known but never answered.");
                    QueueMatching2SignalingEvent(*ctx, ctx->room_id, member_id,
                                                 ORBIS_NP_MATCHING2_SIGNALING_EVENT_DEAD,
                                                 ORBIS_NP_MATCHING2_SIGNALING_ERROR_TIMEOUT);
                    continue;
                }
                MaybeEscalateToRelay(member_id, peer, now);
                if (!started || now - peer.last_attempt >= kMatching2HandshakeRetry) {
                    SendMatching2Handshake(*ctx, ctx->room_id, member_id,
                                           peer.sent_check ? Matching2HandshakeKind::Check
                                                           : Matching2HandshakeKind::Offer,
                                           peer.sent_check ? peer.nonce : 0);
                }
            }
        }
        if (should_stun_ping) {
            last_stun_ping = now;
        }
        if (should_log_status) {
            last_status_log = now;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace

bool SendMatching2StunPing(const ContextObject& ctx) {
    if (!NpSignaling::Stubs::Matching2Enabled()) {
        return false;
    }
    if (!ctx.started || ctx.online_id.data[0] == '\0') {
        return false;
    }
    if (!NpSignaling::Stubs::EnsureTransport()) {
        return false;
    }

    const u32 server_addr = NpSignaling::Stubs::MmServerAddr();
    const u16 server_udp = NpSignaling::Stubs::MmServerUdpPort();
    if (server_addr == 0 || server_udp == 0) {
        return false;
    }

    Matching2StunPing ping{};
    ping.cmd = kMatching2StunPingCmd;
    std::memcpy(ping.online_id, ctx.online_id.data, ORBIS_NP_ONLINEID_MAX_LENGTH);
    ping.local_ip = NpSignaling::Stubs::AdvertisedAddr();

    const int rc =
        NpSignaling::Stubs::SignalingSendTo(&ping, sizeof(ping), server_addr, server_udp);
    LOG_DEBUG(Lib_NpMatching2,
              "Matching2 STUN ping: ctx={} online_id='{}' server={:#x}:{} local_ip={:#x} rc={}",
              ctx.ctx_id, OnlineIdToString(ctx.online_id), server_addr,
              Libraries::Net::sceNetNtohs(server_udp), ping.local_ip, rc);

    // Same socket, second destination. Comparing the two mappings the server reports is the only
    // way to tell a NAT a punch can get through from one that picks a fresh mapping per peer, and
    // these titles never open an NpSignaling context, so this cannot be left to that path.
    const u16 alt_port = GetStunAltPort();
    if (alt_port != 0) {
        ping.cmd = kMatching2StunAltPingCmd;
        NpSignaling::Stubs::SignalingSendTo(&ping, sizeof(ping), server_addr,
                                            Libraries::Net::sceNetHtons(alt_port));
    }
    return rc >= 0;
}

void QueueMatching2SignalingEvent(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                  OrbisNpMatching2RoomMemberId member_id,
                                  OrbisNpMatching2Event event, s32 error_code) {
    PendingEvent ev{};
    ev.type = PendingEvent::SIGNALING_CB;
    ev.ctx_id = ctx.ctx_id;
    ev.fire_at = std::chrono::steady_clock::now();
    ev.room_id = room_id;
    ev.member_id = member_id;
    ev.sig_event = event;
    ev.error_code = error_code;
    ScheduleEvent(std::move(ev));
}

void StartMatching2PeerHandshake(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                 OrbisNpMatching2RoomMemberId member_id) {
    auto room_it = ctx.room_cache.find(room_id);
    if (room_it == ctx.room_cache.end() ||
        !ShouldConnectToPeer(room_it->second, ctx.my_member_id, member_id)) {
        return;
    }
    auto member_it = room_it->second.members.find(member_id);
    if (member_it == room_it->second.members.end()) {
        return;
    }

    Net::EnsureP2PTransport();

    PeerInfo& peer = ctx.peers[member_id];
    peer.member_id = member_id;
    if (peer.status == kMatching2ConnActive) {
        return;
    }
    ResolvePeerEndpoint(member_it->second, peer);
    peer.status = kMatching2ConnPending;
    peer.handshake_started = true;
    peer.sent_check = false;
    if (peer.first_attempt.time_since_epoch().count() == 0) {
        peer.first_attempt = std::chrono::steady_clock::now();
    }
    if (peer.nonce == 0) {
        peer.nonce = static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count() ^
                                      (static_cast<u64>(ctx.ctx_id) << 48) ^
                                      (static_cast<u64>(member_id) << 16));
    }
    SendMatching2Handshake(ctx, room_id, member_id, Matching2HandshakeKind::Offer, 0);
}

void StartMatching2SignalingForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id) {
    const auto room_it = ctx.room_cache.find(room_id);
    if (room_it == ctx.room_cache.end()) {
        return;
    }
    for (const auto& [member_id, member] : room_it->second.members) {
        StartMatching2PeerHandshake(ctx, room_id, member_id);
    }
}

void QueueMatching2DeadForRoomPeers(ContextObject& ctx, OrbisNpMatching2RoomId room_id,
                                    s32 error_code) {
    auto room_it = ctx.room_cache.find(room_id);
    if (room_it == ctx.room_cache.end()) {
        return;
    }

    for (const auto& [member_id, member] : room_it->second.members) {
        if (member_id == 0 || member_id == ctx.my_member_id) {
            continue;
        }

        auto peer_it = ctx.peers.find(member_id);
        if (peer_it != ctx.peers.end()) {
            PeerInfo& peer = peer_it->second;
            if (peer.relay_active && peer.addr != 0 && peer.port != 0) {
                Net::SetP2PPeerRelayed(peer.addr, peer.port, false);
            }
            peer.relay_active = false;
            peer.status = kMatching2ConnInactive;
            peer.handshake_started = false;
            peer.sent_check = false;
            peer.first_attempt = {};
            peer.last_attempt = {};
        }

        QueueMatching2SignalingEvent(ctx, room_id, member_id,
                                     ORBIS_NP_MATCHING2_SIGNALING_EVENT_DEAD, error_code);
        LOG_INFO(Lib_NpMatching2, "Matching2 signaling dead: ctx={} room={} member={} reason={:#x}",
                 ctx.ctx_id, room_id, member_id, static_cast<u32>(error_code));
    }
}

void StartMatching2HandshakeThread() {
    std::lock_guard lock(g_matching2_thread_mutex);
    if (g_matching2_thread.joinable()) {
        return;
    }
    g_matching2_stop.store(false, std::memory_order_relaxed);
    g_matching2_thread = std::thread(Matching2HandshakeThreadMain);
}

void StopMatching2HandshakeThread() {
    {
        std::lock_guard lock(g_matching2_thread_mutex);
        g_matching2_stop.store(true, std::memory_order_relaxed);
    }
    if (g_matching2_thread.joinable()) {
        g_matching2_thread.join();
    }
    Net::SetP2PRelayEndpoint(0, 0);
}

u32 GetRoomPingUs(const ContextObject& ctx, OrbisNpMatching2RoomId roomId) {
    const auto room_it = ctx.room_cache.find(roomId);
    if (room_it == ctx.room_cache.end()) {
        return 0;
    }

    u64 total_ping = 0;
    u32 ping_count = 0;
    for (const auto& [member_id, member] : room_it->second.members) {
        if (member_id == ctx.my_member_id) {
            continue;
        }
        const auto peer_it = ctx.peers.find(member_id);
        if (peer_it == ctx.peers.end() || peer_it->second.ping_us == 0) {
            continue;
        }
        total_ping += peer_it->second.ping_us;
        ++ping_count;
    }

    return ping_count == 0 ? 0 : static_cast<u32>(total_ping / ping_count);
}

void* BuildSignalingGetPingInfoPayload(ContextObject& ctx, OrbisNpMatching2RoomId roomId) {
    CallbackPayload& p =
        ctx.request_payload_override ? *ctx.request_payload_override : ctx.request_payload;
    p.Reset();

    const auto room_it = ctx.room_cache.find(roomId);
    p.ping_info_response = std::make_unique<OrbisNpMatching2SignalingGetPingInfoResponse>();
    auto& out = *p.ping_info_response;
    out = {};
    if (room_it != ctx.room_cache.end()) {
        out.serverId = room_it->second.server_id;
        out.worldId = room_it->second.world_id;
        out.roomId = room_it->second.room_id;
    } else {
        out.serverId = ctx.server_id;
        out.worldId = ctx.world_id;
        out.roomId = roomId;
    }
    out.rtt = GetRoomPingUs(ctx, roomId);

    p.request_data = p.ping_info_response.get();
    return p.request_data;
}

s32 FillMatching2ConnectionInfo(const ContextObject& ctx, OrbisNpMatching2RoomId roomId,
                                OrbisNpMatching2RoomMemberId memberId, u32 infoType, void* connInfo,
                                bool a_variant) {
    if (!connInfo) {
        LOG_ERROR(Lib_NpMatching2, "connInfo null");
        return ORBIS_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
    }

    const auto room_it = ctx.room_cache.find(roomId);
    if (room_it == ctx.room_cache.end()) {
        LOG_INFO(Lib_NpMatching2, "room={} not cached for connection info", roomId);
        if (a_variant) {
            *static_cast<OrbisNpMatching2SignalingConnectionInfoA*>(connInfo) = {};
        } else {
            *static_cast<OrbisNpMatching2SignalingConnectionInfo*>(connInfo) = {};
        }
        return ORBIS_OK;
    }

    const auto member_it = room_it->second.members.find(memberId);
    const MemberCache* member =
        member_it != room_it->second.members.end() ? &member_it->second : nullptr;
    const auto peer_it = ctx.peers.find(memberId);
    const PeerInfo* peer = peer_it != ctx.peers.end() ? &peer_it->second : nullptr;

    if (a_variant) {
        auto* out = static_cast<OrbisNpMatching2SignalingConnectionInfoA*>(connInfo);
        *out = {};
        switch (infoType) {
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_RTT:
            out->rtt = peer ? peer->ping_us : 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_BANDWIDTH:
            out->bandwidth = peer ? peer->bandwidth_bps : 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_ADDR:
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_MAPPED_ADDR:
            if (peer) {
                out->address.addr = peer->addr;
                out->address.port = peer->port;
            } else if (member) {
                out->address.addr = member->addr;
                out->address.port = member->port;
            }
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PACKET_LOSS:
            out->packetLoss = peer ? peer->packet_loss_pct : 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_ADDRESS_A:
            if (member) {
                out->peerAddrA.accountId = member->account_id;
                out->peerAddrA.platform = member->platform;
            }
            break;
        default:
            LOG_WARNING(Lib_NpMatching2, "unsupported connection info A type={}", infoType);
            return ORBIS_NP_MATCHING2_SIGNALING_ERROR_INVALID_ARGUMENT;
        }
    } else {
        auto* out = static_cast<OrbisNpMatching2SignalingConnectionInfo*>(connInfo);
        *out = {};
        switch (infoType) {
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_RTT:
            out->rtt = peer ? peer->ping_us : 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_BANDWIDTH:
            out->bandwidth = peer ? peer->bandwidth_bps : 0;
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_NP_ID:
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_NPID:
            if (member) {
                out->npId = member->np_id;
            }
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PEER_ADDR:
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_MAPPED_ADDR:
            if (peer) {
                out->address.addr = peer->addr;
                out->address.port = peer->port;
            } else if (member) {
                out->address.addr = member->addr;
                out->address.port = member->port;
            }
            break;
        case ORBIS_NP_MATCHING2_SIGNALING_CONN_INFO_PACKET_LOSS:
            out->packetLoss = peer ? peer->packet_loss_pct : 0;
            break;
        default:
            LOG_WARNING(Lib_NpMatching2, "unsupported connection info type={}", infoType);
            return ORBIS_NP_MATCHING2_SIGNALING_ERROR_INVALID_ARGUMENT;
        }
    }

    LOG_INFO(Lib_NpMatching2, "connection info{}: ctx={} room={} member={} type={} rtt={} loss={}",
             a_variant ? "A" : "", ctx.ctx_id, roomId, memberId, infoType, peer ? peer->ping_us : 0,
             peer ? peer->packet_loss_pct : 0);
    return ORBIS_OK;
}

} // namespace Libraries::Np::NpMatching2
