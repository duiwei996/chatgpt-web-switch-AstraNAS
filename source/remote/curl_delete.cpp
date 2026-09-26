// SPDX-License-Identifier: GPL-3.0-or-later
#include "curl_client.hpp"
#include <curl/curl.h>

namespace {
size_t discard_delete_response(void*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}
}

bool CurlRemoteClient::delete_entry(const RemoteDirEntry& entry, bool recursive, std::string& error) {
    error.clear();
    if (!is_webdav_) { error = "当前协议不支持远程删除"; return false; }
    if (entry.path.empty() || entry.path == "/") { error = "禁止删除 WebDAV 根目录"; return false; }
    if (entry.is_dir && !recursive) { error = "删除 WebDAV 文件夹需要递归模式"; return false; }

    CURL* curl = curl_easy_init();
    if (!curl) { error = "curl_easy_init failed"; return false; }
    const auto url = make_url(entry.path);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    apply_common(curl);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_delete_response);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);

    struct curl_slist* headers = nullptr;
    if (entry.identity.rfind("etag:", 0) == 0 && entry.identity.size() > 5)
        headers = curl_slist_append(headers, ("If-Match: " + entry.identity.substr(5)).c_str());
    else if (entry.identity.rfind("last-modified:", 0) == 0 && entry.identity.size() > 14)
        headers = curl_slist_append(headers, ("If-Unmodified-Since: " + entry.identity.substr(14)).c_str());
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        error = curl_easy_strerror(rc);
        return false;
    }
    if (status >= 200 && status < 300) return true;
    if (status == 404) error = "远程项目已不存在，请刷新目录";
    else if (status == 409) error = "WebDAV 删除冲突：服务器拒绝删除该目录或其成员";
    else if (status == 412) error = "远程项目在删除前已发生变化";
    else if (status == 423) error = "WebDAV 项目已锁定，无法删除";
    else error = "WebDAV DELETE 状态 " + std::to_string(status);
    return false;
}
