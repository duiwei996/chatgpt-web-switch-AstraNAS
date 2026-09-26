// SPDX-License-Identifier: GPL-3.0-or-later
#include "smb_client.hpp"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <vector>

bool SmbRemoteClient::upload(const std::string& local_path, const std::string& remote_path,
                             std::uint64_t& transferred, std::uint64_t& total,
                             const UploadProgressCallback& progress, std::string& error) {
    transferred = 0;
    total = 0;
    error.clear();

    std::string share, path;
    bool server_root = false;
    if (!resolve_path(remote_path, share, path, server_root, error)) return false;
    if (server_root || path.empty()) {
        error = "SMB upload destination must be inside a share";
        return false;
    }
    if (!ensure_share(share, error)) return false;

    FILE* in = std::fopen(local_path.c_str(), "rb");
    if (!in) {
        error = "cannot open local upload file";
        return false;
    }
    if (std::fseek(in, 0, SEEK_END) != 0) {
        std::fclose(in);
        error = "cannot determine local upload size";
        return false;
    }
    const long end = std::ftell(in);
    if (end < 0 || std::fseek(in, 0, SEEK_SET) != 0) {
        std::fclose(in);
        error = "cannot determine local upload size";
        return false;
    }
    total = static_cast<std::uint64_t>(end);

    smb2fh* out = smb2_open(ctx_, path.c_str(), O_WRONLY | O_CREAT);
    if (!out) {
        error = smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB create failed";
        std::fclose(in);
        return false;
    }
    if (smb2_ftruncate(ctx_, out, 0) < 0) {
        error = smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB truncate failed";
        smb2_close(ctx_, out);
        std::fclose(in);
        return false;
    }

    constexpr std::size_t kChunk = 1024u * 1024u;
    std::vector<std::uint8_t> buffer(kChunk);
    if (progress && !progress(0, total)) error = "cancelled";
    while (error.empty() && transferred < total) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), total - transferred));
        const std::size_t got = std::fread(buffer.data(), 1, want, in);
        if (got == 0) {
            error = std::ferror(in) ? "local upload read failed" : "local upload ended before EOF";
            break;
        }
        const std::uint64_t chunk_base = transferred;
        std::size_t written = 0;
        while (written < got) {
            const int rc = smb2_pwrite(ctx_, out, buffer.data() + written,
                                       static_cast<std::uint32_t>(got - written), chunk_base + written);
            if (rc <= 0) {
                error = rc < 0 && smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB write made no progress";
                break;
            }
            written += static_cast<std::size_t>(rc);
            transferred = chunk_base + written;
            if (progress && !progress(transferred, total)) {
                error = "cancelled";
                break;
            }
        }
    }

    if (error.empty() && smb2_fsync(ctx_, out) < 0)
        error = smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB flush failed";
    if (smb2_close(ctx_, out) < 0 && error.empty())
        error = smb2_get_error(ctx_) ? smb2_get_error(ctx_) : "SMB close failed";
    if (std::fclose(in) != 0 && error.empty()) error = "failed to close local upload file";
    if (!error.empty()) return false;

    RemoteDirEntry completed;
    if (!remote_info(remote_path, completed, error)) return false;
    if (completed.size != total) {
        error = "SMB upload size verification failed";
        return false;
    }
    return true;
}
