// SPDX-License-Identifier: GPL-3.0-or-later
#include "title_catalog.hpp"
#include "manifest.hpp"
#ifdef __SWITCH__
#include <switch.h>
#endif
#include <algorithm>
#include <cstdio>
#include <memory>
#include <vector>

namespace {
#ifdef __SWITCH__
std::string result_text(Result rc) {
    char buf[48]{};
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(rc));
    return buf;
}
std::uint32_t get_application_version(std::uint64_t application_id, bool& has_patch) {
    has_patch = false;
    s32 count = 0;
    Result rc = nsCountApplicationContentMeta(application_id, &count);
    if (R_FAILED(rc) || count <= 0) return 0;
    std::vector<NsApplicationContentMetaStatus> statuses(static_cast<std::size_t>(count));
    s32 written = 0;
    rc = nsListApplicationContentMetaStatus(application_id, 0, statuses.data(), count, &written);
    if (R_FAILED(rc)) return 0;
    std::uint32_t version = 0;
    const auto limit = std::min<s32>(written, count);
    for (s32 i = 0; i < limit; ++i) {
        const auto& status = statuses[static_cast<std::size_t>(i)];
        if (status.meta_type == NcmContentMetaType_Application || status.meta_type == NcmContentMetaType_Patch) {
            version = std::max(version, status.version);
            if (status.meta_type == NcmContentMetaType_Patch) has_patch = true;
        }
    }
    return version;
}
#endif
}

bool TitleCatalog::refresh(std::string& error) {
    titles_.clear();
#ifndef __SWITCH__
    error = "TitleCatalog is only available on Nintendo Switch";
    return false;
#else
    Result rc = nsInitialize();
    if (R_FAILED(rc)) { error = "nsInitialize failed: " + result_text(rc); return false; }
    constexpr s32 kBatch = 64;
    s32 offset = 0;
    bool ok = true;
    for (;;) {
        NsApplicationRecord records[kBatch]{};
        s32 count = 0;
        rc = nsListApplicationRecord(records, kBatch, offset, &count);
        if (R_FAILED(rc)) { error = "nsListApplicationRecord failed: " + result_text(rc); ok = false; break; }
        if (count <= 0) break;
        for (s32 i = 0; i < count; ++i) {
            InstalledTitle title;
            title.application_id = records[i].application_id;
            title.version = get_application_version(title.application_id, title.has_patch);
            title.name = title_id_hex(title.application_id);
            auto control = std::make_unique<NsApplicationControlData>();
            u64 actual_size = 0;
            rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, title.application_id,
                                             control.get(), sizeof(NsApplicationControlData), &actual_size);
            if (R_SUCCEEDED(rc)) {
                NacpLanguageEntry* lang = nullptr;
                rc = nsGetApplicationDesiredLanguage(&control->nacp, &lang);
                if (R_SUCCEEDED(rc) && lang && lang->name[0] != '\0') title.name = lang->name;
            }
            titles_.push_back(std::move(title));
        }
        offset += count;
        if (count < kBatch) break;
    }
    nsExit();
    std::sort(titles_.begin(), titles_.end(), [](const InstalledTitle& a, const InstalledTitle& b) { return a.name < b.name; });
    return ok;
#endif
}

bool TitleCatalog::uninstall(std::uint64_t application_id, std::string& error) {
#ifndef __SWITCH__
    (void)application_id;
    error = "uninstall is only available on Nintendo Switch";
    return false;
#else
    Result rc = nsInitialize();
    if (R_FAILED(rc)) { error = "nsInitialize failed: " + result_text(rc); return false; }
    rc = nsDeleteApplicationEntity(application_id);
    nsExit();
    if (R_FAILED(rc)) { error = "nsDeleteApplicationEntity failed: " + result_text(rc); return false; }
    return true;
#endif
}
