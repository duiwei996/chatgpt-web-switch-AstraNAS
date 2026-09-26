// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include "config.hpp"
#include "remote/remote_client.hpp"
#include "ui/gui.hpp"
#include "ui/input.hpp"
#include <string>
#include <switch.h>

namespace astranas::log_upload {

enum class LogDestinationKind { Install, NetDiag };

std::string make_log_filename(const std::string& prefix);
bool write_text_file(const std::string& path, const std::string& text, std::string& error);

// AstraNAS and AstraNAS-NetDiag each keep exactly one log upload directory.
// SMB/WebDAV connection profiles remain independent, but switching protocol does not
// create an additional log-directory setting for the same program.
std::string configured_directory(const AppConfig& config, LogDestinationKind kind);

bool configure_directory(RemoteClient& remote,
                         const AppConfig& config,
                         LogDestinationKind kind,
                         astranas::ui::Gui& gui,
                         astranas::ui::InputRouter& input,
                         PadState& pad,
                         bool& exit_requested,
                         const std::string& picker_title,
                         std::string& selected_directory,
                         std::string& error);

void clear_configured_directory(LogDestinationKind kind);

// Upload a small diagnostic log through the already configured SMB/WebDAV profile.
// If this program has no saved directory for the active endpoint, the same picker used
// by the explicit setting is opened as a first-upload fallback. Cancelling only skips
// this upload; the local log and the install/benchmark result remain intact.
bool upload_log(RemoteClient& remote,
                const AppConfig& config,
                astranas::ui::Gui& gui,
                astranas::ui::InputRouter& input,
                PadState& pad,
                bool& exit_requested,
                const std::string& local_path,
                const std::string& filename_prefix,
                const std::string& picker_title,
                std::string& uploaded_remote_path,
                std::string& error);

} // namespace astranas::log_upload
#endif
