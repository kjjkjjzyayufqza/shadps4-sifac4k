// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shadnet/link_liveness.h"

#ifndef _WIN32
#include <netinet/tcp.h>
#endif

namespace ShadNet {

std::vector<std::string_view> ApplyLinkLiveness(ShadSocketHandle sock) {
    std::vector<std::string_view> refused;
    const auto apply = [&](int level, int name, u32 value, std::string_view label) {
        const int option = static_cast<int>(value);
        if (::setsockopt(sock, level, name, reinterpret_cast<const char*>(&option),
                         sizeof(option)) != 0) {
            refused.push_back(label);
        }
    };

    apply(SOL_SOCKET, SO_KEEPALIVE, 1, "SO_KEEPALIVE");
#if defined(__APPLE__)
    // macOS names the idle time before the first probe TCP_KEEPALIVE.
    apply(IPPROTO_TCP, TCP_KEEPALIVE, SHAD_KEEPALIVE_IDLE_SEC, "TCP_KEEPALIVE");
#else
    apply(IPPROTO_TCP, TCP_KEEPIDLE, SHAD_KEEPALIVE_IDLE_SEC, "TCP_KEEPIDLE");
#endif
    apply(IPPROTO_TCP, TCP_KEEPINTVL, SHAD_KEEPALIVE_INTERVAL_SEC, "TCP_KEEPINTVL");
    apply(IPPROTO_TCP, TCP_KEEPCNT, SHAD_KEEPALIVE_PROBES, "TCP_KEEPCNT");

    // Keepalive only probes a link with nothing in flight. A link that dies while a request is
    // unacknowledged is bounded by the retransmission limit instead, which defaults to minutes.
#if defined(_WIN32)
    apply(IPPROTO_TCP, TCP_MAXRT, SHAD_UNACKED_DATA_TIMEOUT_SEC, "TCP_MAXRT");
#elif defined(__linux__)
    apply(IPPROTO_TCP, TCP_USER_TIMEOUT, SHAD_UNACKED_DATA_TIMEOUT_SEC * 1000, "TCP_USER_TIMEOUT");
#elif defined(__APPLE__)
    apply(IPPROTO_TCP, TCP_RXT_CONNDROPTIME, SHAD_UNACKED_DATA_TIMEOUT_SEC, "TCP_RXT_CONNDROPTIME");
#endif
    // FreeBSD has no per-socket bound on unacknowledged data; its system retransmission limit
    // applies there.
    return refused;
}

} // namespace ShadNet
