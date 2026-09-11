// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <gtest/gtest.h>
#include "common/ipv4_interface.h"

using namespace Common::Network;

TEST(Ipv4Interface, ParsesOnlyCompleteIpv4Addresses) {
    EXPECT_EQ(ParseIpv4("192.168.50.171"), 0xc0a832abu);
    EXPECT_EQ(Ipv4ToString(0xc0a832abu), "192.168.50.171");
    for (const auto invalid : {"", "1.2.3", "1.2.3.4.5", "256.1.2.3", "-1.2.3.4", "1.2.3.4x"}) {
        EXPECT_FALSE(ParseIpv4(invalid));
    }
}

TEST(Ipv4Interface, SelectsAllFieldsFromOneInterface) {
    const std::array interfaces{
        Ipv4Interface{0x0a000002, 0xffffff00, 0x0a000001, {0x02, 1, 2, 3, 4, 5}},
        Ipv4Interface{0xc0a832ab, 0xffffff00, 0xc0a83201, {0x02, 6, 7, 8, 9, 10}},
    };
    const auto selected = SelectIpv4Interface(interfaces, 0xc0a832ab);
    ASSERT_TRUE(selected);
    EXPECT_EQ(selected->address, interfaces[1].address);
    EXPECT_EQ(selected->netmask, interfaces[1].netmask);
    EXPECT_EQ(selected->gateway, interfaces[1].gateway);
    EXPECT_EQ(selected->mac, interfaces[1].mac);
    EXPECT_FALSE(SelectIpv4Interface(interfaces, 0xc0a832ac));
}

TEST(Ipv4Interface, LoopbackAliasesHaveDistinctLocallyAdministeredIdentities) {
    const std::array interfaces{Ipv4Interface{0x7f000001, 0xff000000, 0, {}}};
    const auto first = SelectIpv4Interface(interfaces, 0x7f000002);
    const auto second = SelectIpv4Interface(interfaces, 0x7f000003);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(first->address, 0x7f000002u);
    EXPECT_EQ(first->netmask, 0xff000000u);
    EXPECT_NE(first->mac, second->mac);
    EXPECT_EQ(first->mac[0] & 3, 2);
    EXPECT_FALSE(SelectIpv4Interface({}, 0x7f000002));
}

TEST(Ipv4Interface, LoopbackBroadcastCannotEscapeToPhysicalNetwork) {
    const std::array<std::string, 3> peers{"127.0.0.3", "127.0.0.2", "127.0.0.3"};
    const auto parsed = ParseLoopbackPeers(0x7f000002, peers);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 2);
    EXPECT_EQ((*parsed)[0], 0x7f000002u);
    EXPECT_EQ((*parsed)[1], 0x7f000003u);
    const std::array<std::string, 1> external{"192.168.50.171"};
    EXPECT_FALSE(ParseLoopbackPeers(0x7f000002, external));
    EXPECT_FALSE(ParseLoopbackPeers(0xc0a832ab, peers));
    EXPECT_TRUE(ParseLoopbackPeers(0xc0a832ab, {})->empty());
    EXPECT_TRUE(IsLoopbackBroadcast(0x7f000002, 0xffffffff));
    EXPECT_TRUE(IsLoopbackBroadcast(0x7f000002, 0x7fffffff));
    EXPECT_FALSE(IsLoopbackBroadcast(0x7f000002, 0x7f000003));
    EXPECT_FALSE(IsLoopbackBroadcast(0xc0a832ab, 0xffffffff));
}

TEST(Ipv4Interface, InterfaceBroadcastPeersStayOnSelectedSubnet) {
    constexpr u32 uu_local = 0xac13a303; // 172.19.163.3
    constexpr u32 uu_mask = 0xffffff00;
    constexpr u32 uu_peer = 0xac13a302;  // 172.19.163.2
    EXPECT_TRUE(IsInterfaceBroadcast(uu_local, uu_mask, 0xffffffff));
    EXPECT_TRUE(IsInterfaceBroadcast(uu_local, uu_mask, 0xac13a3ff));
    EXPECT_FALSE(IsInterfaceBroadcast(uu_local, uu_mask, uu_peer));
    EXPECT_TRUE(IsInterfaceBroadcast(uu_local, 0, 0xffffffff));
    EXPECT_FALSE(IsInterfaceBroadcast(uu_local, 0, 0xac13a3ff));
    EXPECT_TRUE(ShouldFanoutBroadcast(uu_local, uu_mask, 0xffffffff));
    EXPECT_TRUE(ShouldFanoutBroadcast(0x7f000002, 0xff000000, 0xffffffff));
    EXPECT_FALSE(ShouldFanoutBroadcast(uu_local, uu_mask, uu_peer));

    const std::array<std::string, 2> peers{"172.19.163.2", "172.19.163.3"};
    const auto parsed = ParseInterfaceBroadcastPeers(uu_local, uu_mask, peers);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 1);
    EXPECT_EQ((*parsed)[0], uu_peer);
    const std::array<std::string, 0> none{};
    EXPECT_TRUE(ParseInterfaceBroadcastPeers(uu_local, uu_mask, none)->empty());
    const std::array<std::string, 1> other_subnet{"192.168.50.171"};
    EXPECT_FALSE(ParseInterfaceBroadcastPeers(uu_local, uu_mask, other_subnet));
    const std::array<std::string, 1> loopback{"127.0.0.2"};
    EXPECT_FALSE(ParseInterfaceBroadcastPeers(uu_local, uu_mask, loopback));
    const auto from_list = ParseInterfaceBroadcastPeerList(uu_local, uu_mask, "172.19.163.2");
    ASSERT_TRUE(from_list);
    ASSERT_EQ(from_list->size(), 1);
    EXPECT_EQ((*from_list)[0], uu_peer);
}

TEST(Ipv4Interface, ResolvesHostLoopbackWithoutInternetConnection) {
    const auto resolved = ResolveIpv4Interface("127.0.0.2");
    ASSERT_TRUE(resolved);
    EXPECT_EQ(resolved->address, 0x7f000002u);
    EXPECT_EQ(resolved->netmask, 0xff000000u);
    EXPECT_FALSE(ResolveIpv4Interface("not-an-address"));
}
