// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui.hpp"
#ifdef __SWITCH__
#include "app/constants.hpp"
#include "log_upload.hpp"
#include "ui/dialogs.hpp"
#include "remote/remote_client.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <curl/curl.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <switch.h>
#include <utility>

#include "ui_parts/ui_part_01.inc"
#include "ui_parts/ui_part_02.inc"
#include "ui_parts/ui_part_03.inc"
#include "ui_parts/ui_part_04.inc"
#endif
