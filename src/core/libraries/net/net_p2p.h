// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "common/types.h"

namespace Core::Net::P2P {
class Keyring;
}

namespace Libraries::Net {

struct P2PPortStats {
    u64 sent_direct = 0;
    u64 sent_relayed = 0;
    u64 recv_socket = 0;
    u64 recv_relayed = 0;
    u64 recv_relay_rejected = 0;
    u64 send_failed = 0;
};

void ConfigureP2P(u16 udp_port, u32 advertised_addr);
u16 StartP2P(u16 udp_port);
void StopP2P();
std::shared_ptr<Core::Net::P2P::Keyring> GetP2PKeyring();

// Used by NP signaling/matching.
bool EnsureP2PTransport();
bool P2PTransportIsReady();
u16 GetP2PConfiguredPort(); // host order
u16 GetP2PBoundPort();      // host order, 0 when not running
u32 GetP2PAdvertisedAddr(); // Network order, 0 if unknown.
u32 GetP2PLocalAddr();      // Network order of the bound interface, 0 if unknown.
u16 GetP2PAdvertisedPort();
void SetP2PRelayEndpoint(u32 addr, u16 port);
void SetP2PPeerRelayed(u32 addr, u16 port, bool relayed);
void ClearP2PRelayedPeers();
P2PPortStats GetP2PTransportStats();
void ReleaseP2PPortMapping();
int P2PSignalingSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port);
int P2PSignalingRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port);
int P2PControlSendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port);
int P2PControlRecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port);
int P2PMatching2SendTo(const void* data, u32 len, u32 dest_addr, u16 dest_port);
int P2PMatching2RecvFrom(void* buf, u32 len, u32* from_addr, u16* from_port,
                         bool* relayed = nullptr);

} // namespace Libraries::Net
