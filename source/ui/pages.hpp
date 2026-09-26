// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifdef __SWITCH__
#include "gui.hpp"
#include "../config.hpp"
#include "../installed/catalog.hpp"
#include "../local_fs.hpp"
#include "../model.hpp"
#include "../app/remote_install_queue.hpp"
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace astranas::ui {
enum class Tab { Remote=0, Local=1, Cache=2, Installed=3, Settings=4 };
inline constexpr std::size_t kSettingsCount=15;
const char* tab_text(Tab tab);
Tab cycle_tab(Tab tab,int delta,bool show_cache=true);
void draw_remote(Gui& gui,const std::vector<RemoteDirEntry>& entries,std::size_t selected,const std::string& current,const std::vector<astranas::app::RemoteInstallQueueEntry>& queue,const std::string& protocol,const std::string& status,bool show_cache=true);
void draw_local(Gui& gui,const std::vector<LocalEntry>& entries,std::size_t selected,const std::string& current,const std::vector<std::string>& selection,const std::string& location,const std::string& protocol,const std::string& status,bool show_cache=true);
void draw_batch_queue(Gui& gui,const std::string& title,const std::vector<std::string>& items,std::size_t selected);
void draw_cache(Gui& gui,const std::vector<LocalEntry>& entries,std::size_t selected,const std::string& protocol,const std::string& status,bool show_cache=true);
void draw_installed(Gui& gui,const std::vector<astranas::installed::TitleEntry>& entries,std::size_t selected,const std::map<std::uint64_t,astranas::installed::IconBitmap>& icons,const std::string& protocol,const std::string& status,bool show_cache=true);
void draw_settings(Gui& gui,const AppConfig& config,std::size_t selected,const std::string& protocol,const std::string& status,double speed,bool show_cache=true);
const char* setting_label(std::size_t index);
} // namespace astranas::ui
#endif
