#pragma once

#include <atomic>
#include <cstdint>

namespace astranas::atmo_xl_bridge {

using ProgressHook = bool (*)(const char* stage, std::uint64_t current,
                              std::uint64_t total, void* context);
using InvalidNcaHook = bool (*)(const char* content_id, void* context);

void set_install_hooks(ProgressHook progress, InvalidNcaHook invalid_nca,
                       void* context, std::atomic_bool* cancel);
void clear_install_hooks();
bool report_install_progress(const char* stage, std::uint64_t current, std::uint64_t total);
bool install_cancel_requested();
bool request_disable_nca_validation(const char* content_id);

} // namespace astranas::atmo_xl_bridge
