// SPDX-License-Identifier: GPL-3.0-or-later
#include "util/error.hpp"

#include <stdexcept>
#include <string>

int main()
{
    const std::string payload(1536, 'x');
    try {
        THROW_FORMAT("long=%s sentinel=%s", payload.c_str(), "ASTRANAS_ERROR_TAIL_OK");
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        if (message.size() < payload.size()) return 1;
        if (message.find("ASTRANAS_ERROR_TAIL_OK") == std::string::npos) return 2;
        if (message.find("long=") == std::string::npos) return 3;
        return 0;
    }
    return 4;
}
