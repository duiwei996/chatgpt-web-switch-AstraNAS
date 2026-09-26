// SPDX-License-Identifier: GPL-3.0-or-later
#include "manifest.hpp"
#include <json-c/json.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <regex>

namespace {
bool parse_title_id(const std::string& text, std::uint64_t& out) {
    if (text.size() != 16) return false;
    for (char c : text) if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    try { out = std::stoull(text, nullptr, 16); return true; } catch (...) { return false; }
}
std::string basename_without_extension(const std::string& filename) {
    const auto slash = filename.find_last_of("/\\");
    std::string base = slash == std::string::npos ? filename : filename.substr(slash + 1);
    const auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base.resize(dot);
    return base;
}
}

std::string title_id_hex(std::uint64_t title_id) {
    char buf[17]{};
    std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(title_id));
    return buf;
}

bool parse_manifest_json(const std::string& text, std::vector<RemoteItem>& items, std::string& error) {
    items.clear();
    json_tokener* tokener = json_tokener_new();
    if (!tokener) { error = "json_tokener_new failed"; return false; }
    json_object* root = json_tokener_parse_ex(tokener, text.c_str(), static_cast<int>(text.size()));
    const auto parse_error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    if (!root || parse_error != json_tokener_success) {
        if (root) json_object_put(root);
        error = "invalid library.json";
        return false;
    }
    json_object* array = nullptr;
    if (!json_object_object_get_ex(root, "items", &array) || !json_object_is_type(array, json_type_array)) {
        json_object_put(root); error = "library.json: items[] missing"; return false;
    }
    const auto count = json_object_array_length(array);
    for (std::size_t i = 0; i < count; ++i) {
        json_object* obj = json_object_array_get_idx(array, i);
        if (!obj || !json_object_is_type(obj, json_type_object)) continue;
        json_object *id_obj=nullptr, *name_obj=nullptr, *version_obj=nullptr, *path_obj=nullptr, *type_obj=nullptr, *size_obj=nullptr, *sha_obj=nullptr;
        if (!json_object_object_get_ex(obj, "title_id", &id_obj) || !json_object_object_get_ex(obj, "path", &path_obj)) continue;
        RemoteItem item;
        const char* id_text = json_object_get_string(id_obj);
        if (!id_text || !parse_title_id(id_text, item.application_id)) continue;
        item.path = json_object_get_string(path_obj) ? json_object_get_string(path_obj) : "";
        if (item.path.empty()) continue;
        if (json_object_object_get_ex(obj, "name", &name_obj) && json_object_get_string(name_obj)) item.name = json_object_get_string(name_obj);
        if (item.name.empty()) item.name = basename_without_extension(item.path);
        if (json_object_object_get_ex(obj, "version", &version_obj)) {
            const auto version = json_object_get_int64(version_obj);
            if (version > 0) item.version = static_cast<std::uint32_t>(std::min<std::int64_t>(version, 0xFFFFFFFFll));
        }
        if (json_object_object_get_ex(obj, "size", &size_obj)) item.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, json_object_get_int64(size_obj)));
        if (json_object_object_get_ex(obj, "type", &type_obj) && json_object_get_string(type_obj)) item.content_type = json_object_get_string(type_obj);
        if (json_object_object_get_ex(obj, "sha256", &sha_obj) && json_object_get_string(sha_obj)) {
            std::string sha = json_object_get_string(sha_obj);
            if (sha.size() == 64 && std::all_of(sha.begin(), sha.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
                std::transform(sha.begin(), sha.end(), sha.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                item.sha256 = std::move(sha);
            }
        }
        items.push_back(std::move(item));
    }
    json_object_put(root);
    if (items.empty()) { error = "library.json contains no usable items"; return false; }
    return true;
}

bool parse_remote_filename(const std::string& filename, const std::string& path, std::uint64_t size, RemoteItem& out) {
    static const std::regex pattern(R"(^\s*(.*?)\s*\[([0-9A-Fa-f]{16})\](?:\[v([0-9]+)\])?.*$)");
    std::smatch match;
    if (!std::regex_match(filename, match, pattern)) return false;
    std::uint64_t title_id = 0;
    if (!parse_title_id(match[2].str(), title_id)) return false;
    out = RemoteItem{};
    out.application_id = title_id;
    out.name = match[1].str();
    if (out.name.empty()) out.name = basename_without_extension(filename);
    out.path = path;
    out.size = size;
    if (match[3].matched) {
        try { out.version = static_cast<std::uint32_t>(std::min<unsigned long long>(std::stoull(match[3].str()), 0xFFFFFFFFull)); }
        catch (...) { out.version = 0; }
    }
    return true;
}
