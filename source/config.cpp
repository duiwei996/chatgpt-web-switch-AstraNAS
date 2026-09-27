// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {
std::string trim(std::string value) {
    const auto non_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), non_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), non_space).base(), value.end());
    return value;
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool parse_bool(const std::string& value, bool fallback) {
    const auto s = lower_copy(value);
    if (s == "1" || s == "true" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "no" || s == "off") return false;
    return fallback;
}

bool path_at_or_below(const std::string& candidate, const std::string& root) {
    if (candidate == root) return true;
    if (root.empty()) return false;
    std::string prefix = root;
    if (prefix.back() != '/') prefix.push_back('/');
    return candidate.rfind(prefix, 0) == 0;
}

bool commit_config_filesystem(const std::string& path, std::string& error) {
#ifdef __SWITCH__
    const std::size_t separator = path.find(":/");
    if (separator == std::string::npos || separator == 0) return true;
    const std::string device = path.substr(0, separator);
    const Result rc = fsdevCommitDevice(device.c_str());
    if (R_FAILED(rc)) {
        std::ostringstream out;
        out << "fsdevCommitDevice(" << device << ") failed (0x"
            << std::hex << static_cast<u32>(rc) << ")";
        error = out.str();
        return false;
    }
#else
    (void)path;
#endif
    error.clear();
    return true;
}

int parse_int(const std::string& value, int fallback, int low, int high) {
    try {
        const int parsed = std::stoi(value);
        return std::max(low, std::min(high, parsed));
    } catch (...) {
        return fallback;
    }
}

std::string norm_root(std::string value) {
    value = trim(std::move(value));
    if (value.empty()) return "/";
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.front() != '/') value.insert(value.begin(), '/');
    while (value.size() > 1 && value.back() == '/') value.pop_back();
    return value;
}

std::string join_root(std::string a, std::string b) {
    a = norm_root(std::move(a));
    b = norm_root(std::move(b));
    if (a == "/") return b;
    if (b == "/") return a;
    return a + b;
}

void split_authority(std::string authority, std::string& host, int& port) {
    host = authority;
    port = 0;
    if (authority.empty()) return;
    if (authority.front() == '[') {
        const auto close = authority.find(']');
        if (close != std::string::npos) {
            host = authority.substr(0, close + 1);
            if (close + 1 < authority.size() && authority[close + 1] == ':')
                port = parse_int(authority.substr(close + 2), 0, 0, 65535);
            return;
        }
    }
    const auto colon = authority.find_last_of(':');
    if (colon != std::string::npos && authority.find(':') == colon) {
        const int parsed = parse_int(authority.substr(colon + 1), -1, -1, 65535);
        if (parsed >= 0) {
            host = authority.substr(0, colon);
            port = parsed;
        }
    }
}

void normalize_smb(SmbProfileConfig& profile) {
    profile.server = trim(profile.server);
    profile.share = trim(profile.share);
    profile.root = norm_root(profile.root);
    if (!profile.remote_dir.empty()) profile.remote_dir = norm_root(profile.remote_dir);
    if (profile.port < 0 || profile.port > 65535) profile.port = 0;
}

void normalize_webdav(WebDavProfileConfig& profile) {
    profile.server = trim(profile.server);
    profile.root = norm_root(profile.root);
    if (!profile.remote_dir.empty()) profile.remote_dir = norm_root(profile.remote_dir);
    if (profile.port < 0 || profile.port > 65535) profile.port = 0;
}

void migrate_legacy_url(AppConfig& config) {
    if (config.url.empty() || !config.server.empty()) return;
    const auto lower = lower_copy(config.url);
    std::size_t prefix = 0;
    std::string protocol;
    bool tls = true;
    if (lower.rfind("smb://", 0) == 0) { protocol = "smb"; prefix = 6; }
    else if (lower.rfind("smb2://", 0) == 0) { protocol = "smb"; prefix = 7; }
    else if (lower.rfind("webdavs://", 0) == 0) { protocol = "webdav"; prefix = 10; tls = true; }
    else if (lower.rfind("webdav://", 0) == 0) { protocol = "webdav"; prefix = 9; tls = false; }
    else if (lower.rfind("https://", 0) == 0) { protocol = "webdav"; prefix = 8; tls = true; }
    else if (lower.rfind("http://", 0) == 0) { protocol = "webdav"; prefix = 7; tls = false; }
    else return;

    std::string rest = config.url.substr(prefix);
    const auto slash = rest.find('/');
    const auto authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    split_authority(authority, config.server, config.port);
    config.protocol = protocol;
    config.webdav_tls = tls;
    if (protocol == "smb") {
        while (!path.empty() && path.front() == '/') path.erase(path.begin());
        const auto next = path.find('/');
        config.share = next == std::string::npos ? path : path.substr(0, next);
        const std::string sub = next == std::string::npos ? "/" : "/" + path.substr(next + 1);
        config.root = join_root(sub, config.root);
    } else {
        config.root = join_root(path, config.root);
    }
}
} // namespace

int default_remote_port(const AppConfig& config) {
    return lower_copy(config.protocol) == "webdav" ? (config.webdav_tls ? 443 : 80) : 445;
}

std::string build_remote_url(const AppConfig& config) {
    const bool dav = lower_copy(config.protocol) == "webdav";
    std::string authority = config.server;
    const int port = config.port > 0 ? config.port : default_remote_port(config);
    const bool default_port = (!dav && port == 445) || (dav && config.webdav_tls && port == 443) ||
                              (dav && !config.webdav_tls && port == 80);
    if (!authority.empty() && !default_port) authority += ":" + std::to_string(port);
    if (!dav) {
        std::string out = "smb://" + authority + "/";
        if (!config.share.empty()) out += config.share;
        return out;
    }
    return std::string(config.webdav_tls ? "webdavs://" : "webdav://") + authority;
}

std::string remote_endpoint_identity(const AppConfig& config) {
    std::ostringstream out;
    out << lower_copy(config.protocol) << '\n' << config.server << '\n'
        << (config.port > 0 ? config.port : default_remote_port(config)) << '\n'
        << config.share << '\n' << norm_root(config.root) << '\n'
        << (config.webdav_tls ? "tls" : "plain");
    return out.str();
}

void capture_active_remote_profile(AppConfig& config) {
    if (lower_copy(config.protocol) == "webdav") {
        config.webdav.server = config.server;
        config.webdav.port = config.port;
        config.webdav.root = config.root;
        config.webdav.username = config.username;
        config.webdav.password = config.password;
        config.webdav.remote_dir = config.remote_dir;
        config.webdav.tls = config.webdav_tls;
        config.webdav.tls_verify = config.tls_verify;
        normalize_webdav(config.webdav);
    } else {
        config.smb.server = config.server;
        config.smb.port = config.port;
        config.smb.share = config.share;
        config.smb.root = config.root;
        config.smb.username = config.username;
        config.smb.password = config.password;
        config.smb.remote_dir = config.remote_dir;
        normalize_smb(config.smb);
    }
}

void activate_remote_profile(AppConfig& config) {
    config.protocol = lower_copy(config.protocol) == "webdav" ? "webdav" : "smb";
    if (config.protocol == "webdav") {
        normalize_webdav(config.webdav);
        config.server = config.webdav.server;
        config.port = config.webdav.port;
        config.share.clear();
        config.root = config.webdav.root;
        config.username = config.webdav.username;
        config.password = config.webdav.password;
        config.remote_dir = config.webdav.remote_dir;
        config.webdav_tls = config.webdav.tls;
        config.tls_verify = config.webdav.tls_verify;
    } else {
        normalize_smb(config.smb);
        config.server = config.smb.server;
        config.port = config.smb.port;
        config.share = config.smb.share;
        config.root = config.smb.root;
        config.username = config.smb.username;
        config.password = config.smb.password;
        config.remote_dir = config.smb.remote_dir;
        config.webdav_tls = true;
        config.tls_verify = true;
    }
    config.url = build_remote_url(config);
}

void set_active_protocol(AppConfig& config, const std::string& protocol) {
    capture_active_remote_profile(config);
    config.protocol = lower_copy(protocol) == "webdav" ? "webdav" : "smb";
    activate_remote_profile(config);
    normalize_config_endpoint(config);
}

void normalize_config_endpoint(AppConfig& config) {
    config.protocol = lower_copy(config.protocol) == "webdav" ? "webdav" : "smb";
    config.server = trim(config.server);
    config.share = trim(config.share);
    config.root = norm_root(config.root);
    if (!config.remote_dir.empty()) config.remote_dir = norm_root(config.remote_dir);
    if (config.port < 0 || config.port > 65535) config.port = 0;
    if (config.local_dir.empty()) config.local_dir = "sdmc:/";
    if (config.local_root.empty()) config.local_root = "sdmc:/";
    if (config.cache_dir.empty()) config.cache_dir = "sdmc:/switch/AstraNAS/cache";
    config.url = build_remote_url(config);
    capture_active_remote_profile(config);
}

bool load_config(const std::string& path, AppConfig& out, std::string& error) {
    out = AppConfig{};
    int file_config_version = 0;
    bool saw_profile_key = false;
    std::ifstream in(path);
    if (!in) {
        error = "config not found: " + path;
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const auto key = trim(line.substr(0, pos));
        const auto value = trim(line.substr(pos + 1));

        if (key == "config_version") file_config_version = parse_int(value, file_config_version, 0, 999);
        else if (key == "protocol") out.protocol = value;
        else if (key == "server") out.server = value;
        else if (key == "port") out.port = parse_int(value, out.port, 0, 65535);
        else if (key == "share") out.share = value;
        else if (key == "webdav_tls") out.webdav_tls = parse_bool(value, out.webdav_tls);
        else if (key == "url") out.url = value;
        else if (key == "root") out.root = value;
        else if (key == "username") out.username = value;
        else if (key == "password") out.password = value;
        else if (key == "remote_dir") out.remote_dir = value;
        else if (key == "tls_verify") out.tls_verify = parse_bool(value, out.tls_verify);
        else if (key == "smb_server") { out.smb.server = value; saw_profile_key = true; }
        else if (key == "smb_port") { out.smb.port = parse_int(value, out.smb.port, 0, 65535); saw_profile_key = true; }
        else if (key == "smb_share") { out.smb.share = value; saw_profile_key = true; }
        else if (key == "smb_root") { out.smb.root = value; saw_profile_key = true; }
        else if (key == "smb_username") { out.smb.username = value; saw_profile_key = true; }
        else if (key == "smb_password") { out.smb.password = value; saw_profile_key = true; }
        else if (key == "smb_remote_dir") { out.smb.remote_dir = value; saw_profile_key = true; }
        else if (key == "webdav_server") { out.webdav.server = value; saw_profile_key = true; }
        else if (key == "webdav_port") { out.webdav.port = parse_int(value, out.webdav.port, 0, 65535); saw_profile_key = true; }
        else if (key == "webdav_root") { out.webdav.root = value; saw_profile_key = true; }
        else if (key == "webdav_username") { out.webdav.username = value; saw_profile_key = true; }
        else if (key == "webdav_password") { out.webdav.password = value; saw_profile_key = true; }
        else if (key == "webdav_remote_dir") { out.webdav.remote_dir = value; saw_profile_key = true; }
        else if (key == "webdav_profile_tls") { out.webdav.tls = parse_bool(value, out.webdav.tls); saw_profile_key = true; }
        else if (key == "webdav_profile_tls_verify") { out.webdav.tls_verify = parse_bool(value, out.webdav.tls_verify); saw_profile_key = true; }
        else if (key == "manifest") out.manifest = value;
        else if (key == "local_dir") out.local_dir = value;
        else if (key == "local_root") out.local_root = value;
        else if (key == "local_focus_path") out.local_focus_path = value;
        else if (key == "local_download_path" && !value.empty()) {
            if (std::find(out.local_download_paths.begin(), out.local_download_paths.end(), value) ==
                    out.local_download_paths.end() &&
                out.local_download_paths.size() < kMaxRememberedLocalDownloads)
                out.local_download_paths.push_back(value);
        } else if (key == "cache_dir") out.cache_dir = value;
        else if (key == "download_retries") out.download_retries = parse_int(value, out.download_retries, 0, 5);
        else if (key == "network_direct_install") out.network_direct_install = parse_bool(value, out.network_direct_install);
        else if (key == "verify_sha256") out.verify_sha256 = parse_bool(value, out.verify_sha256);
        else if (key == "verify_nca_content_hash") out.verify_nca_content_hash = parse_bool(value, out.verify_nca_content_hash);
        else if (key == "install_target") {
            const auto target = lower_copy(value);
            out.install_to_nand = target == "nand" || target == "internal" || target == "builtinuser";
        }
        else if (key == "delete_source_after_install") out.delete_source_after_install = parse_bool(value, out.delete_source_after_install);
        else if (key == "ignore_required_firmware") out.ignore_required_firmware = parse_bool(value, out.ignore_required_firmware);
        else if (key == "validate_nca") out.validate_nca = parse_bool(value, out.validate_nca);
    }

    if (file_config_version < 2) {
        out.network_direct_install = true;
        out.verify_sha256 = false;
        out.verify_nca_content_hash = false;
    }

    if (file_config_version < 3 || !saw_profile_key) {
        migrate_legacy_url(out);
        out.protocol = lower_copy(out.protocol) == "webdav" ? "webdav" : "smb";
        normalize_config_endpoint(out);
    } else {
        out.protocol = lower_copy(out.protocol) == "webdav" ? "webdav" : "smb";
        activate_remote_profile(out);
        normalize_config_endpoint(out);
    }

    out.config_version = 3;
    if (out.manifest.empty()) out.manifest = "library.json";
    error.clear();
    return true;
}

bool save_config(const std::string& path, const AppConfig& input, std::string& error) {
    AppConfig config = input;
    config.config_version = 3;
    normalize_config_endpoint(config);
    normalize_smb(config.smb);
    normalize_webdav(config.webdav);

    const std::string temp = path + ".new";
    std::ofstream out(temp, std::ios::trunc | std::ios::binary);
    if (!out) {
        error = "cannot write config: " + temp;
        return false;
    }

    out << "# AstraNAS configuration\n"
        << "config_version=3\n"
        << "protocol=" << config.protocol << "\n"
        << "smb_server=" << config.smb.server << "\n"
        << "smb_port=" << config.smb.port << "\n"
        << "smb_share=" << config.smb.share << "\n"
        << "smb_root=" << config.smb.root << "\n"
        << "smb_username=" << config.smb.username << "\n"
        << "smb_password=" << config.smb.password << "\n"
        << "smb_remote_dir=" << config.smb.remote_dir << "\n"
        << "webdav_server=" << config.webdav.server << "\n"
        << "webdav_port=" << config.webdav.port << "\n"
        << "webdav_root=" << config.webdav.root << "\n"
        << "webdav_username=" << config.webdav.username << "\n"
        << "webdav_password=" << config.webdav.password << "\n"
        << "webdav_remote_dir=" << config.webdav.remote_dir << "\n"
        << "webdav_profile_tls=" << (config.webdav.tls ? "true" : "false") << "\n"
        << "webdav_profile_tls_verify=" << (config.webdav.tls_verify ? "true" : "false") << "\n"
        << "manifest=" << config.manifest << "\n"
        << "local_dir=" << config.local_dir << "\n"
        << "local_root=" << config.local_root << "\n"
        << "local_focus_path=" << config.local_focus_path << "\n"
        << "cache_dir=" << config.cache_dir << "\n"
        << "download_retries=" << config.download_retries << "\n"
        << "network_direct_install=" << (config.network_direct_install ? "true" : "false") << "\n"
        << "verify_sha256=" << (config.verify_sha256 ? "true" : "false") << "\n"
        << "verify_nca_content_hash=" << (config.verify_nca_content_hash ? "true" : "false") << "\n"
        << "install_target=" << (config.install_to_nand ? "nand" : "sd") << "\n"
        << "delete_source_after_install=" << (config.delete_source_after_install ? "true" : "false") << "\n"
        << "ignore_required_firmware=" << (config.ignore_required_firmware ? "true" : "false") << "\n"
        << "validate_nca=" << (config.validate_nca ? "true" : "false") << "\n";

    for (const auto& downloadPath : config.local_download_paths)
        if (!downloadPath.empty()) out << "local_download_path=" << downloadPath << "\n";

    out.flush();
    if (!out) {
        out.close();
        std::remove(temp.c_str());
        error = "failed while writing config";
        return false;
    }
    out.close();

    // std::ofstream::flush/close only hands buffered data to the device driver.
    // Make the temporary file durable before publishing it so a power-off or
    // app exit cannot leave the previous browsing location in config.ini.
    FILE* sync_file = std::fopen(temp.c_str(), "rb+");
    if (!sync_file) {
        error = "cannot reopen config for sync: " + std::string(std::strerror(errno));
        std::remove(temp.c_str());
        return false;
    }
    const bool sync_ok = ::fsync(fileno(sync_file)) == 0;
    const int sync_error = errno;
    const bool close_ok = std::fclose(sync_file) == 0;
    if (!sync_ok || !close_ok) {
        error = "cannot sync config: " + std::string(std::strerror(sync_ok ? errno : sync_error));
        std::remove(temp.c_str());
        return false;
    }

    if (std::rename(temp.c_str(), path.c_str()) != 0) {
        const int first = errno;
        if (std::remove(path.c_str()) != 0 && errno != ENOENT) {
            error = std::strerror(first);
            std::remove(temp.c_str());
            return false;
        }
        if (std::rename(temp.c_str(), path.c_str()) != 0) {
            error = std::strerror(errno);
            std::remove(temp.c_str());
            return false;
        }
    }
    std::string commit_error;
    if (!commit_config_filesystem(path, commit_error)) {
        error = "config was replaced but filesystem commit failed: " + commit_error;
        return false;
    }
    error.clear();
    return true;
}

void remember_local_download_path(AppConfig& config, const std::string& path) {
    if (path.empty()) return;
    config.local_download_paths.erase(
        std::remove(config.local_download_paths.begin(), config.local_download_paths.end(), path),
        config.local_download_paths.end());
    config.local_download_paths.push_back(path);
    if (config.local_download_paths.size() > kMaxRememberedLocalDownloads)
        config.local_download_paths.erase(
            config.local_download_paths.begin(),
            config.local_download_paths.begin() +
                static_cast<std::ptrdiff_t>(config.local_download_paths.size() -
                                            kMaxRememberedLocalDownloads));
}

void forget_local_downloads_at_or_below(AppConfig& config, const std::string& root) {
    config.local_download_paths.erase(
        std::remove_if(config.local_download_paths.begin(), config.local_download_paths.end(),
                       [&](const std::string& path) { return path_at_or_below(path, root); }),
        config.local_download_paths.end());
}

void relocate_local_downloads_at_or_below(AppConfig& config, const std::string& oldRoot,
                                          const std::string& newRoot) {
    if (oldRoot.empty() || newRoot.empty() || oldRoot == newRoot) return;
    for (auto& path : config.local_download_paths) {
        if (!path_at_or_below(path, oldRoot)) continue;
        path = newRoot + path.substr(oldRoot.size());
    }
}

bool write_example_config(const std::string& path, std::string& error) {
    AppConfig config;
    config.smb.server.clear();
    config.smb.share.clear();
    config.smb.root = "/";
    config.webdav.server.clear();
    config.webdav.root = "/";
    activate_remote_profile(config);
    return save_config(path, config, error);
}
