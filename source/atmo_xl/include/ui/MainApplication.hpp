#pragma once
#include <initializer_list>
#include <string>

namespace inst::ui {
class MainApplication {
public:
    int CreateShowDialog(const std::string&, const std::string&, std::initializer_list<std::string>, bool);
};
extern MainApplication* mainApp;
}
