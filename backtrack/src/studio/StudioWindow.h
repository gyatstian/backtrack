#pragma once

#include "core/Types.h"
#include "settings/SettingsStore.h"

#include <functional>
#include <memory>

namespace backtrack {

class RecorderController;

class StudioWindow {
public:
    StudioWindow(RecorderController& controller, SettingsStore& store, const AppSettings& settings,
                 SettingsStore::LoadStatus loadStatus, std::function<void()> prepareCapture);
    ~StudioWindow();

    bool create(HINSTANCE instance, int showCommand, bool startMinimized);
    int runMessageLoop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace backtrack
