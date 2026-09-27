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

#pragma once

extern "C"
{
#include <switch/services/fs.h>
}

#include <memory>
#include <array>
#include <exception>
#include <string>
#include <tuple>
#include <vector>

#include "install/simple_filesystem.hpp"
#include "data/byte_buffer.hpp"

#include "nx/content_meta.hpp"
#include "nx/ipc/tin_ipc.h"

namespace tin::install
{
    struct PreparedContentInfo
    {
        u64 titleId = 0;
        u64 applicationId = 0;
        u64 installSize = 0;
        u32 version = 0;
        u32 requiredSystemVersion = 0;
        NcmContentMetaType type = NcmContentMetaType_Unknown;
    };

    struct PreparedStorageInfo
    {
        u64 requiredSpace = 0;
        u64 forceRequiredSpace = 0;
        u64 freeSpace = 0;
    };

    class Install
    {
        protected:
            struct PreparedMetaRecord
            {
                NcmContentMetaKey key{};
                tin::data::ByteBuffer install;
                bool existed = false;
                tin::data::ByteBuffer previous;
            };

            struct ExpectedContentHash
            {
                NcmContentId contentId{};
                std::array<u8, 32> hash{};
            };

            struct SupersededMetaRecord
            {
                NcmContentMetaKey key{};
                tin::data::ByteBuffer previous;
                std::vector<NcmContentId> contentIds;
            };

            struct ApplicationRecordSnapshot
            {
                u64 applicationId = 0;
                bool existed = false;
                bool touched = false;
                std::vector<ContentStorageRecord> previous;
                std::vector<ContentStorageRecord> installed;
            };

            const NcmStorageId m_destStorageId;
            bool m_ignoreReqFirmVersion = false;
            bool m_declinedValidation = false;
            bool m_forceReinstall = false;
            bool m_metadataTouched = false;
            bool m_beginCompleted = false;
            bool m_committed = false;

            std::vector<nx::ncm::ContentMeta> m_contentMeta;
            std::vector<NcmContentInfo> m_cnmtContentRecords;
            std::vector<PreparedMetaRecord> m_preparedMetaRecords;
            std::vector<SupersededMetaRecord> m_supersededMetaRecords;
            std::vector<ExpectedContentHash> m_expectedContentHashes;
            std::vector<ApplicationRecordSnapshot> m_applicationRecords;
            std::vector<NcmContentId> m_newContentIds;
            std::vector<std::array<u8, 16>> m_requiredRightsIds;
            std::vector<std::array<u8, 16>> m_importedRightsIds;
            std::string m_warning;

            Install(NcmStorageId destStorageId, bool ignoreReqFirmVersion);

            virtual std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> ReadCNMT() = 0;

            void SnapshotContentMetaRecords();
            void SnapshotApplicationRecords();
            void CommitContentMetaRecords();
            void InstallApplicationRecords();
            void CleanupSupersededContent() noexcept;
            virtual void InstallTicketCert() = 0;
            virtual void InstallNCA(const NcmContentInfo& contentInfo) = 0;

            void InstallNcaTracked(const NcmContentInfo& contentInfo);
            void ReinstallNcaTracked(const NcmContentInfo& contentInfo);
            nx::ncm::ContentMeta InstallAndReadCnmtWithRepair(NcmContentInfo& contentInfo);
            std::string AuditRegisteredContent(const NcmContentId& contentId);
            const u8* FindExpectedContentHash(const NcmContentId& contentId) const;
            void RegisterRequiredRightsId(const u8 rightsId[16]);
            bool IsRequiredRightsId(const u8 rightsId[16]) const;
            bool TicketExists(const u8 rightsId[16]) const;
            void RecordImportedTicket(const u8 rightsId[16]);
            static std::array<u8, 16> ExtractTicketRightsId(const u8* ticket, size_t size);
            std::array<u8, 16> ResolveTicketRightsId(const std::string& ticketName,
                                                     const u8* ticket, size_t size) const;
            static NcmPlaceHolderId PlaceholderIdForContent(const NcmContentId& contentId);
            static NcmPlaceHolderId ReplacementPlaceholderIdForContent(const NcmContentId& contentId);
            bool Rollback(std::string* error = nullptr) noexcept;
            [[noreturn]] void RethrowAfterRollback(std::exception_ptr original);

        public:
            virtual ~Install();

            virtual void Prepare();
            virtual void Begin();
            virtual void Verify();

            void SetForceReinstall(bool force) { m_forceReinstall = force; }
            bool IsForceReinstall() const { return m_forceReinstall; }

            virtual u64 GetTitleId(int i = 0);
            virtual NcmContentMetaType GetContentMetaType(int i = 0);
            virtual std::vector<PreparedContentInfo> GetPreparedContents();
            virtual PreparedStorageInfo GetPreparedStorageInfo();
            const std::string& GetWarning() const { return m_warning; }
    };
}
