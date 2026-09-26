
    void Install::SnapshotApplicationRecords()
    {
        m_applicationRecords.clear();
        for (auto& meta : m_contentMeta)
        {
            const u64 applicationId = meta.GetApplicationId();
            if (std::any_of(m_applicationRecords.begin(), m_applicationRecords.end(),
                            [&](const ApplicationRecordSnapshot& value) {
                                return value.applicationId == applicationId;
                            }))
                continue;

            ApplicationRecordSnapshot snapshot{};
            snapshot.applicationId = applicationId;
            const s32 count = count_application_records(applicationId);
            if (count < 0 || count > kMaxApplicationMetaRecords)
                THROW_FORMAT("Existing application record is unreasonably large");
            snapshot.existed = count > 0;
            if (count > 0)
            {
                snapshot.previous.resize(static_cast<size_t>(count));
                u32 written = 0;
                ASSERT_OK(nsListApplicationRecordContentMeta(0, applicationId,
                          snapshot.previous.data(), static_cast<u32>(snapshot.previous.size()),
                          &written),
                          "Failed to snapshot existing application record");
                if (written != static_cast<u32>(count))
                    THROW_FORMAT("Existing application record changed while preparing installation");
            }
            m_applicationRecords.push_back(std::move(snapshot));
        }
    }

    void Install::CommitContentMetaRecords()
    {
        NcmContentMetaDatabase database{};
        ASSERT_OK(ncmOpenContentMetaDatabase(&database, m_destStorageId),
                  "Failed to open content meta database");
        try
        {
            m_metadataTouched = true;
            for (const auto& old : m_supersededMetaRecords)
                ASSERT_OK(ncmContentMetaDatabaseRemove(&database, &old.key),
                          "Failed to remove superseded content meta record");
            for (auto& record : m_preparedMetaRecords)
            {
                ASSERT_OK(ncmContentMetaDatabaseSet(&database, &record.key,
                          record.install.GetData(), record.install.GetSize()),
                          "Failed to stage content meta records");
            }
            ASSERT_OK(ncmContentMetaDatabaseCommit(&database),
                      "Failed to commit content meta records");
        }
        catch (...)
        {
            ncmContentMetaDatabaseClose(&database);
            throw;
        }
        ncmContentMetaDatabaseClose(&database);
    }

    void Install::InstallApplicationRecords()
    {
        if (m_applicationRecords.empty()) THROW_FORMAT("No application metadata prepared");

        for (auto& snapshot : m_applicationRecords)
        {
            snapshot.installed = snapshot.previous;
            for (auto& meta : m_contentMeta)
            {
                if (meta.GetApplicationId() != snapshot.applicationId) continue;
                ContentStorageRecord record{};
                record.metaRecord = meta.GetContentMetaKey();
                record.storageId = static_cast<u64>(m_destStorageId);

                snapshot.installed.erase(
                    std::remove_if(snapshot.installed.begin(), snapshot.installed.end(),
                                   [&](const ContentStorageRecord& existing) {
                                        return same_title_slot(existing.metaRecord, record.metaRecord);
                                   }),
                    snapshot.installed.end());
                snapshot.installed.push_back(record);
            }

            std::vector<ContentStorageRecord> unique;
            unique.reserve(snapshot.installed.size());
            for (const auto& record : snapshot.installed)
            {
                if (std::none_of(unique.begin(), unique.end(), [&](const auto& existing) {
                        return same_storage_record(existing, record);
                    }))
                    unique.push_back(record);
            }
            snapshot.installed = std::move(unique);
            if (snapshot.installed.empty() ||
                snapshot.installed.size() > static_cast<size_t>(kMaxApplicationMetaRecords))
                THROW_FORMAT("Merged application record has an invalid size");

            LOG_DEBUG("Pushing merged application record...\n");
            snapshot.touched = true;
            if (snapshot.existed)
                ASSERT_OK(nsDeleteApplicationRecord(snapshot.applicationId),
                          "Failed to replace existing application record");
            ASSERT_OK(nsPushApplicationRecord(snapshot.applicationId,
                      NsApplicationRecordType_Installed, snapshot.installed.data(),
                      static_cast<u32>(snapshot.installed.size())),
                      "Failed to push merged application record");
        }
    }

    void Install::Prepare()
    {
        if (!m_contentMeta.empty() || !m_preparedMetaRecords.empty())
            THROW_FORMAT("Install task was already prepared");

        try
        {
            const auto tuples = this->ReadCNMT();
            if (tuples.empty())
                THROW_FORMAT("Package does not contain a content metadata NCA");

            for (const auto& tuple : tuples)
            {
                m_contentMeta.push_back(std::get<0>(tuple));
                const NcmContentInfo cnmtContentRecord = std::get<1>(tuple);
                append_unique_content_info(m_cnmtContentRecords, cnmtContentRecord);
                auto& meta = m_contentMeta.back();

                const NcmContentMetaKey key = meta.GetContentMetaKey();
                if (std::any_of(m_preparedMetaRecords.begin(), m_preparedMetaRecords.end(),
                                [&](const PreparedMetaRecord& record) {
                                    return same_title_slot(record.key, key);
                                }))
                    THROW_FORMAT("Package contains multiple versions of one content metadata slot");
                const auto type = static_cast<NcmContentMetaType>(key.type);
                if (key.install_type != NcmContentInstallType_Full ||
                    (type != NcmContentMetaType_Application &&
                     type != NcmContentMetaType_Patch &&
                     type != NcmContentMetaType_AddOnContent &&
                     type != NcmContentMetaType_DataPatch))
                    THROW_FORMAT("Package contains unsupported or system content metadata");

                for (const auto& packaged : meta.GetPackagedContentInfos())
                {
                    if (std::memcmp(packaged.hash, &packaged.content_info.content_id,
                                    sizeof(NcmContentId)) != 0)
                        THROW_FORMAT("CNMT content hash does not match its content ID");
                    const auto existing = std::find_if(
                        m_expectedContentHashes.begin(), m_expectedContentHashes.end(),
                        [&](const ExpectedContentHash& value) {
                            return same_content_id(value.contentId,
                                                   packaged.content_info.content_id);
                        });
                    if (existing != m_expectedContentHashes.end())
                    {
                        if (std::memcmp(existing->hash.data(), packaged.hash,
                                        existing->hash.size()) != 0)
                            THROW_FORMAT("Package contains conflicting hashes for one content ID");
                    }
                    else
                    {
                        ExpectedContentHash value{};
                        value.contentId = packaged.content_info.content_id;
                        std::memcpy(value.hash.data(), packaged.hash, value.hash.size());
                        m_expectedContentHashes.push_back(value);
                    }
                }

                if (m_ignoreReqFirmVersion)
                    LOG_DEBUG("WARNING: Required system firmware version is being IGNORED!\n");

                PreparedMetaRecord prepared{};
                prepared.key = key;
                meta.GetInstallContentMeta(prepared.install, cnmtContentRecord,
                                           m_ignoreReqFirmVersion);
                m_preparedMetaRecords.push_back(std::move(prepared));
            }

            this->SnapshotContentMetaRecords();
            this->SnapshotApplicationRecords();
        }
        catch (...)
        {
            this->RethrowAfterRollback(std::current_exception());
        }
    }

    void Install::Begin()
    {
        if (m_contentMeta.empty() || m_preparedMetaRecords.empty())
            THROW_FORMAT("Install task has not been prepared");
        if (m_beginCompleted || m_committed)
            THROW_FORMAT("Install task was already started");

        try
        {
            std::vector<NcmContentInfo> streamed;
            auto install_once = [&](const NcmContentInfo& record) {
                if (!append_unique_content_info(streamed, record)) return;
                LOG_DEBUG("Installing from %s\n",
                          tin::util::GetNcaIdString(record.content_id).c_str());
                this->InstallNcaTracked(record);
            };

            // Prepare stages a previously-missing CNMT so FS can mount it. In force
            // mode only rewrite CNMT content that existed before this transaction;
            // a CNMT newly staged by Prepare already contains the package bytes.
            if (m_forceReinstall)
                for (const auto& cnmt : m_cnmtContentRecords)
                {
                    const bool stagedByPrepare = std::any_of(
                        m_newContentIds.begin(), m_newContentIds.end(),
                        [&](const NcmContentId& id) {
                            return same_content_id(id, cnmt.content_id);
                        });
                    if (!stagedByPrepare) install_once(cnmt);
                }
            for (auto& contentMeta : m_contentMeta)
            {
                LOG_DEBUG("Installing NCAs...\n");
                for (const auto& record : contentMeta.GetContentInfos())
                    install_once(record);
            }

            LOG_DEBUG("Installing required ticket and cert...\n");
            this->InstallTicketCert();
            this->CommitContentMetaRecords();
            this->InstallApplicationRecords();
            m_beginCompleted = true;
        }
        catch (...)
        {
            this->RethrowAfterRollback(std::current_exception());
        }
    }
