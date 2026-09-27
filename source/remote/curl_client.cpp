// SPDX-License-Identifier: GPL-3.0-or-later
#include "curl_client.hpp"
#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <unistd.h>

namespace {
struct StringSink {
    std::string* output = nullptr;
    std::size_t limit = 0;
    bool overflow = false;
};

size_t write_string(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    auto* sink = static_cast<StringSink*>(userdata);
    if (!sink || !sink->output) return 0;
    if (bytes > sink->limit || sink->output->size() > sink->limit - bytes) {
        sink->overflow = true;
        return 0;
    }
    sink->output->append(static_cast<const char*>(ptr), bytes);
    return bytes;
}

size_t write_file(void* ptr, size_t size, size_t nmemb, void* userdata) {
    return std::fwrite(ptr, 1, size * nmemb, static_cast<FILE*>(userdata));
}

size_t discard_write(void*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

struct BenchSink {
    std::uint64_t received = 0;
    std::uint64_t limit = 0;
};

struct RangeSink {
    unsigned char* data = nullptr;
    std::size_t capacity = 0;
    std::size_t received = 0;
    bool overflow = false;
};

struct RangeHeaders {
    bool content_range_seen = false;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t total = 0;
};

struct CurlProgressContext {
    const TransferProgressCallback* callback = nullptr;
    std::uint64_t resume = 0;
    std::uint64_t last_total = 0;
    std::uint64_t last_transferred = 0;
    bool upload = false;
};

int xfer_progress(void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    auto* ctx = static_cast<CurlProgressContext*>(userdata);
    if (!ctx || !ctx->callback || !(*ctx->callback)) return 0;
    const curl_off_t raw_now = ctx->upload ? ulnow : dlnow;
    const curl_off_t raw_total = ctx->upload ? ultotal : dltotal;
    const std::uint64_t now = ctx->resume + static_cast<std::uint64_t>(std::max<curl_off_t>(0, raw_now));
    const std::uint64_t total = raw_total > 0 ? ctx->resume + static_cast<std::uint64_t>(raw_total) : 0;
    ctx->last_total = total;
    ctx->last_transferred = now;
    return (*ctx->callback)(now, total) ? 0 : 1;
}

size_t write_bench(void* ptr, size_t size, size_t nmemb, void* userdata) {
    (void)ptr;
    const size_t bytes = size * nmemb;
    auto* sink = static_cast<BenchSink*>(userdata);
    const auto remaining = sink->limit > sink->received ? sink->limit - sink->received : 0;
    const auto accepted = static_cast<size_t>(std::min<std::uint64_t>(bytes, remaining));
    sink->received += accepted;
    return accepted < bytes ? 0 : bytes;
}

size_t write_range(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    auto* sink = static_cast<RangeSink*>(userdata);
    if (!sink || !sink->data) return 0;
    if (sink->received > sink->capacity || bytes > sink->capacity - sink->received) {
        sink->overflow = true;
        return 0;
    }
    std::memcpy(sink->data + sink->received, ptr, bytes);
    sink->received += bytes;
    return bytes;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

size_t read_range_header(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    auto* headers = static_cast<RangeHeaders*>(userdata);
    if (!headers || bytes == 0) return bytes;
    std::string line(ptr, bytes);
    const std::string lower = lower_copy(line);
    constexpr const char* prefix = "content-range:";
    if (lower.rfind(prefix, 0) != 0) return bytes;

    const auto units = lower.find("bytes", std::strlen(prefix));
    if (units == std::string::npos) return bytes;
    const auto dash = lower.find('-', units + 5);
    const auto slash = lower.find('/', dash == std::string::npos ? units + 5 : dash + 1);
    if (dash == std::string::npos || slash == std::string::npos) return bytes;
    try {
        const std::string start = lower.substr(units + 5, dash - (units + 5));
        const std::string end = lower.substr(dash + 1, slash - dash - 1);
        std::size_t totalEnd = slash + 1;
        while (totalEnd < lower.size() && std::isdigit(static_cast<unsigned char>(lower[totalEnd]))) ++totalEnd;
        const std::string total = lower.substr(slash + 1, totalEnd - slash - 1);
        headers->start = std::stoull(start);
        headers->end = std::stoull(end);
        headers->total = std::stoull(total);
        headers->content_range_seen = true;
    } catch (...) {
        headers->content_range_seen = false;
    }
    return bytes;
}

std::string strip_webdav_scheme(std::string url) {
    const auto lower = lower_copy(url);
    if (lower.rfind("webdavs://", 0) == 0) return "https://" + url.substr(10);
    if (lower.rfind("webdav://", 0) == 0) return "http://" + url.substr(9);
    return url;
}

std::string encode_url_path(const std::string& path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(path[i]);
        const bool unreserved = std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved || c == '/') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

std::string trim_cr(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    return line;
}

std::string xml_unescape(std::string value) {
    struct Pair { const char* from; const char* to; };
    static const Pair pairs[] = {
        {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}
    };
    for (const auto& pair : pairs) {
        std::size_t pos = 0;
        while ((pos = value.find(pair.from, pos)) != std::string::npos) {
            value.replace(pos, std::strlen(pair.from), pair.to);
            pos += std::strlen(pair.to);
        }
    }
    return value;
}

std::string last_path_component(std::string path) {
    while (!path.empty() && path.back() == '/') path.pop_back();
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string normalized_url_path(std::string value) {
    const auto scheme = value.find("://");
    if (scheme != std::string::npos) {
        const auto slash = value.find('/', scheme + 3);
        value = slash == std::string::npos ? "/" : value.substr(slash);
    }
    const auto suffix = value.find_first_of("?#");
    if (suffix != std::string::npos) value.erase(suffix);
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    if (value.empty()) value = "/";
    return value;
}

bool safe_remote_component(const std::string& name) {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
    return std::none_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7f || c == '/' || c == '\\';
    });
}

std::string safe_revision_value(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    if (value.empty() || std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return c == '\r' || c == '\n' || c == 0;
    })) return {};
    return value;
}

bool parse_mlsd_line(std::string line, std::string& name, bool& isDir,
                     std::uint64_t& size) {
    line = trim_cr(std::move(line));
    const auto delimiter = line.find(' ');
    if (delimiter == std::string::npos || delimiter + 1 >= line.size()) return false;
    const std::string facts = lower_copy(line.substr(0, delimiter));
    name = line.substr(delimiter + 1);
    if (!safe_remote_component(name)) return false;

    std::string type;
    size = 0;
    std::size_t start = 0;
    while (start < facts.size()) {
        const auto end = facts.find(';', start);
        const std::string fact = facts.substr(start, end == std::string::npos ? std::string::npos : end - start);
        const auto equals = fact.find('=');
        if (equals != std::string::npos) {
            const std::string key = fact.substr(0, equals);
            const std::string value = fact.substr(equals + 1);
            if (key == "type") type = value;
            else if (key == "size") {
                try { size = std::stoull(value); }
                catch (...) { size = 0; }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (type == "cdir" || type == "pdir" || type.empty()) return false;
    isDir = type == "dir";
    return isDir || type == "file";
}
}

CurlRemoteClient::~CurlRemoteClient() {
    close_direct_read();
}

bool CurlRemoteClient::connect(const AppConfig& config, std::string& error) {
    close_direct_read();
    config_ = config;
    base_url_ = strip_webdav_scheme(config.url);
    while (!base_url_.empty() && base_url_.back() == '/') base_url_.pop_back();

    const auto lower = lower_copy(config.url);
    is_webdav_ = lower.rfind("webdav://", 0) == 0 || lower.rfind("webdavs://", 0) == 0;
    is_ftp_ = lower.rfind("ftp://", 0) == 0 || lower.rfind("ftps://", 0) == 0;
    if (is_webdav_) protocol_name_ = lower.rfind("webdavs://", 0) == 0 ? "WebDAVS" : "WebDAV";
    else if (is_ftp_) protocol_name_ = lower.rfind("ftps://", 0) == 0 ? "FTPS" : "FTP";
    else protocol_name_ = lower.rfind("https://", 0) == 0 ? "HTTPS" : "HTTP";

    if (base_url_.empty()) {
        error = "empty URL";
        return false;
    }
    return true;
}

bool CurlRemoteClient::reset_direct_handle(std::string& error) {
    if (direct_curl_) {
        curl_easy_cleanup(static_cast<CURL*>(direct_curl_));
        direct_curl_ = nullptr;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl 网络直读初始化失败";
        return false;
    }
    direct_curl_ = curl;
    const auto url = make_url(direct_entry_.path);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    apply_common(curl);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    // 网络直装需要快速失败/重试，不能沿用缓存下载的长时间卡死窗口。
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_range);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_range_header);
    return true;
}

void CurlRemoteClient::apply_common(void* curl_handle) const {
    CURL* curl = static_cast<CURL*>(curl_handle);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    const auto scheme = lower_copy(base_url_);
#if LIBCURL_VERSION_NUM >= 0x075500
    if (scheme.rfind("https://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    else if (scheme.rfind("ftps://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "ftps");
    else if (scheme.rfind("http://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    else if (scheme.rfind("ftp://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "ftp,ftps");
#else
    if (scheme.rfind("https://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
    else if (scheme.rfind("ftps://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_FTPS);
    else if (scheme.rfind("http://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    else if (scheme.rfind("ftp://", 0) == 0)
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_FTP | CURLPROTO_FTPS);
#endif
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    // Large game packages can legitimately take hours. Abort only when the
    // connection remains effectively stalled, not after a fixed total time.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 120L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // Ask libcurl for a substantially larger receive buffer on Switch. This is a
    // transport buffer request, not the 8 MiB install ring; callback granularity is
    // measured separately by NetDiag because libcurl may still deliver smaller chunks.
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 1024L * 1024L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "AstraNAS/1.1.15");
    if (!config_.username.empty()) curl_easy_setopt(curl, CURLOPT_USERNAME, config_.username.c_str());
    if (!config_.password.empty()) curl_easy_setopt(curl, CURLOPT_PASSWORD, config_.password.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, config_.tls_verify ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, config_.tls_verify ? 2L : 0L);
}

std::string CurlRemoteClient::make_url(const std::string& remote_path) const {
    if (remote_path.empty() || remote_path == "/") return base_url_ + "/";
    const std::string encoded = encode_url_path(remote_path);
    if (encoded.front() == '/') return base_url_ + encoded;
    return base_url_ + "/" + encoded;
}

bool CurlRemoteClient::fetch_text(const std::string& remote_path, std::string& text, std::string& error) {
    text.clear();
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }
    const auto url = make_url(remote_path);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    apply_common(curl);
    StringSink sink{&text, 8ull * 1024ull * 1024ull, false};
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        error = sink.overflow ? "response too large (>8 MiB)" : curl_easy_strerror(rc);
        return false;
    }
    if (!is_ftp_ && status >= 400) {
        error = "HTTP status " + std::to_string(status);
        return false;
    }
    return true;
}

bool CurlRemoteClient::list_dir(const std::string& remote_path, std::vector<RemoteDirEntry>& entries, std::string& error) {
    entries.clear();
    if (is_ftp_) {
        const auto url = make_url(remote_path);
        std::string listing;
        bool usedMlsd = true;
        StringSink listing_sink{&listing, 16ull * 1024ull * 1024ull, false};
        auto fetchListing = [&](bool mlsd) {
            CURL* curl = curl_easy_init();
            if (!curl) return CURLE_FAILED_INIT;
            listing.clear();
            listing_sink.overflow = false;
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            apply_common(curl);
            if (mlsd) curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MLSD");
            else curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &listing_sink);
            const CURLcode result = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            return result;
        };
        CURLcode rc = fetchListing(true);
        if (rc != CURLE_OK) {
            usedMlsd = false;
            rc = fetchListing(false);
        }
        if (rc != CURLE_OK) {
            error = listing_sink.overflow ? "FTP listing too large (>16 MiB)" : curl_easy_strerror(rc);
            return false;
        }
        std::istringstream lines(listing);
        std::string name;
        while (std::getline(lines, name)) {
            bool isDir = false;
            std::uint64_t size = 0;
            if (usedMlsd) {
                std::string parsedName;
                if (!parse_mlsd_line(name, parsedName, isDir, size)) continue;
                name = std::move(parsedName);
            } else {
                name = trim_cr(name);
                if (!safe_remote_component(name)) continue;
            }
            if (entries.size() >= 16384) {
                error = "remote directory contains too many entries";
                return false;
            }
            RemoteDirEntry entry;
            entry.name = name;
            entry.path = remote_path;
            if (!entry.path.empty() && entry.path.back() != '/') entry.path.push_back('/');
            entry.path += name;
            entry.is_dir = isDir;
            entry.size = size;
            entries.push_back(std::move(entry));
        }
        return true;
    }

    if (!is_webdav_) {
        error = "plain HTTP directory listing is not supported; provide library.json";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }
    const auto url = make_url(remote_path);
    std::string xml;
    StringSink xml_sink{&xml, 16ull * 1024ull * 1024ull, false};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Depth: 1");
    headers = curl_slist_append(headers, "Content-Type: application/xml; charset=utf-8");
    const char* body = "<?xml version=\"1.0\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/><d:getcontentlength/><d:getetag/><d:getlastmodified/></d:prop></d:propfind>";
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    apply_common(curl);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PROPFIND");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &xml_sink);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        error = xml_sink.overflow ? "WebDAV response too large (>16 MiB)" : curl_easy_strerror(rc);
        curl_easy_cleanup(curl);
        return false;
    }
    if (status >= 400) {
        error = "WebDAV status " + std::to_string(status);
        curl_easy_cleanup(curl);
        return false;
    }

    const std::regex response_re(R"(<(?:[A-Za-z0-9_-]+:)?response\b[^>]*>([\s\S]*?)</(?:[A-Za-z0-9_-]+:)?response\s*>)", std::regex::icase);
    const std::regex href_re(R"(<(?:[A-Za-z0-9_-]+:)?href\b[^>]*>([\s\S]*?)</(?:[A-Za-z0-9_-]+:)?href\s*>)", std::regex::icase);
    const std::regex length_re(R"(<(?:[A-Za-z0-9_-]+:)?getcontentlength\b[^>]*>([0-9]+)</(?:[A-Za-z0-9_-]+:)?getcontentlength\s*>)", std::regex::icase);
    const std::regex etag_re(R"(<(?:[A-Za-z0-9_-]+:)?getetag\b[^>]*>([\s\S]*?)</(?:[A-Za-z0-9_-]+:)?getetag\s*>)", std::regex::icase);
    const std::regex modified_re(R"(<(?:[A-Za-z0-9_-]+:)?getlastmodified\b[^>]*>([\s\S]*?)</(?:[A-Za-z0-9_-]+:)?getlastmodified\s*>)", std::regex::icase);
    const std::regex collection_re(R"(<(?:[A-Za-z0-9_-]+:)?collection\b)", std::regex::icase);

    int requestedDecodedLength = 0;
    char* requestedDecoded = curl_easy_unescape(curl, url.c_str(),
                                                static_cast<int>(url.size()),
                                                &requestedDecodedLength);
    const std::string requestedPath = normalized_url_path(
        requestedDecoded ? std::string(requestedDecoded, static_cast<std::size_t>(requestedDecodedLength))
                         : url);
    if (requestedDecoded) curl_free(requestedDecoded);

    std::unordered_set<std::string> seenNames;
    for (std::sregex_iterator it(xml.begin(), xml.end(), response_re), end; it != end; ++it) {
        const std::string block = (*it)[1].str();
        std::smatch href_match;
        if (!std::regex_search(block, href_match, href_re)) continue;
        std::string href = xml_unescape(href_match[1].str());
        int decoded_len = 0;
        char* decoded = curl_easy_unescape(curl, href.c_str(), static_cast<int>(href.size()), &decoded_len);
        if (decoded) {
            href.assign(decoded, static_cast<std::size_t>(decoded_len));
            curl_free(decoded);
        }
        if (normalized_url_path(href) == requestedPath) continue;
        const bool is_dir = std::regex_search(block, collection_re);
        std::string name = last_path_component(href);
        if (!safe_remote_component(name)) continue;
        if (!seenNames.insert(name).second) continue;
        if (entries.size() >= 16384) {
            error = "remote directory contains too many entries";
            curl_easy_cleanup(curl);
            return false;
        }
        RemoteDirEntry entry;
        entry.name = name;
        entry.path = remote_path;
        if (!entry.path.empty() && entry.path.back() != '/') entry.path.push_back('/');
        entry.path += name;
        entry.is_dir = is_dir;
        std::smatch length_match;
        if (std::regex_search(block, length_match, length_re)) {
            try { entry.size = std::stoull(length_match[1].str()); }
            catch (...) { entry.size = 0; }
        }
        std::smatch revisionMatch;
        if (std::regex_search(block, revisionMatch, etag_re)) {
            const std::string value = safe_revision_value(xml_unescape(revisionMatch[1].str()));
            // Weak ETags cannot safely drive If-Match/If-Range. Prefer a
            // Last-Modified fallback instead of pretending they are strong.
            if (!value.empty() && value.rfind("W/", 0) != 0 && value.rfind("w/", 0) != 0)
                entry.identity = "etag:" + value;
        }
        if (entry.identity.empty() && std::regex_search(block, revisionMatch, modified_re)) {
            const std::string value = safe_revision_value(xml_unescape(revisionMatch[1].str()));
            if (!value.empty()) entry.identity = "last-modified:" + value;
        }
        entries.push_back(std::move(entry));
    }
    curl_easy_cleanup(curl);
    return true;
}

bool CurlRemoteClient::download(const RemoteDirEntry& entry, const std::string& local_path,
                                std::uint64_t& transferred, std::uint64_t& total,
                                const DownloadProgressCallback& progress, std::string& error) {
    transferred = 0;
    total = 0;
    std::uint64_t offset = 0;
    if (FILE* existing = std::fopen(local_path.c_str(), "rb")) {
        std::fseek(existing, 0, SEEK_END);
        const long pos = std::ftell(existing);
        if (pos > 0) offset = static_cast<std::uint64_t>(pos);
        std::fclose(existing);
    }
    if (entry.identity.empty() || (entry.size != 0 && offset > entry.size)) offset = 0;
    if (!entry.identity.empty() && entry.size != 0 && offset == entry.size) {
        transferred = entry.size;
        total = entry.size;
        return true;
    }

    long response_status = 0;
    auto perform = [&](std::uint64_t resume, bool append) -> CURLcode {
        response_status = 0;
        FILE* out = std::fopen(local_path.c_str(), append ? "ab" : "wb");
        if (!out) return CURLE_WRITE_ERROR;
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::fclose(out);
            return CURLE_FAILED_INIT;
        }
        const auto url = make_url(entry.path);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        apply_common(curl);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
        struct curl_slist* requestHeaders = nullptr;
        std::string validator;
        if (entry.identity.rfind("etag:", 0) == 0) {
            validator = entry.identity.substr(5);
            requestHeaders = curl_slist_append(requestHeaders, ("If-Match: " + validator).c_str());
        } else if (entry.identity.rfind("last-modified:", 0) == 0) {
            validator = entry.identity.substr(14);
            requestHeaders = curl_slist_append(requestHeaders, ("If-Unmodified-Since: " + validator).c_str());
        }
        if (resume > 0 && !validator.empty())
            requestHeaders = curl_slist_append(requestHeaders, ("If-Range: " + validator).c_str());
        if (requestHeaders) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, requestHeaders);
        if (!is_ftp_) curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        CurlProgressContext progress_ctx{&progress, resume, 0, resume, false};
        if (progress) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_progress);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_ctx);
        }
        if (resume > 0) curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, static_cast<curl_off_t>(resume));
        const CURLcode rc = curl_easy_perform(curl);
        curl_off_t content_length = -1;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_status);
        curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &content_length);
        if (progress_ctx.last_total > 0) total = progress_ctx.last_total;
        else if (content_length >= 0) total = resume + static_cast<std::uint64_t>(content_length);
        if (requestHeaders) curl_slist_free_all(requestHeaders);
        curl_easy_cleanup(curl);
        bool syncOk = std::fflush(out) == 0 && ::fsync(fileno(out)) == 0;
        if (std::fclose(out) != 0) syncOk = false;
        return syncOk ? rc : CURLE_WRITE_ERROR;
    };

    CURLcode rc = perform(offset, offset > 0);
    if ((rc == CURLE_RANGE_ERROR || (rc == CURLE_OK && !is_ftp_ && response_status != 206)) && offset > 0) {
        offset = 0;
        total = 0;
        rc = perform(0, false);
    }
    if (rc != CURLE_OK) {
        if (response_status == 412) error = "remote file changed since it was listed";
        else if (rc == CURLE_ABORTED_BY_CALLBACK) error = "cancelled";
        else error = curl_easy_strerror(rc);
        return false;
    }
    if (!is_ftp_ && response_status >= 400) {
        error = "HTTP status " + std::to_string(response_status);
        return false;
    }
    if (FILE* done = std::fopen(local_path.c_str(), "rb")) {
        std::fseek(done, 0, SEEK_END);
        const long pos = std::ftell(done);
        if (pos > 0) transferred = static_cast<std::uint64_t>(pos);
        std::fclose(done);
    }
    if (total == 0) total = transferred;
    return true;
}

bool CurlRemoteClient::upload(const std::string& local_path, const std::string& remote_path,
                              std::uint64_t& transferred, std::uint64_t& total,
                              const UploadProgressCallback& progress, std::string& error) {
    transferred = 0;
    total = 0;
    error.clear();
    if (!is_webdav_ && !is_ftp_) {
        error = "remote protocol does not support upload";
        return false;
    }
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
    if (progress && !progress(0, total)) {
        std::fclose(in);
        error = "cancelled";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::fclose(in);
        error = "curl_easy_init failed";
        return false;
    }
    const auto url = make_url(remote_path);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    apply_common(curl);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, in);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(total));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    if (!is_ftp_) curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    CurlProgressContext progress_ctx{&progress, 0, total, 0, true};
    if (progress) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_ctx);
    }
    const CURLcode rc = curl_easy_perform(curl);
    long response_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_status);
    curl_easy_cleanup(curl);
    const bool close_ok = std::fclose(in) == 0;
    transferred = std::min<std::uint64_t>(total, progress_ctx.last_transferred);
    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        error = "cancelled";
        return false;
    }
    if (rc != CURLE_OK) {
        error = curl_easy_strerror(rc);
        return false;
    }
    if (!close_ok) {
        error = "failed to close local upload file";
        return false;
    }
    if (!is_ftp_ && response_status >= 400) {
        error = "HTTP status " + std::to_string(response_status);
        return false;
    }
    transferred = total;
    if (progress && progress_ctx.last_transferred < total && !progress(total, total)) {
        error = "cancelled";
        return false;
    }
    return true;
}

bool CurlRemoteClient::prepare_direct_read(const RemoteDirEntry& entry, std::string& error) {
    close_direct_read();
    error.clear();
    if (!is_webdav_) {
        error = "网络直装需要 WebDAV 随机读取能力";
        return false;
    }
    if (entry.is_dir || entry.size < 2) {
        error = "远程项目不是可直接读取的安装包文件";
        return false;
    }
    // 网络直装强制要求强 ETag。Last-Modified 可用于缓存下载续传，
    // 但在不做全量内容哈希时，不足以支撑数千次 Range 读取期间的文件版本锁定。
    if (entry.identity.rfind("etag:", 0) != 0 || entry.identity.size() <= 5) {
        error = "WebDAV 网络直装不可用：服务器未提供强 ETag";
        return false;
    }
    direct_entry_ = entry;
    if (!reset_direct_handle(error)) {
        direct_entry_ = {};
        return false;
    }

    unsigned char probe = 0;
    std::size_t actual = 0;
    if (!read_range(entry, 1, &probe, 1, actual, error) || actual != 1) {
        close_direct_read();
        if (error.empty()) error = "WebDAV Range 能力探测失败";
        return false;
    }
    return true;
}

bool CurlRemoteClient::read_range(const RemoteDirEntry& entry, std::uint64_t offset,
                                  void* buffer, std::size_t size, std::size_t& actual,
                                  std::string& error) {
    actual = 0;
    error.clear();
    if (!buffer && size != 0) { error = "网络直读缓冲区为空"; return false; }
    if (entry.path != direct_entry_.path || entry.identity != direct_entry_.identity ||
        entry.size != direct_entry_.size || !direct_curl_) {
        error = "WebDAV 网络直读会话未为该文件准备";
        return false;
    }
    if (offset > entry.size || size > entry.size - offset) {
        error = "WebDAV 网络直读范围超出文件";
        return false;
    }
    if (size == 0) return true;

    const int max_attempts = std::max(1, config_.download_retries + 1);
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        CURL* curl = static_cast<CURL*>(direct_curl_);
        RangeSink sink{static_cast<unsigned char*>(buffer), size, 0, false};
        RangeHeaders response_headers{};
        const std::uint64_t end = offset + size - 1;
        const std::string range = std::to_string(offset) + "-" + std::to_string(end);
        const std::string etag = entry.identity.substr(5);
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("If-Match: " + etag).c_str());

        curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
        const CURLcode rc = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, nullptr);
        curl_slist_free_all(headers);

        actual = sink.received;
        if (status == 412) {
            error = "WebDAV 网络直装期间远程文件已变化";
            return false;
        }
        if (status == 200) {
            error = "WebDAV 网络直装不可用：服务器忽略 Range 请求";
            return false;
        }
        if (rc == CURLE_OK && status == 206 && !sink.overflow && sink.received == size &&
            response_headers.content_range_seen && response_headers.start == offset &&
            response_headers.end == end && response_headers.total == entry.size) {
            return true;
        }

        if (sink.overflow) error = "WebDAV Range 响应超过请求缓冲区";
        else if (rc != CURLE_OK) error = curl_easy_strerror(rc);
        else if (status != 206) error = "WebDAV Range 返回异常状态 " + std::to_string(status);
        else if (!response_headers.content_range_seen) error = "WebDAV Range 响应缺少 Content-Range";
        else error = "WebDAV Content-Range 与请求偏移不匹配";

        if (attempt < max_attempts) {
            std::string reset_error;
            if (!reset_direct_handle(reset_error)) {
                error += "; 重连失败: " + reset_error;
                return false;
            }
        }
    }
    return false;
}

bool CurlRemoteClient::finish_direct_read(const RemoteDirEntry& entry, std::string& error) {
    if (entry.size == 0) {
        error = "无法对空的网络直读文件完成收尾检查";
        close_direct_read();
        return false;
    }
    unsigned char probe = 0;
    std::size_t actual = 0;
    const bool ok = read_range(entry, entry.size - 1, &probe, 1, actual, error) && actual == 1;
    close_direct_read();
    return ok;
}

void CurlRemoteClient::close_direct_read() {
    if (direct_curl_) {
        curl_easy_cleanup(static_cast<CURL*>(direct_curl_));
        direct_curl_ = nullptr;
    }
    direct_entry_ = {};
}

bool CurlRemoteClient::benchmark(const std::string& remote_path, std::size_t bytes,
                                 double& mib_per_sec, std::string& error) {
    mib_per_sec = 0.0;
    if (bytes == 0) {
        error = "benchmark size is zero";
        return false;
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }
    const auto url = make_url(remote_path);
    BenchSink sink{0, static_cast<std::uint64_t>(bytes)};
    const std::string range = "0-" + std::to_string(bytes - 1);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    apply_common(curl);
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_bench);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const auto start = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(curl);
    const auto end = std::chrono::steady_clock::now();
    long response_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_status);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK && rc != CURLE_WRITE_ERROR) {
        error = curl_easy_strerror(rc);
        return false;
    }
    if (!is_ftp_ && response_status >= 400) {
        error = "HTTP status " + std::to_string(response_status);
        return false;
    }
    if (sink.received == 0) {
        error = "benchmark read failed";
        return false;
    }
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (seconds <= 0.0) {
        error = "benchmark timer error";
        return false;
    }
    mib_per_sec = (static_cast<double>(sink.received) / (1024.0 * 1024.0)) / seconds;
    return true;
}
