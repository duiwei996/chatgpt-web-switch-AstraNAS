// SPDX-License-Identifier: GPL-3.0-or-later
#include "package_inspect.hpp"
#include <cassert>
int main(){assert(detect_package_container("game.nsp")==PackageContainerKind::Nsp);assert(detect_package_container("game.nsz")==PackageContainerKind::Nsz);assert(detect_package_container("game.xci")==PackageContainerKind::Xci);assert(detect_package_container("homebrew.nro")==PackageContainerKind::HomebrewNro);return 0;}
