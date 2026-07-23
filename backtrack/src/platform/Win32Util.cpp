#include "platform/Win32Util.h"

#include "core/Logger.h"
#include "core/Types.h"

#include <avrt.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <chrono>
#include <cwctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace backtrack {

namespace {

constexpr wchar_t kRunPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kStartupApprovedRunPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kStartupValueName[] = L"Backtrack";
constexpr uint8_t kStartupApprovedEnabled[] = {
    0x02, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};

std::wstring win32StatusText(LSTATUS status) {
    return hresultToString(HRESULT_FROM_WIN32(static_cast<DWORD>(status)));
}

LSTATUS createCurrentUserKey(const wchar_t* path, REGSAM access, HKEY& key) {
    key = nullptr;
    LSTATUS status = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        path,
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        access | KEY_WOW64_64KEY,
        nullptr,
        &key,
        nullptr);
    if (status == ERROR_INVALID_PARAMETER) {
        status = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            path,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            access,
            nullptr,
            &key,
            nullptr);
    }
    return status;
}

bool setRegistryString(HKEY key, const wchar_t* valueName, const std::wstring& value) {
    const LSTATUS status = RegSetValueExW(
        key,
        valueName,
        0,
        REG_SZ,
        reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (status != ERROR_SUCCESS) {
        Logger::instance().warning(
            std::wstring(L"Could not write Windows startup registry value: ") + win32StatusText(status));
        return false;
    }
    return true;
}

bool setStartupApprovedEnabled() {
    HKEY key = nullptr;
    const LSTATUS createStatus = createCurrentUserKey(kStartupApprovedRunPath, KEY_SET_VALUE, key);
    if (createStatus != ERROR_SUCCESS) {
        Logger::instance().warning(
            std::wstring(L"Could not open Windows StartupApproved registry key: ") + win32StatusText(createStatus));
        return false;
    }

    const LSTATUS status = RegSetValueExW(
        key,
        kStartupValueName,
        0,
        REG_BINARY,
        reinterpret_cast<const BYTE*>(kStartupApprovedEnabled),
        static_cast<DWORD>(sizeof(kStartupApprovedEnabled)));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        Logger::instance().warning(
            std::wstring(L"Could not enable Backtrack in Windows Startup Apps: ") + win32StatusText(status));
        return false;
    }
    return true;
}

bool deleteRegistryValue(const wchar_t* keyPath, const wchar_t* valueName, const wchar_t* warningPrefix) {
    HKEY key = nullptr;
    const LSTATUS createStatus = createCurrentUserKey(keyPath, KEY_SET_VALUE, key);
    if (createStatus != ERROR_SUCCESS) {
        Logger::instance().warning(std::wstring(warningPrefix) + L": " + win32StatusText(createStatus));
        return false;
    }

    const LSTATUS status = RegDeleteValueW(key, valueName);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
        Logger::instance().warning(std::wstring(warningPrefix) + L": " + win32StatusText(status));
        return false;
    }
    return true;
}

std::wstring quotedStartupCommand() {
    const std::wstring path = moduleFilePath();
    if (path.empty()) {
        return {};
    }
    return L"\"" + path + L"\" " + kBacktrackStartupArgument;
}

std::wstring processImagePath(DWORD processId) {
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return {};
    }

    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    while (!QueryFullProcessImageNameW(process, 0, buffer.data(), &size)) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            CloseHandle(process);
            return {};
        }
        buffer.resize(buffer.size() * 2);
        size = static_cast<DWORD>(buffer.size());
    }
    CloseHandle(process);
    buffer.resize(size);
    return buffer;
}

std::wstring trimText(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring sanitizeFileNamePart(std::wstring value) {
    value = trimText(std::move(value));
    for (wchar_t& ch : value) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            ch = L'_';
        }
    }
    while (!value.empty() && (value.back() == L'.' || value.back() == L' ')) {
        value.pop_back();
    }
    return value;
}

} // namespace

std::filesystem::path localAppDataPath() {
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath))) {
        std::filesystem::path path(rawPath);
        CoTaskMemFree(rawPath);
        return path / L"Backtrack";
    }
    return std::filesystem::temp_directory_path() / L"Backtrack";
}

std::filesystem::path defaultClipDirectory() {
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_CREATE, nullptr, &rawPath))) {
        std::filesystem::path path(rawPath);
        CoTaskMemFree(rawPath);
        return path / L"Backtrack";
    }
    return localAppDataPath() / L"Clips";
}

std::wstring makeTimestampedFileName(const wchar_t* prefix, const wchar_t* extension) {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    std::wstringstream stream;
    stream << prefix << L"_" << std::put_time(&localTime, L"%ml%dl%Y_%Hl%M") << extension;
    return stream.str();
}

std::wstring foregroundApplicationName() {
    HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindow(foreground)) {
        return {};
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return {};
    }

    std::wstring name = std::filesystem::path(processImagePath(processId)).stem().wstring();
    if (name.empty()) {
        const int titleLength = GetWindowTextLengthW(foreground);
        if (titleLength > 0) {
            std::wstring title(static_cast<size_t>(titleLength + 1), L'\0');
            GetWindowTextW(foreground, title.data(), titleLength + 1);
            title.resize(static_cast<size_t>(titleLength));
            name = std::move(title);
        }
    }
    return sanitizeFileNamePart(std::move(name));
}

void setThreadDescriptionSafe(const wchar_t* description) {
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static auto fn = reinterpret_cast<SetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "SetThreadDescription"));
    if (fn) {
        fn(GetCurrentThread(), description);
    }
}

HANDLE enableMmcssForCaptureThread() {
    DWORD taskIndex = 0;
    HANDLE handle = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    if (handle) {
        AvSetMmThreadPriority(handle, AVRT_PRIORITY_HIGH);
    }
    return handle;
}

void disableMmcssForThread(HANDLE handle) {
    if (handle) {
        AvRevertMmThreadCharacteristics(handle);
    }
}

std::wstring moduleFilePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
        return {};
    }
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0) {
            return {};
        }
    }
    buffer.resize(size);
    return buffer;
}

std::wstring moduleDirectory() {
    return std::filesystem::path(moduleFilePath()).parent_path().wstring();
}

bool updateWindowsStartupRegistration(bool enabled) {
    if (enabled) {
        const std::wstring command = quotedStartupCommand();
        if (command.empty()) {
            Logger::instance().warning(L"Could not register Backtrack for Windows startup because the module path is unavailable");
            return false;
        }

        HKEY key = nullptr;
        const LSTATUS createStatus = createCurrentUserKey(kRunPath, KEY_SET_VALUE, key);
        if (createStatus != ERROR_SUCCESS) {
            Logger::instance().warning(
                std::wstring(L"Could not open current-user startup registry key: ") + win32StatusText(createStatus));
            return false;
        }

        const bool runValueSet = setRegistryString(key, kStartupValueName, command);
        RegCloseKey(key);
        const bool startupApproved = setStartupApprovedEnabled();
        if (runValueSet && startupApproved) {
            Logger::instance().info(L"Backtrack registered for Windows startup: " + command);
        }
        return runValueSet && startupApproved;
    }

    const bool runValueDeleted =
        deleteRegistryValue(kRunPath, kStartupValueName, L"Could not remove Backtrack from Windows startup");
    const bool startupApprovedDeleted =
        deleteRegistryValue(kStartupApprovedRunPath, kStartupValueName, L"Could not remove Backtrack from Windows Startup Apps");
    if (runValueDeleted && startupApprovedDeleted) {
        Logger::instance().info(L"Backtrack removed from Windows startup");
    }
    return runValueDeleted && startupApprovedDeleted;
}

HMONITOR monitorFromIndex(uint32_t index) {
    struct EnumState {
        uint32_t target = 0;
        uint32_t current = 0;
        HMONITOR monitor = nullptr;
    } state{index, 0, nullptr};

    EnumDisplayMonitors(
        nullptr,
        nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<EnumState*>(param);
            if (state->current == state->target) {
                state->monitor = monitor;
                return FALSE;
            }
            ++state->current;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&state));

    if (!state.monitor && index != 0) {
        return monitorFromIndex(0);
    }

    if (!state.monitor) {
        POINT origin{0, 0};
        return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    }
    return state.monitor;
}

uint32_t dxgiOutputIndexForMonitor(IDXGIAdapter* adapter, HMONITOR monitor) {
    if (!adapter || !monitor) {
        return UINT32_MAX;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIOutput> output;
        const HRESULT hr = adapter->EnumOutputs(index, &output);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(hr) || !output) {
            continue;
        }
        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc))) {
            continue;
        }
        if (desc.Monitor == monitor) {
            return index;
        }
    }
    return UINT32_MAX;
}

HMONITOR focusedMonitorOrFallback(uint32_t fallbackIndex) {
    HWND foreground = GetForegroundWindow();
    if (foreground && IsWindow(foreground)) {
        HMONITOR monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONULL);
        if (monitor) {
            return monitor;
        }
    }

    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL);
        if (monitor) {
            return monitor;
        }
    }

    return monitorFromIndex(fallbackIndex);
}

HMONITOR cursorMonitorOrFallback(uint32_t fallbackIndex) {
    POINT cursor{};
    if (GetCursorPos(&cursor)) {
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL);
        if (monitor) {
            return monitor;
        }
    }

    return monitorFromIndex(fallbackIndex);
}

UINT backtrackActivationMessage() {
    static const UINT message = RegisterWindowMessageW(L"Backtrack.ActivateExistingInstance");
    return message;
}

} // namespace backtrack
