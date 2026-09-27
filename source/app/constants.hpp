// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>

namespace astranas::app {
inline constexpr const char* kVersion = "1.1.14";
inline constexpr const char* kDisplayName = "Switch游戏安装";
inline constexpr const char* kConfigPath = "sdmc:/switch/AstraNAS/config.ini";
inline constexpr const char* kBaseDir = "sdmc:/switch/AstraNAS";
inline constexpr const char* kWifiNoticeSeenPath = "sdmc:/switch/AstraNAS/.wifi_notice_seen";
inline constexpr const char* kCacheRoot = "sdmc:/switch/AstraNAS/cache";
inline constexpr const char* kUiCacheDir = "sdmc:/switch/AstraNAS/ui-cache";
inline constexpr const char* kLogPath = "sdmc:/switch/AstraNAS/astranas.log";
inline constexpr std::size_t kBenchmarkBytes = 64u * 1024u * 1024u;
}
