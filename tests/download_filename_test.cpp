// SPDX-License-Identifier: GPL-3.0-or-later
#include "download_cache.hpp"

#include <cassert>
#include <cstddef>
#include <string>

namespace {
bool valid_utf8(const std::string& value) {
    std::size_t i = 0;
    while (i < value.size()) {
        const unsigned char c0 = static_cast<unsigned char>(value[i++]);
        if (c0 < 0x80) continue;
        std::size_t extra = 0;
        if (c0 >= 0xC2 && c0 <= 0xDF) extra = 1;
        else if (c0 >= 0xE0 && c0 <= 0xEF) extra = 2;
        else if (c0 >= 0xF0 && c0 <= 0xF4) extra = 3;
        else return false;
        if (i + extra > value.size()) return false;
        for (std::size_t j = 0; j < extra; ++j)
            if ((static_cast<unsigned char>(value[i + j]) & 0xC0) != 0x80) return false;
        i += extra;
    }
    return true;
}
}

int main() {
    AppConfig config;
    config.url = "smb://nas/Games";
    config.username = "tester";
    config.local_dir = "sdmc:/";

    RemoteDirEntry entry;
    entry.name = "塞尔达传说 王国之泪.nsp";
    entry.path = "/Switch/塞尔达传说 王国之泪.nsp";
    entry.identity = "smb:123";
    entry.size = 123456;

    const std::string chinese = remote_cache_filename(config, entry);
    assert(chinese == "塞尔达传说 王国之泪.nsp");
    assert(valid_utf8(chinese));
    assert(chinese.find(remote_object_key(config, entry).substr(0, 16)) == std::string::npos);

    entry.name = "bad:name?.nsp";
    assert(remote_cache_filename(config, entry) == "bad_name_.nsp");

    entry.name = std::string("中文") + std::string("\xE4\xB8", 2) + ".nsp";
    const std::string repaired = remote_cache_filename(config, entry);
    assert(valid_utf8(repaired));
    assert(repaired.find('_') != std::string::npos);
    assert(repaired.size() >= 4 && repaired.compare(repaired.size() - 4, 4, ".nsp") == 0);

    entry.name.clear();
    for (int i = 0; i < 100; ++i) entry.name += "中";
    entry.name += ".nsp";
    const std::string long_chinese = remote_cache_filename(config, entry);
    assert(long_chinese.size() <= 180);
    assert(long_chinese.size() >= 4);
    assert(long_chinese.compare(long_chinese.size() - 4, 4, ".nsp") == 0);
    assert(long_chinese.find('~') != std::string::npos);
    assert(valid_utf8(long_chinese));

    config.local_dir = "sdmc:/" + std::string(650, 'd');
    const std::string deep_name = remote_cache_filename(config, entry);
    assert(config.local_dir.size() + 1 + deep_name.size() + sizeof(".astranas-meta.new") - 1 <= 0x300);
    assert(deep_name.size() >= 4);
    assert(valid_utf8(deep_name));
    config.local_dir = "sdmc:/";

    RemoteDirEntry same_name_a{};
    same_name_a.name = "游戏.nsp";
    same_name_a.path = "/A/游戏.nsp";
    same_name_a.identity = "smb:a";
    same_name_a.size = 1;
    RemoteDirEntry same_name_b = same_name_a;
    same_name_b.path = "/B/游戏.nsp";
    same_name_b.identity = "smb:b";
    assert(remote_cache_filename(config, same_name_a) == "游戏.nsp");
    assert(remote_cache_filename(config, same_name_b) == "游戏.nsp");
    assert(remote_object_key(config, same_name_a) != remote_object_key(config, same_name_b));

    return 0;
}
