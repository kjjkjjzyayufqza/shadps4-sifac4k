// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <Ws2tcpip.h>
#include <iphlpapi.h>
#include <winsock2.h>
typedef SOCKET net_socket;
typedef int socklen_t;
#else
#include <cerrno>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int net_socket;
#endif
#if defined(__APPLE__)
#include <net/if_dl.h>
#include <net/route.h>
#endif
#if defined(__linux__) || defined(__FreeBSD__)
#include <fstream>
#include <iostream>
#include <sstream>
#endif

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <string.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "core/libraries/error_codes.h"
#include "net.h"
#include "net_error.h"
#include "net_util.h"

namespace NetUtil {

bool NetUtilInternal::RetrieveSelectedInterface() {
    const auto address = EmulatorSettings.GetNetworkInterfaceAddress();
    if (!selected_interface_checked || selected_interface_address != address) {
        selected_interface_checked = true;
        selected_interface_address = address;
        selected_interface = Common::Network::ResolveIpv4Interface(address);
        if (!selected_interface) {
            LOG_ERROR(Lib_NetCtl, "Selected IPv4 interface '{}' is unavailable", address);
        } else {
            LOG_INFO(Lib_NetCtl, "Selected IPv4 interface: address={} netmask={} gateway={}",
                     Common::Network::Ipv4ToString(selected_interface->address),
                     Common::Network::Ipv4ToString(selected_interface->netmask),
                     Common::Network::Ipv4ToString(selected_interface->gateway));
        }
    }
    if (!selected_interface) {
        return false;
    }
    ip = Common::Network::Ipv4ToString(selected_interface->address);
    netmask = Common::Network::Ipv4ToString(selected_interface->netmask);
    default_gateway = Common::Network::Ipv4ToString(selected_interface->gateway);
    ether_address = selected_interface->mac;
    return true;
}

const std::array<u8, 6>& NetUtilInternal::GetEthernetAddr() const {
    return ether_address;
}

bool NetUtilInternal::RetrieveEthernetAddr() {
    std::scoped_lock lock{m_mutex};
    if (!EmulatorSettings.GetNetworkInterfaceAddress().empty()) {
        return RetrieveSelectedInterface();
    }
#ifdef _WIN32
    std::vector<u8> adapter_infos(sizeof(IP_ADAPTER_INFO));
    ULONG size_infos = sizeof(IP_ADAPTER_INFO);

    if (GetAdaptersInfo(reinterpret_cast<PIP_ADAPTER_INFO>(adapter_infos.data()), &size_infos) ==
        ERROR_BUFFER_OVERFLOW)
        adapter_infos.resize(size_infos);

    if (GetAdaptersInfo(reinterpret_cast<PIP_ADAPTER_INFO>(adapter_infos.data()), &size_infos) ==
            NO_ERROR &&
        size_infos) {
        PIP_ADAPTER_INFO info = reinterpret_cast<PIP_ADAPTER_INFO>(adapter_infos.data());
        memcpy(ether_address.data(), info[0].Address, 6);
        return true;
    }
#elif defined(__APPLE__)
    ifaddrs* ifap;

    if (getifaddrs(&ifap) == 0) {
        ifaddrs* p;
        for (p = ifap; p; p = p->ifa_next) {
            if (p->ifa_addr->sa_family == AF_LINK) {
                sockaddr_dl* sdp = reinterpret_cast<sockaddr_dl*>(p->ifa_addr);
                memcpy(ether_address.data(), sdp->sdl_data + sdp->sdl_nlen, 6);
                freeifaddrs(ifap);
                return true;
            }
        }
        freeifaddrs(ifap);
    }
#elif defined(__FreeBSD__)
    // todo
#else
    ifreq ifr;
    ifconf ifc;
    char buf[1024];
    int success = 0;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock == -1)
        return false;

    ifc.ifc_len = sizeof(buf);
    ifc.ifc_buf = buf;
    if (ioctl(sock, SIOCGIFCONF, &ifc) == -1)
        return false;

    ifreq* it = ifc.ifc_req;
    const ifreq* const end = it + (ifc.ifc_len / sizeof(ifreq));

    for (; it != end; ++it) {
        strcpy(ifr.ifr_name, it->ifr_name);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
            if (!(ifr.ifr_flags & IFF_LOOPBACK)) {
                if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
                    success = 1;
                    break;
                }
            }
        }
    }

    close(sock);

    if (success) {
        memcpy(ether_address.data(), ifr.ifr_hwaddr.sa_data, 6);
        return true;
    }
#endif
    return false;
}

const std::string& NetUtilInternal::GetDefaultGateway() const {
    return default_gateway;
}

bool NetUtilInternal::RetrieveDefaultGateway() {
    std::scoped_lock lock{m_mutex};
    if (!EmulatorSettings.GetNetworkInterfaceAddress().empty()) {
        return RetrieveSelectedInterface();
    }

#ifdef _WIN32
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG family = AF_INET; // Only IPv4
    ULONG buffer_size = 15000;

    std::vector<BYTE> buffer(buffer_size);
    PIP_ADAPTER_ADDRESSES adapter_addresses =
        reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    DWORD result = GetAdaptersAddresses(family, flags, nullptr, adapter_addresses, &buffer_size);
    if (result != NO_ERROR) {
        return false;
    }

    for (PIP_ADAPTER_ADDRESSES adapter = adapter_addresses; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp)
            continue;

        IP_ADAPTER_GATEWAY_ADDRESS_LH* gateway = adapter->FirstGatewayAddress;
        while (gateway) {
            sockaddr* sa = gateway->Address.lpSockaddr;
            if (sa->sa_family == AF_INET) {
                char str[INET_ADDRSTRLEN];
                sockaddr_in* sa_in = reinterpret_cast<sockaddr_in*>(sa);
                if (inet_ntop(AF_INET, &sa_in->sin_addr, str, sizeof(str))) {
                    this->default_gateway = str;
                    return true;
                }
            }
            gateway = gateway->Next;
        }
    }

    return false;
#elif defined(__APPLE__)
    // adapted from
    // https://github.com/seladb/PcapPlusPlus/blob/a49a79e0b67b402ad75ffa96c1795def36df75c8/Pcap%2B%2B/src/PcapLiveDevice.cpp#L1236
    // route message struct for communication in APPLE device
    struct BSDRoutingMessage {
        struct rt_msghdr header;
        char messageSpace[512];
    };

    struct BSDRoutingMessage routingMessage;
    // It creates a raw socket that can be used for routing-related operations
    int sockfd = socket(PF_ROUTE, SOCK_RAW, 0);
    if (sockfd < 0) {
        return false;
    }
    memset(reinterpret_cast<char*>(&routingMessage), 0, sizeof(routingMessage));
    routingMessage.header.rtm_msglen = sizeof(struct rt_msghdr);
    routingMessage.header.rtm_version = RTM_VERSION;
    routingMessage.header.rtm_type = RTM_GET;
    routingMessage.header.rtm_addrs = RTA_DST | RTA_NETMASK;
    routingMessage.header.rtm_flags = RTF_UP | RTF_GATEWAY | RTF_STATIC;
    routingMessage.header.rtm_msglen += 2 * sizeof(sockaddr_in);

    if (write(sockfd, reinterpret_cast<char*>(&routingMessage), routingMessage.header.rtm_msglen) <
        0) {
        return false;
    }

    // Read the response from the route socket
    if (read(sockfd, reinterpret_cast<char*>(&routingMessage), sizeof(routingMessage)) < 0) {
        return false;
    }

    struct in_addr* gateAddr = nullptr;
    struct sockaddr* sa = nullptr;
    char* spacePtr = (reinterpret_cast<char*>(&routingMessage.header + 1));
    auto rtmAddrs = routingMessage.header.rtm_addrs;
    int index = 1;
    auto roundUpClosestMultiple = [](int multiple, int num) {
        return ((num + multiple - 1) / multiple) * multiple;
    };
    while (rtmAddrs) {
        if (rtmAddrs & 1) {
            sa = reinterpret_cast<sockaddr*>(spacePtr);
            if (index == RTA_GATEWAY) {
                gateAddr = &((sockaddr_in*)sa)->sin_addr;
                break;
            }
            spacePtr += sa->sa_len > 0 ? roundUpClosestMultiple(sizeof(uint32_t), sa->sa_len)
                                       : sizeof(uint32_t);
        }
        index++;
        rtmAddrs >>= 1;
    }

    if (gateAddr == nullptr) {
        return false;
    }

    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, gateAddr, str, sizeof(str));
    this->default_gateway = str;
    return true;
#elif defined(__FreeBSD__)
    return true;
#else
    std::ifstream route{"/proc/net/route"};
    std::string line;

    std::getline(route, line);
    while (std::getline(route, line)) {
        std::istringstream iss{line};
        std::string iface, destination, gateway;
        int flags;

        iss >> iface >> destination >> gateway >> std::hex >> flags;

        if (destination == "00000000") {
            u64 default_gateway{};
            std::stringstream ss;
            ss << std::hex << gateway;
            ss >> default_gateway;

            char str[INET_ADDRSTRLEN];
            in_addr addr;
            addr.s_addr = default_gateway;
            inet_ntop(AF_INET, &addr, str, sizeof(str));
            this->default_gateway = str;
            return true;
        }
    }
#endif
    return false;
}

const std::string& NetUtilInternal::GetNetmask() const {
    return netmask;
}

bool NetUtilInternal::RetrieveNetmask() {
    std::scoped_lock lock{m_mutex};
    if (!EmulatorSettings.GetNetworkInterfaceAddress().empty()) {
        return RetrieveSelectedInterface();
    }
    char netmaskStr[INET_ADDRSTRLEN];
    auto success = false;

#ifdef _WIN32
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG family = AF_INET; // Only IPv4
    ULONG buffer_size = 15000;

    std::vector<BYTE> buffer(buffer_size);
    auto adapter_addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    DWORD result = GetAdaptersAddresses(family, flags, nullptr, adapter_addresses, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        adapter_addresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(family, flags, nullptr, adapter_addresses, &buffer_size);
    }

    if (result != NO_ERROR)
        return false;

    for (auto adapter = adapter_addresses; adapter != nullptr; adapter = adapter->Next) {
        // Skip loopback and down interfaces
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->OperStatus != IfOperStatusUp)
            continue;

        for (auto unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                ULONG prefix_length = unicast->OnLinkPrefixLength;
                ULONG mask = prefix_length == 0 ? 0 : 0xFFFFFFFF << (32 - prefix_length);

                in_addr mask_addr{};
                mask_addr.S_un.S_addr = htonl(mask);

                if (inet_ntop(AF_INET, &mask_addr, netmaskStr, INET_ADDRSTRLEN)) {
                    success = true;
                }
            }
        }
    }
#else
    ifaddrs* ifap;

    if (getifaddrs(&ifap) == 0) {
        ifaddrs* p;
        for (p = ifap; p; p = p->ifa_next) {
            if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET) {
                auto sa = reinterpret_cast<sockaddr_in*>(p->ifa_netmask);
                inet_ntop(AF_INET, &sa->sin_addr, netmaskStr, INET_ADDRSTRLEN);
                success = true;
            }
        }
    }

    freeifaddrs(ifap);
#endif

    if (success) {
        netmask = netmaskStr;
    }
    return success;
}

const std::string& NetUtilInternal::GetIp() const {
    return ip;
}

u32 NetUtilInternal::GetExternalIp() const {
    return external_ip;
}

void NetUtilInternal::SetExternalIp(u32 addr) {
    external_ip = addr;
}

u32 NetUtilInternal::GetNatType() const {
    return nat_type;
}

u16 NetUtilInternal::GetExternalPort() const {
    return external_port;
}

// PS4 NAT types: 1 the console is directly reachable, 2 a NAT that keeps one mapping per socket
// regardless of destination (a hole punched with one peer works for the next), 3 a NAT that picks
// a fresh mapping per destination, which no amount of punching can work around.
void NetUtilInternal::ClassifyNatLocked(u16 local_port) {
    if (external_port == 0 || external_port_alt == 0) {
        nat_type = 0;
        return;
    }
    const bool same_mapping = external_port == external_port_alt;
    u32 local_addr = 0;
    if (!ip.empty()) {
        inet_pton(AF_INET, ip.c_str(), &local_addr);
    }
    const bool unmapped =
        same_mapping && local_port != 0 && external_port == local_port && external_ip == local_addr;
    const u32 previous = nat_type;
    nat_type = unmapped ? 1 : (same_mapping ? 2 : 3);
    if (nat_type != previous) {
        LOG_INFO(Lib_Net,
                 "NAT type {} determined: mapped port {} on the primary STUN endpoint and {} on "
                 "the alternate, local port {}",
                 nat_type, external_port, external_port_alt, local_port);
        if (nat_type == 3) {
            LOG_WARNING(Lib_Net,
                        "This NAT assigns a different mapping per destination, so a direct P2P "
                        "punch cannot succeed. Forward the P2P UDP port or use a server relay.");
        }
    }
}

void NetUtilInternal::UpdateStunMapping(u32 mapped_addr, u16 mapped_port, bool alternate,
                                        u16 local_port) {
    std::scoped_lock lock{m_mutex};
    if (mapped_addr != 0) {
        external_ip = mapped_addr;
    }
    if (alternate) {
        external_port_alt = mapped_port;
    } else {
        external_port = mapped_port;
    }
    ClassifyNatLocked(local_port);
}

bool NetUtilInternal::RetrieveIp() {
    std::scoped_lock lock{m_mutex};
    if (!EmulatorSettings.GetNetworkInterfaceAddress().empty()) {
        return RetrieveSelectedInterface();
    }

    auto sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        return false;
    }

    sockaddr_in sa{};
    socklen_t sa_len{sizeof(sa)};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = inet_addr("1.1.1.1");
    sa.sin_port = htons(80);

#ifdef _WIN32
#define close closesocket
#endif

    if (connect(sockfd, (sockaddr*)&sa, sa_len) == -1) {
        close(sockfd);
        return false;
    }

    if (getsockname(sockfd, (struct sockaddr*)&sa, &sa_len) == -1) {
        close(sockfd);
        return false;
    }

    char netmaskStr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sa.sin_addr, netmaskStr, INET_ADDRSTRLEN);
    ip = netmaskStr;

    close(sockfd);

#ifdef _WIN32
#undef close
#endif

    return true;
}

namespace {

// Budget used when the title passes 0, i.e. asks for the library default.
constexpr auto kDefaultResolveTimeout = std::chrono::seconds(10);

// Shared with an abandoned lookup thread, so it stays alive while that thread still writes to it.
struct ResolveTask {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    u32 addr = 0;
};

// Runs one getaddrinfo on its own thread and reports the outcome through `task`.
void RunLookup(const std::shared_ptr<ResolveTask>& task, std::string hostname) {
    std::thread([task, hostname = std::move(hostname)] {
        const addrinfo hints = {
            .ai_flags = AI_V4MAPPED | AI_ADDRCONFIG,
            .ai_family = AF_INET,
        };
        addrinfo* info = nullptr;
        const int gai_result = getaddrinfo(hostname.c_str(), nullptr, &hints, &info);
        {
            std::lock_guard lock(task->mutex);
            if (gai_result != 0) {
                LOG_ERROR(Lib_Net, "address resolution for {} failed: {}", hostname, gai_result);
            } else if (info == nullptr || info->ai_addr == nullptr) {
                LOG_ERROR(Lib_Net, "address resolution for {} returned no address", hostname);
            } else {
                const in_addr resolved = ((sockaddr_in*)info->ai_addr)->sin_addr;
                LOG_DEBUG(Lib_Net, "resolved address for {}: {}", hostname, inet_ntoa(resolved));
                task->addr = resolved.s_addr;
                task->ok = true;
            }
            task->done = true;
        }
        if (info != nullptr) {
            freeaddrinfo(info);
        }
        task->cv.notify_all();
    }).detach();
}

} // namespace

int NetUtilInternal::ResolveHostname(const char* hostname, Libraries::Net::OrbisNetInAddr* addr,
                                     int timeout_us, int retry) {
    const auto budget =
        timeout_us > 0
            ? std::chrono::microseconds(timeout_us)
            : std::chrono::duration_cast<std::chrono::microseconds>(kDefaultResolveTimeout);
    const int attempts = (retry > 0 ? retry : 0) + 1;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        const auto task = std::make_shared<ResolveTask>();
        RunLookup(task, hostname);

        std::unique_lock lock(task->mutex);
        if (!task->cv.wait_for(lock, budget, [&task] { return task->done; })) {
            // The lookup is still running and owns `task`; leave it to finish into that and die.
            LOG_ERROR(Lib_Net, "address resolution for {} timed out after {} us (attempt {}/{})",
                      hostname, budget.count(), attempt + 1, attempts);
            continue;
        }
        if (task->ok) {
            addr->inaddr_addr = task->addr;
            return ORBIS_OK;
        }
        return ORBIS_NET_ERETURN;
    }

    return ORBIS_NET_RESOLVER_ETIMEDOUT;
}

} // namespace NetUtil
