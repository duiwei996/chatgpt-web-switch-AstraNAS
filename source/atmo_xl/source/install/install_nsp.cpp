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

#include "install/install_nsp.hpp"

#include <algorithm>
#include <cctype>
#include <machine/endian.h>
#include <cstring>
#include <limits>
#include <exception>

#include "install/nca.hpp"
#include "install/nca_header_probe.hpp"
#include "nx/fs.hpp"
#include "nx/ncm.hpp"
#include "util/config.hpp"
#include "util/crypto.hpp"
#include "util/file_util.hpp"
#include "util/title_util.hpp"
#include "util/debug.h"
#include "util/error.hpp"
#include "util/util.hpp"
#include "bridge/install_hooks.hpp"

namespace {
    bool has_ncz_suffix(const std::string& name)
    {
        if (name.size() < 4) return false;
        const std::size_t offset = name.size() - 4;
        return name[offset] == '.' &&
               std::tolower(static_cast<unsigned char>(name[offset + 1])) == 'n' &&
               std::tolower(static_cast<unsigned char>(name[offset + 2])) == 'c' &&
               std::tolower(static_cast<unsigned char>(name[offset + 3])) == 'z';
    }

    std::string audit_field(const std::string& audit, const char* field)
    {
        const std::size_t key = audit.find(field);
        if (key == std::string::npos) return {};
        const std::size_t begin = key + std::strlen(field);
        const std::size_t end = audit.find_first_of(" \r\n]", begin);
        return audit.substr(begin, end == std::string::npos ? end : end - begin);
    }
}

namespace tin::install::nsp
{
    NSPInstall::NSPInstall(NcmStorageId destStorageId, bool ignoreReqFirmVersion, const std::shared_ptr<NSP>& remoteNSP) :
        Install(destStorageId, ignoreReqFirmVersion), m_NSP(remoteNSP)
    {
        m_NSP->RetrieveHeader();
    }

    std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> NSPInstall::ReadCNMT()
    {
        std::vector<std::tuple<nx::ncm::ContentMeta, NcmContentInfo>> CNMTList;

        auto cnmtEntries = m_NSP->GetFileEntriesByExtension("cnmt.nca");
        auto compressedCnmtEntries = m_NSP->GetFileEntriesByExtension("cnmt.ncz");
        cnmtEntries.insert(cnmtEntries.end(), compressedCnmtEntries.begin(), compressedCnmtEntries.end());
        for (const PFS0FileEntry* fileEntry : cnmtEntries) {
            std::string cnmtNcaName(m_NSP->GetFileEntryName(fileEntry));
            NcmContentId cnmtContentId = tin::util::GetNcaIdFromString(cnmtNcaName);
            // For NCZ metadata do not read the package header just to discover the
            // decompressed size. A healthy registered CNMT can be mounted first;
            // InstallAndReadCnmtWithRepair fills the actual registered size.
            const bool compressedCnmt = has_ncz_suffix(cnmtNcaName);
            u64 cnmtNcaSize = compressedCnmt ? 0 : fileEntry->fileSize;

            LOG_DEBUG("CNMT Name: %s\n", cnmtNcaName.c_str());

            NcmContentInfo cnmtContentInfo{};
            cnmtContentInfo.content_id = cnmtContentId;
            if (cnmtNcaSize != 0)
                ncmU64ToContentInfoSize(cnmtNcaSize, &cnmtContentInfo);
            cnmtContentInfo.content_type = NcmContentType_Meta;

            // Prepare needs a mounted CNMT. Reuse a healthy registered CNMT, but
            // automatically replace stale/corrupt content from the verified package
            // before retrying the mount. If mounting fails, include both the source
            // entry's full SHA-256 and the registered NCA audit so transport/write
            // differences cannot be mistaken for a bad package.
            try {
                CNMTList.push_back({this->InstallAndReadCnmtWithRepair(cnmtContentInfo),
                                    cnmtContentInfo});
            } catch (const std::exception& installError) {
                std::string sourceAudit;
                std::string sourceSha256;
                std::string sourceBodySha256;
                try {
                    const auto audit = m_NSP->AuditFileEntry(
                        fileEntry, cnmtContentId, !compressedCnmt);
                    sourceAudit = audit.details;
                    sourceSha256 = audit.entrySha256;
                    sourceBodySha256 = audit.entryBodySha256;
                } catch (const std::exception& auditError) {
                    sourceAudit = std::string("audit_failed=") + auditError.what();
                }

                const std::string registeredSha256 =
                    audit_field(installError.what(), "registered_sha256=");
                const std::string registeredBodySha256 =
                    audit_field(installError.what(), "registered_body_sha256=");
                const bool sourceFullHashMatches = !sourceSha256.empty() &&
                                                   sourceSha256 == registeredSha256;
                const bool sourceContentIdMatches =
                    sourceAudit.find("content_id_match=yes") != std::string::npos;
                const char* comparison = "unavailable";
                if (compressedCnmt) comparison = "not_applicable_compressed";
                else if (!sourceBodySha256.empty() && !registeredBodySha256.empty())
                    comparison = sourceBodySha256 == registeredBodySha256 ? "match" : "different";

                const char* fullComparison = "unavailable";
                if (!sourceSha256.empty() && !registeredSha256.empty())
                    fullComparison = sourceFullHashMatches ? "match" : "different";

                if (!compressedCnmt && sourceContentIdMatches && sourceFullHashMatches &&
                    std::string(installError.what()).find("0x001fd602") != std::string::npos) {
                    THROW_FORMAT("Horizon rejected the registered CNMT NCA filesystem (0x001fd602 / 2002-4075). The source Content ID prefix matches and the source and registered NCA have identical full SHA-256; AstraNAS did not alter the CNMT NCA during transfer or registration, and disabling the optional NCA hash setting will not bypass this system mount check. Check the exact package being installed and the console's CNMT/NCA filesystem state; compare package-specific audit data rather than weakening integrity validation. [source_vs_registered_body_sha256=match source_vs_registered_full_sha256=match source_full_sha256=%s registered_full_sha256=%s registered_audit=[%s] source_audit=[%s]]",
                                 sourceSha256.c_str(), registeredSha256.c_str(),
                                 installError.what(), sourceAudit.c_str());
                }

                THROW_FORMAT("NSP CNMT install/mount failed [entry=%s entry_size=0x%lx content_id=%s source_vs_registered_body_sha256=%s source_vs_registered_full_sha256=%s source_full_sha256=%s registered_full_sha256=%s]; cause=[%s]; source_audit=[%s]",
                             cnmtNcaName.c_str(), static_cast<u64>(fileEntry->fileSize),
                             tin::util::GetNcaIdString(cnmtContentId).c_str(), comparison, fullComparison,
                             sourceSha256.empty() ? "unavailable" : sourceSha256.c_str(),
                             registeredSha256.empty() ? "unavailable" : registeredSha256.c_str(),
                             installError.what(), sourceAudit.c_str());
            }
        }

        return CNMTList;
    }

    void NSPInstall::InstallNCA(const NcmContentInfo& contentInfo)
    {
        const NcmContentId& ncaId = contentInfo.content_id;
        const PFS0FileEntry* fileEntry = m_NSP->GetFileEntryByNcaId(ncaId);
        if (!fileEntry) THROW_FORMAT("package is missing a referenced NCA");
        std::string ncaFileName = m_NSP->GetFileEntryName(fileEntry);
        const bool compressed = has_ncz_suffix(ncaFileName);
        const u64 headerOffset = m_NSP->GetDataOffset() + fileEntry->dataOffset;
        tin::install::NcaHeader header{};
        try {
            header = tin::install::ReadValidatedNcaHeader(
                "NSP", ncaFileName, headerOffset, fileEntry->fileSize, compressed,
                m_destStorageId,
                [&](void* out, std::size_t size) {
                    m_NSP->BufferData(out, static_cast<off_t>(headerOffset), size);
                });
        } catch (const std::exception& headerError) {
            if (contentInfo.content_type != NcmContentType_Meta) throw;
            tin::install::SourceEntryAudit auditResult{};
            bool auditReady = false;
            std::string audit;
            try {
                auditResult = m_NSP->AuditFileEntry(fileEntry, ncaId, !compressed);
                audit = auditResult.details;
                auditReady = true;
            } catch (const std::exception& auditError) {
                audit = std::string("audit_failed=") + auditError.what();
            }

            if (auditReady && auditResult.provesContentIdMismatch()) {
                THROW_FORMAT(
                    "安装包 CNMT 内容与 Content ID 不一致: file=%s expected_content_id=%s actual_sha256=%s; "
                    "请重新获取、重新复制或重新打包安装包; source_audit=[%s]",
                    ncaFileName.c_str(), auditResult.expectedContentId.c_str(),
                    auditResult.entrySha256.c_str(), audit.c_str());
            }
            THROW_FORMAT("%s; source_audit=[%s]", headerError.what(), audit.c_str());
        }
        u64 expectedSize = 0;
        ncmContentInfoSizeToU64(&contentInfo, &expectedSize);
        if (expectedSize != 0 && header.nca_size != expectedSize)
            THROW_FORMAT("NCA size does not match CNMT content record");
        if (!compressed && fileEntry->fileSize != header.nca_size)
            THROW_FORMAT("Uncompressed NCA size does not match its header");
        #ifdef NXLINK_DEBUG
        LOG_DEBUG("Installing %s to storage Id %u\n", ncaFileName.c_str(), m_destStorageId);
        #endif

        if (inst::config::validateNCAs && !m_declinedValidation)
        {
            if (!Crypto::rsa2048PssVerify(&header.magic, 0x200, header.fixed_key_sig, Crypto::NCAHeaderSignature))
            {
                const std::string contentId = tin::util::GetNcaIdString(ncaId);
                if (!astranas::atmo_xl_bridge::request_disable_nca_validation(contentId.c_str()))
                    THROW_FORMAT("NCA header signature verification failed for %s", contentId.c_str());
                m_declinedValidation = true;
                inst::config::validateNCAs = false;
            }
        }

        std::shared_ptr<nx::ncm::ContentStorage> contentStorage(new nx::ncm::ContentStorage(m_destStorageId));
        this->RegisterRequiredRightsId(reinterpret_cast<const u8*>(header.m_rightsId));
        const bool replacing = contentStorage->Has(ncaId);
        if (replacing && !this->IsForceReinstall()) return;
        const NcmPlaceHolderId placeholderId = replacing
            ? this->ReplacementPlaceholderIdForContent(ncaId)
            : this->PlaceholderIdForContent(ncaId);

        try { contentStorage->DeletePlaceholder(placeholderId); }
        catch (...) {}

        LOG_DEBUG("Size: 0x%lx\n", fileEntry->fileSize);
        bool existingDeleted = false;
        try
        {
            if (replacing)
                m_NSP->StreamToReplacementPlaceholder(contentStorage, ncaId, placeholderId,
                                                       this->FindExpectedContentHash(ncaId));
            else
                m_NSP->StreamToPlaceholder(contentStorage, ncaId,
                                           this->FindExpectedContentHash(ncaId));
            LOG_DEBUG("Registering placeholder...\n");
            if (replacing) {
                contentStorage->Delete(ncaId);
                existingDeleted = true;
            }
            contentStorage->Register(placeholderId, ncaId);
        }
        catch (...)
        {
            if (existingDeleted) {
                // 替换 placeholder 在删除旧 NCA 前必须完整写入并通过大小检查。
                // 强内容哈希为可选项。
                bool recovered = false;
                try { contentStorage->Register(placeholderId, ncaId); recovered = true; }
                catch (...) {}
                if (!recovered)
                    THROW_FORMAT("替换内容无法注册，已保留恢复 placeholder");
            } else {
                try { contentStorage->DeletePlaceholder(placeholderId); }
                catch (...) {}
            }
            throw;
        }

        try { contentStorage->DeletePlaceholder(placeholderId); }
        catch (...) {}
    }

    void NSPInstall::InstallTicketCert()
    {
        constexpr u64 kMaxTicketSize = 64ULL * 1024ULL;
        constexpr u64 kMaxCertificateSize = 128ULL * 1024ULL;
        struct TicketCandidate {
            const PFS0FileEntry* ticket;
            const PFS0FileEntry* certificate;
            std::array<u8, 16> rightsId;
        };
        std::vector<const PFS0FileEntry*> tikFileEntries = m_NSP->GetFileEntriesByExtension("tik");
        std::vector<const PFS0FileEntry*> certFileEntries = m_NSP->GetFileEntriesByExtension("cert");
        std::vector<TicketCandidate> candidates;
        std::vector<std::array<u8, 16>> matchedRightsIds;
        for (size_t i = 0; i < tikFileEntries.size(); i++)
        {
            if (tikFileEntries[i] == nullptr) {
                LOG_DEBUG("Remote tik file is missing.\n");
                THROW_FORMAT("Remote tik file is not present!");
            }

            u64 tikSize = tikFileEntries[i]->fileSize;
            if (tikSize < 4 || tikSize > kMaxTicketSize || tikSize > std::numeric_limits<size_t>::max())
                THROW_FORMAT("Ticket has an invalid size");
            auto tikBuf = std::make_unique<u8[]>(static_cast<size_t>(tikSize));
            LOG_DEBUG("> Reading tik\n");
            m_NSP->BufferData(tikBuf.get(), m_NSP->GetDataOffset() + tikFileEntries[i]->dataOffset,
                              static_cast<size_t>(tikSize));

            const std::string ticketName = m_NSP->GetFileEntryName(tikFileEntries[i]);
            const auto rightsId = this->ResolveTicketRightsId(ticketName, tikBuf.get(), static_cast<size_t>(tikSize));
            if (!this->IsRequiredRightsId(rightsId.data()))
            {
                LOG_DEBUG("Skipping ticket not referenced by installed NCAs.\n");
                continue;
            }
            if (std::find(matchedRightsIds.begin(), matchedRightsIds.end(), rightsId) != matchedRightsIds.end())
                continue;

            matchedRightsIds.push_back(rightsId);
            if (this->TicketExists(rightsId.data()))
                continue;

            const std::string certName = ticketName.substr(0, ticketName.size() - 4) + ".cert";
            const PFS0FileEntry* certEntry = m_NSP->GetFileEntryByName(certName);
            if (certEntry == nullptr && certFileEntries.size() == tikFileEntries.size() && i < certFileEntries.size())
                certEntry = certFileEntries[i];
            if (certEntry == nullptr)
            {
                LOG_DEBUG("Remote cert file is missing.\n");
                THROW_FORMAT("Remote cert file is not present!");
            }

            u64 certSize = certEntry->fileSize;
            if (certSize == 0 || certSize > kMaxCertificateSize || certSize > std::numeric_limits<size_t>::max())
                THROW_FORMAT("Ticket certificate has an invalid size");

            candidates.push_back({tikFileEntries[i], certEntry, rightsId});
        }
        for (const auto& required : m_requiredRightsIds)
        {
            const bool covered = std::find(matchedRightsIds.begin(), matchedRightsIds.end(), required) !=
                                 matchedRightsIds.end();
            if (!covered && !this->TicketExists(required.data()))
                THROW_FORMAT("A required ticket is neither installed nor present in the package");
        }

        // Import only after the complete ticket set and every matching
        // certificate have been validated, avoiding partial imports caused by
        // a later malformed package entry.
        for (const auto& candidate : candidates)
        {
            const u64 tikSize = candidate.ticket->fileSize;
            const u64 certSize = candidate.certificate->fileSize;
            auto tikBuf = std::make_unique<u8[]>(static_cast<size_t>(tikSize));
            auto certBuf = std::make_unique<u8[]>(static_cast<size_t>(certSize));
            LOG_DEBUG("> Reading tik\n");
            m_NSP->BufferData(tikBuf.get(), m_NSP->GetDataOffset() + candidate.ticket->dataOffset,
                              static_cast<size_t>(tikSize));
            LOG_DEBUG("> Reading cert\n");
            m_NSP->BufferData(certBuf.get(), m_NSP->GetDataOffset() + candidate.certificate->dataOffset,
                              static_cast<size_t>(certSize));

            this->RecordImportedTicket(candidate.rightsId.data());
            ASSERT_OK(esImportTicket(tikBuf.get(), tikSize, certBuf.get(), certSize), "Failed to import ticket");
        }
    }
}
