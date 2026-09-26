#pragma once
#include <string>

namespace inst::ui {
class instPage {
public:
    static void setTopInstInfoText(std::string);
    static void setInstInfoText(std::string);
    static void setInstBarPerc(double);
    static void loadMainMenu();
    static void loadInstallScreen();
};
}
