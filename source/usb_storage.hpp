// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace astranas::usb_storage {

bool initialize();
void finalize();
std::vector<std::string> roots();

} // namespace astranas::usb_storage
