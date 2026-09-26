// SPDX-License-Identifier: GPL-3.0-or-later
#include "usb_storage.hpp"

#include <mutex>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#include <usbhsfs.h>
#endif

namespace astranas::usb_storage {
namespace {
std::mutex g_mutex;
std::vector<std::string> g_roots;
bool g_initialized = false;

#ifdef __SWITCH__
void populate(const UsbHsFsDevice* devices, u32 count, void*) {
    std::vector<std::string> next;
    next.reserve(count);
    for (u32 i = 0; devices && i < count; ++i) {
        std::string root = devices[i].name;
        if (root.empty()) continue;
        if (root.back() == ':') root.push_back('/');
        else if (root.back() != '/') root += ":/";
        next.push_back(std::move(root));
    }
    std::scoped_lock lock(g_mutex);
    g_roots.swap(next);
}
#endif
} // namespace

bool initialize() {
#ifdef __SWITCH__
    if (g_initialized) return true;
    const Result rc = usbHsFsInitialize(0);
    if (R_FAILED(rc)) return false;
    usbHsFsSetPopulateCallback(&populate, nullptr);
    g_initialized = true;
    return true;
#else
    return false;
#endif
}

void finalize() {
#ifdef __SWITCH__
    if (!g_initialized) return;
    usbHsFsSetPopulateCallback(nullptr, nullptr);
    usbHsFsExit();
    g_initialized = false;
#endif
    std::scoped_lock lock(g_mutex);
    g_roots.clear();
}

std::vector<std::string> roots() {
    std::scoped_lock lock(g_mutex);
    return g_roots;
}

} // namespace astranas::usb_storage
