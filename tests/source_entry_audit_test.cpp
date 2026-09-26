// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/source_entry_audit.hpp"

#include <cstring>

int main()
{
    using tin::install::SourceContentIdStatus;
    using tin::install::SourceContentIdStatusName;
    using tin::install::SourceEntryAudit;

    SourceEntryAudit audit{};
    audit.contentIdStatus = SourceContentIdStatus::Mismatch;
    audit.targetBoundsOk = true;
    audit.tableBoundsOk = true;
    audit.tableOverlap = false;
    if (!audit.provesContentIdMismatch()) return 1;

    audit.tableOverlap = true;
    if (audit.provesContentIdMismatch()) return 2;
    audit.tableOverlap = false;

    audit.targetBoundsOk = false;
    if (audit.provesContentIdMismatch()) return 3;
    audit.targetBoundsOk = true;

    audit.contentIdStatus = SourceContentIdStatus::Match;
    if (audit.provesContentIdMismatch()) return 4;

    if (std::strcmp(SourceContentIdStatusName(SourceContentIdStatus::Mismatch), "no") != 0) return 5;
    if (std::strcmp(SourceContentIdStatusName(SourceContentIdStatus::Match), "yes") != 0) return 6;
    if (std::strcmp(SourceContentIdStatusName(SourceContentIdStatus::NotApplicable),
                    "not_applicable_compressed") != 0) return 7;
    return 0;
}
