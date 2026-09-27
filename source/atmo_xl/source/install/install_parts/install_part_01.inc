/*
Copyright (c) 2017-2018 Adubbz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "install/install.hpp"

#include <switch.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sstream>
#include <vector>

#include "install/nca_header_probe.hpp"
#include "nx/ncm.hpp"
#include "sha256.hpp"
#include "util/config.hpp"
#include "util/error.hpp"
#include "util/file_util.hpp"
#include "util/title_util.hpp"

namespace tin::install
{
    namespace
    {
        constexpr u64 kMaxInstalledMetaRecordSize = 16ULL * 1024ULL * 1024ULL;
        constexpr s32 kMaxApplicationMetaRecords = 4096;
        constexpr u32 kMaxTicketCount = 1024U * 1024U;

        bool is_application_id_not_found(Result rc)
        {
            return rc == MAKERESULT(16, 2);
        }

        s32 count_application_records(u64 applicationId)
        {
            s32 count = 0;
            const Result rc = nsCountApplicationContentMeta(applicationId, &count);
            if (is_application_id_not_found(rc)) return 0;
            ASSERT_OK(rc, "Failed to count existing application content metadata");
            return count;
        }

        bool same_content_id(const NcmContentId& lhs, const NcmContentId& rhs)
        {
            return std::memcmp(&lhs, &rhs, sizeof(NcmContentId)) == 0;
        }

        u64 content_info_size(const NcmContentInfo& info)
        {
            u64 size = 0;
            ncmContentInfoSizeToU64(&info, &size);
            return size;
        }

        bool compatible_content_info(const NcmContentInfo& lhs, const NcmContentInfo& rhs)
        {
            return content_info_size(lhs) == content_info_size(rhs) &&
                   lhs.content_type == rhs.content_type &&
                   lhs.id_offset == rhs.id_offset;
        }

        bool append_unique_content_info(std::vector<NcmContentInfo>& records,
                                        const NcmContentInfo& record)
        {
            const auto existing = std::find_if(records.begin(), records.end(),
                                               [&](const NcmContentInfo& value) {
                                                   return same_content_id(value.content_id,
                                                                          record.content_id);
                                               });
            if (existing == records.end()) {
                records.push_back(record);
                return true;
            }
            if (!compatible_content_info(*existing, record))
                THROW_FORMAT("Package contains conflicting metadata for one content ID");
            return false;
        }

        bool same_meta_key(const NcmContentMetaKey& lhs, const NcmContentMetaKey& rhs)
        {
            return lhs.id == rhs.id && lhs.version == rhs.version &&
                   lhs.type == rhs.type && lhs.install_type == rhs.install_type;
        }

        bool same_title_slot(const NcmContentMetaKey& lhs, const NcmContentMetaKey& rhs)
        {
            return lhs.id == rhs.id && lhs.type == rhs.type &&
                   lhs.install_type == rhs.install_type;
        }

        bool same_storage_record(const ContentStorageRecord& lhs,
                                 const ContentStorageRecord& rhs)
        {
            return same_meta_key(lhs.metaRecord, rhs.metaRecord) &&
                   lhs.storageId == rhs.storageId;
        }

        u32 read_be32(const u8* data)
        {
            return (static_cast<u32>(data[0]) << 24) |
                   (static_cast<u32>(data[1]) << 16) |
                   (static_cast<u32>(data[2]) << 8) |
                   static_cast<u32>(data[3]);
        }

        void append_error(std::string& errors, const std::string& value)
        {
            if (value.empty()) return;
            if (!errors.empty()) errors += "; ";
            errors += value;
        }

        std::string current_exception_message()
        {
            try
            {
                throw;
            }
            catch (const std::exception& error)
            {
                return error.what();
            }
            catch (...)
            {
                return "unknown error";
            }
        }
    }

    Install::Install(NcmStorageId destStorageId, bool ignoreReqFirmVersion) :
        m_destStorageId(destStorageId), m_ignoreReqFirmVersion(ignoreReqFirmVersion),
        m_contentMeta()
    {
        appletSetMediaPlaybackState(true);
    }

    Install::~Install()
    {
        if (!m_committed)
            this->Rollback();
        appletSetMediaPlaybackState(false);
    }

    void Install::SnapshotContentMetaRecords()
    {
        NcmContentMetaDatabase database{};
        ASSERT_OK(ncmOpenContentMetaDatabase(&database, m_destStorageId),
                  "Failed to open content meta database");

        try
        {
            for (auto& record : m_preparedMetaRecords)
            {
                ASSERT_OK(ncmContentMetaDatabaseHas(&database, &record.existed, &record.key),
                          "Failed to check existing content meta record");
                if (!record.existed) continue;

                u64 size = 0;
                ASSERT_OK(ncmContentMetaDatabaseGetSize(&database, &size, &record.key),
                          "Failed to query existing content meta record size");
                if (size > kMaxInstalledMetaRecordSize ||
                    size > std::numeric_limits<size_t>::max())
                    THROW_FORMAT("Existing content meta record is unreasonably large");
                record.previous.Resize(static_cast<size_t>(size));
                u64 actual = 0;
                ASSERT_OK(ncmContentMetaDatabaseGet(&database, &record.key, &actual,
                          record.previous.GetData(), record.previous.GetSize()),
                          "Failed to snapshot existing content meta record");
                if (actual != size)
                    THROW_FORMAT("Existing content meta record changed while preparing installation");
            }

            constexpr s32 kInitialVersionListCapacity = 32;
            for (const auto& prepared : m_preparedMetaRecords)
            {
                std::vector<NcmContentMetaKey> keys(kInitialVersionListCapacity);
                s32 total = 0;
                s32 written = 0;
                auto list_versions = [&] {
                    ASSERT_OK(ncmContentMetaDatabaseList(
                                  &database, &total, &written, keys.data(),
                                  static_cast<s32>(keys.size()),
                                  static_cast<NcmContentMetaType>(prepared.key.type), prepared.key.id,
                                  0, UINT64_MAX,
                                  static_cast<NcmContentInstallType>(prepared.key.install_type)),
                              "Failed to list previous content meta versions");
                    if (total < 0 || written < 0 || written > static_cast<s32>(keys.size()))
                        THROW_FORMAT("System returned an invalid content meta version count");
                };
                list_versions();
                if (total > kMaxApplicationMetaRecords)
                    THROW_FORMAT("Too many installed versions for one title slot");
                if (total > static_cast<s32>(keys.size())) {
                    keys.resize(static_cast<size_t>(total));
                    list_versions();
                    if (written != total)
                        THROW_FORMAT("Installed content meta versions changed during snapshot");
                }
                for (s32 i = 0; i < written; ++i)
                {
                    const auto& key = keys[static_cast<size_t>(i)];
                    if (!same_title_slot(key, prepared.key) || same_meta_key(key, prepared.key))
                        continue;
                    if (std::any_of(m_supersededMetaRecords.begin(), m_supersededMetaRecords.end(),
                                    [&](const auto& value) { return same_meta_key(value.key, key); }))
                        continue;

                    SupersededMetaRecord old{};
                    old.key = key;
                    u64 size = 0;
                    ASSERT_OK(ncmContentMetaDatabaseGetSize(&database, &size, &key),
                              "Failed to query previous content meta size");
                    if (size > kMaxInstalledMetaRecordSize ||
                        size > std::numeric_limits<size_t>::max())
                        THROW_FORMAT("Previous content meta record is unreasonably large");
                    old.previous.Resize(static_cast<size_t>(size));
                    u64 actual = 0;
                    ASSERT_OK(ncmContentMetaDatabaseGet(&database, &key, &actual,
                              old.previous.GetData(), old.previous.GetSize()),
                              "Failed to snapshot previous content meta record");
                    if (actual != size) THROW_FORMAT("Previous content meta changed during snapshot");

                    constexpr s32 kContentInfoPageSize = 256;
                    constexpr s32 kMaxContentInfosFromFormat = 0xFFFF;
                    s32 startIndex = 0;
                    for (;;)
                    {
                        std::array<NcmContentInfo, kContentInfoPageSize> contentInfos{};
                        s32 contentCount = 0;
                        ASSERT_OK(ncmContentMetaDatabaseListContentInfo(
                                      &database, &contentCount, contentInfos.data(),
                                      static_cast<s32>(contentInfos.size()), &key, startIndex),
                                  "Failed to list previous content records");
                        if (contentCount < 0 || contentCount > static_cast<s32>(contentInfos.size()))
                            THROW_FORMAT("System returned an invalid previous content count");
                        if (contentCount == 0) break;
                        if (startIndex > kMaxContentInfosFromFormat - contentCount)
                            THROW_FORMAT("Previous content metadata exceeds the format content-count limit");
                        for (s32 contentIndex = 0; contentIndex < contentCount; ++contentIndex)
                            old.contentIds.push_back(
                                contentInfos[static_cast<size_t>(contentIndex)].content_id);
                        startIndex += contentCount;
                    }
                    NcmContentId metaContentId{};
                    ASSERT_OK(ncmContentMetaDatabaseGetContentIdByType(
                                  &database, &metaContentId, &key, NcmContentType_Meta),
                              "Failed to locate previous CNMT content");
                    if (std::none_of(old.contentIds.begin(), old.contentIds.end(),
                                     [&](const auto& id) {
                                         return same_content_id(id, metaContentId);
                                     }))
                        old.contentIds.push_back(metaContentId);
                    m_supersededMetaRecords.push_back(std::move(old));
                }
            }
        }
        catch (...)
        {
            ncmContentMetaDatabaseClose(&database);
            throw;
        }
        ncmContentMetaDatabaseClose(&database);
    }
