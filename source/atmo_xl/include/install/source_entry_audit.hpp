// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace tin::install
{
    enum class SourceContentIdStatus
    {
        NotApplicable,
        Match,
        Mismatch,
        Unavailable,
    };

    inline const char* SourceContentIdStatusName(SourceContentIdStatus status) noexcept
    {
        switch (status)
        {
            case SourceContentIdStatus::NotApplicable: return "not_applicable_compressed";
            case SourceContentIdStatus::Match: return "yes";
            case SourceContentIdStatus::Mismatch: return "no";
            case SourceContentIdStatus::Unavailable: return "unavailable";
        }
        return "unavailable";
    }

    struct SourceEntryAudit
    {
        SourceContentIdStatus contentIdStatus = SourceContentIdStatus::Unavailable;
        bool targetBoundsOk = false;
        bool tableBoundsOk = false;
        bool tableOverlap = false;
        std::string entrySha256;
        std::string expectedContentId;
        std::string details;

        bool provesContentIdMismatch() const noexcept
        {
            return contentIdStatus == SourceContentIdStatus::Mismatch &&
                   targetBoundsOk && tableBoundsOk && !tableOverlap;
        }
    };
}
