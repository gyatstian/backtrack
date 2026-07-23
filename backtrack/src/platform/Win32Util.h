#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace backtrack {

inline constexpr wchar_t kBacktrackMainWindowClassName[] = L"BacktrackMainWindow";
inline constexpr wchar_t kBacktrackSingleInstanceMutexName[] = L"Local\\Backtrack.SingleInstance";
inline constexpr wchar_t kBacktrackStartupArgument[] = L"--startup";

struct DxgiOutputLocation {
    uint32_t adapterIndex = UINT32_MAX;
    uint32_t outputIndex = UINT32_MAX;

    bool valid() const {
        return adapterIndex != UINT32_MAX && outputIndex != UINT32_MAX;
    }
};

std::filesystem::path localAppDataPath();
std::filesystem::path defaultClipDirectory();
std::wstring makeTimestampedFileName(const wchar_t* prefix, const wchar_t* extension);
std::wstring foregroundApplicationName();
void setThreadDescriptionSafe(const wchar_t* description);
HANDLE enableMmcssForCaptureThread();
void disableMmcssForThread(HANDLE handle);
std::wstring moduleFilePath();
std::wstring moduleDirectory();
bool updateWindowsStartupRegistration(bool enabled);
void pruneStaleMicrophoneConsentEntries();
HMONITOR monitorFromIndex(uint32_t index);
HMONITOR focusedMonitorOrFallback(uint32_t fallbackIndex);
HMONITOR cursorMonitorOrFallback(uint32_t fallbackIndex);
// Returns DXGI output index on the given adapter for HMONITOR, or UINT32_MAX if none.
uint32_t dxgiOutputIndexForMonitor(IDXGIAdapter* adapter, HMONITOR monitor);
// Finds monitor's output across every DXGI adapter. Desktop Duplication needs a
// D3D device created on this adapter.
DxgiOutputLocation dxgiOutputForMonitor(HMONITOR monitor);
// Current encoder factory supports these adapter vendors only.
bool dxgiAdapterSupportsHardwareEncode(uint32_t adapterIndex);
// Returns preferred adapter when it can encode, otherwise first supported one.
uint32_t dxgiHardwareEncoderAdapterOr(uint32_t preferredAdapterIndex);
UINT backtrackActivationMessage();

} // namespace backtrack
