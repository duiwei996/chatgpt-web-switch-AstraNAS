// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/curl_client.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: remote_upload_test <port>\n";
        return 2;
    }
    const int port = std::atoi(argv[1]);
    if (port <= 0 || port > 65535) return 3;

    const std::string source = "/tmp/AstraNAS-remote-upload-source.bin";
    const std::string payload = "AstraNAS WebDAV upload regression\n";
    {
        std::ofstream out(source, std::ios::binary | std::ios::trunc);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        if (!out) return 4;
    }

    AppConfig config;
    config.protocol = "webdav";
    config.server = "127.0.0.1";
    config.port = port;
    config.webdav_tls = false;
    config.url = "webdav://127.0.0.1:" + std::to_string(port);

    CurlRemoteClient client;
    std::string error;
    if (!client.connect(config, error)) {
        std::cerr << error << '\n';
        return 5;
    }

    std::uint64_t transferred = 0;
    std::uint64_t total = 0;
    int progress_calls = 0;
    if (!client.upload(source, "/logs/astranas.log", transferred, total,
            [&](std::uint64_t, std::uint64_t) {
                ++progress_calls;
                return true;
            }, error)) {
        std::cerr << error << '\n';
        return 6;
    }
    if (transferred != payload.size() || total != payload.size() || progress_calls == 0) {
        std::cerr << "unexpected upload progress/result\n";
        return 7;
    }
    return 0;
}
