// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote_client.hpp"
#include "smb_client.hpp"
#include "curl_client.hpp"

std::string join_remote_path(const std::string& root,const std::string& child){if(root.empty()||root=="/")return child.empty()?std::string("/"):std::string("/")+(child.front()=='/'?child.substr(1):child);if(child.empty())return root;const bool a=root.back()=='/',b=child.front()=='/';if(a&&b)return root+child.substr(1);if(!a&&!b)return root+"/"+child;return root+child;}
std::unique_ptr<RemoteClient> make_remote_client(const AppConfig& config,std::string& error){if(config.protocol=="smb")return std::make_unique<SmbRemoteClient>();if(config.protocol=="webdav")return std::make_unique<CurlRemoteClient>();error="unsupported protocol; choose SMB or WebDAV";return nullptr;}
