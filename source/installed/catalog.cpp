// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#ifdef __SWITCH__
#include "../local_fs.hpp"
#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <setjmp.h>
#include <sstream>
#include <sys/stat.h>
#include <vector>

namespace astranas::installed {
namespace {
std::string bounded_text(const char* value, std::size_t capacity) {
    std::size_t length = 0;
    while (value && length < capacity && value[length] != '\0') ++length;
    return value ? std::string(value, length) : std::string{};
}
std::string title_hex(std::uint64_t id) {
    std::ostringstream out; out << std::uppercase << std::hex << std::setfill('0') << std::setw(16) << id; return out.str();
}
bool file_exists(const std::string& path) {
    struct stat st{}; return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
char hex_digit(unsigned value) { return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('A' + value - 10); }
std::string hex_encode(const std::string& value) {
    std::string out; out.reserve(value.size() * 2);
    for (unsigned char c : value) { out.push_back(hex_digit(c >> 4)); out.push_back(hex_digit(c & 15)); }
    return out;
}
int from_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}
bool hex_decode(const std::string& value, std::string& out) {
    if (value.size() % 2) return false;
    out.clear(); out.reserve(value.size() / 2);
    for (std::size_t i = 0; i < value.size(); i += 2) {
        const int hi = from_hex(value[i]), lo = from_hex(value[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return true;
}
std::string metadata_path(const std::string& cache_dir) { return cache_dir + "/installed.tsv"; }
bool write_bytes_atomic(const std::string& path, const void* data, std::size_t size) {
    const std::string temp = path + ".new";
    FILE* out = std::fopen(temp.c_str(), "wb"); if (!out) return false;
    bool ok = std::fwrite(data, 1, size, out) == size && std::fflush(out) == 0;
    if (std::fclose(out) != 0) ok = false;
    if (!ok) { std::remove(temp.c_str()); return false; }
    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        if (std::remove(path.c_str()) != 0 && errno != ENOENT) { std::remove(temp.c_str()); return false; }
        if (std::rename(temp.c_str(), path.c_str()) != 0) { std::remove(temp.c_str()); return false; }
    }
    return true;
}
bool save_cache(const std::string& cache_dir, const std::vector<TitleEntry>& entries, std::string& error) {
    if (!local_mkdir_p(cache_dir)) { error = "无法创建游戏信息缓存目录"; return false; }
    std::ostringstream text;
    text << "# AstraNAS installed cache v2\n";
    for (const auto& entry : entries) {
        text << title_hex(entry.application_id) << '\t' << entry.base_version << '\t' << entry.patch_version << '\t'
             << static_cast<int>(entry.storage) << '\t' << entry.last_updated << '\t'
             << hex_encode(entry.name) << '\t' << hex_encode(entry.publisher) << '\t' << hex_encode(entry.display_version) << '\n';
    }
    const std::string data = text.str();
    if (!write_bytes_atomic(metadata_path(cache_dir), data.data(), data.size())) { error = "无法保存游戏信息缓存"; return false; }
    return true;
}
bool read_versions(std::uint64_t application_id, std::uint32_t& base_version,
                   std::uint32_t& patch_version, NcmStorageId& storage) {
    base_version = patch_version = 0; storage = NcmStorageId_None;
    s32 meta_count = 0;
    if (R_FAILED(nsCountApplicationContentMeta(application_id, &meta_count)) || meta_count <= 0 || meta_count > 256) return false;
    std::vector<NsApplicationContentMetaStatus> statuses(static_cast<std::size_t>(meta_count));
    s32 written = 0;
    if (R_FAILED(nsListApplicationContentMetaStatus(application_id, 0, statuses.data(), meta_count, &written)) || written < 0 || written > meta_count) return false;
    for (s32 i = 0; i < written; ++i) {
        const auto& content = statuses[static_cast<std::size_t>(i)];
        if (content.meta_type == NcmContentMetaType_Application) { base_version = std::max(base_version, content.version); storage = static_cast<NcmStorageId>(content.storageID); }
        else if (content.meta_type == NcmContentMetaType_Patch) { patch_version = std::max(patch_version, content.version); storage = static_cast<NcmStorageId>(content.storageID); }
    }
    return true;
}
struct JpegErrorManager { jpeg_error_mgr pub{}; jmp_buf jump{}; };
void jpeg_error_exit(j_common_ptr cinfo) { longjmp(reinterpret_cast<JpegErrorManager*>(cinfo->err)->jump, 1); }
} // namespace

std::string icon_path(const std::string& cache_dir, std::uint64_t application_id) { return cache_dir + "/" + title_hex(application_id) + ".jpg"; }
bool load_cache(const std::string& cache_dir, std::vector<TitleEntry>& entries, std::string& error) {
    entries.clear(); error.clear();
    std::ifstream in(metadata_path(cache_dir)); if (!in) return true;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> fields; std::size_t start = 0;
        for (;;) {
            const auto end = line.find('\t', start);
            fields.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break; start = end + 1;
        }
        if (fields.size() != 7 && fields.size() != 8) continue;
        const bool v2 = fields.size() == 8;
        TitleEntry entry{};
        try {
            entry.application_id = std::stoull(fields[0], nullptr, 16);
            entry.base_version = static_cast<std::uint32_t>(std::stoul(fields[1]));
            entry.patch_version = static_cast<std::uint32_t>(std::stoul(fields[2]));
            entry.storage = static_cast<NcmStorageId>(std::stoi(fields[3]));
            if (v2) entry.last_updated = std::stoull(fields[4]);
        } catch (...) { continue; }
        const std::size_t text_base = v2 ? 5 : 4;
        if (!hex_decode(fields[text_base], entry.name) || !hex_decode(fields[text_base + 1], entry.publisher) || !hex_decode(fields[text_base + 2], entry.display_version)) continue;
        entries.push_back(std::move(entry));
    }
    return true;
}
bool refresh(const std::string& cache_dir, std::vector<TitleEntry>& entries, std::string& error) {
    error.clear();
    std::vector<TitleEntry> previous; std::string ignored; load_cache(cache_dir, previous, ignored);
    std::map<std::uint64_t, TitleEntry> previous_by_id;
    for (const auto& item : previous) previous_by_id[item.application_id] = item;
    if (!local_mkdir_p(cache_dir)) { error = "无法创建游戏信息缓存目录"; return false; }
    std::vector<TitleEntry> fresh;
    constexpr s32 kChunk = 64, kMaxRecords = 4096;
    for (s32 offset = 0; offset < kMaxRecords;) {
        std::array<NsApplicationRecord, kChunk> records{}; s32 written = 0;
        const Result rc = nsListApplicationRecord(records.data(), records.size(), offset, &written);
        if (R_FAILED(rc)) { error = "无法读取已安装应用记录"; return false; }
        if (written < 0 || written > kChunk) { error = "系统返回了异常的应用数量"; return false; }
        if (written == 0) break;
        for (s32 i = 0; i < written; ++i) {
            TitleEntry entry{};
            const auto& record = records[static_cast<std::size_t>(i)];
            entry.application_id = record.application_id;
            entry.last_updated = record.last_updated;
            const auto cached = previous_by_id.find(entry.application_id);
            const std::string jpeg = icon_path(cache_dir, entry.application_id);
            const bool unchanged = cached != previous_by_id.end() && cached->second.last_updated != 0 && cached->second.last_updated == entry.last_updated && file_exists(jpeg);
            if (unchanged) {
                entry.base_version = cached->second.base_version; entry.patch_version = cached->second.patch_version;
                entry.storage = cached->second.storage; entry.name = cached->second.name; entry.publisher = cached->second.publisher; entry.display_version = cached->second.display_version;
            } else {
                read_versions(entry.application_id, entry.base_version, entry.patch_version, entry.storage);
                auto control = std::make_unique<NsApplicationControlData>(); u64 actual_size = 0;
                if (R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, entry.application_id,
                                    control.get(), sizeof(*control), &actual_size)) && actual_size >= sizeof(NacpStruct)) {
                    NacpLanguageEntry* language = nullptr;
                    if (R_SUCCEEDED(nsGetApplicationDesiredLanguage(&control->nacp, &language)) && language) {
                        entry.name = bounded_text(language->name, sizeof(language->name));
                        entry.publisher = bounded_text(language->author, sizeof(language->author));
                    }
                    entry.display_version = bounded_text(control->nacp.display_version, sizeof(control->nacp.display_version));
                    constexpr std::size_t icon_offset = offsetof(NsApplicationControlData, icon);
                    if (actual_size > icon_offset) {
                        const std::size_t icon_size = std::min<std::size_t>(static_cast<std::size_t>(actual_size - icon_offset), sizeof(control->icon));
                        if (icon_size > 4 && control->icon[0] == 0xFF && control->icon[1] == 0xD8) write_bytes_atomic(jpeg, control->icon, icon_size);
                    }
                }
                if (entry.name.empty() && cached != previous_by_id.end()) {
                    entry.name = cached->second.name; entry.publisher = cached->second.publisher; entry.display_version = cached->second.display_version;
                }
            }
            fresh.push_back(std::move(entry));
        }
        offset += written; if (written < kChunk) break;
    }
    std::sort(fresh.begin(), fresh.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.name.empty() != rhs.name.empty()) return !lhs.name.empty();
        if (lhs.name != rhs.name) return lhs.name < rhs.name;
        return lhs.application_id < rhs.application_id;
    });
    if (!save_cache(cache_dir, fresh, error)) return false;
    entries = std::move(fresh); return true;
}
bool load_icon_rgb(const std::string& cache_dir, std::uint64_t application_id,
                   std::vector<std::uint8_t>& rgb, int& width, int& height, std::string& error) {
    rgb.clear(); width = height = 0; error.clear();
    FILE* input = std::fopen(icon_path(cache_dir, application_id).c_str(), "rb");
    if (!input) { error = "没有可用的游戏图标缓存"; return false; }
    jpeg_decompress_struct cinfo{}; JpegErrorManager manager{};
    cinfo.err = jpeg_std_error(&manager.pub); manager.pub.error_exit = jpeg_error_exit;
    if (setjmp(manager.jump)) {
        jpeg_destroy_decompress(&cinfo); std::fclose(input); error = "游戏图标数据损坏"; rgb.clear(); width = height = 0; return false;
    }
    jpeg_create_decompress(&cinfo); jpeg_stdio_src(&cinfo, input); jpeg_read_header(&cinfo, TRUE); cinfo.out_color_space = JCS_RGB; jpeg_start_decompress(&cinfo);
    if (cinfo.output_width == 0 || cinfo.output_height == 0 || cinfo.output_width > 2048 || cinfo.output_height > 2048 || cinfo.output_components != 3) {
        jpeg_abort_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); std::fclose(input); error = "游戏图标尺寸无效"; return false;
    }
    width = static_cast<int>(cinfo.output_width); height = static_cast<int>(cinfo.output_height);
    rgb.resize(static_cast<std::size_t>(width) * height * 3u);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = rgb.data() + static_cast<std::size_t>(cinfo.output_scanline) * width * 3u;
        jpeg_read_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); std::fclose(input); return true;
}
} // namespace astranas::installed
#endif
