#pragma once

#include "app/RecorderController.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "settings/SettingsStore.h"

namespace backtrack {

inline const wchar_t* publishWindowSettings(SettingsStore& store, RecorderController& controller,
    AppSettings& baseline, const AppSettings& candidate, bool readable) {
    if (!readable) return L"Settings could not be read at startup; restart after the file becomes readable";
    auto normalized = sanitizeSettings(candidate);
    if (!store.save(normalized)) return L"Settings could not be saved; try again";
    const bool startupChanged = baseline.startWithWindowsMinimized != normalized.startWithWindowsMinimized;
    baseline = std::move(normalized);
    if (startupChanged) updateWindowsStartupRegistration(baseline.startWithWindowsMinimized);
    Logger::instance().setEnabled(baseline.loggingEnabled);
    controller.setUiPreferences(baseline.libraryGalleryView, baseline.loggingEnabled);
    return nullptr;
}

} // namespace backtrack
