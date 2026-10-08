// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.hpp"
#include "download_cache.hpp"
#include "install_cleanup.hpp"
#include "installer.hpp"
#include "user_title_backend.hpp"
#include "library_match.hpp"
#include "local_fs.hpp"
#include "package_inspect.hpp"
#include "manifest.hpp"
#include "sha256.hpp"
#include "title_backend/install_session.hpp"
#include "title_backend/package_archive.hpp"
#include "title_backend/package_source.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <string>
#include <vector>


namespace {
struct MockBehavior {
    bool supported = true;
    bool begin = true;
    bool write = true;
    bool finalize = true;
    bool verify = true;
    bool skip = false;
    bool cancel_during_write = false;
};

struct MockTrace {
    int begin = 0;
    int write = 0;
    int finalize = 0;
    int verify = 0;
    int close = 0;
};

class MockBackendProvider final : public astranas::title_backend::BackendProvider {
public:
    MockBackendProvider(MockBehavior behavior, MockTrace& trace)
        : behavior_(behavior), trace_(trace) {}

    bool supports(PackageContainerKind) const override { return behavior_.supported; }
    bool begin_session(const PackageInspection&, std::string& error) override {
        ++trace_.begin;
        if (!behavior_.begin) error = "mock begin failure";
        return behavior_.begin;
    }
    bool write_content(astranas::title_backend::PackageSource& input,
                       const astranas::user_backend::ProgressCallback& progress,
                       std::atomic_bool& cancel, std::string& error) override {
        ++trace_.write;
        if (progress) progress({input.size() / 2, input.size(), "Mock write"});
        if (behavior_.cancel_during_write) {
            cancel.store(true);
            error = "mock cancellation";
            return false;
        }
        if (behavior_.skip) return false;
        if (!behavior_.write) error = "mock write failure";
        return behavior_.write;
    }
    bool finalize(std::string& error) override {
        ++trace_.finalize;
        if (!behavior_.finalize) error = "mock finalize failure";
        return behavior_.finalize;
    }
    bool verify(std::string& error) override {
        ++trace_.verify;
        if (!behavior_.verify) error = "mock verify failure";
        return behavior_.verify;
    }
    bool skipped() const override { return behavior_.skip; }
    void close() override { ++trace_.close; }

private:
    MockBehavior behavior_;
    MockTrace& trace_;
};

astranas::user_backend::Result run_mock_session(const std::string& path,
                                                const MockBehavior& behavior,
                                                MockTrace& trace,
                                                int& progressEvents,
                                                std::string& error) {
    auto provider = std::make_unique<MockBackendProvider>(behavior, trace);
    astranas::title_backend::InstallSession session(std::move(provider));
    std::atomic_bool cancel{false};
    return session.run(path, [&](const astranas::user_backend::Progress&) {
        ++progressEvents;
    }, cancel, error);
}

void put_le32(std::vector<unsigned char>& b, std::size_t off, std::uint32_t v) {
    b[off+0] = static_cast<unsigned char>(v);
    b[off+1] = static_cast<unsigned char>(v >> 8);
    b[off+2] = static_cast<unsigned char>(v >> 16);
    b[off+3] = static_cast<unsigned char>(v >> 24);
}

void put_le64(std::vector<unsigned char>& b, std::size_t off, std::uint64_t v) {
    for (unsigned int i = 0; i < 8; ++i)
        b[off + i] = static_cast<unsigned char>(v >> (i * 8));
}

void write_invalid_pfs0(const std::string& path, bool duplicate_names) {
    std::string strings = duplicate_names ? std::string("same.nca\0", 9)
                                          : std::string("one.nca\0two.nca\0", 16);
    const std::size_t header_size = 0x10 + 2 * 0x18 + strings.size();
    std::vector<unsigned char> bytes(header_size + 16, 0);
    bytes[0]='P'; bytes[1]='F'; bytes[2]='S'; bytes[3]='0';
    put_le32(bytes, 4, 2);
    put_le32(bytes, 8, static_cast<std::uint32_t>(strings.size()));
    put_le64(bytes, 0x10, 0);
    put_le64(bytes, 0x18, 12);
    put_le32(bytes, 0x20, 0);
    put_le64(bytes, 0x28, duplicate_names ? 12 : 4);
    put_le64(bytes, 0x30, 4);
    put_le32(bytes, 0x38, duplicate_names ? 0 : 8);
    std::copy(strings.begin(), strings.end(), bytes.begin() + 0x40);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_synthetic_pfs0(const std::string& path, bool compressed = false) {
    std::string strings = compressed ? "meta.cnmt.ncz" : "meta.cnmt.nca";
    strings.push_back('\0');
    const auto ticket_offset = strings.size();
    strings.append("ticket.tik");
    strings.push_back('\0');
    std::vector<unsigned char> bytes(0x10 + 2 * 0x18 + strings.size(), 0);
    bytes[0]='P'; bytes[1]='F'; bytes[2]='S'; bytes[3]='0';
    put_le32(bytes, 4, 2);
    put_le32(bytes, 8, static_cast<std::uint32_t>(strings.size()));
    put_le32(bytes, 0x10 + 16, 0);
    put_le32(bytes, 0x10 + 0x18 + 16, static_cast<std::uint32_t>(ticket_offset));
    std::copy(strings.begin(), strings.end(), bytes.begin() + 0x10 + 2 * 0x18);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void write_synthetic_xci(const std::string& path,
                         std::uint64_t declared_secure_size = 0,
                         std::size_t truncate_bytes = 0) {
    const std::string root_strings = std::string("secure\0", 7);
    const std::string secure_strings = std::string("bundle.cert\0", 12);
    const std::size_t base = 0xF000;
    const std::size_t root_data = base + 0x10 + 0x40 + root_strings.size();
    const std::size_t secure_size = 0x10 + 0x40 + secure_strings.size();
    std::vector<unsigned char> bytes(root_data + secure_size, 0);
    bytes[base+0]='H'; bytes[base+1]='F'; bytes[base+2]='S'; bytes[base+3]='0';
    put_le32(bytes, base + 4, 1);
    put_le32(bytes, base + 8, static_cast<std::uint32_t>(root_strings.size()));
    put_le32(bytes, base + 0x10 + 8, static_cast<std::uint32_t>(
        declared_secure_size ? declared_secure_size : secure_size));
    put_le32(bytes, base + 0x10 + 16, 0);
    std::copy(root_strings.begin(), root_strings.end(), bytes.begin() + base + 0x10 + 0x40);

    bytes[root_data+0]='H'; bytes[root_data+1]='F'; bytes[root_data+2]='S'; bytes[root_data+3]='0';
    put_le32(bytes, root_data + 4, 1);
    put_le32(bytes, root_data + 8, static_cast<std::uint32_t>(secure_strings.size()));
    put_le32(bytes, root_data + 0x10 + 16, 0);
    std::copy(secure_strings.begin(), secure_strings.end(), bytes.begin() + root_data + 0x10 + 0x40);
    if (truncate_bytes) bytes.resize(bytes.size() - truncate_bytes);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
}

int main() {
    std::string error;
#ifndef ASTRANAS_MINIMAL_HOST
    std::vector<RemoteItem> items;
    const std::string json = R"JSON({"items":[
      {"title_id":"0100AABBCCDDEEFF","name":"Alpha","version":131072,"size":99,"sha256":"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD","path":"Alpha [0100AABBCCDDEEFF][v131072].zip"},
      {"title_id":"0100111122223333","name":"Beta","version":65536,"path":"Beta [0100111122223333][v65536].zip"}
    ]})JSON";
    assert(parse_manifest_json(json, items, error));
    assert(items.size() == 2);
    assert(items[0].application_id == 0x0100AABBCCDDEEFFull);
    assert(items[0].version == 131072);
    assert(items[0].sha256 == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert(title_id_hex(items[0].application_id) == "0100AABBCCDDEEFF");

    std::vector<RemoteItem> edge_items;
    const std::string edge_json = R"JSON({"items":[
      {"title_id":"0100000000000001","name":"Negative","version":-1,"sha256":"not-a-hash","path":"neg.zip"},
      {"title_id":"0100000000000002","name":"Huge","version":4294967297,"path":"huge.zip"}
    ]})JSON";
    assert(parse_manifest_json(edge_json, edge_items, error));
    assert(edge_items.size() == 2);
    assert(edge_items[0].version == 0);
    assert(edge_items[0].sha256.empty());
    assert(edge_items[1].version == 0xFFFFFFFFu);

    RemoteItem filename_item;
    assert(parse_remote_filename("Gamma [0100999988887777][v42].zip", "/Gamma.zip", 123, filename_item));
    assert(filename_item.name == "Gamma");
    assert(filename_item.version == 42);
    assert(filename_item.size == 123);

    std::vector<InstalledTitle> installed = {
        {0x0100AABBCCDDEEFFull, 65536, "Alpha", true},
        {0x0100111122223333ull, 65536, "Beta", false},
        {0x0100999988887777ull, 84, "Gamma", false},
    };
    items.push_back(filename_item);
    compare_library(items, installed);
    assert(items[0].state == CompareState::RemoteNewer);
    assert(items[1].state == CompareState::Same);
    assert(items[2].state == CompareState::LocalNewer);

    RemoteItem missing;
    assert(parse_remote_filename("Delta [0100123412341234][v1].zip", "Delta.zip", 1, missing));
    std::vector<RemoteItem> one{missing};
    compare_library(one, installed);
    assert(one[0].state == CompareState::NotInstalled);

    const std::string config_path = "/tmp/AstraNAS-host-test.ini";
    {
        std::ofstream out(config_path);
        out << "url=webdav://nas.local/library\nroot=/packages\nusername=test\npassword=pw\nmanifest=library.json\nlocal_dir=/tmp/downloads\ncache_dir=/tmp/cache\ntls_verify=false\ndownload_retries=4\nverify_sha256=false\ninstall_target=nand\ndelete_source_after_install=false\nignore_required_firmware=true\nvalidate_nca=true\n";
    }
    AppConfig cfg;
    assert(load_config(config_path, cfg, error));
    assert(cfg.url == "webdav://nas.local/library");
    assert(cfg.root == "/packages");
    assert(cfg.username == "test");
    assert(cfg.local_dir == "/tmp/downloads");
    assert(!cfg.tls_verify);
    assert(cfg.download_retries == 4);
    assert(!cfg.verify_sha256);
    assert(cfg.install_to_nand);
    assert(!cfg.delete_source_after_install);
    assert(cfg.ignore_required_firmware);
    assert(cfg.validate_nca);
    assert(!cfg.allow_insecure_protocols);
    cfg.download_retries = 2;
    cfg.verify_sha256 = true;
    cfg.delete_source_after_install = true;
    assert(save_config(config_path, cfg, error));
    AppConfig cfg_roundtrip;
    assert(load_config(config_path, cfg_roundtrip, error));
    assert(cfg_roundtrip.download_retries == 2);
    assert(cfg_roundtrip.verify_sha256);
    assert(cfg_roundtrip.install_to_nand);
    assert(cfg_roundtrip.delete_source_after_install);
    assert(cfg_roundtrip.ignore_required_firmware);
    assert(cfg_roundtrip.validate_nca);
    assert(!cfg_roundtrip.allow_insecure_protocols);
    std::remove(config_path.c_str());
#endif

    AppConfig secure_defaults;
    assert(secure_defaults.validate_nca);
    assert(secure_defaults.verify_sha256);
    assert(!secure_defaults.install_to_nand);
    assert(secure_defaults.delete_source_after_install);
    assert(!secure_defaults.allow_insecure_protocols);

    using astranas::user_backend::InstalledRelation;
    using astranas::user_backend::PackageContentInfo;
    using astranas::user_backend::PackageInstallInfo;
    using astranas::user_backend::PreflightDecision;
    PackageInstallInfo batch_info;
    assert(astranas::user_backend::safe_batch_preflight_decision(batch_info) == PreflightDecision::Skip);
    batch_info.contents = {PackageContentInfo{}};
    batch_info.contents[0].relation = InstalledRelation::NotInstalled;
    assert(astranas::user_backend::safe_batch_preflight_decision(batch_info) == PreflightDecision::Install);
    batch_info.contents[0].relation = InstalledRelation::SameVersion;
    assert(astranas::user_backend::safe_batch_preflight_decision(batch_info) == PreflightDecision::Skip);
    batch_info.contents.push_back(PackageContentInfo{});
    batch_info.contents[1].relation = InstalledRelation::Upgrade;
    assert(astranas::user_backend::safe_batch_preflight_decision(batch_info) == PreflightDecision::Install);
    batch_info.contents.push_back(PackageContentInfo{});
    batch_info.contents[2].relation = InstalledRelation::Downgrade;
    assert(astranas::user_backend::safe_batch_preflight_decision(batch_info) == PreflightDecision::Skip);

    RemoteDirEntry cache_a;
    cache_a.name = "game.nsp";
    cache_a.path = "/one/game.nsp";
    cache_a.identity = "etag:\"one\"";
    cache_a.size = 123;
    RemoteDirEntry cache_b = cache_a;
    cache_b.path = "/two/game.nsp";
    cache_b.identity = "etag:\"two\"";
    assert(remote_object_key(secure_defaults, cache_a) != remote_object_key(secure_defaults, cache_b));
    assert(remote_cache_filename(secure_defaults, cache_a) != remote_cache_filename(secure_defaults, cache_b));

    const std::string metadata_path = "/tmp/AstraNAS-transfer-meta";
    const TransferMetadata metadata{remote_object_key(secure_defaults, cache_a),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"};
    assert(write_transfer_metadata(metadata_path, metadata, error));
    TransferMetadata metadata_roundtrip;
    assert(read_transfer_metadata(metadata_path, metadata_roundtrip));
    assert(metadata_roundtrip.object_key == metadata.object_key);
    assert(metadata_roundtrip.sha256 == metadata.sha256);
    std::remove(metadata_path.c_str());


    assert(detect_install_candidate("demo.nro") == InstallCandidateKind::HomebrewNro);
    assert(detect_install_candidate("game.NSP") == InstallCandidateKind::NintendoPackageNeedsBackend);
    assert(detect_install_candidate("game.ns0") == InstallCandidateKind::NintendoPackageNeedsBackend);
    assert(detect_install_candidate("game.ns1") == InstallCandidateKind::None);
    assert(detect_install_candidate("game.xci.00") == InstallCandidateKind::NintendoPackageNeedsBackend);
    assert(detect_install_candidate("game.xci.01") == InstallCandidateKind::None);
    assert(detect_install_candidate("readme.txt") == InstallCandidateKind::None);
    {
        std::atomic_bool cancel_requested{false};
        std::string backend_error;
        const auto backend_result = astranas::user_backend::install_title_package(
            "/tmp/game.nsp", {}, cancel_requested, backend_error);
        assert(backend_result == astranas::user_backend::Result::Failed);
        assert(backend_error.find("open") != std::string::npos);
    }
    assert(detect_package_container("game.NSZ") == PackageContainerKind::Nsz);

    const std::string pfs_path = "/tmp/Test [0100AABBCCDDEEFF][v131072].nsp";
    write_synthetic_pfs0(pfs_path);
    PackageInspection pfs_info;
    assert(inspect_package_file(pfs_path, pfs_info, error));
    assert(pfs_info.kind == PackageContainerKind::Nsp);
    assert(pfs_info.deep_inspected);
    assert(pfs_info.file_count == 2);
    assert(pfs_info.has_cnmt_nca);
    assert(pfs_info.has_ticket);
    assert(pfs_info.ticket_count == 1);
    assert(pfs_info.certificate_count == 0);
    assert(pfs_info.title_id_hint == "0100AABBCCDDEEFF");
    assert(pfs_info.version_hint == 131072);

    const std::string split_dir = "/tmp/Split.nsp";
    std::filesystem::remove_all(split_dir);
    std::filesystem::create_directories(split_dir);
    std::vector<char> pfs_bytes;
    {
        std::ifstream in(pfs_path, std::ios::binary);
        pfs_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const auto middle = pfs_bytes.size() / 2;
    { std::ofstream out(split_dir + "/00", std::ios::binary); out.write(pfs_bytes.data(), static_cast<std::streamsize>(middle)); }
    { std::ofstream out(split_dir + "/01", std::ios::binary); out.write(pfs_bytes.data() + middle, static_cast<std::streamsize>(pfs_bytes.size() - middle)); }
    PackageInspection split_info;
    assert(inspect_package_file(split_dir, split_info, error));
    assert(split_info.kind == PackageContainerKind::Nsp);
    assert(split_info.deep_inspected && split_info.file_count == 2);
    assert(split_info.summary.find("split") != std::string::npos);
    {
        astranas::title_backend::FilePackageSource split_source;
        assert(split_source.open(split_dir, error));
        assert(split_source.split());
        assert(split_source.size() == pfs_bytes.size());
        std::vector<char> roundtrip(pfs_bytes.size());
        std::size_t actual = 0;
        assert(split_source.read_at(0, roundtrip.data(), roundtrip.size(), actual, error));
        assert(actual == roundtrip.size() && roundtrip == pfs_bytes);
    }
    std::filesystem::remove_all(split_dir);

    const std::string numbered_base = "/tmp/SplitFile.nsp.";
    { std::ofstream out(numbered_base + "00", std::ios::binary); out.write(pfs_bytes.data(), static_cast<std::streamsize>(middle)); }
    { std::ofstream out(numbered_base + "01", std::ios::binary); out.write(pfs_bytes.data() + middle, static_cast<std::streamsize>(pfs_bytes.size() - middle)); }
    PackageInspection numbered_info;
    assert(inspect_package_file(numbered_base + "00", numbered_info, error));
    assert(numbered_info.kind == PackageContainerKind::Nsp);
    std::remove((numbered_base + "00").c_str());
    std::remove((numbered_base + "01").c_str());

    const std::string gapped_base = "/tmp/GappedFile.nsp.";
    { std::ofstream out(gapped_base + "00", std::ios::binary); out << "first"; }
    { std::ofstream out(gapped_base + "02", std::ios::binary); out << "orphan"; }
    {
        std::vector<std::string> parts;
        assert(!astranas::title_backend::resolve_package_parts(gapped_base + "00", parts, error));
        assert(error.find("missing") != std::string::npos);
    }
    std::remove((gapped_base + "00").c_str());
    std::remove((gapped_base + "02").c_str());

    const std::string cleanup_regular = "/tmp/Cleanup.nsp";
    { std::ofstream out(cleanup_regular, std::ios::binary); out.write(pfs_bytes.data(), static_cast<std::streamsize>(pfs_bytes.size())); }
    { std::ofstream out(cleanup_regular + ".astranas-meta"); out << "metadata"; }
    assert(remove_installed_package_source(cleanup_regular, error));
    assert(!std::filesystem::exists(cleanup_regular));
    assert(!std::filesystem::exists(cleanup_regular + ".astranas-meta"));

    const std::string cleanup_split = "/tmp/CleanupSplit.nsp.";
    { std::ofstream out(cleanup_split + "00", std::ios::binary); out.write(pfs_bytes.data(), static_cast<std::streamsize>(middle)); }
    { std::ofstream out(cleanup_split + "01", std::ios::binary); out.write(pfs_bytes.data() + middle, static_cast<std::streamsize>(pfs_bytes.size() - middle)); }
    assert(remove_installed_package_source(cleanup_split + "00", error));
    assert(!std::filesystem::exists(cleanup_split + "00"));
    assert(!std::filesystem::exists(cleanup_split + "01"));

    const std::string cleanup_directory = "/tmp/CleanupDirectory.nsp";
    std::filesystem::create_directories(cleanup_directory);
    { std::ofstream out(cleanup_directory + "/00", std::ios::binary); out.write(pfs_bytes.data(), static_cast<std::streamsize>(middle)); }
    { std::ofstream out(cleanup_directory + "/01", std::ios::binary); out.write(pfs_bytes.data() + middle, static_cast<std::streamsize>(pfs_bytes.size() - middle)); }
    assert(remove_installed_package_source(cleanup_directory, error));
    assert(!std::filesystem::exists(cleanup_directory));

    const std::string mixed_cleanup_directory = "/tmp/CleanupMixedDirectory.nsp";
    std::filesystem::create_directories(mixed_cleanup_directory);
    { std::ofstream out(mixed_cleanup_directory + "/00", std::ios::binary); out << "part0"; }
    { std::ofstream out(mixed_cleanup_directory + "/01", std::ios::binary); out << "part1"; }
    { std::ofstream out(mixed_cleanup_directory + "/notes.txt"); out << "must survive"; }
    assert(remove_installed_package_source(mixed_cleanup_directory, error));
    assert(std::filesystem::exists(mixed_cleanup_directory));
    assert(!std::filesystem::exists(mixed_cleanup_directory + "/00"));
    assert(!std::filesystem::exists(mixed_cleanup_directory + "/01"));
    assert(std::filesystem::exists(mixed_cleanup_directory + "/notes.txt"));
    std::filesystem::remove_all(mixed_cleanup_directory);

    {
        MockTrace trace;
        int progressEvents = 0;
        assert(run_mock_session(pfs_path, {}, trace, progressEvents, error) ==
               astranas::user_backend::Result::Success);
        assert(trace.begin == 1 && trace.write == 1 && trace.finalize == 1 &&
               trace.verify == 1 && trace.close == 1);
        assert(progressEvents >= 4);
    }
    {
        MockBehavior behavior;
        behavior.skip = true;
        MockTrace trace;
        int progressEvents = 0;
        assert(run_mock_session(pfs_path, behavior, trace, progressEvents, error) ==
               astranas::user_backend::Result::Skipped);
        assert(trace.begin == 1 && trace.write == 1 && trace.finalize == 0 &&
               trace.verify == 0 && trace.close == 1);
    }
    {
        MockBehavior behavior;
        behavior.begin = false;
        MockTrace trace;
        int progressEvents = 0;
        assert(run_mock_session(pfs_path, behavior, trace, progressEvents, error) ==
               astranas::user_backend::Result::Failed);
        assert(trace.begin == 1 && trace.write == 0 && trace.close == 1);
    }
    {
        MockBehavior behavior;
        behavior.write = false;
        MockTrace trace;
        int progressEvents = 0;
        assert(run_mock_session(pfs_path, behavior, trace, progressEvents, error) ==
               astranas::user_backend::Result::Failed);
        assert(trace.begin == 1 && trace.write == 1 && trace.finalize == 0 &&
               trace.close == 1);
    }
    {
        MockBehavior behavior;
        behavior.cancel_during_write = true;
        MockTrace trace;
        int progressEvents = 0;
        assert(run_mock_session(pfs_path, behavior, trace, progressEvents, error) ==
               astranas::user_backend::Result::Cancelled);
        assert(trace.begin == 1 && trace.write == 1 && trace.finalize == 0 &&
               trace.verify == 0 && trace.close == 1);
    }
    {
        MockBehavior behavior;
        behavior.finalize = false;
        MockTrace trace;
        int progressEvents = 0;
        assert(run_mock_session(pfs_path, behavior, trace, progressEvents, error) ==
               astranas::user_backend::Result::Failed);
        assert(trace.finalize == 1 && trace.verify == 0 && trace.close == 1);
    }
    {
        MockBehavior behavior;
        behavior.verify = false;
        MockTrace trace;
        int progressEvents = 0;
        assert(run_mock_session(pfs_path, behavior, trace, progressEvents, error) ==
               astranas::user_backend::Result::Failed);
        assert(trace.verify == 1 && trace.close == 1);
    }
    {
        MockBehavior behavior;
        behavior.supported = false;
        MockTrace trace;
        int progressEvents = 0;
        assert(run_mock_session(pfs_path, behavior, trace, progressEvents, error) ==
               astranas::user_backend::Result::Unsupported);
        assert(trace.begin == 0 && trace.close == 0);
    }
    std::remove(pfs_path.c_str());

    const std::string xci_path = "/tmp/Test.xci";
    write_synthetic_xci(xci_path);
    PackageInspection xci_info;
    assert(inspect_package_file(xci_path, xci_info, error));
    assert(xci_info.kind == PackageContainerKind::Xci);
    assert(xci_info.file_count == 1);
    assert(xci_info.has_cert);
    assert(xci_info.certificate_count == 1);
    {
        astranas::title_backend::FilePackageSource source;
        assert(source.open(xci_path, error));
        astranas::title_backend::PackageArchive archive;
        assert(archive.open(source, PackageContainerKind::Xci, error));
        assert(archive.suffix(".cert").size() == 1);
    }
    std::remove(xci_path.c_str());

    // An XCZ outer HFS0 may declare an aligned length without writing
    // the final padding; only the last root entry may use that allowance.
    const std::string xcz_path = "/tmp/Aligned.xcz";
    write_synthetic_xci(xcz_path, 0x200);
    PackageInspection xcz_info;
    assert(inspect_package_file(xcz_path, xcz_info, error));
    assert(xcz_info.kind == PackageContainerKind::Xcz);
    assert(xcz_info.file_count == 1);
    std::remove(xcz_path.c_str());

    // XCI is strict; overlong padding or truncated secure tables also fail.
    const std::string strict_xci_path = "/tmp/Padded.xci";
    write_synthetic_xci(strict_xci_path, 0x200);
    PackageInspection strict_info;
    assert(!inspect_package_file(strict_xci_path, strict_info, error));
    assert(error.find("end past EOF") != std::string::npos);
    std::remove(strict_xci_path.c_str());

    write_synthetic_xci(xcz_path, 0x400);
    assert(!inspect_package_file(xcz_path, xcz_info, error));
    assert(error.find("end past EOF") != std::string::npos);
    std::remove(xcz_path.c_str());

    write_synthetic_xci(xcz_path, 0x200, 1);
    assert(!inspect_package_file(xcz_path, xcz_info, error));
    assert(error.find("truncated") != std::string::npos);
    std::remove(xcz_path.c_str());

    const std::string nsz_path = "/tmp/game.nsz";
    write_synthetic_pfs0(nsz_path, true);
    PackageInspection nsz_info;
    assert(inspect_package_file(nsz_path, nsz_info, error));
    assert(nsz_info.kind == PackageContainerKind::Nsz);
    assert(nsz_info.deep_inspected);
    assert(nsz_info.has_cnmt_nca);
    assert(nsz_info.summary.find("compressed") != std::string::npos);
    {
        std::atomic_bool cancel_requested{false};
        std::string backend_error;
        const auto backend_result = astranas::user_backend::install_title_package(
            nsz_path, {}, cancel_requested, backend_error);
        assert(backend_result == astranas::user_backend::Result::Failed);
        assert(backend_error.find("Switch/libnx") != std::string::npos);
    }
    std::remove(nsz_path.c_str());

    for (const bool duplicate_names : {true, false}) {
        const std::string invalid_path = duplicate_names ? "/tmp/duplicate.nsp" : "/tmp/overlap.nsp";
        write_invalid_pfs0(invalid_path, duplicate_names);
        astranas::title_backend::FilePackageSource source;
        assert(source.open(invalid_path, error));
        astranas::title_backend::PackageArchive archive;
        assert(!archive.open(source, PackageContainerKind::Nsp, error));
        assert(error.find(duplicate_names ? "duplicate" : "overlapping") != std::string::npos);
        std::remove(invalid_path.c_str());
    }
    assert(local_path_is_within("/tmp/root", "/tmp/root/file.bin"));
    assert(!local_path_is_within("/tmp/root", "/tmp/root2/file.bin"));
    assert(!local_path_is_within("/tmp/root", "/tmp/root/../escape.bin"));
    assert(local_parent_within("/tmp/root", "/tmp/root/a/b") == "/tmp/root/a");

    const std::string fs_root = "/tmp/AstraNAS-fs-test";
    const std::string outside = "/tmp/AstraNAS-fs-outside";
    std::filesystem::remove_all(fs_root);
    std::filesystem::remove_all(outside);
    std::filesystem::create_directories(fs_root + "/child");
    std::filesystem::create_directories(outside);
    { std::ofstream out(outside + "/keep.txt"); out << "keep"; }
    assert(::symlink(outside.c_str(), (fs_root + "/child/link").c_str()) == 0);
    assert(!local_path_is_within(fs_root, fs_root + "/child/link/escaped.bin"));
    LocalEntry link_entry;
    link_entry.path = fs_root + "/child/link";
    assert(delete_local_entry(fs_root, link_entry, error));
    assert(std::filesystem::exists(outside + "/keep.txt"));

    const std::string copy_src = fs_root + "/source.bin";
    const std::string copy_dst = fs_root + "/dest.bin";
    { std::ofstream out(copy_src, std::ios::binary); out << "new-content"; }
    { std::ofstream out(copy_dst, std::ios::binary); out << "old-content"; }
    assert(copy_local_file(copy_src, copy_dst, error));
    { std::ifstream in(copy_dst, std::ios::binary); std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()); assert(content == "new-content"); }
    assert(!std::filesystem::exists(copy_dst + ".astranas-part"));
    assert(!copy_local_file(copy_src, fs_root + "/../escape.bin", error));
    std::filesystem::remove_all(fs_root);
    std::filesystem::remove_all(outside);

    const std::string empty;
    assert(sha256_bytes(empty.data(), empty.size()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string abc = "abc";
    assert(sha256_bytes(abc.data(), abc.size()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    AstraSha256Context incremental_hash;
    incremental_hash.update("a", 1);
    incremental_hash.update("bc", 2);
    assert(sha256_digest_hex(incremental_hash.final()) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string hash_path = "/tmp/AstraNAS-sha-test.bin";
    {
        std::ofstream out(hash_path, std::ios::binary);
        out << abc;
    }
    std::string file_hash;
    assert(sha256_file(hash_path, file_hash, error));
    assert(file_hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::remove(hash_path.c_str());

    std::puts("host tests: PASS");
    return 0;
}
