#include "app/Application.h"

#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "ui/MainWindow.h"

#include <objbase.h>
#include <ole2.h>
#include <shellapi.h>

#include <string>

namespace backtrack {

namespace {

bool hasCommandLineArgument(PWSTR commandLine, const wchar_t* argument) {
    if (!commandLine || !argument) {
        return false;
    }

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine, &argc);
    if (!argv) {
        return std::wstring(commandLine).find(argument) != std::wstring::npos;
    }

    bool found = false;
    for (int index = 0; index < argc; ++index) {
        if (std::wstring(argv[index]) == argument) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

bool requestExistingInstanceActivation() {
    const UINT activationMessage = backtrackActivationMessage();
    for (int attempt = 0; attempt < 40; ++attempt) {
        HWND window = FindWindowW(kBacktrackMainWindowClassName, nullptr);
        if (window) {
            DWORD processId = 0;
            GetWindowThreadProcessId(window, &processId);
            if (processId != 0) {
                AllowSetForegroundWindow(processId);
            }

            if (activationMessage != 0) {
                PostMessageW(window, activationMessage, 0, 0);
            } else {
                ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOWNORMAL);
                SetForegroundWindow(window);
            }
            return true;
        }
        Sleep(50);
    }
    return false;
}

} // namespace

Application::Application()
    : settingsStore_(localAppDataPath() / L"settings.ini") {
}

int Application::run(HINSTANCE instance, PWSTR commandLine, int showCommand) {
    auto settings = settingsStore_.load();
    settingsStore_.save(settings);

    HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, kBacktrackSingleInstanceMutexName);
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        requestExistingInstanceActivation();
        CloseHandle(instanceMutex);
        return 0;
    }

    Logger::instance().initialize(localAppDataPath() / L"logs" / L"backtrack.log");
    Logger::instance().setMinLevel(settings.logLevel);
    Logger::instance().info(L"Backtrack starting");
    if (!instanceMutex) {
        Logger::instance().warning(L"Single-instance mutex could not be created; continuing without duplicate launch protection");
    }
    Logger::instance().info(L"app", std::wstring(L"Session config: logLevel=") + logLevelName(settings.logLevel) +
                                     L", captureBackend=" + captureBackendName(settings.preferredCaptureBackend) +
                                     L", replay=" + (settings.replay.enabled ? L"enabled" : L"disabled") +
                                     L", resolution=" + std::to_wstring(settings.video.width) + L"x" +
                                     std::to_wstring(settings.video.height) + L"@" + std::to_wstring(settings.video.fps));
    updateWindowsStartupRegistration(settings.startWithWindowsMinimized);

    if (settings.pruneStaleMicrophoneConsentEntries) {
        pruneStaleMicrophoneConsentEntries();
    }

    const HRESULT com = OleInitialize(nullptr);

    controller_.initialize(settings);
    const auto capabilities = controller_.encoderCapabilities();
    Logger::instance().info(
        L"app",
        L"Environment: Windows=" + std::to_wstring(GetVersion()) +
            L", adapter=" + (capabilities.adapterName.empty() ? L"unknown" : capabilities.adapterName) +
            L", encoder=" + (capabilities.backendName.empty() ? L"unknown" : capabilities.backendName) +
            L", encoderAvailable=" + (capabilities.available ? L"yes" : L"no") +
            L", H264=" + (capabilities.h264 ? L"yes" : L"no") +
            L", HEVC=" + (capabilities.hevc ? L"yes" : L"no"));

    MainWindow window(controller_, settingsStore_);
    const bool startMinimized = settings.startWithWindowsMinimized &&
                                hasCommandLineArgument(commandLine, kBacktrackStartupArgument);
    if (!window.create(instance, showCommand, startMinimized)) {
        Logger::instance().error(L"Main window creation failed");
        if (SUCCEEDED(com)) {
            OleUninitialize();
        }
        if (instanceMutex) {
            ReleaseMutex(instanceMutex);
            CloseHandle(instanceMutex);
        }
        return 1;
    }

    const int result = window.runMessageLoop();
    controller_.shutdown();
    Logger::instance().info(L"Backtrack exiting");

    if (SUCCEEDED(com)) {
        OleUninitialize();
    }
    if (instanceMutex) {
        ReleaseMutex(instanceMutex);
        CloseHandle(instanceMutex);
    }
    return result;
}

} // namespace backtrack
