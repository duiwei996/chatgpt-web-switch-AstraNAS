// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

int main() {
    std::string error;

    AppConfig fresh;
    assert(fresh.config_version == 3);
    assert(fresh.local_root == "sdmc:/");
    assert(fresh.local_dir == "sdmc:/");
    assert(fresh.network_direct_install);
    assert(!fresh.verify_sha256);
    assert(!fresh.verify_nca_content_hash);
    assert(fresh.validate_nca);

    const std::string legacy_webdav = "/tmp/AstraNAS-v05-webdav.ini";
    {
        std::ofstream out(legacy_webdav);
        out << "url=webdav://nas.local/library\n"
            << "root=/packages\n"
            << "username=test\n"
            << "password=pw\n";
    }
    AppConfig webdav;
    assert(load_config(legacy_webdav, webdav, error));
    assert(webdav.config_version == 3);
    assert(webdav.protocol == "webdav");
    assert(webdav.server == "nas.local");
    assert(!webdav.webdav_tls);
    assert(webdav.root == "/library/packages");
    assert(webdav.webdav.server == "nas.local");
    assert(webdav.webdav.root == "/library/packages");
    assert(webdav.webdav.username == "test");
    assert(webdav.webdav.password == "pw");
    assert(default_remote_port(webdav) == 80);
    std::remove(legacy_webdav.c_str());

    const std::string legacy_smb = "/tmp/AstraNAS-v05-smb.ini";
    {
        std::ofstream out(legacy_smb);
        out << "url=smb://192.168.1.20/Games/Switch\n"
            << "root=/NSP\n";
    }
    AppConfig smb;
    assert(load_config(legacy_smb, smb, error));
    assert(smb.config_version == 3);
    assert(smb.protocol == "smb");
    assert(smb.server == "192.168.1.20");
    assert(smb.share == "Games");
    assert(smb.root == "/Switch/NSP");
    assert(smb.smb.server == "192.168.1.20");
    assert(smb.smb.share == "Games");
    assert(default_remote_port(smb) == 445);
    std::remove(legacy_smb.c_str());

    const std::string legacy_v1 = "/tmp/AstraNAS-v10-legacy.ini";
    {
        std::ofstream out(legacy_v1);
        out << "protocol=smb\nserver=nas.local\nverify_sha256=true\n";
    }
    AppConfig migrated;
    assert(load_config(legacy_v1, migrated, error));
    assert(migrated.config_version == 3);
    assert(migrated.network_direct_install);
    assert(!migrated.verify_sha256);
    assert(!migrated.verify_nca_content_hash);
    assert(migrated.smb.server == "nas.local");
    std::remove(legacy_v1.c_str());

    AppConfig separate;
    separate.protocol = "smb";
    separate.smb.server = "smb.local";
    separate.smb.port = 1445;
    separate.smb.share = "Games";
    separate.smb.root = "/Switch";
    separate.smb.username = "smb-user";
    separate.smb.password = "smb-pass";
    separate.smb.remote_dir = "/Switch/NSP";
    separate.webdav.server = "dav.local";
    separate.webdav.port = 8443;
    separate.webdav.root = "/dav-root";
    separate.webdav.username = "dav-user";
    separate.webdav.password = "dav-pass";
    separate.webdav.remote_dir = "/dav-root/XCI";
    separate.webdav.tls = true;
    separate.webdav.tls_verify = false;
    activate_remote_profile(separate);

    assert(separate.server == "smb.local");
    assert(separate.share == "Games");
    assert(separate.remote_dir == "/Switch/NSP");
    set_active_protocol(separate, "webdav");
    assert(separate.server == "dav.local");
    assert(separate.share.empty());
    assert(separate.port == 8443);
    assert(separate.root == "/dav-root");
    assert(separate.username == "dav-user");
    assert(separate.password == "dav-pass");
    assert(separate.remote_dir == "/dav-root/XCI");
    assert(separate.webdav_tls);
    assert(!separate.tls_verify);
    set_active_protocol(separate, "smb");
    assert(separate.server == "smb.local");
    assert(separate.port == 1445);
    assert(separate.share == "Games");
    assert(separate.root == "/Switch");
    assert(separate.username == "smb-user");
    assert(separate.password == "smb-pass");
    assert(separate.remote_dir == "/Switch/NSP");

    const std::string v3 = "/tmp/AstraNAS-v11-profiles.ini";
    assert(save_config(v3, separate, error));
    AppConfig roundtrip;
    assert(load_config(v3, roundtrip, error));
    assert(roundtrip.config_version == 3);
    assert(roundtrip.protocol == "smb");
    assert(roundtrip.smb.server == "smb.local");
    assert(roundtrip.smb.share == "Games");
    assert(roundtrip.smb.remote_dir == "/Switch/NSP");
    assert(roundtrip.webdav.server == "dav.local");
    assert(roundtrip.webdav.port == 8443);
    assert(roundtrip.webdav.remote_dir == "/dav-root/XCI");
    assert(!roundtrip.webdav.tls_verify);
    std::remove(v3.c_str());
    return 0;
}
