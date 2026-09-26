// SPDX-License-Identifier: GPL-3.0-or-later
#include "smb_client.hpp"
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

bool SmbRemoteClient::delete_entry(const RemoteDirEntry& entry, bool recursive, std::string& error) {
    error.clear();
    if (!ctx_) { error = "SMB 尚未连接"; return false; }
    if (entry.path.empty() || entry.path == "/" || entry.identity == "smb-share") {
        error = "禁止删除 SMB 服务器根目录或共享根目录";
        return false;
    }

    if (entry.is_dir) {
        if (!recursive) { error = "删除非空 SMB 文件夹需要递归模式"; return false; }
        std::vector<RemoteDirEntry> children;
        if (!list_dir(entry.path, children, error)) return false;
        for (const auto& child : children) {
            if (!delete_entry(child, true, error)) return false;
        }
    }

    std::string share, inner;
    bool server_root = false;
    if (!resolve_path(entry.path, share, inner, server_root, error)) return false;
    if (server_root || inner.empty()) {
        error = "禁止删除 SMB 服务器根目录或共享根目录";
        return false;
    }
    if (!ensure_share(share, error)) return false;

    const int rc = entry.is_dir ? smb2_rmdir(ctx_, inner.c_str()) : smb2_unlink(ctx_, inner.c_str());
    if (rc < 0) {
        const char* detail = smb2_get_error(ctx_);
        error = detail && *detail ? detail : (entry.is_dir ? "SMB 删除文件夹失败" : "SMB 删除文件失败");
        return false;
    }
    return true;
}
