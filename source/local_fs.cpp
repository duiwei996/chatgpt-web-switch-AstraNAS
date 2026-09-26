// SPDX-License-Identifier: GPL-3.0-or-later
#include "local_fs.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
std::string trim_trailing_slash(std::string p) {
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p;
}

bool path_has_parent_reference(const std::string& path) {
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto end = path.find('/', start);
        const auto length = (end == std::string::npos ? path.size() : end) - start;
        if (length == 2 && path.compare(start, 2, "..") == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

bool path_has_symlink_component(const std::string& root, const std::string& path) {
    std::string current = trim_trailing_slash(root);
    struct stat st{};
    if (lstat(current.c_str(), &st) != 0) return errno != ENOENT;
    if (S_ISLNK(st.st_mode)) return true;

    std::size_t start = current.size();
    while (start < path.size()) {
        if (path[start] == '/') ++start;
        if (start >= path.size()) break;
        const auto slash = path.find('/', start);
        current = path.substr(0, slash);
        if (lstat(current.c_str(), &st) != 0) {
            if (errno == ENOENT) return false;
            return true;
        }
        if (S_ISLNK(st.st_mode)) return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

bool path_is_lexically_within(const std::string& root, const std::string& path) {
    const std::string r = trim_trailing_slash(root);
    const std::string p = trim_trailing_slash(path);
    if (r.empty() || p.empty() || path_has_parent_reference(r) || path_has_parent_reference(p)) return false;
    return p == r ||
           (p.size() > r.size() && p.compare(0, r.size(), r) == 0 && p[r.size()] == '/');
}

bool mkdir_one(const std::string& path) {
    if (path.empty() || path_has_parent_reference(path)) return false;
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    if (errno != ENOENT) return false;
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    if (errno != EEXIST || lstat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

bool remove_tree(const std::string& root, const std::string& path, std::string& error) {
    const std::string managedRoot = trim_trailing_slash(root);
    const auto slash = trim_trailing_slash(path).find_last_of('/');
    const std::string parent = slash == std::string::npos ? std::string{} : path.substr(0, slash);
    if (!path_is_lexically_within(managedRoot, path) || path == managedRoot ||
        !path_is_lexically_within(managedRoot, parent) ||
        path_has_symlink_component(managedRoot, parent)) {
        error = "refusing to delete outside managed directory";
        return false;
    }
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        error = std::strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (std::remove(path.c_str()) != 0) {
            error = std::strerror(errno);
            return false;
        }
        return true;
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) { error = std::strerror(errno); return false; }
    while (dirent* ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        const std::string child = local_join_path(path, ent->d_name);
        if (!remove_tree(root, child, error)) { closedir(dir); return false; }
    }
    closedir(dir);
    if (rmdir(path.c_str()) != 0) { error = std::strerror(errno); return false; }
    return true;
}
}

bool local_path_exists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

bool local_mkdir_p(const std::string& path) {
    if (path.empty()) return false;
    std::string current;
    current.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        if (path[i] != '/') continue;
        if (current == "/" || current == "sdmc:/") continue;
        if (!mkdir_one(current.substr(0, current.size() - 1))) return false;
    }
    return mkdir_one(path);
}

std::string local_join_path(const std::string& parent, const std::string& child) {
    if (parent.empty()) return child;
    if (parent.back() == '/') return parent + child;
    return parent + "/" + child;
}

bool list_local_dir(const std::string& path, std::vector<LocalEntry>& entries, std::string& error) {
    entries.clear();
    DIR* dir = opendir(path.c_str());
    if (!dir) { error = std::strerror(errno); return false; }
    while (dirent* ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        LocalEntry item;
        item.name = ent->d_name;
        item.path = local_join_path(path, item.name);
        struct stat st{};
        if (lstat(item.path.c_str(), &st) == 0 || stat(item.path.c_str(), &st) == 0) {
            item.is_dir = S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
            if (!item.is_dir && st.st_size > 0) item.size = static_cast<std::uint64_t>(st.st_size);
        }
        entries.push_back(std::move(item));
    }
    closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const LocalEntry& a, const LocalEntry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
        std::string an = a.name, bn = b.name;
        std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return an < bn;
    });
    return true;
}

bool local_path_is_within(const std::string& root, const std::string& path) {
    const std::string r = trim_trailing_slash(root);
    const std::string p = trim_trailing_slash(path);
    return path_is_lexically_within(r, p) && !path_has_symlink_component(r, p);
}

std::string local_parent_within(const std::string& root, const std::string& current) {
    const std::string r = trim_trailing_slash(root);
    std::string c = trim_trailing_slash(current);
    if (!local_path_is_within(r, c) || c == r) return r;
    const auto slash = c.find_last_of('/');
    if (slash == std::string::npos) return r;
    c.resize(slash);
    if (!local_path_is_within(r, c)) return r;
    return c;
}

bool delete_local_entry(const std::string& root, const LocalEntry& entry, std::string& error) {
    return remove_tree(root, entry.path, error);
}

bool clear_local_directory(const std::string& root, std::string& error) {
    std::vector<LocalEntry> entries;
    if (!list_local_dir(root, entries, error)) return false;
    for (const auto& entry : entries) if (!remove_tree(root, entry.path, error)) return false;
    return true;
}

bool copy_local_file(const std::string& source, const std::string& destination, std::string& error) {
    if (source.empty() || destination.empty() || source == destination || path_has_parent_reference(destination)) {
        error = "invalid copy path";
        return false;
    }
    std::FILE* in = std::fopen(source.c_str(), "rb");
    if (!in) { error = "cannot open source"; return false; }

    const std::string temporary = destination + ".astranas-part";
    std::remove(temporary.c_str());
    std::FILE* out = std::fopen(temporary.c_str(), "wb");
    if (!out) { std::fclose(in); error = "cannot open temporary destination"; return false; }

    unsigned char buffer[256 * 1024];
    bool ok = true;
    for (;;) {
        const std::size_t got = std::fread(buffer, 1, sizeof(buffer), in);
        if (got && std::fwrite(buffer, 1, got, out) != got) { ok = false; error = "write failed"; break; }
        if (got < sizeof(buffer)) {
            if (std::ferror(in)) { ok = false; error = "read failed"; }
            break;
        }
    }
    if (ok && std::fflush(out) != 0) { ok = false; error = "flush failed"; }
    if (ok && ::fsync(fileno(out)) != 0) { ok = false; error = "sync failed"; }
    if (std::fclose(out) != 0 && ok) { ok = false; error = "close failed"; }
    std::fclose(in);
    if (!ok) { std::remove(temporary.c_str()); return false; }
    if (std::rename(temporary.c_str(), destination.c_str()) != 0) {
        error = std::strerror(errno);
        std::remove(temporary.c_str());
        return false;
    }
    return true;
}


namespace {
bool copy_tree_for_move(const std::string& source, const std::string& destination,
                        std::string& error) {
    struct stat st{};
    if (lstat(source.c_str(), &st) != 0) { error = std::strerror(errno); return false; }
    if (S_ISLNK(st.st_mode)) { error = "refusing to move a symbolic link"; return false; }
    if (!S_ISDIR(st.st_mode)) return copy_local_file(source, destination, error);
    if (!local_mkdir_p(destination)) { error = "cannot create move destination directory"; return false; }
    DIR* dir = opendir(source.c_str());
    if (!dir) { error = std::strerror(errno); return false; }
    bool ok = true;
    while (dirent* ent = readdir(dir)) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        const std::string src = local_join_path(source, ent->d_name);
        const std::string dst = local_join_path(destination, ent->d_name);
        if (!copy_tree_for_move(src, dst, error)) { ok = false; break; }
    }
    closedir(dir);
    return ok;
}
}

bool move_local_entry(const std::string& source_root, const std::string& destination_root,
                      const LocalEntry& entry, const std::string& destination_directory,
                      std::string& destination_path, std::string& error) {
    destination_path.clear();
    error.clear();
    if (entry.path.empty() || entry.name.empty() || entry.path == source_root ||
        !local_path_is_within(source_root, entry.path) ||
        !local_path_is_within(destination_root, destination_directory)) {
        error = "invalid move source or destination";
        return false;
    }
    const std::string target = local_join_path(destination_directory, entry.name);
    if (!local_path_is_within(destination_root, target)) {
        error = "move destination is outside the selected storage";
        return false;
    }
    if (target == entry.path) {
        error = "source is already in this directory";
        return false;
    }
    if (entry.is_dir && path_is_lexically_within(entry.path, target)) {
        error = "cannot move a directory inside itself";
        return false;
    }
    if (local_path_exists(target)) {
        error = "destination already contains an item with the same name";
        return false;
    }

    if (std::rename(entry.path.c_str(), target.c_str()) == 0) {
        if (!entry.is_dir) {
            const std::string src_meta = entry.path + ".astranas-meta";
            const std::string dst_meta = target + ".astranas-meta";
            if (local_path_exists(src_meta)) {
                std::remove(dst_meta.c_str());
                (void)std::rename(src_meta.c_str(), dst_meta.c_str());
            }
        }
        destination_path = target;
        return true;
    }
    const int rename_error = errno;
    if (rename_error != EXDEV) {
        error = std::strerror(rename_error);
        return false;
    }

    if (!copy_tree_for_move(entry.path, target, error)) {
        LocalEntry partial{entry.name, target, 0, entry.is_dir};
        std::string cleanup_error;
        if (local_path_exists(target)) (void)delete_local_entry(destination_root, partial, cleanup_error);
        return false;
    }
    if (!entry.is_dir) {
        const std::string src_meta = entry.path + ".astranas-meta";
        const std::string dst_meta = target + ".astranas-meta";
        if (local_path_exists(src_meta)) {
            std::string meta_error;
            if (!copy_local_file(src_meta, dst_meta, meta_error)) std::remove(dst_meta.c_str());
        }
    }
    if (!delete_local_entry(source_root, entry, error)) {
        error = "copied to destination but could not remove source: " + error;
        destination_path = target;
        return false;
    }
    if (!entry.is_dir) std::remove((entry.path + ".astranas-meta").c_str());
    destination_path = target;
    return true;
}
