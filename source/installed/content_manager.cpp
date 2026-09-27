// SPDX-License-Identifier: GPL-3.0-or-later
#include "content_manager.hpp"
#ifdef __SWITCH__

#include "nx/ipc/tin_ipc.h"
#include "util/title_util.hpp"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace astranas::installed {
namespace {

constexpr s32 kMaxApplicationRecords = 4096;
constexpr s32 kMaxDatabaseApplicationKeys = 65536;
constexpr u64 kMaxMetaRecordSize = 16ULL * 1024ULL * 1024ULL;

std::string result_text(const char* prefix, Result rc) {
    std::ostringstream out;
    out << prefix << "（0x" << std::hex << std::setw(8) << std::setfill('0') << rc << "）";
    return out.str();
}

bool same_key(const NcmContentMetaKey& a, const NcmContentMetaKey& b) {
    return a.id == b.id && a.version == b.version &&
           a.type == b.type && a.install_type == b.install_type;
}

bool same_record(const ContentStorageRecord& a, const ContentStorageRecord& b) {
    return same_key(a.metaRecord, b.metaRecord) && a.storageId == b.storageId;
}

using StatusKey = std::tuple<u8, u8, u32>; // content-meta type, storage ID, version
using DatabaseKey = std::pair<u8, u8>;    // storage ID, content-meta type

bool list_records(u64 application_id, std::vector<ContentStorageRecord>& records,
                  std::string& error) {
    records.clear();
    error.clear();
    s32 count = 0;
    Result rc = nsCountApplicationContentMeta(application_id, &count);
    if (R_FAILED(rc)) { error = result_text("无法读取应用内容数量", rc); return false; }
    if (count <= 0 || count > kMaxApplicationRecords) {
        error = count <= 0 ? "该应用没有可管理的已安装内容" : "应用内容记录数量异常";
        return false;
    }

    // NS's application-record list can fail even when the title's metadata is
    // available. Read the status list, then resolve those exact entries back to
    // NCM keys. Never rebuild the application record from unverified DB rows.
    std::vector<NsApplicationContentMetaStatus> statuses(static_cast<std::size_t>(count));
    s32 status_written = 0;
    rc = nsListApplicationContentMetaStatus(
        application_id, 0, statuses.data(), count, &status_written);
    if (R_FAILED(rc)) { error = result_text("无法读取应用内容状态", rc); return false; }
    if (status_written != count) {
        error = "应用内容记录在读取过程中发生变化";
        return false;
    }

    std::map<StatusKey, s32> expected;
    std::map<StatusKey, std::vector<NcmContentMetaKey>> candidates_by_status;
    std::set<DatabaseKey> databases;
    for (const auto& status : statuses) {
        // nsListApplicationContentMetaStatus is already scoped to the requested
        // application. Its per-entry application_id may name a linked content
        // title (not the base title being managed), so do not use that field as
        // the ownership check. The database scan below is filtered by the exact
        // requested application_id before any ContentStorageRecord is built.
        if (status.storageID == NcmStorageId_None ||
            status.storageID >= NcmStorageId_Any) {
            error = "应用内容状态包含无效存储位置";
            return false;
        }
        if (status.meta_type < NcmContentMetaType_Application ||
            status.meta_type > NcmContentMetaType_DataPatch) {
            error = "应用内容状态包含暂不支持的内容类型";
            return false;
        }
        const StatusKey status_key{status.meta_type, status.storageID, status.version};
        ++expected[status_key];
        databases.emplace(status.storageID, status.meta_type);
    }

    for (const auto& request : databases) {
        const u8 storage_id = request.first;
        const u8 meta_type = request.second;
        NcmContentMetaDatabase db{};
        rc = ncmOpenContentMetaDatabase(&db, static_cast<NcmStorageId>(storage_id));
        if (R_FAILED(rc)) {
            error = result_text("无法打开应用内容元数据库", rc);
            records.clear();
            return false;
        }

        NcmApplicationContentMetaKey first{};
        s32 total = 0;
        s32 written = 0;
        rc = ncmContentMetaDatabaseListApplication(
            &db, &total, &written, &first, 1,
            static_cast<NcmContentMetaType>(meta_type));
        if (R_FAILED(rc)) {
            ncmContentMetaDatabaseClose(&db);
            error = result_text("无法读取应用内容元数据库索引", rc);
            records.clear();
            return false;
        }
        if (total < 0 || total > kMaxDatabaseApplicationKeys ||
            written < 0 || written > 1 || written > total) {
            ncmContentMetaDatabaseClose(&db);
            error = "应用内容元数据库索引数量异常";
            records.clear();
            return false;
        }

        std::vector<NcmApplicationContentMetaKey> keys;
        try {
            keys.resize(static_cast<std::size_t>(total));
        } catch (const std::bad_alloc&) {
            ncmContentMetaDatabaseClose(&db);
            error = "应用内容元数据库索引占用内存过大";
            records.clear();
            return false;
        }
        s32 listed_total = 0;
        s32 listed = 0;
        if (total > 0) {
            rc = ncmContentMetaDatabaseListApplication(
                &db, &listed_total, &listed, keys.data(), total,
                static_cast<NcmContentMetaType>(meta_type));
        }
        ncmContentMetaDatabaseClose(&db);
        if (total > 0 && R_FAILED(rc)) {
            error = result_text("无法读取应用内容元数据库索引", rc);
            records.clear();
            return false;
        }
        if (listed_total != total || listed != total) {
            error = "应用内容元数据库在读取过程中发生变化";
            records.clear();
            return false;
        }

        for (const auto& application_key : keys) {
            if (application_key.application_id != application_id) continue;
            const auto& key = application_key.key;
            if (key.type != meta_type) {
                error = "应用内容元数据库返回了不匹配的内容类型";
                records.clear();
                return false;
            }
            const StatusKey status_key{key.type, storage_id, key.version};
            candidates_by_status[status_key].push_back(key);
        }
    }

    for (const auto& item : expected) {
        const auto found = candidates_by_status.find(item.first);
        if (item.second <= 0 || found == candidates_by_status.end() ||
            found->second.size() != static_cast<std::size_t>(item.second)) {
            records.clear();
            error = "应用内容状态与元数据库记录不一致，已停止管理以保护已安装内容";
            return false;
        }
    }

    records.reserve(statuses.size());
    for (const auto& status : statuses) {
        const StatusKey status_key{status.meta_type, status.storageID, status.version};
        auto found = candidates_by_status.find(status_key);
        if (found == candidates_by_status.end() || found->second.empty()) {
            records.clear();
            error = "无法将系统内容状态对应到已安装元数据";
            return false;
        }
        ContentStorageRecord record{};
        record.metaRecord = found->second.back();
        record.storageId = status.storageID;
        found->second.pop_back();
        records.push_back(record);
    }

    if (records.size() != static_cast<std::size_t>(count) ||
        std::any_of(candidates_by_status.begin(), candidates_by_status.end(), [](const auto& item) {
            return !item.second.empty();
        })) {
        records.clear();
        error = "应用内容状态与元数据库记录不一致，已停止管理以保护已安装内容";
        return false;
    }
    return true;
}

struct MetaSnapshot {
    ContentStorageRecord record{};
    std::vector<u8> raw;
    std::vector<NcmContentId> content_ids;
    u64 data_patch_id = 0;
};

bool read_snapshot(const ContentStorageRecord& record, MetaSnapshot& out, std::string& error) {
    out = {};
    out.record = record;
    const NcmStorageId storage = static_cast<NcmStorageId>(record.storageId);
    NcmContentMetaDatabase db{};
    Result rc = ncmOpenContentMetaDatabase(&db, storage);
    if (R_FAILED(rc)) { error = result_text("无法打开内容元数据库", rc); return false; }

    bool ok = false;
    do {
        u64 size = 0;
        rc = ncmContentMetaDatabaseGetSize(&db, &size, &record.metaRecord);
        if (R_FAILED(rc)) { error = result_text("无法读取内容元数据大小", rc); break; }
        if (size < sizeof(NcmContentMetaHeader) || size > kMaxMetaRecordSize) {
            error = "内容元数据大小异常"; break;
        }
        out.raw.resize(static_cast<std::size_t>(size));
        u64 actual = 0;
        rc = ncmContentMetaDatabaseGet(&db, &record.metaRecord, &actual,
                                       out.raw.data(), out.raw.size());
        if (R_FAILED(rc)) { error = result_text("无法快照内容元数据", rc); break; }
        if (actual != size) { error = "内容元数据在读取过程中发生变化"; break; }

        NcmContentMetaHeader header{};
        std::memcpy(&header, out.raw.data(), sizeof(header));
        const std::size_t extended_end =
            sizeof(header) + static_cast<std::size_t>(header.extended_header_size);
        if (extended_end > out.raw.size()) { error = "内容元数据扩展头越界"; break; }

        if (record.metaRecord.type == NcmContentMetaType_AddOnContent &&
            header.extended_header_size >= sizeof(NcmAddOnContentMetaExtendedHeader)) {
            NcmAddOnContentMetaExtendedHeader ext{};
            std::memcpy(&ext, out.raw.data() + sizeof(header), sizeof(ext));
            out.data_patch_id = ext.data_patch_id;
        }

        constexpr s32 kChunk = 128;
        for (s32 index = 0; index < kMaxApplicationRecords;) {
            NcmContentInfo infos[kChunk]{};
            s32 written = 0;
            rc = ncmContentMetaDatabaseListContentInfo(
                &db, &written, infos, kChunk, &record.metaRecord, index);
            if (R_FAILED(rc)) { error = result_text("无法读取内容文件列表", rc); break; }
            if (written < 0 || written > kChunk) { error = "系统返回了异常的内容文件数量"; break; }
            for (s32 i = 0; i < written; ++i)
                out.content_ids.push_back(infos[static_cast<std::size_t>(i)].content_id);
            index += written;
            if (written < kChunk) { ok = true; break; }
        }
    } while (false);

    ncmContentMetaDatabaseClose(&db);
    return ok;
}

bool restore_meta(const std::vector<MetaSnapshot>& snapshots, std::string& warning) {
    std::map<int, std::vector<const MetaSnapshot*>> groups;
    for (const auto& item : snapshots)
        groups[static_cast<int>(item.record.storageId)].push_back(&item);

    bool all_ok = true;
    for (const auto& group : groups) {
        NcmContentMetaDatabase db{};
        Result rc = ncmOpenContentMetaDatabase(&db, static_cast<NcmStorageId>(group.first));
        if (R_FAILED(rc)) { all_ok = false; continue; }
        bool group_ok = true;
        for (const auto* item : group.second) {
            rc = ncmContentMetaDatabaseSet(
                &db, &item->record.metaRecord, item->raw.data(), item->raw.size());
            if (R_FAILED(rc)) { group_ok = false; break; }
        }
        if (group_ok && R_FAILED(ncmContentMetaDatabaseCommit(&db))) group_ok = false;
        ncmContentMetaDatabaseClose(&db);
        if (!group_ok) all_ok = false;
    }
    if (!all_ok) warning = "恢复内容元数据时发生错误；请重新刷新已安装内容状态";
    return all_ok;
}

bool replace_application_record(u64 application_id,
                                const std::vector<ContentStorageRecord>& records,
                                bool& old_record_deleted,
                                std::string& error) {
    old_record_deleted = false;
    if (records.empty()) { error = "部分卸载不能删除最后一个本体记录；请使用“卸载全部游戏内容”"; return false; }
    Result rc = nsDeleteApplicationRecord(application_id);
    if (R_FAILED(rc)) { error = result_text("无法更新应用记录", rc); return false; }
    old_record_deleted = true;
    rc = nsPushApplicationRecord(application_id, NsApplicationRecordType_Installed,
                                 const_cast<ContentStorageRecord*>(records.data()),
                                 static_cast<u32>(records.size()));
    if (R_FAILED(rc)) { error = result_text("无法写回应用记录", rc); return false; }
    return true;
}

bool cleanup_orphans(const std::vector<MetaSnapshot>& snapshots,
                     RemovalSummary& summary, std::string& warning) {
    std::map<int, std::vector<NcmContentId>> ids_by_storage;
    for (const auto& item : snapshots) {
        auto& ids = ids_by_storage[static_cast<int>(item.record.storageId)];
        for (const auto& id : item.content_ids) {
            if (std::none_of(ids.begin(), ids.end(), [&](const NcmContentId& value) {
                    return std::memcmp(&value, &id, sizeof(id)) == 0;
                }))
                ids.push_back(id);
        }
    }

    bool all_ok = true;
    for (auto& group : ids_by_storage) {
        NcmContentMetaDatabase db{};
        NcmContentStorage storage{};
        const auto storage_id = static_cast<NcmStorageId>(group.first);
        Result rc = ncmOpenContentMetaDatabase(&db, storage_id);
        if (R_FAILED(rc)) { all_ok = false; continue; }
        rc = ncmOpenContentStorage(&storage, storage_id);
        if (R_FAILED(rc)) { ncmContentMetaDatabaseClose(&db); all_ok = false; continue; }

        for (const auto& id : group.second) {
            bool orphaned = false;
            rc = ncmContentMetaDatabaseLookupOrphanContent(&db, &orphaned, &id, 1);
            if (R_FAILED(rc)) { all_ok = false; continue; }
            if (!orphaned) continue;
            bool present = false;
            rc = ncmContentStorageHas(&storage, &present, &id);
            if (R_FAILED(rc)) { all_ok = false; continue; }
            if (present) {
                rc = ncmContentStorageDelete(&storage, &id);
                if (R_FAILED(rc)) { all_ok = false; continue; }
                ++summary.content_files;
            }
        }
        ncmContentStorageClose(&storage);
        ncmContentMetaDatabaseClose(&db);
    }
    if (!all_ok) warning = "内容记录已卸载，但部分 orphan NCA 清理失败；不会影响剩余本体/DLC";
    return all_ok;
}

bool remove_records(u64 application_id,
                    const std::vector<ContentStorageRecord>& all_records,
                    const std::vector<ContentStorageRecord>& targets,
                    RemovalSummary& summary, std::string& error) {
    summary = {};
    error.clear();
    if (targets.empty()) { error = "没有找到对应的已安装内容"; return false; }

    std::vector<MetaSnapshot> snapshots;
    snapshots.reserve(targets.size());
    for (const auto& record : targets) {
        MetaSnapshot snapshot;
        if (!read_snapshot(record, snapshot, error)) return false;
        snapshots.push_back(std::move(snapshot));
    }

    std::vector<ContentStorageRecord> remaining;
    for (const auto& record : all_records) {
        if (std::none_of(targets.begin(), targets.end(),
                         [&](const auto& target) { return same_record(record, target); }))
            remaining.push_back(record);
    }
    if (remaining.empty()) {
        error = "部分卸载会删除最后一个应用记录；请使用“卸载全部游戏内容”";
        return false;
    }

    std::map<int, std::vector<const MetaSnapshot*>> groups;
    for (const auto& item : snapshots)
        groups[static_cast<int>(item.record.storageId)].push_back(&item);

    std::vector<MetaSnapshot> committed;
    for (const auto& group : groups) {
        NcmContentMetaDatabase db{};
        Result rc = ncmOpenContentMetaDatabase(&db, static_cast<NcmStorageId>(group.first));
        if (R_FAILED(rc)) { error = result_text("无法打开内容元数据库", rc); restore_meta(committed, summary.warning); return false; }
        bool group_ok = true;
        for (const auto* item : group.second) {
            rc = ncmContentMetaDatabaseRemove(&db, &item->record.metaRecord);
            if (R_FAILED(rc)) { error = result_text("无法删除内容元数据", rc); group_ok = false; break; }
        }
        if (group_ok) {
            rc = ncmContentMetaDatabaseCommit(&db);
            if (R_FAILED(rc)) { error = result_text("无法提交内容元数据删除", rc); group_ok = false; }
        }
        ncmContentMetaDatabaseClose(&db);
        if (!group_ok) {
            restore_meta(committed, summary.warning);
            return false;
        }
        for (const auto* item : group.second) committed.push_back(*item);
    }

    std::string record_error;
    bool old_record_deleted = false;
    if (!replace_application_record(application_id, remaining, old_record_deleted, record_error)) {
        std::string restore_warning;
        restore_meta(snapshots, restore_warning);
        Result restore_rc = 0;
        const bool restore_needed = old_record_deleted;
        if (restore_needed) {
            // The old record was definitely removed but the replacement push failed.
            nsDeleteApplicationRecord(application_id);
            restore_rc = nsPushApplicationRecord(
                application_id, NsApplicationRecordType_Installed,
                const_cast<ContentStorageRecord*>(all_records.data()),
                static_cast<u32>(all_records.size()));
        }
        error = record_error;
        if (!restore_warning.empty()) error += "；" + restore_warning;
        if (restore_needed && R_FAILED(restore_rc)) error += "；原应用记录自动恢复失败";
        return false;
    }

    summary.meta_records = snapshots.size();
    cleanup_orphans(snapshots, summary, summary.warning);
    return true;
}

bool load_records_for_action(u64 application_id,
                             std::vector<ContentStorageRecord>& records,
                             std::string& error) {
    return list_records(application_id, records, error);
}

} // namespace

bool list_dlc(std::uint64_t application_id, std::vector<ManagedContentMeta>& entries,
              std::string& error) {
    entries.clear();
    error.clear();

    s32 count = 0;
    Result rc = nsCountApplicationContentMeta(application_id, &count);
    if (R_FAILED(rc)) {
        error = result_text("无法读取应用内容数量", rc);
        return false;
    }
    if (count < 0 || count > kMaxApplicationRecords) {
        error = "应用内容记录数量异常";
        return false;
    }
    if (count == 0) return true;

    // NS's status list is the authoritative installed-DLC list. The status
    // application_id is the add-on's own title ID, not the selected base game's
    // ID, so resolve ownership by the standard DLC title-ID mapping and avoid
    // requiring NS ApplicationRecord or a count-perfect NCM index reconciliation.
    std::vector<NsApplicationContentMetaStatus> statuses(static_cast<std::size_t>(count));
    s32 written = 0;
    rc = nsListApplicationContentMetaStatus(
        application_id, 0, statuses.data(), count, &written);
    if (R_FAILED(rc)) {
        error = result_text("无法读取应用内容状态", rc);
        return false;
    }
    if (written != count) {
        error = "DLC 内容状态在读取过程中发生变化";
        return false;
    }

    std::set<std::tuple<u64, u32, u8>> seen;
    for (const auto& status : statuses) {
        if (status.meta_type != NcmContentMetaType_AddOnContent ||
            status.application_id == 0 ||
            status.storageID == NcmStorageId_None ||
            status.storageID >= NcmStorageId_Any ||
            tin::util::GetBaseTitleId(
                status.application_id, NcmContentMetaType_AddOnContent) != application_id)
            continue;

        const auto identity = std::make_tuple(
            status.application_id, status.version, status.storageID);
        if (!seen.insert(identity).second) continue;

        ManagedContentMeta item{};
        item.id = status.application_id;
        item.version = status.version;
        item.type = NcmContentMetaType_AddOnContent;
        item.storage = static_cast<NcmStorageId>(status.storageID);
        entries.push_back(item);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.id != b.id) return a.id < b.id;
        return a.version > b.version;
    });
    return true;
}

bool remove_update(std::uint64_t application_id, RemovalSummary& summary, std::string& error) {
    std::vector<ContentStorageRecord> records;
    if (!load_records_for_action(application_id, records, error)) return false;
    std::vector<ContentStorageRecord> targets;
    for (const auto& record : records)
        if (record.metaRecord.type == NcmContentMetaType_Patch)
            targets.push_back(record);
    if (targets.empty()) { error = "当前游戏没有已安装升级包"; return false; }
    return remove_records(application_id, records, targets, summary, error);
}

bool remove_all_dlc(std::uint64_t application_id, RemovalSummary& summary, std::string& error) {
    std::vector<ContentStorageRecord> records;
    if (!load_records_for_action(application_id, records, error)) return false;
    std::vector<ContentStorageRecord> targets;
    for (const auto& record : records)
        if (record.metaRecord.type == NcmContentMetaType_AddOnContent ||
            record.metaRecord.type == NcmContentMetaType_DataPatch)
            targets.push_back(record);
    if (targets.empty()) { error = "当前游戏没有已安装 DLC"; return false; }
    return remove_records(application_id, records, targets, summary, error);
}

bool remove_dlc(std::uint64_t application_id, const ManagedContentMeta& dlc,
                RemovalSummary& summary, std::string& error) {
    std::vector<ContentStorageRecord> records;
    if (!load_records_for_action(application_id, records, error)) return false;

    std::vector<ContentStorageRecord> targets;
    u64 data_patch_id = 0;
    for (const auto& record : records) {
        if (record.metaRecord.type == NcmContentMetaType_AddOnContent &&
            record.metaRecord.id == dlc.id &&
            record.metaRecord.version == dlc.version &&
            static_cast<NcmStorageId>(record.storageId) == dlc.storage) {
            targets.push_back(record);
            MetaSnapshot snapshot;
            if (!read_snapshot(record, snapshot, error)) return false;
            data_patch_id = snapshot.data_patch_id;
            break;
        }
    }
    if (targets.empty()) { error = "所选 DLC 已不在当前应用记录中，请刷新后重试"; return false; }

    if (data_patch_id != 0) {
        for (const auto& record : records)
            if (record.metaRecord.type == NcmContentMetaType_DataPatch &&
                record.metaRecord.id == data_patch_id)
                targets.push_back(record);
    }
    return remove_records(application_id, records, targets, summary, error);
}

} // namespace astranas::installed
#endif
