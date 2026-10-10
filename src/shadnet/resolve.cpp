// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include "common/logging/log.h"
#include "shadnet/resolve.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ShadNet {

namespace {

#ifdef _WIN32
using DnsSocket = SOCKET;
static constexpr DnsSocket kInvalidDnsSocket = INVALID_SOCKET;
#define SHAD_DNS_CLOSE(s) ::closesocket(s)

void EnsureWinsock() {
    static bool done = false;
    if (!done) {
        WSADATA wd;
        ::WSAStartup(MAKEWORD(2, 2), &wd);
        done = true;
    }
}
#else
using DnsSocket = int;
static constexpr DnsSocket kInvalidDnsSocket = -1;
#define SHAD_DNS_CLOSE(s) ::close(s)
void EnsureWinsock() {}
#endif

struct Slot {
    std::mutex mu;
    std::vector<u32> addrs;
    bool done = false;

    void Finish(std::vector<u32> found) {
        std::lock_guard lock(mu);
        addrs = std::move(found);
        done = true;
    }

    struct Snap {
        bool done = false;
        std::vector<u32> addrs;
    };

    Snap Snapshot() {
        std::lock_guard lock(mu);
        return {done, addrs};
    }
};

std::vector<u32> LookupSystem(const std::string& host) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::vector<u32> found;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        return found;
    }
    for (const addrinfo* it = res; it != nullptr; it = it->ai_next) {
        if (it->ai_family != AF_INET || it->ai_addr == nullptr) {
            continue;
        }
        const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
        found.push_back(sin->sin_addr.s_addr);
    }
    ::freeaddrinfo(res);
    return found;
}

bool AppendLabelledName(const std::string& host, std::vector<u8>& out) {
    if (host.empty() || host.size() > 253) {
        return false;
    }
    std::size_t i = 0;
    while (i < host.size()) {
        const std::size_t dot = host.find('.', i);
        const std::size_t end = dot == std::string::npos ? host.size() : dot;
        const std::size_t len = end - i;
        if (len == 0 || len > 63) {
            return false;
        }
        out.push_back(static_cast<u8>(len));
        out.insert(out.end(), host.begin() + static_cast<std::ptrdiff_t>(i),
                   host.begin() + static_cast<std::ptrdiff_t>(end));
        if (dot == std::string::npos) {
            break;
        }
        i = dot + 1;
        if (i >= host.size()) {
            return false;
        }
    }
    out.push_back(0);
    return true;
}

// Advances past a (possibly compressed) domain name. The packet offset moves by the
// encoded length, not by the compression target.
bool SkipName(const u8* pkt, std::size_t len, std::size_t& off) {
    while (off < len) {
        const u8 label = pkt[off];
        if (label == 0) {
            ++off;
            return true;
        }
        if ((label & 0xC0) == 0xC0) {
            if (off + 1 >= len) {
                return false;
            }
            off += 2;
            return true;
        }
        if ((label & 0xC0) != 0) {
            return false;
        }
        off += static_cast<std::size_t>(1 + label);
    }
    return false;
}

std::vector<u32> ParseA(const u8* pkt, std::size_t len) {
    std::vector<u32> found;
    if (len < 12) {
        return found;
    }
    const u16 qd = static_cast<u16>((pkt[4] << 8) | pkt[5]);
    const u16 an = static_cast<u16>((pkt[6] << 8) | pkt[7]);
    std::size_t off = 12;
    for (u16 i = 0; i < qd; ++i) {
        if (!SkipName(pkt, len, off) || off + 4 > len) {
            return found;
        }
        off += 4;
    }
    for (u16 i = 0; i < an; ++i) {
        if (!SkipName(pkt, len, off) || off + 10 > len) {
            return found;
        }
        const u16 type = static_cast<u16>((pkt[off] << 8) | pkt[off + 1]);
        const u16 rdlen = static_cast<u16>((pkt[off + 8] << 8) | pkt[off + 9]);
        off += 10;
        if (off + rdlen > len) {
            return found;
        }
        if (type == 1 && rdlen == 4) {
            u32 be = 0;
            std::memcpy(&be, pkt + off, 4);
            found.push_back(be);
        }
        off += rdlen;
    }
    return found;
}

std::vector<u32> QueryA(const char* server, const std::string& host, u32 timeout_ms) {
    std::vector<u8> query(12);
    static std::atomic<u16> next_id{1};
    const u16 id = next_id.fetch_add(1);
    query[0] = static_cast<u8>(id >> 8);
    query[1] = static_cast<u8>(id);
    query[2] = 0x01; // recursion desired
    query[5] = 1;    // one question
    if (!AppendLabelledName(host, query)) {
        return {};
    }
    query.insert(query.end(), {0x00, 0x01, 0x00, 0x01}); // A, IN

    const DnsSocket sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kInvalidDnsSocket) {
        return {};
    }
#ifdef _WIN32
    const DWORD rcv = timeout_ms;
#else
    const timeval rcv{static_cast<long>(timeout_ms / 1000),
                      static_cast<long>((timeout_ms % 1000) * 1000)};
#endif
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv));

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    if (::inet_pton(AF_INET, server, &dst.sin_addr) != 1) {
        SHAD_DNS_CLOSE(sock);
        return {};
    }
    const int sent =
        ::sendto(sock, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()),
                 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    if (sent != static_cast<int>(query.size())) {
        SHAD_DNS_CLOSE(sock);
        return {};
    }
    u8 reply[512];
    const int got =
        ::recvfrom(sock, reinterpret_cast<char*>(reply), sizeof(reply), 0, nullptr, nullptr);
    SHAD_DNS_CLOSE(sock);
    if (got < 12) {
        return {};
    }
    return ParseA(reply, static_cast<std::size_t>(got));
}

// Authoritative DNS for the public shadNet name is on Cloudflare. Mainland recursive
// resolvers often drop that query and sit until their own timeout, which is the minute
// MBON's login screen waits. These two answer without that hop. 1.1.1.1 is not used:
// it is itself a Cloudflare address.
std::vector<u32> LookupPublic(const std::string& host) {
    for (const char* server : {"8.8.8.8", "8.8.4.4"}) {
        std::vector<u32> found = QueryA(server, host, 1000);
        if (!found.empty()) {
            return found;
        }
    }
    return {};
}

void JoinOrDetach(std::thread& worker, const std::shared_ptr<Slot>& slot) {
    if (!worker.joinable()) {
        return;
    }
    if (slot->Snapshot().done) {
        worker.join();
    } else {
        worker.detach();
    }
}

} // namespace

std::vector<u32> ResolveIPv4(const std::string& host) {
    EnsureWinsock();
    in_addr literal{};
    if (::inet_pton(AF_INET, host.c_str(), &literal) == 1) {
        return {literal.s_addr};
    }

    auto system = std::make_shared<Slot>();
    auto direct = std::make_shared<Slot>();
    std::thread system_thread([system, host] { system->Finish(LookupSystem(host)); });
    std::thread direct_thread([direct, host] { direct->Finish(LookupPublic(host)); });

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(SHAD_RESOLVE_TIMEOUT_MS);
    std::vector<u32> chosen;
    const char* via = nullptr;
    while (std::chrono::steady_clock::now() < deadline) {
        const Slot::Snap sys = system->Snapshot();
        const Slot::Snap pub = direct->Snapshot();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (sys.done && !sys.addrs.empty()) {
            chosen = sys.addrs;
            via = "system";
            break;
        }
        if (!pub.addrs.empty() && (sys.done || elapsed >= SHAD_RESOLVE_SYSTEM_GRACE_MS)) {
            chosen = pub.addrs;
            via = "8.8.8.8";
            break;
        }
        if (sys.done && pub.done) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (chosen.empty()) {
        const Slot::Snap sys = system->Snapshot();
        const Slot::Snap pub = direct->Snapshot();
        if (!sys.addrs.empty()) {
            chosen = sys.addrs;
            via = "system";
        } else if (!pub.addrs.empty()) {
            chosen = pub.addrs;
            via = "8.8.8.8";
        }
    }

    JoinOrDetach(system_thread, system);
    JoinOrDetach(direct_thread, direct);

    if (chosen.empty()) {
        LOG_ERROR(ShadNet, "DNS resolution failed for '{}' within {} ms", host,
                  SHAD_RESOLVE_TIMEOUT_MS);
        return {};
    }
    char ip[INET_ADDRSTRLEN] = {};
    in_addr shown{};
    shown.s_addr = chosen.front();
    ::inet_ntop(AF_INET, &shown, ip, sizeof(ip));
    LOG_INFO(ShadNet, "resolved '{}' via {} -> {} ({} address{})", host, via, ip, chosen.size(),
             chosen.size() == 1 ? "" : "es");
    return chosen;
}

} // namespace ShadNet
