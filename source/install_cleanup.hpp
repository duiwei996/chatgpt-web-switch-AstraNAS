// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

// Removes exactly one successfully installed package source. Split files are
// resolved first and every part must remain in the selected file's directory.
// A split-package directory is removed as one bounded tree.
bool remove_installed_package_source(const std::string& path, std::string& error);
