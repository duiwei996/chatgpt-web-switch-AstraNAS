// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "../local_fs.hpp"
#include "../usb_storage.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace astranas::app::runtime_detail {
struct LocalLocation { std::string label; std::string root; };
inline std::vector<LocalLocation> local_locations(bool usb_ready) {
    std::vector<LocalLocation> result{{"SD 卡", "sdmc:/"}};
    if (!usb_ready) return result;
    const auto roots = astranas::usb_storage::roots();
    for (std::size_t i = 0; i < roots.size(); ++i) result.push_back({"USB " + std::to_string(i + 1), roots[i]});
    return result;
}
inline std::string local_location_label(const std::string& root, bool usb_ready) {
    for (const auto& location : local_locations(usb_ready)) if (location.root == root) return location.label;
    return root == "sdmc:/" ? "SD 卡" : "设备已移除";
}
inline void clamp_scroll(std::size_t& selected, std::size_t count, int rows) {
    if (count == 0) { selected = 0; return; }
    long long next = static_cast<long long>(selected) + rows;
    if (next < 0) next = 0;
    if (next >= static_cast<long long>(count)) next = static_cast<long long>(count) - 1;
    selected = static_cast<std::size_t>(next);
}
inline std::size_t visible_begin(std::size_t selected, std::size_t count, std::size_t rows) {
    if (count <= rows || selected < rows) return 0;
    return std::min(selected - rows + 1, count - rows);
}
inline bool path_looks_like_address_only(const std::string& text) {
    return text.find("://") == std::string::npos && text.find('/') == std::string::npos && text.find('\\') == std::string::npos;
}
enum class BatchQueueTouchAction { None, Start, Back, Remove, Clear };
inline BatchQueueTouchAction batch_queue_touch_action(int x, int y) {
    if (y < 630 || y >= 720 || x < 0 || x >= 1280) return BatchQueueTouchAction::None;
    if (x < 360) return BatchQueueTouchAction::Start;
    if (x < 680) return BatchQueueTouchAction::Back;
    if (x < 960) return BatchQueueTouchAction::Remove;
    return BatchQueueTouchAction::Clear;
}
} // namespace astranas::app::runtime_detail
