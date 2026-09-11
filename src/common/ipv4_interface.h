// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/types.h"

namespace Common::Network {

struct Ipv4Interface {
    u32 address{};
    u32 netmask{};
    u32 gateway{};
    std::array<u8, 6> mac{};
};

inline std::optional<u32> ParseIpv4(std::string_view text) {
    u32 address{};
    for (int octet = 0; octet < 4; ++octet) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        unsigned int value{};
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || error != std::errc{} || end != part.data() + part.size() ||
            value > 255 || ((octet == 3) != (dot == std::string_view::npos))) {
            return std::nullopt;
        }
        address = (address << 8) | value;
        if (octet != 3) {
            text.remove_prefix(dot + 1);
        }
    }
    return address;
}

inline std::string Ipv4ToString(u32 address) {
    return std::to_string(address >> 24) + "." + std::to_string((address >> 16) & 255) + "." +
           std::to_string((address >> 8) & 255) + "." + std::to_string(address & 255);
}

inline bool IsLoopback(u32 address) {
    return (address >> 24) == 127;
}

inline std::optional<Ipv4Interface> SelectIpv4Interface(std::span<const Ipv4Interface> interfaces,
                                                        u32 address) {
    for (const auto& candidate : interfaces) {
        if (candidate.address == address ||
            (IsLoopback(candidate.address) && IsLoopback(address))) {
            auto selected = candidate;
            selected.address = address;
            if (IsLoopback(address)) {
                selected.netmask = 0xff000000;
                selected.gateway = 0;
                selected.mac = {0x02,
                                0x00,
                                static_cast<u8>(address >> 24),
                                static_cast<u8>(address >> 16),
                                static_cast<u8>(address >> 8),
                                static_cast<u8>(address)};
            }
            return selected;
        }
    }
    return std::nullopt;
}

inline bool IsLoopbackBroadcast(u32 local_address, u32 destination) {
    return IsLoopback(local_address) && (destination == 0xffffffff || destination == 0x7fffffff);
}

inline bool IsInterfaceBroadcast(u32 local_address, u32 netmask, u32 destination) {
    if (destination == 0xffffffff) {
        return true;
    }
    if (netmask == 0 || netmask == 0xffffffff) {
        return false;
    }
    return destination == (local_address | ~netmask);
}

inline bool ShouldFanoutBroadcast(u32 local_address, u32 netmask, u32 destination) {
    return IsLoopbackBroadcast(local_address, destination) ||
           IsInterfaceBroadcast(local_address, netmask, destination);
}

inline std::optional<std::vector<u32>> ParseInterfaceBroadcastPeers(
    u32 local_address, u32 netmask, std::span<const std::string> peers) {
    std::vector<u32> addresses;
    if (peers.empty()) {
        return addresses;
    }
    if (netmask == 0 || netmask == 0xffffffff || peers.size() > 64 || IsLoopback(local_address)) {
        return std::nullopt;
    }
    const u32 network = local_address & netmask;
    const u32 directed = local_address | ~netmask;
    for (const auto& peer : peers) {
        const auto address = ParseIpv4(peer);
        if (!address || IsLoopback(*address) || *address == 0xffffffff || *address == directed ||
            (*address & netmask) != network || (*address >> 24) >= 224) {
            return std::nullopt;
        }
        if (*address == local_address) {
            continue;
        }
        if (std::ranges::find(addresses, *address) == addresses.end()) {
            addresses.push_back(*address);
        }
    }
    return addresses;
}

inline std::optional<std::vector<u32>> ParseInterfaceBroadcastPeerList(u32 local_address,
                                                                       u32 netmask,
                                                                       std::string_view list) {
    std::vector<std::string> peers;
    while (!list.empty()) {
        const auto comma = list.find(',');
        peers.emplace_back(list.substr(0, comma));
        if (peers.size() > 64) {
            return std::nullopt;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
        if (list.empty()) {
            return std::nullopt;
        }
    }
    return ParseInterfaceBroadcastPeers(local_address, netmask, peers);
}

inline std::optional<std::vector<u32>> ParseLoopbackPeers(u32 local_address,
                                                          std::span<const std::string> peers) {
    std::vector<u32> addresses;
    if (peers.empty()) {
        return addresses;
    }
    if (!IsLoopback(local_address) || peers.size() > 64) {
        return std::nullopt;
    }
    addresses.push_back(local_address);
    for (const auto& peer : peers) {
        const auto address = ParseIpv4(peer);
        if (!address || !IsLoopback(*address)) {
            return std::nullopt;
        }
        if (std::ranges::find(addresses, *address) == addresses.end()) {
            addresses.push_back(*address);
        }
    }
    return addresses;
}

std::optional<Ipv4Interface> ResolveIpv4Interface(std::string_view address);

inline std::optional<std::vector<u32>> ParseLoopbackPeerList(u32 local_address,
                                                           std::string_view list) {
    std::vector<std::string> peers;
    while (!list.empty()) {
        const auto comma = list.find(',');
        peers.emplace_back(list.substr(0, comma));
        if (peers.size() > 64) {
            return std::nullopt;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        list.remove_prefix(comma + 1);
        if (list.empty()) {
            return std::nullopt;
        }
    }
    return ParseLoopbackPeers(local_address, peers);
}

} // namespace Common::Network
