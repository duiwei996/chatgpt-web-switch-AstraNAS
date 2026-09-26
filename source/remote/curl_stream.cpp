// SPDX-License-Identifier: GPL-3.0-or-later
#include "curl_client.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
struct BenchSink {
    std::uint64_t received = 0;
    std::uint64_t limit = 0;
    std::uint64_t callbacks = 0;
};

std::uint32_t socket_recv_buffer(curl_socket_t fd) {
    if (fd == CURL_SOCKET_BAD) return 0;
    int value = 0;
    socklen_t length = sizeof(value);
    if (::getsockopt(static_cast<int>(fd), SOL_SOCKET, SO_RCVBUF, &value, &length) != 0 || value <= 0)
        return 0;
    return static_cast<std::uint32_t>(value);
}

int benchmark_socket_options(void* clientp, curl_socket_t fd, curlsocktype) {
    if (!clientp || fd == CURL_SOCKET_BAD) return CURL_SOCKOPT_OK;
    const auto requested_u32 = *static_cast<const std::uint32_t*>(clientp);
    if (requested_u32 == 0) return CURL_SOCKOPT_OK;
    const int requested = static_cast<int>(std::min<std::uint32_t>(requested_u32, 0x7fffffffu));
    (void)::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_RCVBUF, &requested, sizeof(requested));
    return CURL_SOCKOPT_OK;
}

struct RangeHeaders {
    bool content_range_seen = false;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    std::uint64_t total = 0;
};

struct StreamSink {
    const RemoteStreamDataCallback* callback = nullptr;
    std::uint64_t received = 0;
    std::uint64_t total = 0;
    bool callback_failed = false;
};

struct CurlProgressContext {
    const TransferProgressCallback* callback = nullptr;
    std::uint64_t resume = 0;
    std::uint64_t last_total = 0;
    std::uint64_t last_transferred = 0;
    bool upload = false;
};

std::string lower_copy(std::string value);

size_t write_file(void* ptr, size_t size, size_t nmemb, void* userdata) {
    return std::fwrite(ptr, 1, size * nmemb, static_cast<FILE*>(userdata));
}

size_t write_bench(void* ptr, size_t size, size_t nmemb, void* userdata) {
    (void)ptr;
    const size_t bytes = size * nmemb;
    auto* sink = static_cast<BenchSink*>(userdata);
    const auto remaining = sink->limit > sink->received ? sink->limit - sink->received : 0;
    const auto accepted = static_cast<size_t>(std::min<std::uint64_t>(bytes, remaining));
    if (accepted > 0) ++sink->callbacks;
    sink->received += accepted;
    return accepted < bytes ? 0 : bytes;
}

size_t write_stream(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const std::size_t bytes = size * nmemb;
    auto* sink = static_cast<StreamSink*>(userdata);
    if (!sink || !sink->callback || !(*sink->callback)) return 0;
    if (sink->received > sink->total || bytes > sink->total - sink->received) {
        sink->callback_failed = true;
        return 0;
    }
    if (!(*sink->callback)(static_cast<const unsigned char*>(ptr), bytes, sink->received)) {
        sink->callback_failed = true;
        return 0;
    }
    sink->received += bytes;
    return bytes;
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

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}
}

bool CurlRemoteClient::download_responsive(const RemoteDirEntry& entry, const std::string& local_path,
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

bool CurlRemoteClient::benchmark_detailed(const std::string& remote_path, std::size_t bytes,
                                          RemoteBenchmarkStats& stats, std::string& error) {
    stats = {};
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
    BenchSink sink{0, static_cast<std::uint64_t>(bytes), 0};
    const std::string range = "0-" + std::to_string(bytes - 1);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    apply_common(curl);
    if (benchmark_recv_buffer_ > 0) {
        curl_easy_setopt(curl, CURLOPT_SOCKOPTFUNCTION, benchmark_socket_options);
        curl_easy_setopt(curl, CURLOPT_SOCKOPTDATA, &benchmark_recv_buffer_);
    }
    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_bench);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    const auto start = std::chrono::steady_clock::now();
    const CURLcode rc = curl_easy_perform(curl);
    const auto end = std::chrono::steady_clock::now();
    long response_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_status);
    curl_socket_t active_socket = CURL_SOCKET_BAD;
#if LIBCURL_VERSION_NUM >= 0x072D00
    curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &active_socket);
#endif
    const std::uint32_t recv_buffer = socket_recv_buffer(active_socket);
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
    stats.bytes = sink.received;
    stats.seconds = seconds;
    stats.mib_per_sec = (static_cast<double>(sink.received) / (1024.0 * 1024.0)) / seconds;
    stats.request_count = 1;
    stats.average_request_bytes = sink.received;
    stats.negotiated_read_size = 0;
    stats.pipeline_request_size = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(sink.received, 0xffffffffull));
    stats.pipeline_window = 1;
    stats.requested_socket_recv_buffer = benchmark_recv_buffer_;
    stats.socket_recv_buffer = recv_buffer;
    stats.receive_callback_count = sink.callbacks;
    stats.average_callback_bytes = sink.callbacks ? sink.received / sink.callbacks : 0;
    return true;
}

bool CurlRemoteClient::stream_range(const RemoteDirEntry& entry, std::uint64_t offset,
                                    std::uint64_t size, const RemoteStreamDataCallback& data,
                                    const TransferProgressCallback& progress,
                                    std::string& error) {
    error.clear();
    if (!data) { error = "WebDAV 连续流消费者不可用"; return false; }
    if (entry.path != direct_entry_.path || entry.identity != direct_entry_.identity ||
        entry.size != direct_entry_.size || !direct_curl_) {
        error = "WebDAV 网络直读会话未为该文件准备";
        return false;
    }
    if (size == 0) return true;
    if (offset > entry.size || size > entry.size - offset) {
        error = "WebDAV 连续流范围超出文件";
        return false;
    }

    CURL* curl = static_cast<CURL*>(direct_curl_);
    const std::uint64_t end = offset + size - 1;
    const std::string range = std::to_string(offset) + "-" + std::to_string(end);
    const std::string etag = entry.identity.substr(5);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("If-Match: " + etag).c_str());
    RangeHeaders response_headers{};
    StreamSink sink{&data, 0, size, false};
    CurlProgressContext progress_ctx{&progress, 0, size, 0, false};

    curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_stream);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, read_range_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    if (progress) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_ctx);
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
    }

    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, nullptr);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, nullptr);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, nullptr);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
    curl_slist_free_all(headers);

    if (status == 412) { error = "WebDAV 网络直装期间远程文件已变化"; return false; }
    if (status == 200) { error = "WebDAV 网络直装不可用：服务器忽略 Range 请求"; return false; }
    if (rc == CURLE_ABORTED_BY_CALLBACK) { error = "cancelled"; return false; }
    if (sink.callback_failed) { error = "installation cancelled"; return false; }
    if (rc != CURLE_OK) { error = curl_easy_strerror(rc); return false; }
    if (status != 206) { error = "WebDAV Range 返回异常状态 " + std::to_string(status); return false; }
    if (!response_headers.content_range_seen) { error = "WebDAV Range 响应缺少 Content-Range"; return false; }
    if (response_headers.start != offset || response_headers.end != end || response_headers.total != entry.size) {
        error = "WebDAV Content-Range 与连续流请求不匹配";
        return false;
    }
    if (sink.received != size) { error = "WebDAV 连续流在请求范围结束前提前结束"; return false; }
    std::string reset_error;
    if (!reset_direct_handle(reset_error)) {
        error = "WebDAV 连续流完成，但恢复随机读取会话失败：" + reset_error;
        return false;
    }
    return true;
}
