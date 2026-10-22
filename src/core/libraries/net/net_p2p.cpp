// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <fmt/format.h>

#include "common/ipv4_interface.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/libraries/net/net.h"
#include "core/libraries/net/net_log.h"
#include "core/libraries/net/net_p2p.h"
#include "core/libraries/net/net_translate.h"
#include "core/libraries/net/net_upnp.h"
#include "core/libraries/net/net_util.h"
#include "core/net/guest_net.h"
#include "core/net/p2p_codec.h"

namespace Libraries::Net {

namespace {

using Host::Error;

enum SignalingChannel : size_t { Signaling = 0, Control = 1, Matching2 = 2, ChannelCount };

size_t ClassifySignaling(std::span<const u8> packet) {
    if (packet.size() < 5 || std::memcmp(packet.data(), "SHAD", 4) != 0) {
        return Signaling;
    }
    switch (packet[4]) {
    case 0x10:
        return Control;
    case 0x21:
        return Matching2;
    default:
        return Signaling;
    }
}

struct P2PState {
    std::mutex mutex;
    std::shared_ptr<P2P::Transport> transport;
    std::shared_ptr<P2P::Keyring> keyring = std::make_shared<P2P::Keyring>();
    u16 configured_port = 3658; // host order
    u32 advertised_addr = 0;    // network order
    std::chrono::steady_clock::time_point retry_after{};
    std::pair<u16, Error> last_failure{0, Error::Ok};

    // Separate from mutex: the transport thread locks this while it already holds the transport
    // mutex. Callers must not take mutex and then call into the transport.
    std::mutex relay_mutex;
    u32 relay_addr = 0; // network order
    u16 relay_port = 0; // network order
    std::set<u64> relayed_peers;
};

P2PState& P2PStateInstance() {
    static P2PState state;
    return state;
}

constexpr u8 kRelayForward = 0x10;
constexpr u8 kRelayDeliver = 0x11;
constexpr size_t kRelayHeaderSize = 7; // cmd(1) + addr(4) + port(2)
constexpr size_t kSignalingHeaderSize = 6;
constexpr size_t kMaxUdpDatagram = 65535;
constexpr u8 kSignalingHeader[kSignalingHeaderSize] = {0xFF, 0x83, 0xFF, 0xFF, 0xFF, 0xFF};

u64 RelayPeerKey(u32 addr, u16 port) {
    return (static_cast<u64>(addr) << 16) | port;
}

bool ReadIpv4(const P2P::Endpoint& endpoint, u32& addr, u16& port) {
    if (endpoint.Family() != AF_INET) {
        return false;
    }
    const auto* in = reinterpret_cast<const sockaddr_in*>(endpoint.Sockaddr());
    addr = in->sin_addr.s_addr;
    port = in->sin_port;
    return true;
}

P2P::Endpoint MakeIpv4(u32 addr, u16 port) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = addr;
    to.sin_port = port;
    return P2P::Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// Wraps an already-framed datagram so the matching server can forward it. Packets aimed at the
// relay itself stay direct: that is the STUN ping, and wrapping it would hide the command.
std::optional<P2P::OutgoingWrap> WrapOutgoing(std::span<const u8> packet, const P2P::Endpoint& to) {
    u32 dest_addr = 0;
    u16 dest_port = 0;
    if (!ReadIpv4(to, dest_addr, dest_port)) {
        return std::nullopt;
    }
    u32 relay_addr = 0;
    u16 relay_port = 0;
    {
        auto& state = P2PStateInstance();
        std::scoped_lock lock{state.relay_mutex};
        relay_addr = state.relay_addr;
        relay_port = state.relay_port;
        if (relay_port == 0 || (dest_addr == relay_addr && dest_port == relay_port) ||
            !state.relayed_peers.contains(RelayPeerKey(dest_addr, dest_port))) {
            return std::nullopt;
        }
    }
    if (packet.size() > kMaxUdpDatagram - (kSignalingHeaderSize + kRelayHeaderSize)) {
        P2P::OutgoingWrap failed;
        failed.failed = true;
        return failed;
    }
    P2P::OutgoingWrap wrapped;
    wrapped.packet.reserve(kSignalingHeaderSize + kRelayHeaderSize + packet.size());
    wrapped.packet.insert(wrapped.packet.end(), kSignalingHeader,
                          kSignalingHeader + kSignalingHeaderSize);
    wrapped.packet.push_back(kRelayForward);
    const auto* ip = reinterpret_cast<const u8*>(&dest_addr);
    const auto* port_bytes = reinterpret_cast<const u8*>(&dest_port);
    wrapped.packet.insert(wrapped.packet.end(), ip, ip + 4);
    wrapped.packet.insert(wrapped.packet.end(), port_bytes, port_bytes + 2);
    wrapped.packet.insert(wrapped.packet.end(), packet.begin(), packet.end());
    wrapped.to = MakeIpv4(relay_addr, relay_port);
    return wrapped;
}

std::optional<P2P::IncomingUnwrap> UnwrapIncoming(std::span<const u8> payload,
                                                  const P2P::Endpoint& from) {
    // Shorter than a deliver header is ordinary signaling. A STUN echo is 6 bytes and its first
    // byte is an IP octet, which can be 0x11.
    if (payload.size() < kRelayHeaderSize || payload[0] != kRelayDeliver) {
        return std::nullopt;
    }
    u32 relay_addr = 0;
    u16 relay_port = 0;
    {
        auto& state = P2PStateInstance();
        std::scoped_lock lock{state.relay_mutex};
        relay_addr = state.relay_addr;
        relay_port = state.relay_port;
    }
    u32 from_addr = 0;
    u16 from_port = 0;
    const bool from_relay = ReadIpv4(from, from_addr, from_port) && relay_port != 0 &&
                            from_addr == relay_addr && from_port == relay_port;
    P2P::IncomingUnwrap unwrapped;
    if (!from_relay) {
        unwrapped.rejected = true;
        return unwrapped;
    }
    u32 origin_addr = 0;
    u16 origin_port = 0;
    std::memcpy(&origin_addr, payload.data() + 1, sizeof(origin_addr));
    std::memcpy(&origin_port, payload.data() + 5, sizeof(origin_port));
    unwrapped.origin = MakeIpv4(origin_addr, origin_port);
    unwrapped.packet.assign(payload.begin() + static_cast<std::ptrdiff_t>(kRelayHeaderSize),
                            payload.end());
    return unwrapped;
}

void LogBroadcast(std::span<const u8> packet, const P2P::Endpoint& from) {
    char address[INET6_ADDRSTRLEN] = "?";
    const auto* sa = from.Sockaddr();
    if (sa->sa_family == AF_INET) {
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, address,
                  sizeof(address));
    } else if (sa->sa_family == AF_INET6) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, address,
                  sizeof(address));
    }
    std::string hex;
    const size_t shown = std::min<size_t>(packet.size(), 64);
    hex.reserve(shown * 3);
    for (size_t i = 0; i < shown; ++i) {
        fmt::format_to(std::back_inserter(hex), "{}{:02x}", i == 0 ? "" : " ", packet[i]);
    }
    LOG_WARNING(Lib_Net,
                "P2P broadcast packet received (layout assumed; please report this line): "
                "from {}:{}, {} bytes: {}{}",
                address, from.Port(), packet.size(), hex, packet.size() > shown ? " ..." : "");
}

constexpr u16 P2PPortFallbacks = 16;

u16 StartLocked(P2PState& state, u16 udp_port, bool allow_fallback) {
    if (!Host::Initialize()) {
        return 0;
    }
    std::string bind_address = "0.0.0.0";
    const auto configured = EmulatorSettings.GetNetworkInterfaceAddress();
    if (!configured.empty()) {
        if (!Common::Network::ParseIpv4(configured)) {
            LOG_ERROR(Lib_Net, "network_interface_address {} is not an IPv4 address", configured);
            return 0;
        }
        bind_address = std::string(configured);
    }
    // A pinned interface must not slide onto the next port: two loopback instances each own 3658.
    const bool fallback = allow_fallback && bind_address == "0.0.0.0";
    P2P::TransportConfig config;
    config.signaling_channels = ChannelCount;
    config.classify_signaling = ClassifySignaling;
    config.wrap_outgoing = WrapOutgoing;
    config.unwrap_incoming = UnwrapIncoming;
    const auto try_port = [&](u16 port, Error* error) {
        auto codec = std::make_unique<P2P::FramingCodec>(state.keyring, LogBroadcast);
        return P2P::Transport::Create(AF_INET, P2P::Endpoint::IPv4(bind_address.c_str(), port),
                                      std::move(codec), config, error);
    };
    Error error;
    auto transport = try_port(udp_port, &error);
    for (u16 i = 1; !transport && fallback && error == Error::AddrInUse && udp_port != 0 &&
                    i <= P2PPortFallbacks && udp_port + i <= 0xffff;
         ++i) {
        transport = try_port(static_cast<u16>(udp_port + i), &error);
    }
    if (!transport) {
        if (state.last_failure != std::pair{udp_port, error}) {
            state.last_failure = {udp_port, error};
            LOG_ERROR(Lib_Net, "cannot start P2P on UDP port {}: {}", udp_port,
                      ErrnoName(static_cast<int>(error)));
        } else {
            LOG_DEBUG(Lib_Net, "cannot start P2P on UDP port {}: {}", udp_port,
                      ErrnoName(static_cast<int>(error)));
        }
        return 0;
    }
    state.last_failure = {0, Error::Ok};
    const u16 port = transport->BoundPort();
    state.transport = transport;
    Core::Net::SetP2PTransport(AF_INET, std::move(transport));
    if (udp_port != 0 && port != udp_port) {
        LOG_WARNING(Lib_Net,
                    "UDP port {} is in use (another instance?), P2P transport on UDP port {} "
                    "instead. Set p2p_port to choose one.",
                    udp_port, port);
    } else {
        LOG_INFO(Lib_Net, "P2P transport on UDP port {}", port);
    }
    return port;
}

u16 PortToBind(const P2PState& state, const SystemHooks& hooks) {
    return hooks.p2p_port ? hooks.p2p_port() : state.configured_port;
}

std::shared_ptr<P2P::Transport> RunningTransport() {
    auto& state = P2PStateInstance();
    const auto hooks = GetSystemHooks();
    std::shared_ptr<P2P::Transport> transport;
    u16 started = 0;
    {
        std::scoped_lock lock{state.mutex};
        const auto now = std::chrono::steady_clock::now();
        if (!state.transport && now >= state.retry_after) {
            started = StartLocked(state, PortToBind(state, hooks), true);
            if (started == 0) {
                state.retry_after = now + std::chrono::seconds{2};
            }
        }
        transport = state.transport;
    }
    if (started != 0 && hooks.p2p_started) {
        hooks.p2p_started(started);
    }
    return transport;
}

int ChannelSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    const auto transport = RunningTransport();
    if (!transport || (data == nullptr && len != 0)) {
        return -1;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = dest_addr;
    to.sin_port = dest_port;
    const auto endpoint =
        P2P::Endpoint::FromSockaddr(reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    const Error e = transport->SendSignaling(
        {static_cast<const u8*>(data), static_cast<size_t>(len)}, endpoint);
    return e == Error::Ok ? static_cast<int>(len) : -1;
}

int ChannelRecvFrom(size_t channel, void* buf, u32 len, u32* from_addr, u16* from_port,
                    bool* relayed) {
    const auto transport = RunningTransport();
    std::vector<u8> packet;
    P2P::Endpoint from;
    bool arrived_relayed = false;
    if (!transport || !transport->RecvSignaling(channel, &packet, &from, &arrived_relayed)) {
        return -1;
    }
    if (relayed != nullptr) {
        *relayed = arrived_relayed;
    }
    const u32 n = std::min<u32>(len, static_cast<u32>(packet.size()));
    if (buf != nullptr) {
        std::memcpy(buf, packet.data(), n);
    }
    const auto* in = reinterpret_cast<const sockaddr_in*>(&from.addr);
    if (from_addr != nullptr) {
        *from_addr = in->sin_addr.s_addr;
    }
    if (from_port != nullptr) {
        *from_port = in->sin_port;
    }
    return static_cast<int>(n);
}

} // namespace

void ConfigureP2P(u16 udp_port, u32 advertised_addr) {
    auto& state = P2PStateInstance();
    std::scoped_lock lock{state.mutex};
    state.configured_port = udp_port;
    state.advertised_addr = advertised_addr;
    const u32 ip = sceNetNtohl(advertised_addr);
    LOG_INFO(Lib_Net, "P2P configured: UDP port {}, advertised address {}.{}.{}.{}", udp_port,
             ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
}

u16 StartP2P(u16 udp_port) {
    StopP2P();
    auto& state = P2PStateInstance();
    u16 started = 0;
    {
        std::scoped_lock lock{state.mutex};
        started = StartLocked(state, udp_port, false);
        state.retry_after = {};
    }
    const auto hooks = GetSystemHooks();
    if (started != 0 && hooks.p2p_started) {
        hooks.p2p_started(started);
    }
    return started;
}

void StopP2P() {
    auto& state = P2PStateInstance();
    u16 stopped = 0;
    {
        std::scoped_lock lock{state.mutex};
        if (state.transport) {
            stopped = state.transport->BoundPort();
        }
        state.transport.reset();
        state.retry_after = {};
        Core::Net::SetP2PTransport(AF_INET, nullptr);
    }
    if (stopped != 0) {
        LOG_INFO(Lib_Net, "P2P transport on UDP port {} stopped", stopped);
    }
    const auto hooks = GetSystemHooks();
    if (stopped != 0 && hooks.p2p_stopped) {
        hooks.p2p_stopped(stopped);
    }
}

std::shared_ptr<P2P::Keyring> GetP2PKeyring() {
    auto& state = P2PStateInstance();
    std::scoped_lock lock{state.mutex};
    return state.keyring;
}

bool EnsureP2PTransport() {
    return RunningTransport() != nullptr;
}

bool P2PTransportIsReady() {
    auto& state = P2PStateInstance();
    std::scoped_lock lock{state.mutex};
    return state.transport != nullptr;
}

u16 GetP2PConfiguredPort() {
    const auto hooks = GetSystemHooks();
    auto& state = P2PStateInstance();
    std::scoped_lock lock{state.mutex};
    return PortToBind(state, hooks);
}

u16 GetP2PBoundPort() {
    auto& state = P2PStateInstance();
    std::scoped_lock lock{state.mutex};
    return state.transport ? state.transport->BoundPort() : 0;
}

u32 GetP2PAdvertisedAddr() {
    u32 configured = 0;
    {
        auto& state = P2PStateInstance();
        std::scoped_lock lock{state.mutex};
        configured = state.advertised_addr;
    }
    if (configured != 0) {
        return configured;
    }
    const auto hooks = GetSystemHooks();
    if (hooks.public_addr) {
        if (const u32 addr = hooks.public_addr(); addr != 0) {
            return addr;
        }
    }
    return GetP2PLocalAddr();
}

u32 GetP2PLocalAddr() {
    if (const auto parsed =
            Common::Network::ParseIpv4(EmulatorSettings.GetNetworkInterfaceAddress())) {
        return htonl(*parsed);
    }
    auto* netinfo = Common::Singleton<NetUtil::NetUtilInternal>::Instance();
    if (!netinfo->GetIp().empty()) {
        in_addr parsed{};
        if (inet_pton(AF_INET, netinfo->GetIp().c_str(), &parsed) == 1) {
            return parsed.s_addr;
        }
    }
    return 0;
}

u16 GetP2PAdvertisedPort() {
    const u16 bound = GetP2PBoundPort();
    return bound != 0 ? bound : GetP2PConfiguredPort();
}

int P2PSignalingSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    return ChannelSendTo(data, len, dest_addr, dest_port);
}

int P2PSignalingRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return ChannelRecvFrom(Signaling, buf, len, from_addr, from_port, nullptr);
}

int P2PControlSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    return ChannelSendTo(data, len, dest_addr, dest_port);
}

int P2PControlRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port) {
    return ChannelRecvFrom(Control, buf, len, from_addr, from_port, nullptr);
}

int P2PMatching2SendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port) {
    return ChannelSendTo(data, len, dest_addr, dest_port);
}

int P2PMatching2RecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port, bool* relayed) {
    return ChannelRecvFrom(Matching2, buf, len, from_addr, from_port, relayed);
}

void SetP2PRelayEndpoint(u32 addr, u16 port) {
    auto& state = P2PStateInstance();
    {
        std::scoped_lock lock{state.relay_mutex};
        if (state.relay_addr == addr && state.relay_port == port) {
            return;
        }
        state.relay_addr = addr;
        state.relay_port = port;
        if (port == 0) {
            state.relayed_peers.clear();
        }
    }
    LOG_INFO(Lib_Net, "P2P relay endpoint set to {:#010x}:{}", ntohl(addr), ntohs(port));
}

void SetP2PPeerRelayed(u32 addr, u16 port, bool relayed) {
    if (addr == 0 || port == 0) {
        return;
    }
    bool changed = false;
    bool missing_relay = false;
    {
        auto& state = P2PStateInstance();
        std::scoped_lock lock{state.relay_mutex};
        if (relayed && state.relay_port == 0) {
            missing_relay = true;
        } else {
            const u64 key = (static_cast<u64>(addr) << 16) | port;
            changed = relayed ? state.relayed_peers.insert(key).second
                              : state.relayed_peers.erase(key) != 0;
        }
    }
    if (missing_relay) {
        LOG_WARNING(Lib_Net, "P2P relay requested for {:#010x}:{} but no relay endpoint is set",
                    ntohl(addr), ntohs(port));
        return;
    }
    if (changed) {
        LOG_INFO(Lib_Net, "P2P peer {:#010x}:{} now on the {} path", ntohl(addr), ntohs(port),
                 relayed ? "relayed" : "direct");
    }
}

void ClearP2PRelayedPeers() {
    auto& state = P2PStateInstance();
    std::scoped_lock lock{state.relay_mutex};
    state.relayed_peers.clear();
}

P2PPortStats GetP2PTransportStats() {
    std::shared_ptr<P2P::Transport> transport;
    {
        auto& state = P2PStateInstance();
        std::scoped_lock lock{state.mutex};
        transport = state.transport;
    }
    if (!transport) {
        return {};
    }
    const auto stats = transport->Stats();
    return {stats.sent_direct,  stats.sent_relayed,        stats.recv_socket,
            stats.recv_relayed, stats.recv_relay_rejected, stats.send_failed};
}

void ReleaseP2PPortMapping() {
    const u16 port = GetP2PBoundPort();
    if (port == 0) {
        return;
    }
    UPnPClient::Instance().RemoveMapping(port);
}

} // namespace Libraries::Net
