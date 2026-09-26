// SPDX-License-Identifier: GPL-3.0-or-later
#include "log_upload.hpp"
#ifdef __SWITCH__
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <utility>
#include <vector>

namespace astranas::log_upload {
namespace {
namespace palette = astranas::ui::palette;

constexpr const char* kInstallDirectoryFile = "sdmc:/switch/AstraNAS/.log-upload-install-dir";
constexpr const char* kNetDiagDirectoryFile = "sdmc:/switch/AstraNAS/.log-upload-netdiag-dir";
// v1.2.1 compatibility inputs. They are migrated into the single per-program file
// and removed whenever the user explicitly clears that program's setting.
constexpr const char* kLegacyInstallSmb = "sdmc:/switch/AstraNAS/.log-upload-install-smb-dir";
constexpr const char* kLegacyInstallWebDav = "sdmc:/switch/AstraNAS/.log-upload-install-webdav-dir";
constexpr const char* kLegacyNetDiagSmb = "sdmc:/switch/AstraNAS/.log-upload-netdiag-smb-dir";
constexpr const char* kLegacyNetDiagWebDav = "sdmc:/switch/AstraNAS/.log-upload-netdiag-webdav-dir";
constexpr std::size_t kVisibleRows = 7;

std::string normalize_path(std::string value) {
    if (value.empty()) return "/";
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.front() != '/') value.insert(value.begin(), '/');
    std::string compact;
    compact.reserve(value.size());
    bool slash = false;
    for (char c : value) {
        if (c == '/') {
            if (slash) continue;
            slash = true;
        } else {
            slash = false;
        }
        compact.push_back(c);
    }
    while (compact.size() > 1 && compact.back() == '/') compact.pop_back();
    return compact.empty() ? "/" : compact;
}

std::string configured_root(const AppConfig& config) {
    if (config.protocol == "smb" && config.share.empty()) return "/";
    return normalize_path(config.root);
}

std::string endpoint_key(const AppConfig& config) {
    std::string key = remote_endpoint_identity(config);
    std::replace(key.begin(), key.end(), '\n', '\t');
    return key;
}

bool path_within(const std::string& root_value, const std::string& path_value) {
    const std::string root = normalize_path(root_value);
    const std::string path = normalize_path(path_value);
    if (root == "/") return !path.empty() && path.front() == '/';
    if (path == root) return true;
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0 && path[root.size()] == '/';
}

std::string parent_within(const std::string& root_value, const std::string& path_value) {
    const std::string root = normalize_path(root_value);
    std::string path = normalize_path(path_value);
    if (!path_within(root, path) || path == root) return root;
    const auto slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) path = "/";
    else path.resize(slash);
    return path_within(root, path) ? path : root;
}

LogDestinationKind destination_kind(const std::string& filename_prefix) {
    return filename_prefix.find("NetDiag") != std::string::npos
        ? LogDestinationKind::NetDiag : LogDestinationKind::Install;
}

const char* canonical_file(LogDestinationKind kind) {
    return kind == LogDestinationKind::NetDiag ? kNetDiagDirectoryFile : kInstallDirectoryFile;
}

const char* legacy_file(const std::string& protocol, LogDestinationKind kind) {
    if (kind == LogDestinationKind::NetDiag)
        return protocol == "webdav" ? kLegacyNetDiagWebDav : kLegacyNetDiagSmb;
    return protocol == "webdav" ? kLegacyInstallWebDav : kLegacyInstallSmb;
}

bool read_saved_file(const char* path, const AppConfig& config, std::string& directory) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string saved_endpoint;
    std::string value;
    std::getline(in, saved_endpoint);
    std::getline(in, value);
    if (saved_endpoint != endpoint_key(config) || value.empty()) return false;
    directory = normalize_path(value);
    return true;
}

void save_directory(const AppConfig& config, LogDestinationKind kind, const std::string& directory) {
    std::ofstream out(canonical_file(kind), std::ios::trunc | std::ios::binary);
    if (out) out << endpoint_key(config) << '\n' << normalize_path(directory) << '\n';
}

std::string load_saved_directory(const AppConfig& config, LogDestinationKind kind) {
    std::string directory;
    if (read_saved_file(canonical_file(kind), config, directory)) return directory;
    // Upgrade path from v1.2.1. Only the legacy entry matching the active endpoint is
    // migrated, so the canonical model remains exactly one setting for each NRO.
    if (read_saved_file(legacy_file(config.protocol, kind), config, directory)) {
        save_directory(config, kind, directory);
        return directory;
    }
    return {};
}

bool list_directories(RemoteClient& remote,
                      const std::string& root,
                      const std::string& current,
                      std::vector<RemoteDirEntry>& directories,
                      std::string& error) {
    std::vector<RemoteDirEntry> entries;
    if (!remote.list_dir(current, entries, error)) return false;
    directories.clear();
    for (const auto& entry : entries) {
        if (!entry.is_dir || !path_within(root, entry.path)) continue;
        directories.push_back(entry);
    }
    std::sort(directories.begin(), directories.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    error.clear();
    return true;
}

std::size_t visible_begin(std::size_t selected, std::size_t count) {
    if (count <= kVisibleRows || selected < kVisibleRows) return 0;
    return std::min(selected - kVisibleRows + 1, count - kVisibleRows);
}

void draw_picker(astranas::ui::Gui& gui,
                 const std::string& title,
                 const std::string& current,
                 bool at_root,
                 const std::vector<RemoteDirEntry>& directories,
                 std::size_t selected,
                 const std::string& notice) {
    gui.begin();
    gui.clear(palette::background);
    gui.fill_rect(0, 0, astranas::ui::Gui::kWidth, 70, palette::navy);
    gui.text(40, 16, 28, palette::white, title);
    gui.round_rect(92, 94, 1096, 548, 18, palette::surface);
    gui.stroke_rect(92, 94, 1096, 548, 1, palette::border);
    gui.text(128, 124, 16, palette::muted, "日志保存目录", 180);
    gui.text(308, 124, 17, palette::text, current, 820);
    gui.text(128, 156, 14, palette::muted,
             notice.empty() ? "A 进入目录 / 选择当前目录；B 返回上级；Y 刷新" : notice,
             1000);

    const std::size_t count = directories.size() + 1;
    const std::size_t begin = visible_begin(selected, count);
    const std::size_t end = std::min(count, begin + kVisibleRows);
    for (std::size_t row = begin; row < end; ++row) {
        const int y = 196 + static_cast<int>(row - begin) * 54;
        const bool active = row == selected;
        gui.round_rect(128, y, 1018, 44, 9, active ? palette::selected : palette::surface_alt);
        gui.stroke_rect(128, y, 1018, 44, 1, palette::border);
        const std::string label = row == 0 ? "使用当前目录" : "目录 · " + directories[row - 1].name;
        gui.text(152, y + 10, 18, active ? palette::accent : palette::text, label, 950);
    }
    gui.divider(128, 588, 1018);
    gui.button_hint(128, 606, "A", "选择 / 进入");
    gui.button_hint(394, 606, "B", at_root ? "取消" : "上级");
    gui.button_hint(638, 606, "Y", "刷新");
    gui.button_hint(858, 606, "+", "退出", palette::danger);
    gui.end();
}

bool choose_directory(RemoteClient& remote,
                      const AppConfig& config,
                      astranas::ui::Gui& gui,
                      astranas::ui::InputRouter& input,
                      PadState& pad,
                      bool& exit_requested,
                      const std::string& title,
                      const std::string& initial,
                      std::string& selected_directory,
                      std::string& error) {
    const std::string root = configured_root(config);
    std::string current = initial.empty()
        ? (config.remote_dir.empty() ? root : normalize_path(config.remote_dir))
        : normalize_path(initial);
    if (!path_within(root, current)) current = root;

    std::vector<RemoteDirEntry> directories;
    if (!list_directories(remote, root, current, directories, error)) {
        if (current == root || !list_directories(remote, root, root, directories, error)) return false;
        current = root;
    }

    std::size_t selected = 0;
    std::string notice;
    while (appletMainLoop()) {
        draw_picker(gui, title, current, current == root, directories, selected, notice);
        const auto frame = input.update(pad);
        if (frame.down & HidNpadButton_Plus) {
            exit_requested = true;
            error = "cancelled";
            return false;
        }
        const std::size_t count = directories.size() + 1;
        if (frame.nav & HidNpadButton_Up)
            selected = count ? (selected == 0 ? count - 1 : selected - 1) : 0;
        if (frame.nav & HidNpadButton_Down)
            selected = count ? (selected + 1) % count : 0;
        if (frame.touch_tap && frame.touch_x >= 128 && frame.touch_x < 1146 && frame.touch_y >= 196) {
            const std::size_t begin = visible_begin(selected, count);
            const int row = (frame.touch_y - 196) / 54;
            if (row >= 0 && row < static_cast<int>(kVisibleRows)) {
                const std::size_t index = begin + static_cast<std::size_t>(row);
                if (index < count) selected = index;
            }
        }
        if (frame.down & HidNpadButton_Y) {
            std::string refresh_error;
            if (!list_directories(remote, root, current, directories, refresh_error))
                notice = "刷新失败：" + refresh_error;
            else
                notice.clear();
            selected = 0;
            continue;
        }
        if (frame.down & HidNpadButton_B) {
            if (current == root) {
                error = "cancelled";
                return false;
            }
            const std::string parent = parent_within(root, current);
            std::vector<RemoteDirEntry> parent_directories;
            std::string parent_error;
            if (!list_directories(remote, root, parent, parent_directories, parent_error)) {
                notice = "读取上级目录失败：" + parent_error;
                continue;
            }
            current = parent;
            directories = std::move(parent_directories);
            selected = 0;
            notice.clear();
            continue;
        }
        if (!(frame.down & HidNpadButton_A)) continue;
        if (selected == 0) {
            selected_directory = current;
            error.clear();
            return true;
        }
        if (selected - 1 >= directories.size()) continue;
        const auto next = normalize_path(directories[selected - 1].path);
        if (!path_within(root, next)) {
            notice = "目录超出当前 NAS 根路径";
            continue;
        }
        std::vector<RemoteDirEntry> child_directories;
        std::string child_error;
        if (!list_directories(remote, root, next, child_directories, child_error)) {
            notice = "读取目录失败：" + child_error;
            continue;
        }
        current = next;
        directories = std::move(child_directories);
        selected = 0;
        notice.clear();
    }
    error = "cancelled";
    return false;
}

} // namespace

std::string make_log_filename(const std::string& prefix) {
    const std::time_t now = std::time(nullptr);
    std::tm* utc = std::gmtime(&now);
    char stamp[32]{};
    if (utc) std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", utc);
    else std::snprintf(stamp, sizeof(stamp), "%lld", static_cast<long long>(now));
    return prefix + "-" + stamp + ".log";
}

bool write_text_file(const std::string& path, const std::string& text, std::string& error) {
    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) {
        error = "无法写入本地日志：" + path;
        return false;
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
        error = "本地日志写入失败：" + path;
        return false;
    }
    error.clear();
    return true;
}

std::string configured_directory(const AppConfig& config, LogDestinationKind kind) {
    return load_saved_directory(config, kind);
}

void clear_configured_directory(LogDestinationKind kind) {
    std::remove(canonical_file(kind));
    if (kind == LogDestinationKind::NetDiag) {
        std::remove(kLegacyNetDiagSmb);
        std::remove(kLegacyNetDiagWebDav);
    } else {
        std::remove(kLegacyInstallSmb);
        std::remove(kLegacyInstallWebDav);
    }
}

bool configure_directory(RemoteClient& remote,
                         const AppConfig& config,
                         LogDestinationKind kind,
                         astranas::ui::Gui& gui,
                         astranas::ui::InputRouter& input,
                         PadState& pad,
                         bool& exit_requested,
                         const std::string& picker_title,
                         std::string& selected_directory,
                         std::string& error) {
    const std::string initial = load_saved_directory(config, kind);
    if (!choose_directory(remote, config, gui, input, pad, exit_requested,
                          picker_title, initial, selected_directory, error))
        return false;
    save_directory(config, kind, selected_directory);
    error.clear();
    return true;
}

bool upload_log(RemoteClient& remote,
                const AppConfig& config,
                astranas::ui::Gui& gui,
                astranas::ui::InputRouter& input,
                PadState& pad,
                bool& exit_requested,
                const std::string& local_path,
                const std::string& filename_prefix,
                const std::string& picker_title,
                std::string& uploaded_remote_path,
                std::string& error) {
    std::ifstream check(local_path, std::ios::binary);
    if (!check) {
        error = "本地日志不存在：" + local_path;
        return false;
    }
    check.close();

    const std::string root = configured_root(config);
    const auto kind = destination_kind(filename_prefix);
    std::string directory = load_saved_directory(config, kind);
    if (!directory.empty() && !path_within(root, directory)) {
        clear_configured_directory(kind);
        directory.clear();
    }
    if (!directory.empty()) {
        std::vector<RemoteDirEntry> ignored;
        std::string verify_error;
        if (!list_directories(remote, root, directory, ignored, verify_error)) {
            clear_configured_directory(kind);
            directory.clear();
        }
    }
    if (directory.empty()) {
        if (!configure_directory(remote, config, kind, gui, input, pad, exit_requested,
                                 picker_title, directory, error))
            return false;
    }

    const std::string remote_path = join_remote_path(directory, make_log_filename(filename_prefix));
    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    if (!remote.upload(local_path, remote_path, transferred, total,
                       [](std::uint64_t, std::uint64_t) { return true; }, error)) {
        // A transient upload error does not invalidate an explicitly chosen directory.
        // It will only be cleared later if listing proves that directory is unusable.
        return false;
    }
    uploaded_remote_path = remote_path;
    error.clear();
    return true;
}

} // namespace astranas::log_upload
#endif
