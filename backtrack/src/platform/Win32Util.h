#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace backtrack {

inline constexpr wchar_t kBacktrackMainWindowClassName[] = L"BacktrackMainWindow";
inline constexpr wchar_t kBacktrackSingleInstanceMutexName[] = L"Local\\Backtrack.SingleInstance";
inline constexpr wchar_t kBacktrackStartupArgument[] = L"--startup";

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
HMONITOR monitorFromIndex(uint32_t index);
HMONITOR focusedMonitorOrFallback(uint32_t fallbackIndex);
HMONITOR cursorMonitorOrFallback(uint32_t fallbackIndex);
UINT backtrackActivationMessage();

} // namespace backtrack
