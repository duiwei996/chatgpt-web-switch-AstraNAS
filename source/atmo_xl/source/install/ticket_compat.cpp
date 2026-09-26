// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/install.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>
#include "util/error.hpp"

namespace {
int hex_value(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

bool rights_id_from_ticket_name(const std::string& ticketName,
                                std::array<u8, 16>& rightsId) {
    const auto slash = ticketName.find_last_of("/\\");
    const std::string base = slash == std::string::npos
        ? ticketName : ticketName.substr(slash + 1);
    if (base.size() < 36) return false;

    std::string suffix = base.substr(base.size() - 4);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (suffix != ".tik") return false;

    const std::string stem = base.substr(0, base.size() - 4);
    if (stem.size() != 32) return false;
    for (std::size_t i = 0; i < rightsId.size(); ++i) {
        const int hi = hex_value(static_cast<unsigned char>(stem[i * 2]));
        const int lo = hex_value(static_cast<unsigned char>(stem[i * 2 + 1]));
        if (hi < 0 || lo < 0) return false;
        rightsId[i] = static_cast<u8>((hi << 4) | lo);
    }
    return true;
}
}

namespace tin::install {
std::array<u8, 16> Install::ResolveTicketRightsId(const std::string& ticketName,
                                                  const u8* ticket,
                                                  size_t size) const {
    if (!ticket || size == 0) THROW_FORMAT("Ticket is empty");

    std::array<u8, 16> fromName{};
    const bool filenameHasRightsId = rights_id_from_ticket_name(ticketName, fromName);
    if (filenameHasRightsId && IsRequiredRightsId(fromName.data())) return fromName;

    std::vector<std::array<u8, 16>> matches;
    for (const auto& required : m_requiredRightsIds) {
        const auto* begin = ticket;
        const auto* end = ticket + size;
        if (std::search(begin, end, required.begin(), required.end()) != end)
            matches.push_back(required);
    }
    if (matches.size() == 1) return matches.front();
    if (matches.size() > 1)
        THROW_FORMAT("Ticket contains more than one required rights ID");

    try {
        return ExtractTicketRightsId(ticket, size);
    } catch (...) {
        // Several commonly encountered NSPs carry a ticket signature layout
        // that the old local parser does not know, while Horizon ES can still
        // decide whether the raw ticket/cert pair is acceptable. Keep the
        // filename/payload resolution separate from ES import instead of
        // rejecting solely on that parser limitation.
        if (filenameHasRightsId) return fromName;
        THROW_FORMAT("Ticket rights ID could not be resolved from its filename or payload");
    }
}
} // namespace tin::install
