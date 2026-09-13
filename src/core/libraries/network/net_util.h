// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include "common/ipv4_interface.h"
#include "common/types.h"

namespace Libraries::Net {
struct OrbisNetInAddr;
}

namespace NetUtil {

class NetUtilInternal {
public:
    explicit NetUtilInternal() = default;
    ~NetUtilInternal() = default;

private:
    std::array<u8, 6> ether_address{};
    std::string default_gateway{};
    std::string netmask{};
    std::string ip{};
    u32 external_ip{0};
    u32 nat_type{0};
    u16 external_port{0};     // host order, as seen by the primary STUN endpoint
    u16 external_port_alt{0}; // host order, as seen by the alternate STUN endpoint
    void ClassifyNatLocked(u16 local_port);
    std::mutex m_mutex;
    bool selected_interface_checked{};
    std::string selected_interface_address;
    std::optional<Common::Network::Ipv4Interface> selected_interface;

    bool RetrieveSelectedInterface();

public:
    const std::array<u8, 6>& GetEthernetAddr() const;
    const std::string& GetDefaultGateway() const;
    const std::string& GetNetmask() const;
    const std::string& GetIp() const;
    u32 GetExternalIp() const;
    void SetExternalIp(u32 addr);
    u32 GetNatType() const;
    u16 GetExternalPort() const;
    /// Records what one STUN endpoint saw. Two endpoints are needed to tell an endpoint-independent
    /// mapping (punchable) from an endpoint-dependent one (symmetric, not punchable), so the NAT
    /// type stays 0 = undetermined until both have answered.
    void UpdateStunMapping(u32 mapped_addr, u16 mapped_port, bool alternate, u16 local_port);
    bool RetrieveEthernetAddr();
    bool RetrieveDefaultGateway();
    bool RetrieveNetmask();
    bool RetrieveIp();
    /// Resolves  hostname, giving up after  timeout_us microseconds per attempt and making
    ///  retry extra attempts. A budget of 0 means "library default". getaddrinfo cannot be
    /// interrupted, so an attempt that overruns its budget is abandoned rather than waited on:
    /// without this a title that asked for a short resolve blocks on the OS default instead,
    /// which is tens of seconds for an unreachable DNS server.
    int ResolveHostname(const char* hostname, Libraries::Net::OrbisNetInAddr* addr, int timeout_us,
                        int retry);
};
} // namespace NetUtil
