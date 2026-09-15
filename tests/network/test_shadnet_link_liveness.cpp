// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>

#include "shadnet/link_liveness.h"

#ifndef _WIN32
#include <netinet/tcp.h>
#endif

using namespace ShadNet;

namespace {

class LinkLivenessTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
#ifdef _WIN32
        WSADATA data;
        ASSERT_EQ(WSAStartup(MAKEWORD(2, 2), &data), 0);
#endif
    }

    static void TearDownTestSuite() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

// A connected loopback TCP pair; the liveness options are applied to the client end, which is
// the end the emulator owns.
struct LoopbackConnection {
    ShadSocketHandle listener = SHAD_INVALID_SOCK;
    ShadSocketHandle client = SHAD_INVALID_SOCK;
    ShadSocketHandle accepted = SHAD_INVALID_SOCK;

    LoopbackConnection() {
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == SHAD_INVALID_SOCK) {
            return;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t len = sizeof(addr);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listener, 1) != 0 ||
            ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return;
        }
        client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (client == SHAD_INVALID_SOCK ||
            ::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return;
        }
        accepted = ::accept(listener, nullptr, nullptr);
    }

    ~LoopbackConnection() {
        for (const ShadSocketHandle sock : {accepted, client, listener}) {
            if (sock != SHAD_INVALID_SOCK) {
                SHAD_CLOSE(sock);
            }
        }
    }

    bool IsConnected() const {
        return client != SHAD_INVALID_SOCK && accepted != SHAD_INVALID_SOCK;
    }
};

int GetIntOption(ShadSocketHandle sock, int level, int name) {
    int value = 0;
    socklen_t len = sizeof(value);
    EXPECT_EQ(::getsockopt(sock, level, name, reinterpret_cast<char*>(&value), &len), 0);
    return value;
}

} // namespace

TEST_F(LinkLivenessTest, AppliesEveryOptionToAConnectedSocket) {
    LoopbackConnection conn;
    ASSERT_TRUE(conn.IsConnected());

    EXPECT_TRUE(ApplyLinkLiveness(conn.client).empty());

    EXPECT_NE(GetIntOption(conn.client, SOL_SOCKET, SO_KEEPALIVE), 0);
#if defined(__APPLE__)
    EXPECT_EQ(GetIntOption(conn.client, IPPROTO_TCP, TCP_KEEPALIVE),
              static_cast<int>(SHAD_KEEPALIVE_IDLE_SEC));
#else
    EXPECT_EQ(GetIntOption(conn.client, IPPROTO_TCP, TCP_KEEPIDLE),
              static_cast<int>(SHAD_KEEPALIVE_IDLE_SEC));
#endif
    EXPECT_EQ(GetIntOption(conn.client, IPPROTO_TCP, TCP_KEEPINTVL),
              static_cast<int>(SHAD_KEEPALIVE_INTERVAL_SEC));
    EXPECT_EQ(GetIntOption(conn.client, IPPROTO_TCP, TCP_KEEPCNT),
              static_cast<int>(SHAD_KEEPALIVE_PROBES));
#if defined(_WIN32)
    EXPECT_EQ(GetIntOption(conn.client, IPPROTO_TCP, TCP_MAXRT),
              static_cast<int>(SHAD_UNACKED_DATA_TIMEOUT_SEC));
#elif defined(__linux__)
    EXPECT_EQ(GetIntOption(conn.client, IPPROTO_TCP, TCP_USER_TIMEOUT),
              static_cast<int>(SHAD_UNACKED_DATA_TIMEOUT_SEC * 1000));
#endif
}

TEST_F(LinkLivenessTest, DeadLinkIsDetectedNoLaterThanAnIdleOne) {
    // A link that dies with data in flight must not outlive one that dies idle, or a request sent
    // into a silent path would keep the user signed in longer than an idle connection would.
    EXPECT_EQ(SHAD_UNACKED_DATA_TIMEOUT_SEC,
              SHAD_KEEPALIVE_IDLE_SEC + SHAD_KEEPALIVE_INTERVAL_SEC * SHAD_KEEPALIVE_PROBES);
}

TEST_F(LinkLivenessTest, ReportsEveryOptionRefusedOnAnInvalidSocket) {
    const auto refused = ApplyLinkLiveness(SHAD_INVALID_SOCK);
    ASSERT_FALSE(refused.empty());
    EXPECT_EQ(refused.front(), "SO_KEEPALIVE");
}
