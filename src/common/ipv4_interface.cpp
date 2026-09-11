// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include "common/ipv4_interface.h"

#ifdef _WIN32
#include <winsock2.h>

#include <iphlpapi.h>
#include <ws2tcpip.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#if defined(__linux__)
#include <fstream>
#include <sstream>
#include <netpacket/packet.h>
#else
#include <net/if_dl.h>
#endif
#endif

namespace Common::Network {

std::optional<Ipv4Interface> ResolveIpv4Interface(std::string_view address) {
    const auto requested = ParseIpv4(address);
    if (!requested) {
        return std::nullopt;
    }
    std::vector<Ipv4Interface> interfaces;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<u8> storage(size);
    constexpr ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                            GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    auto result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
    }
    if (result != NO_ERROR) {
        return std::nullopt;
    }
    for (auto adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) {
            continue;
        }
        Ipv4Interface identity{};
        if (adapter->PhysicalAddressLength >= identity.mac.size()) {
            std::memcpy(identity.mac.data(), adapter->PhysicalAddress, identity.mac.size());
        }
        for (auto gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next) {
            if (gateway->Address.lpSockaddr && gateway->Address.lpSockaddr->sa_family == AF_INET) {
                identity.gateway =
                    ntohl(reinterpret_cast<const sockaddr_in*>(gateway->Address.lpSockaddr)
                              ->sin_addr.s_addr);
                break;
            }
        }
        for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (!unicast->Address.lpSockaddr || unicast->Address.lpSockaddr->sa_family != AF_INET ||
                unicast->OnLinkPrefixLength > 32) {
                continue;
            }
            identity.address = ntohl(
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr.s_addr);
            identity.netmask = unicast->OnLinkPrefixLength == 0
                                   ? 0
                                   : 0xffffffffu << (32 - unicast->OnLinkPrefixLength);
            interfaces.push_back(identity);
        }
    }
#else
    ifaddrs* adapters{};
    if (getifaddrs(&adapters) != 0) {
        return std::nullopt;
    }
    for (auto adapter = adapters; adapter; adapter = adapter->ifa_next) {
        if (!adapter->ifa_addr || !adapter->ifa_netmask || !(adapter->ifa_flags & IFF_UP) ||
            adapter->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        Ipv4Interface identity{};
        identity.address =
            ntohl(reinterpret_cast<const sockaddr_in*>(adapter->ifa_addr)->sin_addr.s_addr);
        identity.netmask =
            ntohl(reinterpret_cast<const sockaddr_in*>(adapter->ifa_netmask)->sin_addr.s_addr);
        for (auto link = adapters; link; link = link->ifa_next) {
            if (!link->ifa_addr || std::strcmp(link->ifa_name, adapter->ifa_name) != 0) {
                continue;
            }
#if defined(__linux__)
            if (link->ifa_addr->sa_family == AF_PACKET) {
                const auto data = reinterpret_cast<const sockaddr_ll*>(link->ifa_addr);
                if (data->sll_halen >= identity.mac.size()) {
                    std::memcpy(identity.mac.data(), data->sll_addr, identity.mac.size());
                }
            }
#else
            if (link->ifa_addr->sa_family == AF_LINK) {
                const auto data = reinterpret_cast<const sockaddr_dl*>(link->ifa_addr);
                if (data->sdl_alen >= identity.mac.size()) {
                    std::memcpy(identity.mac.data(), LLADDR(data), identity.mac.size());
                }
            }
#endif
        }
#if defined(__linux__)
        std::ifstream routes("/proc/net/route");
        std::string line;
        while (std::getline(routes, line)) {
            std::istringstream row(line);
            std::string name;
            u32 destination{}, gateway{}, route_flags{};
            if (row >> name >> std::hex >> destination >> gateway >> route_flags;
                row && name == adapter->ifa_name && destination == 0 && (route_flags & 3) == 3) {
                identity.gateway = ntohl(gateway);
                break;
            }
        }
#endif
        interfaces.push_back(identity);
    }
    freeifaddrs(adapters);
#endif
    return SelectIpv4Interface(interfaces, *requested);
}

} // namespace Common::Network
