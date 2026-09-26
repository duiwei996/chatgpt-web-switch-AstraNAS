// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include "gui.hpp"
#include "input.hpp"
#include "../config.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <switch.h>

namespace astranas::ui {
void draw_simple_message(Gui& gui, const std::string& title, const std::string& detail,
                         const std::string& primary_button, const std::string& primary_label,
                         const std::string& secondary_button = {}, const std::string& secondary_label = {},
                         const std::string& header_title = {});
bool confirm_action(Gui& gui, PadState& pad, bool& exit_requested,
                    const std::string& title, const std::string& detail,
                    const std::string& confirm_label = "确认", const std::string& cancel_label = "取消");
bool prompt_text(const char* title, const std::string& initial, std::string& output, bool password, bool username);
bool choose_protocol(Gui& gui, PadState& pad, InputRouter& input, bool& exit_requested,
                     const std::string& current, std::string& selected);
bool edit_smb_profile(Gui& gui, PadState& pad, InputRouter& input, bool& exit_requested,
                      const SmbProfileConfig& initial, SmbProfileConfig& output);
bool edit_webdav_profile(Gui& gui, PadState& pad, InputRouter& input, bool& exit_requested,
                         const WebDavProfileConfig& initial, WebDavProfileConfig& output);
void show_message_wait(Gui& gui, PadState& pad, bool& exit_requested,
                       const std::string& title, const std::string& detail,
                       const std::string& header_title = {});
int choose_action(Gui& gui, PadState& pad, InputRouter& input, bool& exit_requested,
                  const std::string& title, const std::string& detail,
                  const std::vector<std::string>& actions,
                  const std::string& header_title = {});
void draw_progress_page(Gui& gui, const std::string& title, const std::string& name,
                        const std::string& detail, std::uint64_t done, std::uint64_t total,
                        double mib_per_sec, int attempt = 0, int max_attempts = 0);
void show_wifi_notice(Gui& gui, PadState& pad, bool& exit_requested, bool allow_remind_later);
} // namespace astranas::ui
#endif
