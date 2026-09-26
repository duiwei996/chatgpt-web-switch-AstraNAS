// AstraNAS integration bridge for the unmodified AtmoXL installation sources.
#include <string>
#include <vector>
#include <utility>
#include <filesystem>
#include <atomic>
#include <switch.h>

#include "bridge/install_hooks.hpp"
#include "ui/MainApplication.hpp"
#include "ui/instPage.hpp"
#include "util/config.hpp"
#include "util/error.hpp"
#include "util/lang.hpp"
#include "util/util.hpp"
#include "nx/ipc/tin_ipc.h"

namespace inst::ui {
MainApplication* mainApp = nullptr;
int MainApplication::CreateShowDialog(const std::string&, const std::string&, std::initializer_list<std::string>, bool) { return 1; }
void instPage::setTopInstInfoText(std::string) {}
void instPage::setInstInfoText(std::string) {}
void instPage::setInstBarPerc(double) {}
void instPage::loadMainMenu() {}
void instPage::loadInstallScreen() {}
}

namespace inst::config {
std::string gAuthKey;
std::string lastNetUrl;
std::string httpIndexUrl;
std::string themeColorTextTopInfo;
std::string themeColorTextBottomInfo;
std::string themeColorTextMenu;
std::string themeColorTextFile;
std::string themeColorTextDir;
std::string themeColorTextInstall;
std::vector<std::string> updateInfo;
int languageSetting = 99;
int themeMenuFontSize = 84;
bool ignoreReqVers = false;
bool validateNCAs = true;
bool verifyNcaContentHashes = false;
bool overClock = false;
bool deletePrompt = false;
bool enableSound = false;
bool enableLightning = false;
bool autoUpdate = false;
bool usbAck = false;
}

namespace inst::util {
void initInstallServices() {
    ASSERT_OK(ncmInitialize(), "Failed to initialize NCM");
    bool nsextReady = false;
    bool esReady = false;
    bool splCryptoReady = false;
    bool splReady = false;
    try {
        ASSERT_OK(nsextInitialize(), "Failed to initialize NS extension service");
        nsextReady = true;
        ASSERT_OK(esInitialize(), "Failed to initialize ES");
        esReady = true;
        ASSERT_OK(splCryptoInitialize(), "Failed to initialize SPL crypto");
        splCryptoReady = true;
        ASSERT_OK(splInitialize(), "Failed to initialize SPL");
        splReady = true;
    } catch (...) {
        if (splReady) splExit();
        if (splCryptoReady) splCryptoExit();
        if (esReady) esExit();
        if (nsextReady) nsextExit();
        ncmExit();
        throw;
    }
}
void deinitInstallServices() {
    splExit();
    splCryptoExit();
    esExit();
    nsextExit();
    ncmExit();
}
void playAudio(std::string) {}
void lightningStart() {}
void lightningStop() {}
}

namespace astranas::atmo_xl_bridge {
namespace {
ProgressHook g_progress = nullptr;
InvalidNcaHook g_invalid_nca = nullptr;
void* g_context = nullptr;
std::atomic_bool* g_cancel = nullptr;
}

void set_install_hooks(ProgressHook progress, InvalidNcaHook invalid_nca,
                       void* context, std::atomic_bool* cancel) {
    g_progress = progress;
    g_invalid_nca = invalid_nca;
    g_context = context;
    g_cancel = cancel;
}

void clear_install_hooks() {
    g_progress = nullptr;
    g_invalid_nca = nullptr;
    g_context = nullptr;
    g_cancel = nullptr;
}

bool request_disable_nca_validation(const char* content_id) {
    if (install_cancel_requested()) return false;
    return g_invalid_nca && g_invalid_nca(content_id ? content_id : "", g_context);
}

bool install_cancel_requested() {
    return g_cancel && g_cancel->load();
}

bool report_install_progress(const char* stage, std::uint64_t current, std::uint64_t total) {
    if (install_cancel_requested()) return false;
    if (g_progress && !g_progress(stage, current, total, g_context)) {
        if (g_cancel) g_cancel->store(true);
        return false;
    }
    return !install_cancel_requested();
}
} // namespace astranas::atmo_xl_bridge

namespace Language {
void Load() {}
std::string LanguageEntry(std::string key) { return key; }
std::string GetRandomMsg() { return {}; }
}
