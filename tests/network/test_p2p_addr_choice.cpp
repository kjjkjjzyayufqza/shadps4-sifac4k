// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "core/libraries/network/p2p_addr_choice.h"

using namespace Libraries::Net;

namespace {

u32 Ip(const char* dotted) {
    in_addr parsed{};
    EXPECT_EQ(inet_pton(AF_INET, dotted, &parsed), 1);
    return parsed.s_addr;
}

} // namespace

TEST(P2PAddrChoice, StunMappingIsAdvertisedWhileTitleKeepsLocalNic) {
    const u32 ethernet = Ip("192.168.50.171");
    const u32 uu = Ip("172.19.163.3");
    const auto choice = ChooseP2PAddresses(uu, ethernet, htonl(INADDR_ANY));
    EXPECT_EQ(choice.advertised, uu);
    EXPECT_EQ(choice.local, ethernet);
    EXPECT_NE(choice.advertised, choice.local);
}

TEST(P2PAddrChoice, NoMappingFallsBackToLocalForBoth) {
    const u32 ethernet = Ip("192.168.50.171");
    const auto choice = ChooseP2PAddresses(0, ethernet, htonl(INADDR_ANY));
    EXPECT_EQ(choice.advertised, ethernet);
    EXPECT_EQ(choice.local, ethernet);
}

TEST(P2PAddrChoice, ExplicitBindStaysLocalWhileStunIsAdvertised) {
    const u32 ethernet = Ip("192.168.50.171");
    const u32 uu = Ip("172.19.163.3");
    const u32 bound = Ip("127.0.0.2");
    const auto choice = ChooseP2PAddresses(uu, ethernet, bound);
    EXPECT_EQ(choice.advertised, uu);
    EXPECT_EQ(choice.local, bound);
}

TEST(P2PAddrChoice, ExplicitBindWithoutMappingIsAdvertised) {
    const u32 ethernet = Ip("192.168.50.171");
    const u32 bound = Ip("127.0.0.2");
    const auto choice = ChooseP2PAddresses(0, ethernet, bound);
    EXPECT_EQ(choice.advertised, bound);
    EXPECT_EQ(choice.local, bound);
}

TEST(P2PBindAddressFromConfig, EmptyStringBindsWildcard) {
    u32 addr = 0xFFFFFFFFu;
    ASSERT_TRUE(P2PBindAddressFromConfig("", addr));
    EXPECT_EQ(addr, htonl(INADDR_ANY));
}

TEST(P2PBindAddressFromConfig, DottedIpv4PinsTheBind) {
    u32 addr = 0;
    ASSERT_TRUE(P2PBindAddressFromConfig("172.19.163.3", addr));
    EXPECT_EQ(addr, Ip("172.19.163.3"));
}

TEST(P2PBindAddressFromConfig, RejectsUnparseable) {
    u32 addr = 0;
    EXPECT_FALSE(P2PBindAddressFromConfig("not-an-ip", addr));
}
