// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/libraries/network/p2p_port.h"

using namespace Libraries::Net;

namespace {

// Delivery goes through the host stack twice (peer socket, then the inbox hop), so every
// expectation waits instead of assuming the datagram is already there.
constexpr s64 kDeliveryTimeoutUs = 2'000'000;
// Long enough to catch a delivery that should not happen, short enough not to slow the suite.
constexpr s64 kNoDeliveryTimeoutUs = 200'000;

sockaddr_in Loopback(u16 port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = port;
    return addr;
}

// Stands in for the inbox a guest P2PSocket owns: the port forwards into it and the guest reads
// from it, which is exactly what makes the guest-visible fd pollable.
struct Inbox {
    net_socket sock{};
    sockaddr_in addr{};

    Inbox() {
        EXPECT_TRUE(CreateP2PInbox(sock, addr));
    }
    ~Inbox() {
        CloseP2PSocket(sock);
    }

    bool Receive(P2PInboxHeader& header, std::string& payload, s64 timeout_us) {
        if (!WaitP2PSocketReadable(sock, timeout_us)) {
            return false;
        }
        char buffer[2048];
        const int received = recvfrom(sock, buffer, sizeof(buffer), 0, nullptr, nullptr);
        if (received < static_cast<int>(sizeof(P2PInboxHeader))) {
            return false;
        }
        std::memcpy(&header, buffer, sizeof(header));
        payload.assign(buffer + sizeof(header), received - sizeof(header));
        return true;
    }
};

// Stands in for shadNet's UDP forwarder: unwraps a RelayForward, and re-sends the inner frame to
// the named endpoint behind a RelayDeliver naming the true origin. Everything above the port then
// sees a direct conversation, which is the whole point of the relay path.
class FakeRelay {
public:
    FakeRelay() {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        EXPECT_TRUE(IsValidP2PSocket(sock));
        sockaddr_in bind_addr = Loopback(0);
        EXPECT_EQ(::bind(sock, reinterpret_cast<const sockaddr*>(&bind_addr), sizeof(bind_addr)),
                  0);
        socklen_t len = sizeof(addr);
        EXPECT_EQ(getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len), 0);
        worker = std::thread([this] { Run(); });
    }

    ~FakeRelay() {
        stop.store(true);
        // Wake the blocking receive with an empty datagram the loop discards.
        sendto(sock, "", 0, 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
        if (worker.joinable()) {
            worker.join();
        }
        CloseP2PSocket(sock);
    }

    u16 Port() const {
        return addr.sin_port;
    }
    u32 Addr() const {
        return addr.sin_addr.s_addr;
    }
    u64 Forwarded() const {
        return forwarded.load();
    }

    /// Sends a RelayDeliver that did not come from the relay, to prove the receiver refuses it.
    static void SendSpoofedDeliver(const sockaddr_in& victim, u32 claimed_addr, u16 claimed_port,
                                   const std::string& inner) {
        net_socket rogue = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        std::string frame;
        const u16 header[2] = {htons(kP2PInternalMarker),
                               htons(static_cast<u16>(P2PInternalChannel::Relay))};
        frame.append(reinterpret_cast<const char*>(header), kP2PInternalHeaderSize);
        frame.push_back(static_cast<char>(kP2PRelayDeliver));
        frame.append(reinterpret_cast<const char*>(&claimed_addr), sizeof(claimed_addr));
        frame.append(reinterpret_cast<const char*>(&claimed_port), sizeof(claimed_port));
        frame.append(inner);
        sendto(rogue, frame.data(), static_cast<int>(frame.size()), 0,
               reinterpret_cast<const sockaddr*>(&victim), sizeof(victim));
        CloseP2PSocket(rogue);
    }

private:
    void Run() {
        std::vector<char> buffer(kP2PMaxDatagram);
        while (!stop.load()) {
            sockaddr_in from{};
            socklen_t from_len = sizeof(from);
            const int received = recvfrom(sock, buffer.data(), static_cast<int>(buffer.size()), 0,
                                          reinterpret_cast<sockaddr*>(&from), &from_len);
            if (received <= static_cast<int>(kP2PInternalHeaderSize + kP2PRelayHeaderSize)) {
                continue;
            }
            u16 header[2];
            std::memcpy(header, buffer.data(), kP2PInternalHeaderSize);
            if (ntohs(header[0]) != kP2PInternalMarker ||
                ntohs(header[1]) != static_cast<u16>(P2PInternalChannel::Relay)) {
                continue;
            }
            const char* body = buffer.data() + kP2PInternalHeaderSize;
            if (static_cast<u8>(body[0]) != kP2PRelayForward) {
                continue;
            }
            sockaddr_in dst{};
            dst.sin_family = AF_INET;
            std::memcpy(&dst.sin_addr.s_addr, body + 1, sizeof(u32));
            std::memcpy(&dst.sin_port, body + 5, sizeof(u16));

            std::string out;
            out.append(reinterpret_cast<const char*>(header), kP2PInternalHeaderSize);
            out.push_back(static_cast<char>(kP2PRelayDeliver));
            out.append(reinterpret_cast<const char*>(&from.sin_addr.s_addr), sizeof(u32));
            out.append(reinterpret_cast<const char*>(&from.sin_port), sizeof(u16));
            out.append(body + kP2PRelayHeaderSize,
                       received - kP2PInternalHeaderSize - kP2PRelayHeaderSize);
            sendto(sock, out.data(), static_cast<int>(out.size()), 0,
                   reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
            forwarded.fetch_add(1);
        }
    }

    net_socket sock{};
    sockaddr_in addr{};
    std::atomic<bool> stop{false};
    std::atomic<u64> forwarded{0};
    std::thread worker;
};

class P2PPortTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
#ifdef _WIN32
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
};

// The load-bearing case: LittleBigPlanet 3 sends to its own bound (address, port, vport) and polls
// for the datagram to come back. With a real socket this is an OS round-trip, not an emulated one.
TEST_F(P2PPortTest, SelfProbeRoundTrips) {
    auto port = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(port, nullptr);

    Inbox inbox;
    const u16 vport = htons(3658);
    ASSERT_EQ(port->Claim(vport, false, &inbox, inbox.addr), vport);

    const std::string probe = "self-probe";
    ASSERT_EQ(port->Send(probe.data(), static_cast<u32>(probe.size()), vport, vport,
                         Loopback(port->BoundPort()), kP2PFlagDgram),
              static_cast<int>(probe.size()));

    P2PInboxHeader header{};
    std::string payload;
    ASSERT_TRUE(inbox.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_EQ(payload, probe);
    EXPECT_EQ(header.from_addr, htonl(INADDR_LOOPBACK));
    EXPECT_EQ(header.from_port, port->BoundPort());
    EXPECT_EQ(header.from_vport, vport);
}

TEST_F(P2PPortTest, DemultiplexesByVport) {
    auto port = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(port, nullptr);

    Inbox first, second;
    const u16 first_vport = htons(100);
    const u16 second_vport = htons(200);
    ASSERT_EQ(port->Claim(first_vport, false, &first, first.addr), first_vport);
    ASSERT_EQ(port->Claim(second_vport, false, &second, second.addr), second_vport);

    const std::string message = "for the second endpoint";
    ASSERT_EQ(port->Send(message.data(), static_cast<u32>(message.size()), first_vport,
                         second_vport, Loopback(port->BoundPort()), kP2PFlagDgram),
              static_cast<int>(message.size()));

    P2PInboxHeader header{};
    std::string payload;
    ASSERT_TRUE(second.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_EQ(payload, message);
    EXPECT_EQ(header.from_vport, first_vport);
    EXPECT_FALSE(first.Receive(header, payload, kNoDeliveryTimeoutUs));
}

TEST_F(P2PPortTest, RejectsVportInUseUnlessBothShare) {
    auto port = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(port, nullptr);

    Inbox owner, intruder;
    const u16 vport = htons(4242);
    ASSERT_EQ(port->Claim(vport, false, &owner, owner.addr), vport);
    // The holder did not opt into sharing, so nobody else gets the vport.
    EXPECT_EQ(port->Claim(vport, true, &intruder, intruder.addr), 0);

    port->Release(&owner);
    ASSERT_EQ(port->Claim(vport, true, &owner, owner.addr), vport);
    ASSERT_EQ(port->Claim(vport, true, &intruder, intruder.addr), vport);

    // Both opted in, so both receive the datagram, as a real stack would deliver it.
    const std::string message = "shared";
    ASSERT_EQ(port->Send(message.data(), static_cast<u32>(message.size()), vport, vport,
                         Loopback(port->BoundPort()), kP2PFlagDgram),
              static_cast<int>(message.size()));

    P2PInboxHeader header{};
    std::string payload;
    ASSERT_TRUE(owner.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_EQ(payload, message);
    ASSERT_TRUE(intruder.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_EQ(payload, message);
}

TEST_F(P2PPortTest, AllocatesEphemeralVportAndKeepsZeroReserved) {
    auto port = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(port, nullptr);

    Inbox first, second;
    const u16 first_vport = port->Claim(kP2PSignalingVport, false, &first, first.addr);
    const u16 second_vport = port->Claim(kP2PSignalingVport, false, &second, second.addr);
    EXPECT_NE(first_vport, kP2PSignalingVport);
    EXPECT_NE(second_vport, kP2PSignalingVport);
    EXPECT_NE(first_vport, second_vport);
    EXPECT_GE(ntohs(first_vport), kP2PFirstEphemeralVport);
    EXPECT_GE(ntohs(second_vport), kP2PFirstEphemeralVport);
}

// Two guest sockets bound to different vports of one host port must end up on the same OS socket,
// or the second bind would collide with the first.
TEST_F(P2PPortTest, SharesOneSocketPerHostPort) {
    auto first = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(first, nullptr);
    auto second = P2PPort::Acquire(htonl(INADDR_LOOPBACK), first->BoundPort());
    EXPECT_EQ(first.get(), second.get());

    auto other = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(other, nullptr);
    EXPECT_NE(first.get(), other.get());
    EXPECT_NE(first->BoundPort(), other->BoundPort());
}

TEST_F(P2PPortTest, DropsDatagramForUnclaimedVport) {
    auto port = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(port, nullptr);

    Inbox inbox;
    const u16 vport = htons(700);
    ASSERT_EQ(port->Claim(vport, false, &inbox, inbox.addr), vport);

    const std::string message = "nobody is listening";
    ASSERT_EQ(port->Send(message.data(), static_cast<u32>(message.size()), vport, htons(701),
                         Loopback(port->BoundPort()), kP2PFlagDgram),
              static_cast<int>(message.size()));

    P2PInboxHeader header{};
    std::string payload;
    EXPECT_FALSE(inbox.Receive(header, payload, kNoDeliveryTimeoutUs));
}

// A zero-length datagram is a legal datagram: it must arrive, not be mistaken for "no data".
TEST_F(P2PPortTest, DeliversEmptyDatagram) {
    auto port = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(port, nullptr);

    Inbox inbox;
    const u16 vport = htons(900);
    ASSERT_EQ(port->Claim(vport, false, &inbox, inbox.addr), vport);
    ASSERT_EQ(port->Send(nullptr, 0, vport, vport, Loopback(port->BoundPort()), kP2PFlagDgram), 0);

    P2PInboxHeader header{};
    std::string payload;
    ASSERT_TRUE(inbox.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_TRUE(payload.empty());
    EXPECT_EQ(header.from_vport, vport);
}

// The largest payload LittleBigPlanet 3 was measured sending is 918 bytes; check that a datagram
// well past that survives encapsulation intact.
TEST_F(P2PPortTest, DeliversLargePayloadIntact) {
    auto port = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(port, nullptr);

    Inbox inbox;
    const u16 vport = htons(1500);
    ASSERT_EQ(port->Claim(vport, false, &inbox, inbox.addr), vport);

    std::string message(1400, '\0');
    for (size_t i = 0; i < message.size(); ++i) {
        message[i] = static_cast<char>(i & 0xff);
    }
    ASSERT_EQ(port->Send(message.data(), static_cast<u32>(message.size()), vport, vport,
                         Loopback(port->BoundPort()), kP2PFlagDgram),
              static_cast<int>(message.size()));

    P2PInboxHeader header{};
    std::string payload;
    ASSERT_TRUE(inbox.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_EQ(payload, message);
}

// Releasing an endpoint must stop its delivery, or a closed guest socket would keep a stale inbox
// alive in the port's routing table.
TEST_F(P2PPortTest, ReleaseStopsDelivery) {
    auto port = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(port, nullptr);

    Inbox inbox;
    const u16 vport = htons(2600);
    ASSERT_EQ(port->Claim(vport, false, &inbox, inbox.addr), vport);
    port->Release(&inbox);

    const std::string message = "after release";
    ASSERT_EQ(port->Send(message.data(), static_cast<u32>(message.size()), vport, vport,
                         Loopback(port->BoundPort()), kP2PFlagDgram),
              static_cast<int>(message.size()));

    P2PInboxHeader header{};
    std::string payload;
    EXPECT_FALSE(inbox.Receive(header, payload, kNoDeliveryTimeoutUs));
}

// ===========================================================================
// Relay path
// ===========================================================================

// The relay has to be transparent: a guest datagram that travelled through it must reach the same
// inbox, carrying the *peer's* endpoint and vport, not the relay's. Anything else and the title
// would answer the relay directly and the reply would go nowhere.
TEST_F(P2PPortTest, RelayedGuestDatagramIsAttributedToItsOrigin) {
    FakeRelay relay;
    auto sender = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    auto receiver = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);

    sender->SetRelayEndpoint(relay.Addr(), relay.Port());
    receiver->SetRelayEndpoint(relay.Addr(), relay.Port());
    sender->SetPeerRelayed(htonl(INADDR_LOOPBACK), receiver->BoundPort(), true);
    EXPECT_TRUE(sender->IsPeerRelayed(htonl(INADDR_LOOPBACK), receiver->BoundPort()));

    Inbox inbox;
    const u16 vport = htons(30000);
    ASSERT_EQ(receiver->Claim(vport, false, &inbox, inbox.addr), vport);

    const std::string message = "through the relay";
    ASSERT_EQ(sender->Send(message.data(), static_cast<u32>(message.size()), vport, vport,
                           Loopback(receiver->BoundPort()), kP2PFlagDgram),
              static_cast<int>(message.size()));

    P2PInboxHeader header{};
    std::string payload;
    ASSERT_TRUE(inbox.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_EQ(payload, message);
    EXPECT_EQ(header.from_addr, htonl(INADDR_LOOPBACK));
    EXPECT_EQ(header.from_port, sender->BoundPort());
    EXPECT_EQ(header.from_vport, vport);
    EXPECT_NE(header.from_port, relay.Port());
    EXPECT_EQ(relay.Forwarded(), 1u);

    EXPECT_EQ(sender->Stats().sent_relayed, 1u);
    EXPECT_EQ(sender->Stats().sent_direct, 0u);
    EXPECT_EQ(receiver->Stats().recv_relayed, 1u);
}

// The handshake needs to know a peer answered through the relay, so it can stop trying to answer
// that peer directly and converge on the working path.
TEST_F(P2PPortTest, RelayedInternalDatagramReportsTheRelayFlag) {
    FakeRelay relay;
    auto sender = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    auto receiver = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);

    sender->SetRelayEndpoint(relay.Addr(), relay.Port());
    receiver->SetRelayEndpoint(relay.Addr(), relay.Port());
    sender->SetPeerRelayed(htonl(INADDR_LOOPBACK), receiver->BoundPort(), true);

    const std::string message = "matching2 handshake";
    ASSERT_EQ(sender->SendInternal(P2PInternalChannel::Matching2, message.data(),
                                   static_cast<u32>(message.size()),
                                   Loopback(receiver->BoundPort())),
              static_cast<int>(message.size()));

    char buffer[128]{};
    u32 from_addr = 0;
    u16 from_port = 0;
    bool relayed = false;
    int received = -1;
    for (int attempt = 0; attempt < 200 && received < 0; ++attempt) {
        received = receiver->ReceiveInternal(P2PInternalChannel::Matching2, buffer, sizeof(buffer),
                                             &from_addr, &from_port, &relayed);
        if (received < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ASSERT_EQ(received, static_cast<int>(message.size()));
    EXPECT_EQ(std::string(buffer, message.size()), message);
    EXPECT_TRUE(relayed);
    EXPECT_EQ(from_addr, htonl(INADDR_LOOPBACK));
    EXPECT_EQ(from_port, sender->BoundPort());
}

// A relay envelope is a claim about who sent the inner frame. Accepting one from any source would
// let a peer forge traffic from a third party, so only the configured relay may make that claim.
TEST_F(P2PPortTest, RefusesRelayDeliverFromAnySourceButTheRelay) {
    FakeRelay relay;
    auto receiver = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(receiver, nullptr);
    receiver->SetRelayEndpoint(relay.Addr(), relay.Port());

    Inbox inbox;
    const u16 vport = htons(30001);
    ASSERT_EQ(receiver->Claim(vport, false, &inbox, inbox.addr), vport);

    std::string inner;
    const u16 guest_header[3] = {vport, vport, htons(kP2PFlagDgram)};
    inner.append(reinterpret_cast<const char*>(guest_header), kP2PHeaderSize);
    inner.append("forged");
    FakeRelay::SendSpoofedDeliver(Loopback(receiver->BoundPort()), htonl(INADDR_LOOPBACK),
                                  htons(1234), inner);

    P2PInboxHeader header{};
    std::string payload;
    EXPECT_FALSE(inbox.Receive(header, payload, kNoDeliveryTimeoutUs));
    EXPECT_GE(receiver->Stats().recv_relay_rejected, 1u);
}

// Marking one peer must not divert anyone else, and clearing the marking must restore the direct
// path for that peer too.
TEST_F(P2PPortTest, OnlyMarkedPeersTakeTheRelay) {
    FakeRelay relay;
    auto sender = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    auto receiver = P2PPort::Acquire(htonl(INADDR_LOOPBACK), 0);
    ASSERT_NE(sender, nullptr);
    ASSERT_NE(receiver, nullptr);
    sender->SetRelayEndpoint(relay.Addr(), relay.Port());
    receiver->SetRelayEndpoint(relay.Addr(), relay.Port());

    Inbox inbox;
    const u16 vport = htons(30002);
    ASSERT_EQ(receiver->Claim(vport, false, &inbox, inbox.addr), vport);

    const std::string direct = "direct";
    ASSERT_EQ(sender->Send(direct.data(), static_cast<u32>(direct.size()), vport, vport,
                           Loopback(receiver->BoundPort()), kP2PFlagDgram),
              static_cast<int>(direct.size()));
    P2PInboxHeader header{};
    std::string payload;
    ASSERT_TRUE(inbox.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_EQ(payload, direct);
    EXPECT_EQ(relay.Forwarded(), 0u);
    EXPECT_EQ(sender->Stats().sent_direct, 1u);

    sender->SetPeerRelayed(htonl(INADDR_LOOPBACK), receiver->BoundPort(), true);
    sender->ClearRelayedPeers();
    EXPECT_FALSE(sender->IsPeerRelayed(htonl(INADDR_LOOPBACK), receiver->BoundPort()));

    const std::string again = "direct again";
    ASSERT_EQ(sender->Send(again.data(), static_cast<u32>(again.size()), vport, vport,
                           Loopback(receiver->BoundPort()), kP2PFlagDgram),
              static_cast<int>(again.size()));
    ASSERT_TRUE(inbox.Receive(header, payload, kDeliveryTimeoutUs));
    EXPECT_EQ(payload, again);
    EXPECT_EQ(relay.Forwarded(), 0u);
}

} // namespace
