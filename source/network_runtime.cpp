// SPDX-License-Identifier: GPL-3.0-or-later
#include "network_runtime.hpp"
#include <sstream>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace astranas::network {

SocketProfileInfo socket_profile_info(SocketProfile profile) {
    if (profile == SocketProfile::HighThroughput) {
        // Full-application profile used by AstraNAS network transfers. This mirrors
        // the 4 MiB TCP ceiling / sb_efficiency=8 pattern used by mature Switch
        // network homebrew while retaining four BSD service sessions for SMB/curl.
        return {"AstraNAS 高吞吐", 0x100000, 0x100000, 0x400000, 0x400000, 8, 4};
    }
    return {"libnx 默认", 0x8000, 0x10000, 0x40000, 0x40000, 4, 3};
}

std::string network_link_summary(const NetworkLinkInfo& info) {
    if (!info.available) return "网络状态不可用";
    std::ostringstream out;
    if (info.wifi) {
        out << "Wi-Fi " << info.wifi_strength << "/3";
    } else if (info.ethernet) {
        out << "有线网络";
    } else {
        out << "未知网络";
    }
    out << (info.connected ? " · 已连接" : " · 未连接");
    return out.str();
}

#ifdef __SWITCH__
namespace {
bool g_wireless_priority_optimized = false;
unsigned int g_cpu_boost_refs = 0;
unsigned int g_transfer_awake_refs = 0;
bool g_auto_sleep_previously_disabled = false;
}

bool initialize_socket(SocketProfile profile, std::string& error) {
    const auto info = socket_profile_info(profile);
    SocketInitConfig config{};
    config.tcp_tx_buf_size = info.tcp_tx_initial;
    config.tcp_rx_buf_size = info.tcp_rx_initial;
    config.tcp_tx_buf_max_size = info.tcp_tx_max;
    config.tcp_rx_buf_max_size = info.tcp_rx_max;
    config.udp_tx_buf_size = 0x2400;
    config.udp_rx_buf_size = 0xA500;
    config.sb_efficiency = info.sb_efficiency;
    config.num_bsd_sessions = info.bsd_sessions;
    config.bsd_service_type = BsdServiceType_User;
    const Result rc = socketInitialize(&config);
    if (R_SUCCEEDED(rc)) {
        error.clear();
        return true;
    }
    std::ostringstream out;
    out << info.name << " socket 初始化失败（0x" << std::hex << rc << "）";
    error = out.str();
    return false;
}

bool set_wireless_priority(bool optimized, std::string& error) {
    const auto mode = optimized ? AppletWirelessPriorityMode_OptimizedForWlan
                                : AppletWirelessPriorityMode_Default;
    const Result rc = appletSetWirelessPriorityMode(mode);
    if (R_SUCCEEDED(rc)) {
        g_wireless_priority_optimized = optimized;
        error.clear();
        return true;
    }
    std::ostringstream out;
    out << "WLAN 优先模式设置失败（0x" << std::hex << rc << "）";
    error = out.str();
    return false;
}

bool wireless_priority_optimized() {
    return g_wireless_priority_optimized;
}

NetworkLinkInfo query_network_link() {
    NetworkLinkInfo out;
    NifmInternetConnectionType type{};
    NifmInternetConnectionStatus status{};
    u32 strength = 0;
    const Result rc = nifmGetInternetConnectionStatus(&type, &strength, &status);
    if (R_FAILED(rc)) return out;
    out.available = true;
    out.connected = status == NifmInternetConnectionStatus_Connected;
    out.wifi = type == NifmInternetConnectionType_WiFi;
    out.ethernet = type == NifmInternetConnectionType_Ethernet;
    out.wifi_strength = out.wifi ? strength : 0;
    return out;
}

ScopedTransferAwake::ScopedTransferAwake() {
    if (g_transfer_awake_refs == 0) {
        bool previously_disabled = false;
        const Result query_rc = appletIsAutoSleepDisabled(&previously_disabled);
        if (R_FAILED(query_rc)) {
            std::ostringstream details;
            details << "无法读取自动休眠状态 (0x" << std::hex << query_rc << ")";
            error_ = details.str();
            return;
        }
        if (!previously_disabled) {
            const Result enable_rc = appletSetAutoSleepDisabled(true);
            if (R_FAILED(enable_rc)) {
                std::ostringstream details;
                details << "无法禁止自动休眠 (0x" << std::hex << enable_rc << ")";
                error_ = details.str();
                return;
            }
        }
        g_auto_sleep_previously_disabled = previously_disabled;
    }
    ++g_transfer_awake_refs;
    enabled_ = true;
}

ScopedTransferAwake::~ScopedTransferAwake() {
    if (!enabled_ || g_transfer_awake_refs == 0) return;
    if (--g_transfer_awake_refs == 0 && !g_auto_sleep_previously_disabled)
        (void)appletSetAutoSleepDisabled(false);
}

ScopedCpuBoost::ScopedCpuBoost(bool enable) {
    if (!enable) return;
    if (g_cpu_boost_refs == 0) {
        if (R_FAILED(appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad))) return;
    }
    ++g_cpu_boost_refs;
    enabled_ = true;
}

ScopedCpuBoost::~ScopedCpuBoost() {
    if (!enabled_) return;
    if (g_cpu_boost_refs > 0) --g_cpu_boost_refs;
    if (g_cpu_boost_refs == 0) appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
}
#endif

} // namespace astranas::network
