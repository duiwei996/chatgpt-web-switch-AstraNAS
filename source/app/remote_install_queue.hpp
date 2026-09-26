// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../model.hpp"
#include "../user_title_backend.hpp"

namespace astranas::app {

enum class RemoteInstallInfoState {
    Pending,
    Available,
    NotRequired,
    Unavailable,
};

struct RemoteInstallQueueEntry {
    RemoteDirEntry entry;
    user_backend::PackageInstallInfo install_info;
    RemoteInstallInfoState info_state = RemoteInstallInfoState::Pending;
};

} // namespace astranas::app
