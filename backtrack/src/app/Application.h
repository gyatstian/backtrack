#pragma once

#include "app/RecorderController.h"
#include "settings/SettingsStore.h"

#include <Windows.h>

#include <memory>

namespace backtrack {

class Application {
public:
    Application();
    int run(HINSTANCE instance, PWSTR commandLine, int showCommand);

private:
    SettingsStore settingsStore_;
    RecorderController controller_;
};

} // namespace backtrack
