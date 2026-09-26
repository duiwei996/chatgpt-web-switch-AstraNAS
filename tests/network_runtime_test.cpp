// SPDX-License-Identifier: GPL-3.0-or-later
#include "network_runtime.hpp"
#include <cassert>
#include <string>

int main() {
    using astranas::network::SocketProfile;
    const auto defaults = astranas::network::socket_profile_info(SocketProfile::LibnxDefault);
    const auto high = astranas::network::socket_profile_info(SocketProfile::HighThroughput);
    assert(defaults.tcp_rx_max == 0x40000u);
    assert(defaults.sb_efficiency == 4u);
    assert(high.tcp_tx_initial == 0x100000u);
    assert(high.tcp_rx_initial == 0x100000u);
    assert(high.tcp_tx_max == 0x400000u);
    assert(high.tcp_rx_max == 0x400000u);
    assert(high.sb_efficiency == 8u);
    assert(high.bsd_sessions == 4u);

    astranas::network::NetworkLinkInfo wifi;
    wifi.available = true;
    wifi.connected = true;
    wifi.wifi = true;
    wifi.wifi_strength = 3;
    assert(astranas::network::network_link_summary(wifi).find("Wi-Fi 3/3") != std::string::npos);
    return 0;
}
