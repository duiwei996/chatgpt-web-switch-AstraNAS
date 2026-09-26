// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

namespace astranas::network {

enum class SocketProfile { LibnxDefault, HighThroughput };

struct SocketProfileInfo {
    const char* name = "unknown";
    std::uint32_t tcp_tx_initial = 0;
    std::uint32_t tcp_rx_initial = 0;
    std::uint32_t tcp_tx_max = 0;
    std::uint32_t tcp_rx_max = 0;
    std::uint32_t sb_efficiency = 0;
    std::uint32_t bsd_sessions = 0;
};

struct NetworkLinkInfo {
    bool available = false;
    bool connected = false;
    bool wifi = false;
    bool ethernet = false;
    std::uint32_t wifi_strength = 0;
};

SocketProfileInfo socket_profile_info(SocketProfile profile);
std::string network_link_summary(const NetworkLinkInfo& info);

#ifdef __SWITCH__
bool initialize_socket(SocketProfile profile, std::string& error);
bool set_wireless_priority(bool optimized, std::string& error);
bool wireless_priority_optimized();
NetworkLinkInfo query_network_link();

class ScopedCpuBoost {
public:
    explicit ScopedCpuBoost(bool enable = true);
    ~ScopedCpuBoost();
    ScopedCpuBoost(const ScopedCpuBoost&) = delete;
    ScopedCpuBoost& operator=(const ScopedCpuBoost&) = delete;
    bool enabled() const { return enabled_; }

private:
    bool enabled_ = false;
};
#endif

} // namespace astranas::network
