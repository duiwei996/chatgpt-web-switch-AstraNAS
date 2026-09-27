
    void Install::InstallNcaTracked(const NcmContentInfo& contentInfo)
    {
        nx::ncm::ContentStorage storage(m_destStorageId);
        const bool existed = storage.Has(contentInfo.content_id);
        // A forced replacement is specifically the recovery path for stale/corrupt
        // registered content, including a wrong-size NCA. Do not let the stale size
        // check prevent the verified replacement from being written.
        if (existed && !m_forceReinstall)
        {
            u64 expectedSize = 0;
            ncmContentInfoSizeToU64(&contentInfo, &expectedSize);
            if (expectedSize != 0 && storage.GetSize(contentInfo.content_id) != expectedSize)
                THROW_FORMAT("Existing NCA size does not match the package metadata");
        }

        if (!existed &&
            std::none_of(m_newContentIds.begin(), m_newContentIds.end(),
                         [&](const NcmContentId& id) {
                             return same_content_id(id, contentInfo.content_id);
                         }))
            m_newContentIds.push_back(contentInfo.content_id);
        this->InstallNCA(contentInfo);
    }

    void Install::ReinstallNcaTracked(const NcmContentInfo& contentInfo)
    {
        const bool previousForce = m_forceReinstall;
        m_forceReinstall = true;
        try
        {
            this->InstallNcaTracked(contentInfo);
        }
        catch (...)
        {
            m_forceReinstall = previousForce;
            throw;
        }
        m_forceReinstall = previousForce;
    }

    std::string Install::AuditRegisteredContent(const NcmContentId& contentId)
    {
        nx::ncm::ContentStorage storage(m_destStorageId);
        const u64 size = storage.GetSize(contentId);
        if (size < sizeof(tin::install::NcaHeader))
            THROW_FORMAT("Registered NCA is smaller than its crypto header");

        std::array<u8, sizeof(tin::install::NcaHeader)> rawHeader{};
        storage.Read(contentId, 0, rawHeader.data(), rawHeader.size());
        const auto decoded = tin::install::DecodeNcaHeaderBytes(rawHeader.data(), rawHeader.size());

        AstraSha256Context hash;
        AstraSha256Context bodyHash;
        std::vector<u8> buffer(256u * 1024u);
        u64 offset = 0;
        const u64 bodyOffset = std::min<u64>(size, sizeof(tin::install::NcaHeader));
        while (offset < size)
        {
            const size_t want = static_cast<size_t>(
                std::min<u64>(buffer.size(), size - offset));
            storage.Read(contentId, offset, buffer.data(), want);
            hash.update(buffer.data(), want);
            const u64 chunkEnd = offset + want;
            if (chunkEnd > bodyOffset)
            {
                const u64 bodyBegin = std::max(offset, bodyOffset);
                const size_t bodyBeginInChunk = static_cast<size_t>(bodyBegin - offset);
                bodyHash.update(buffer.data() + bodyBeginInChunk,
                                static_cast<size_t>(chunkEnd - bodyBegin));
            }
            offset += want;
        }

        const std::string digest = sha256_digest_hex(hash.final());
        const std::string bodyDigest = sha256_digest_hex(bodyHash.final());
        const std::string expected = tin::util::GetNcaIdString(contentId);
        const bool idMatch = digest.size() >= expected.size() &&
                             digest.compare(0, expected.size(), expected) == 0;

        std::ostringstream out;
        out << "registered_size=0x" << std::hex << size
            << " registered_sha256=" << digest
            << " registered_body_sha256=" << bodyDigest
            << " content_id_match=" << (idMatch ? "yes" : "no")
            << " header_mode=" << tin::install::NcaHeaderModeLabel(decoded.mode)
            << " raw_magic=0x" << decoded.rawMagic
            << " decrypted_magic=0x" << decoded.decryptedMagic;
        if (decoded.mode != tin::install::NcaHeaderMode::Invalid) {
            out << " distribution=" << std::dec << static_cast<unsigned>(decoded.header.distribution)
                << " declared_size=0x" << std::hex << decoded.header.nca_size;
            const auto bytesToHex = [](const u8* bytes, size_t length) {
                static constexpr char digits[] = "0123456789abcdef";
                std::string value;
                value.reserve(length * 2);
                for (size_t i = 0; i < length; ++i) {
                    value.push_back(digits[bytes[i] >> 4]);
                    value.push_back(digits[bytes[i] & 0x0f]);
                }
                return value;
            };
            const auto readLe32 = [](const u8* bytes) -> u32 {
                return static_cast<u32>(bytes[0]) |
                       (static_cast<u32>(bytes[1]) << 8) |
                       (static_cast<u32>(bytes[2]) << 16) |
                       (static_cast<u32>(bytes[3]) << 24);
            };
            const auto readLe64 = [&](const u8* bytes) -> u64 {
                return static_cast<u64>(readLe32(bytes)) |
                       (static_cast<u64>(readLe32(bytes + 4)) << 32);
            };
            const auto rangeFits = [](u64 offset, u64 length, u64 limit) {
                return offset <= limit && length <= limit - offset;
            };

            const auto* expectedSectionHashes = decoded.header.section_hashes;
            for (size_t i = 0; i < 4; ++i) {
                const auto& section = decoded.header.section_entries[i];
                if (section.media_start_offset == section.media_end_offset) continue;

                const u64 sectionBegin = static_cast<u64>(section.media_start_offset) * 0x200;
                const u64 sectionEnd = static_cast<u64>(section.media_end_offset) * 0x200;
                const bool sectionBounds = sectionBegin < sectionEnd && sectionEnd <= size;
                const auto& fsHeader = decoded.header.fs_headers[i];
                const std::string actualFsHeaderHash = sha256_bytes(&fsHeader, sizeof(fsHeader));
                const std::string expectedFsHeaderHash = bytesToHex(expectedSectionHashes + i * 0x20, 0x20);

                out << " section" << std::dec << i
                    << "_range=0x" << std::hex << sectionBegin << "-0x" << sectionEnd
                    << "_bounds=" << (sectionBounds ? "pass" : "fail")
                    << "_partition_type=" << std::dec << static_cast<unsigned>(fsHeader.partition_type)
                    << "_fs_type=" << static_cast<unsigned>(fsHeader.fs_type)
                    << "_crypt_type=" << static_cast<unsigned>(fsHeader.crypt_type)
                    << "_fs_header_sha256=" << actualFsHeaderHash
                    << "_expected_section_sha256=" << expectedFsHeaderHash
                    << "_section_header_hash=" << (actualFsHeaderHash == expectedFsHeaderHash ? "match" : "different");

                // PFS0's superblock stores the expected master hash and the
                // hash-table/PFS0 ranges. Log these independently from the outer
                // NCA digest so a Horizon HFS rejection can be diagnosed without
                // treating byte-for-byte transport equality as tree validation.
                if (fsHeader.partition_type == 1 && fsHeader.fs_type == 2) {
                    const u8* superblock = fsHeader.superblock_data;
                    const u32 blockSize = readLe32(superblock + 0x20);
                    const u64 hashTableOffset = readLe64(superblock + 0x28);
                    const u64 hashTableSize = readLe64(superblock + 0x30);
                    const u64 pfs0Offset = readLe64(superblock + 0x38);
                    const u64 pfs0Size = readLe64(superblock + 0x40);
                    const u64 sectionSize = sectionBounds ? sectionEnd - sectionBegin : 0;
                    const bool pfsBounds = sectionBounds &&
                        rangeFits(hashTableOffset, hashTableSize, sectionSize) &&
                        rangeFits(pfs0Offset, pfs0Size, sectionSize);
                    out << "_pfs0_master_hash=" << bytesToHex(superblock, 0x20)
                        << "_pfs0_block_size=0x" << std::hex << blockSize
                        << "_pfs0_hash_table_offset=0x" << hashTableOffset
                        << "_pfs0_hash_table_size=0x" << hashTableSize
                        << "_pfs0_data_offset=0x" << pfs0Offset
                        << "_pfs0_data_size=0x" << pfs0Size
                        << "_pfs0_metadata_bounds=" << (pfsBounds ? "pass" : "fail");
                }
            }
        }
        return out.str();
    }

    nx::ncm::ContentMeta Install::InstallAndReadCnmtWithRepair(NcmContentInfo& contentInfo)
    {
        nx::ncm::ContentStorage storage(m_destStorageId);
        const bool existedBefore = storage.Has(contentInfo.content_id);
        u64 expectedSize = 0;
        ncmContentInfoSizeToU64(&contentInfo, &expectedSize);

        auto readInstalledCnmt = [&]() -> nx::ncm::ContentMeta
        {
            auto meta = tin::util::GetContentMetaFromNCA(storage.GetPath(contentInfo.content_id));
            // Compressed CNMT entries may intentionally enter with size=0 so an
            // already-registered healthy CNMT can be reused without reading the
            // package header merely to discover its decompressed size.
            const u64 registeredSize = storage.GetSize(contentInfo.content_id);
            ncmU64ToContentInfoSize(registeredSize, &contentInfo);
            return meta;
        };

        // v1.0.2: a healthy registered CNMT must be reusable without touching the
        // package source at all. Only size/mount failure promotes it to repair.
        std::string existingFailure;
        if (existedBefore)
        {
            try
            {
                const u64 existingSize = storage.GetSize(contentInfo.content_id);
                if (expectedSize != 0 && existingSize != expectedSize)
                    THROW_FORMAT("Existing CNMT size mismatch: expected 0x%lx, found 0x%lx",
                                 expectedSize, existingSize);
                return readInstalledCnmt();
            }
            catch (...)
            {
                existingFailure = current_exception_message();
            }
        }

        // Any CNMT bytes written from the package control the rest of the
        // transaction, so force Content-ID/CNMT SHA-256 validation even when the
        // optional large-NCA hash setting is disabled.
        auto installVerifiedCnmt = [&](bool repairExisting)
        {
            const bool previousHashVerification = inst::config::verifyNcaContentHashes;
            inst::config::verifyNcaContentHashes = true;
            try
            {
                if (repairExisting) this->ReinstallNcaTracked(contentInfo);
                else this->InstallNcaTracked(contentInfo);
            }
            catch (...)
            {
                inst::config::verifyNcaContentHashes = previousHashVerification;
                throw;
            }
            inst::config::verifyNcaContentHashes = previousHashVerification;
        };

        try
        {
            installVerifiedCnmt(existedBefore);
        }
        catch (...)
        {
            const std::string packageFailure = current_exception_message();
            if (existedBefore)
                THROW_FORMAT("Existing CNMT is unusable (%s); verified package repair failed: %s",
                             existingFailure.c_str(), packageFailure.c_str());
            THROW_FORMAT("Verified package CNMT install failed: %s", packageFailure.c_str());
        }

        try
        {
            return readInstalledCnmt();
        }
        catch (...)
        {
            const std::string mountFailure = current_exception_message();
            std::string registeredAudit;
            try
            {
                registeredAudit = this->AuditRegisteredContent(contentInfo.content_id);
            }
            catch (const std::exception& auditError)
            {
                registeredAudit = std::string("audit_failed=") + auditError.what();
            }
            const bool registeredContentIdMatches =
                registeredAudit.find("content_id_match=yes") != std::string::npos;
            const bool hierarchicalHashFailure =
                mountFailure.find("0x001fd602") != std::string::npos;
            if (registeredContentIdMatches && hierarchicalHashFailure)
                THROW_FORMAT("Horizon rejected the registered CNMT NCA filesystem hierarchical SHA-256 (0x001fd602 / 2002-4075). The registered Content ID prefix matches; compare source_audit.entry_body_sha256 with registered_audit.registered_body_sha256 to check copied NCA payload bytes without the canonicalized header. mount=[%s]; registered_audit=[%s]",
                             mountFailure.c_str(), registeredAudit.c_str());
            if (existedBefore)
                THROW_FORMAT("Existing CNMT was replaced with verified package content but mount still failed; before=%s; after=%s; registered_audit=[%s]",
                             existingFailure.c_str(), mountFailure.c_str(), registeredAudit.c_str());
            THROW_FORMAT("New verified CNMT was registered but cannot be mounted: %s; registered_audit=[%s]",
                         mountFailure.c_str(), registeredAudit.c_str());
        }
    }
    const u8* Install::FindExpectedContentHash(const NcmContentId& contentId) const
    {
        const auto value = std::find_if(m_expectedContentHashes.begin(),
                                        m_expectedContentHashes.end(),
                                        [&](const ExpectedContentHash& expected) {
                                            return same_content_id(expected.contentId, contentId);
                                        });
        return value == m_expectedContentHashes.end() ? nullptr : value->hash.data();
    }

    void Install::RegisterRequiredRightsId(const u8 rightsId[16])
    {
        const bool allZero = std::all_of(rightsId, rightsId + 16,
                                         [](u8 value) { return value == 0; });
        if (allZero) return;
        std::array<u8, 16> value{};
        std::memcpy(value.data(), rightsId, value.size());
        if (std::find(m_requiredRightsIds.begin(), m_requiredRightsIds.end(), value) ==
            m_requiredRightsIds.end())
            m_requiredRightsIds.push_back(value);
    }

    bool Install::IsRequiredRightsId(const u8 rightsId[16]) const
    {
        return std::any_of(m_requiredRightsIds.begin(), m_requiredRightsIds.end(),
                           [&](const auto& value) {
                               return std::memcmp(value.data(), rightsId, value.size()) == 0;
                           });
    }

    bool Install::TicketExists(const u8 rightsId[16]) const
    {
        auto listContains = [&](bool personalized) {
            u32 count = 0;
            const Result countRc = personalized
                ? esCountPersonalizedTicket(&count)
                : esCountCommonTicket(&count);
            ASSERT_OK(countRc, "Failed to count installed tickets");
            if (count > kMaxTicketCount)
                THROW_FORMAT("System ticket list is unreasonably large");
            if (count == 0) return false;
            std::vector<EsRightsId> ids(count);
            u32 written = 0;
            const Result rc = personalized
                ? esListPersonalizedTicket(&written, ids.data(), ids.size() * sizeof(ids[0]))
                : esListCommonTicket(&written, ids.data(), ids.size() * sizeof(ids[0]));
            ASSERT_OK(rc, "Failed to list installed tickets");
            if (written > count) THROW_FORMAT("System returned an invalid ticket count");
            return std::any_of(ids.begin(), ids.begin() + written,
                               [&](const EsRightsId& id) {
                                   return std::memcmp(id.bytes, rightsId, 16) == 0;
                               });
        };
        return listContains(false) || listContains(true);
    }

    void Install::RecordImportedTicket(const u8 rightsId[16])
    {
        std::array<u8, 16> value{};
        std::memcpy(value.data(), rightsId, value.size());
        if (std::find(m_importedRightsIds.begin(), m_importedRightsIds.end(), value) ==
            m_importedRightsIds.end())
            m_importedRightsIds.push_back(value);
    }

    std::array<u8, 16> Install::ExtractTicketRightsId(const u8* ticket, size_t size)
    {
        if (!ticket || size < 4) THROW_FORMAT("Ticket is truncated");
        size_t signatureBlockSize = 0;
        switch (read_be32(ticket))
        {
            case 0x00010000:
            case 0x00010003: signatureBlockSize = 0x240; break;
            case 0x00010001:
            case 0x00010004: signatureBlockSize = 0x140; break;
            case 0x00010002:
            case 0x00010005: signatureBlockSize = 0x80; break;
            case 0x00010006: signatureBlockSize = 0x40; break;
            default: THROW_FORMAT("Ticket has an unsupported signature type");
        }
        constexpr size_t kTicketDataSize = 0x180;
        constexpr size_t kTicketVersionOffset = 0x140;
        constexpr size_t kRightsIdOffset = 0x160;
        if (signatureBlockSize > size || size - signatureBlockSize < kTicketDataSize)
            THROW_FORMAT("Ticket data is truncated");
        const u8* data = ticket + signatureBlockSize;
        if (data[kTicketVersionOffset] != 2)
            THROW_FORMAT("Unsupported ticket version");
        std::array<u8, 16> rightsId{};
        std::memcpy(rightsId.data(), data + kRightsIdOffset, rightsId.size());
        return rightsId;
    }

    NcmPlaceHolderId Install::PlaceholderIdForContent(const NcmContentId& contentId)
    {
        static_assert(sizeof(NcmPlaceHolderId) == sizeof(NcmContentId),
                      "Content and placeholder IDs must have equal size");
        NcmPlaceHolderId placeholder{};
        std::memcpy(&placeholder, &contentId, sizeof(placeholder));
        return placeholder;
    }

    NcmPlaceHolderId Install::ReplacementPlaceholderIdForContent(const NcmContentId& contentId)
    {
        NcmPlaceHolderId placeholder = PlaceholderIdForContent(contentId);
        auto* bytes = reinterpret_cast<u8*>(&placeholder);
        bytes[0] ^= 0xA5;
        bytes[sizeof(placeholder) - 1] ^= 0x5A;
        return placeholder;
    }

    bool Install::Rollback(std::string* error) noexcept
    {
        std::string errors;
        bool applicationRecordsRestored = true;
        for (auto it = m_applicationRecords.rbegin(); it != m_applicationRecords.rend(); ++it)
        {
            if (!it->touched) continue;
            try
            {
                if (it->existed)
                {
                    const s32 count = count_application_records(it->applicationId);
                    if (count > 0)
                        ASSERT_OK(nsDeleteApplicationRecord(it->applicationId),
                                  "Failed to clear application record before restoring it");
                    ASSERT_OK(nsPushApplicationRecord(it->applicationId,
                              NsApplicationRecordType_Installed, it->previous.data(),
                              static_cast<u32>(it->previous.size())),
                              "Failed to restore application record");
                }
                else
                {
                    const s32 count = count_application_records(it->applicationId);
                    if (count > 0)
                        ASSERT_OK(nsDeleteApplicationRecord(it->applicationId),
                                  "Failed to remove new application record");
                }
                it->touched = false;
            }
            catch (...)
            {
                applicationRecordsRestored = false;
                append_error(errors, "application record rollback failed: " +
                                      current_exception_message());
            }
        }

        bool metadataRestored = true;
        if (m_metadataTouched)
        {
            try
            {
                NcmContentMetaDatabase database{};
                ASSERT_OK(ncmOpenContentMetaDatabase(&database, m_destStorageId),
                          "Failed to open content meta database for rollback");
                try
                {
                    for (auto it = m_preparedMetaRecords.rbegin();
                         it != m_preparedMetaRecords.rend(); ++it)
                    {
                        if (it->existed)
                        {
                            ASSERT_OK(ncmContentMetaDatabaseSet(&database, &it->key,
                                      it->previous.GetData(), it->previous.GetSize()),
                                      "Failed to restore content meta record");
                        }
                        else
                        {
                            bool present = false;
                            ASSERT_OK(ncmContentMetaDatabaseHas(&database, &present, &it->key),
                                      "Failed to inspect new content meta record");
                            if (present)
                                ASSERT_OK(ncmContentMetaDatabaseRemove(&database, &it->key),
                                          "Failed to remove new content meta record");
                        }
                    }
                    for (const auto& old : m_supersededMetaRecords)
                        ASSERT_OK(ncmContentMetaDatabaseSet(&database, &old.key,
                                  old.previous.GetData(), old.previous.GetSize()),
                                  "Failed to restore superseded content meta record");
                    ASSERT_OK(ncmContentMetaDatabaseCommit(&database),
                              "Failed to commit content meta rollback");
                }
                catch (...)
                {
                    ncmContentMetaDatabaseClose(&database);
                    throw;
                }
                ncmContentMetaDatabaseClose(&database);
                m_metadataTouched = false;
            }
            catch (...)
            {
                metadataRestored = false;
                append_error(errors, "content metadata rollback failed: " +
                                      current_exception_message());
            }
        }

        if (applicationRecordsRestored && metadataRestored)
        {
            std::vector<NcmContentId> retainedContent;
            try
            {
                nx::ncm::ContentStorage storage(m_destStorageId);
                for (auto it = m_newContentIds.rbegin(); it != m_newContentIds.rend(); ++it)
                {
                    bool removed = true;
                    try
                    {
                        if (storage.Has(*it)) storage.Delete(*it);
                    }
                    catch (...)
                    {
                        removed = false;
                        append_error(errors, "installed content rollback failed: " +
                                              current_exception_message());
                    }
                    try
                    {
                        storage.DeletePlaceholder(PlaceholderIdForContent(*it));
                    }
                    catch (...)
                    {
                        // A registered content normally has no remaining placeholder.
                    }
                    if (!removed) retainedContent.push_back(*it);
                }
            }
            catch (...)
            {
                retainedContent = m_newContentIds;
                append_error(errors, "content storage rollback failed: " +
                                      current_exception_message());
            }
            std::reverse(retainedContent.begin(), retainedContent.end());
            m_newContentIds = std::move(retainedContent);

            std::vector<std::array<u8, 16>> retainedTickets;
            for (auto it = m_importedRightsIds.rbegin(); it != m_importedRightsIds.rend(); ++it)
            {
                try
                {
                    if (TicketExists(it->data()))
                    {
                        EsRightsId id{};
                        std::memcpy(id.bytes, it->data(), it->size());
                        ASSERT_OK(esDeleteTicket(&id), "Failed to remove imported ticket");
                    }
                }
                catch (...)
                {
                    retainedTickets.push_back(*it);
                    append_error(errors, "ticket rollback failed: " +
                                          current_exception_message());
                }
            }
            std::reverse(retainedTickets.begin(), retainedTickets.end());
            m_importedRightsIds = std::move(retainedTickets);
        }
        else if (!m_newContentIds.empty() || !m_importedRightsIds.empty())
        {
            append_error(errors,
                         "new content was retained because database rollback was incomplete");
        }

        m_beginCompleted = false;
        if (error) *error = errors;
        return errors.empty();
    }
