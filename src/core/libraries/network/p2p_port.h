// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "common/types.h"
#include "core/libraries/network/sockets.h"

namespace Libraries::Net {

// A P2P socket addresses a peer as (address, port, vport): several vports share one host port, so
// every datagram carries this 6-byte header and the receiver routes the payload by dst_vport.
// Layout and flag values follow RPCS3's, which solves the same problem; no reason to invent
// another encapsulation.
constexpr u32 kP2PHeaderSize = 6;
constexpr u16 kP2PFlagDgram = 1;
constexpr u16 kP2PFlagStream = 2;

// Largest datagram the host stack will carry, and therefore the largest buffer either side needs.
constexpr u32 kP2PMaxDatagram = 65535;

constexpr net_socket kInvalidNetSocket =
#ifdef _WIN32
    INVALID_SOCKET;
#else
    -1;
#endif

// vport 0 carries signaling/control traffic, so it is never handed out to a guest: binding vport 0
// asks for an ephemeral one, allocated from here upwards (as RPCS3 does).
constexpr u16 kP2PSignalingVport = 0;
constexpr u16 kP2PFirstEphemeralVport = 30000;

// NP signaling traffic (STUN with the matching server, NpSignaling and Matching2 handshakes with
// peers) shares the host port with guest P2P data, so the endpoint the server learns from a ping
// is the one peers use to reach the guest's sockets. It carries a 4-byte {0xFFFF, channel} header
// instead of the 6-byte guest header: shadNet's STUN listener expects {0xFFFF, 0xFFFF} in front of
// a ping and answers with the same framing. A guest datagram would only collide by addressing
// vport 0xFFFF.
constexpr u32 kP2PInternalHeaderSize = 4;
constexpr u16 kP2PInternalMarker = 0xFFFF;
enum class P2PInternalChannel : u16 {
    Signaling = 0xFFFF, // STUN ping/echo with the server, NpSignaling peer packets
    Control = 0xFFFE,   // NpSignaling connection control packets
    Matching2 = 0xFFFD, // Matching2 signaling handshake packets
    Relay = 0xFFFC,     // Datagrams forwarded through the matching server, see below
};
// Each internal queue is bounded so a peer flooding the port cannot grow memory without limit;
// the signaling threads drain the queues every few milliseconds.
constexpr size_t kP2PInternalQueueLimit = 256;

// Two peers that cannot punch a hole still both reach the matching server, so the server can pass
// their datagrams along. A relayed datagram is the *original* frame - internal or guest, header
// included - behind one extra header naming the other endpoint, and the receiver re-processes the
// inner frame as if it had arrived from that endpoint. Everything above the port therefore sees a
// direct connection whether or not the path is relayed.
//
//   client -> server : cmd RelayForward, dst_addr, dst_port, <original frame>
//   server -> client : cmd RelayDeliver, src_addr, src_port, <original frame>
constexpr u8 kP2PRelayForward = 0x10;
constexpr u8 kP2PRelayDeliver = 0x11;
constexpr u32 kP2PRelayHeaderSize = 7; // cmd(1) + addr(4) + port(2)
// Worst case a relayed frame carries both the relay header and the internal channel header.
constexpr u32 kP2PRelayOverhead = kP2PInternalHeaderSize + kP2PRelayHeaderSize;

// Prepended by the receive thread when it forwards a demultiplexed datagram into an endpoint's
// inbox, because the loopback hop would otherwise lose the sender. Host-local: never on the wire.
struct P2PInboxHeader {
    u32 from_addr;  // network byte order
    u16 from_port;  // network byte order
    u16 from_vport; // network byte order
};
static_assert(sizeof(P2PInboxHeader) == 8, "P2PInboxHeader must be tightly packed");

/// One real host UDP socket, shared by every vport bound on the same (address, port) pair.
/// Arriving datagrams are demultiplexed by dst_vport and forwarded into the inbox socket of each
/// matching endpoint, which is what lets a guest P2P socket keep a real, pollable fd of its own.
class P2PPort {
public:
    /// Returns the port object owning the real socket for `port` (network byte order; 0 asks the
    /// OS for an ephemeral port), binding it on first use and sharing it afterwards. On failure
    /// returns nullptr with the host socket error left set for the caller to translate.
    static std::shared_ptr<P2PPort> Acquire(u32 addr, u16 port);

    P2PPort(u32 addr, u16 port, net_socket sock);
    ~P2PPort();

    P2PPort(const P2PPort&) = delete;
    P2PPort& operator=(const P2PPort&) = delete;

    /// Claims `vport` for `owner`; datagrams for it are forwarded to `inbox`. A `vport` of 0 gets
    /// an ephemeral one. Returns the claimed vport (network byte order), or 0 when the vport is
    /// already held by a socket that did not opt into sharing — the caller reports EADDRINUSE.
    u16 Claim(u16 vport, bool reusable, const void* owner, const sockaddr_in& inbox);
    void Release(const void* owner);

    /// Sends one datagram with the header prepended. Returns `len` (the payload the guest asked to
    /// send) on success, or -1 with the host socket error left set.
    int Send(const void* data, u32 len, u16 src_vport, u16 dst_vport, const sockaddr_in& dst,
             u16 flags);

    /// Sends NP signaling traffic with the 4-byte internal header. Returns `len` on success, or -1
    /// with the host socket error left set.
    int SendInternal(P2PInternalChannel channel, const void* data, u32 len, const sockaddr_in& dst);

    /// Points the relay at the matching server's STUN endpoint (network byte order). A port of 0
    /// turns relaying off and clears every peer marked relayed.
    void SetRelayEndpoint(u32 addr, u16 port);
    /// Routes datagrams for one peer endpoint through the relay, or back to the direct path.
    void SetPeerRelayed(u32 addr, u16 port, bool relayed);
    bool IsPeerRelayed(u32 addr, u16 port) const;
    /// Drops every relay marking, e.g. when leaving a room.
    void ClearRelayedPeers();
    P2PPortStats Stats() const;
    /// Pops the oldest queued datagram of `channel`, truncated to `len` bytes. Returns the number
    /// of bytes copied, or -1 when nothing is queued; never blocks. Addresses are network order.
    /// `relayed` reports whether the datagram arrived through the server relay, which tells the
    /// handshake that the direct path is not working for this peer in that direction either.
    int ReceiveInternal(P2PInternalChannel channel, void* buf, u32 len, u32* from_addr,
                        u16* from_port, bool* relayed = nullptr);

    u32 BoundAddr() const {
        return bound_addr;
    }
    u16 BoundPort() const {
        return bound_port;
    }

private:
    void ReceiveLoop();
    void WakeReceiver();
    void QueueInternal(P2PInternalChannel channel, const sockaddr_in& from, const u8* data, u32 len,
                       bool relayed);
    /// Demultiplexes one complete frame. `relayed` is set for a frame unwrapped from the relay, and
    /// stops a malicious server from nesting relay envelopes.
    void ProcessDatagram(const sockaddr_in& from, const u8* data, u32 len, bool relayed);
    /// Sends one already-framed datagram, through the relay when `dst` is marked relayed.
    int SendFramed(const u8* frame, u32 frame_len, const sockaddr_in& dst);
    /// Unwraps a datagram the relay forwarded and re-processes it as coming from its origin.
    void HandleRelayDatagram(const sockaddr_in& from, const u8* body, u32 len, bool relayed);

    struct Endpoint {
        const void* owner;
        u16 vport; // network byte order
        bool reusable;
        sockaddr_in inbox;
    };

    u32 bound_addr{}; // network byte order
    u16 bound_port{}; // network byte order
    net_socket sock;
    std::mutex mutex;
    std::vector<Endpoint> endpoints;
    std::atomic<bool> stop{false};
    std::thread receiver;

    struct InternalDatagram {
        u32 from_addr; // network byte order
        u16 from_port; // network byte order
        bool relayed;
        std::vector<u8> payload;
    };
    std::mutex internal_mutex;
    std::array<std::deque<InternalDatagram>, 3> internal_queues;

    mutable std::mutex relay_mutex;
    u32 relay_addr{}; // network byte order
    u16 relay_port{}; // network byte order
    std::set<u64> relayed_peers;

    std::atomic<u64> stat_sent_direct{0};
    std::atomic<u64> stat_sent_relayed{0};
    std::atomic<u64> stat_recv_socket{0};
    std::atomic<u64> stat_recv_relayed{0};
    std::atomic<u64> stat_recv_relay_rejected{0};
    std::atomic<u64> stat_send_failed{0};
};

/// True when `sock` is a usable socket handle.
bool IsValidP2PSocket(net_socket sock);
/// Creates the loopback socket that a guest P2P socket exposes as its fd, and reports the address
/// the port's receive thread must forward to. Returns false with the host error set on failure.
bool CreateP2PInbox(net_socket& sock, sockaddr_in& addr);
void CloseP2PSocket(net_socket sock);
/// Waits for `sock` to become readable. `timeout_us` < 0 waits indefinitely, 0 only polls.
bool WaitP2PSocketReadable(net_socket sock, s64 timeout_us);

} // namespace Libraries::Net
