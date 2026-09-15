// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "core/libraries/network/sockets.h"

namespace Libraries::Net {

// Two IPv4s the P2P stack must keep apart:
//   advertised — told to remote Matching2 peers (handshake mapped_addr)
//   local      — the address a title binds P2P sockets to (GetLocalNetInfo localAddr)
//
// STUN mapping is what a peer can actually reach (UU LAN, public IP). The NetCtl /
// Ethernet address is what the title can bind. Mixing them blacks the title out or
// sends peers to an unroutable NIC.
struct P2PAddrChoice {
    u32 advertised = 0; // network byte order
    u32 local = 0;      // network byte order
};

// Pure choice over the three inputs the live helpers already have. bound_ip of
// INADDR_ANY (wildcard) means "use local_ip for the title". A recorded STUN
// mapping always wins for the peer-advertised address.
inline P2PAddrChoice ChooseP2PAddresses(u32 stun_mapped, u32 local_ip, u32 bound_ip) {
    P2PAddrChoice out{};
    const u32 any = htonl(INADDR_ANY);
    out.local = (bound_ip != any) ? bound_ip : local_ip;
    out.advertised = (stun_mapped != 0) ? stun_mapped : out.local;
    return out;
}

// Empty network_interface_address keeps the shared host port on every NIC.
// A dotted IPv4 pins the bind to that address. Returns false on unparseable input.
inline bool P2PBindAddressFromConfig(std::string_view configured, u32& addr) {
    if (configured.empty()) {
        addr = htonl(INADDR_ANY);
        return true;
    }
    in_addr parsed{};
    const std::string owned(configured);
    if (inet_pton(AF_INET, owned.c_str(), &parsed) != 1) {
        return false;
    }
    addr = parsed.s_addr;
    return true;
}

} // namespace Libraries::Net
