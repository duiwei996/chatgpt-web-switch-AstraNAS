// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

struct SmbProfileConfig {
    std::string server;
    int port = 0;
    std::string share;
    std::string root = "/";
    std::string username;
    std::string password;
    std::string remote_dir;
};

struct WebDavProfileConfig {
    std::string server;
    int port = 0;
    std::string root = "/";
    std::string username;
    std::string password;
    std::string remote_dir;
    bool tls = true;
    bool tls_verify = true;
};

struct AppConfig {
    int config_version = 3;
    std::string protocol = "smb";
    SmbProfileConfig smb;
    WebDavProfileConfig webdav;

    // Active endpoint compatibility view. Runtime/network code reads these fields;
    // save_config() writes them back only to the currently selected profile.
    std::string server;
    int port = 0;
    std::string share;
    bool webdav_tls = true;
    std::string url;
    std::string root = "/";
    std::string username;
    std::string password;
    std::string remote_dir;
    bool tls_verify = true;

    std::string manifest = "library.json";
    std::string local_dir = "sdmc:/";
    std::string local_root = "sdmc:/";
    std::string cache_dir = "sdmc:/switch/AstraNAS/cache";
    int download_retries = 2;
    bool network_direct_install = true;
    bool verify_sha256 = false;
    bool verify_nca_content_hash = false;
    bool install_to_nand = false;
    bool delete_source_after_install = true;
    bool ignore_required_firmware = false;
    bool validate_nca = true;
};

int default_remote_port(const AppConfig& config);
std::string build_remote_url(const AppConfig& config);
std::string remote_endpoint_identity(const AppConfig& config);
void capture_active_remote_profile(AppConfig& config);
void activate_remote_profile(AppConfig& config);
void set_active_protocol(AppConfig& config, const std::string& protocol);
void normalize_config_endpoint(AppConfig& config);
bool load_config(const std::string& path, AppConfig& out, std::string& error);
bool save_config(const std::string& path, const AppConfig& config, std::string& error);
bool write_example_config(const std::string& path, std::string& error);
