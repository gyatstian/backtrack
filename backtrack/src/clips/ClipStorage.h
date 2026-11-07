#pragma once

#include <Windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace backtrack::ClipStorage {

inline std::wstring gameFolderName(std::wstring name) {
    // Dot-prefixed names include the internal recovery and ring namespaces.
    if (!name.empty() && name.front() == L'.') {
        name.insert(name.begin(), L'_');
    }
    return name;
}

inline std::filesystem::path outputDirectory(
    const std::filesystem::path& root, const std::wstring& name, bool organizeByGame) {
    return organizeByGame && !name.empty() ? root / gameFolderName(name) : root;
}

inline DWORD tryReserveClipOutput(const std::filesystem::path& path) {
    for (const auto suffix : std::array{L".favorite", L".tags"}) {
        const auto sidecar = path.wstring() + suffix;
        if (GetFileAttributesW(sidecar.c_str()) != INVALID_FILE_ATTRIBUTES) return ERROR_FILE_EXISTS;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return error;
    }
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(file);
    return ERROR_SUCCESS;
}

inline std::filesystem::path reserveClipOutput(const std::filesystem::path& initial) {
    const auto stem = initial.stem().wstring();
    for (uint32_t suffix = 1; suffix < 1000; ++suffix) {
        const auto candidate = suffix == 1 ? initial : initial.parent_path() /
            (stem + L"_" + std::to_wstring(suffix) + initial.extension().wstring());
        const DWORD error = tryReserveClipOutput(candidate);
        if (error == ERROR_SUCCESS) return candidate;
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return {};
    }
    const auto candidate = initial.parent_path() /
        (stem + L"_" + std::to_wstring(GetTickCount64()) + initial.extension().wstring());
    return tryReserveClipOutput(candidate) == ERROR_SUCCESS ? candidate : std::filesystem::path{};
}

inline bool isDirectoryWithoutReparsePoint(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

// The library root, its game folders, and the old per-game kill-clip folders.
inline std::vector<std::filesystem::path> clipRoots(const std::filesystem::path& root, std::error_code& error,
                                                    const std::function<bool()>& cancelled = {}) {
    std::vector<std::filesystem::path> roots;
    error.clear();
    if (root.empty()) {
        return roots;
    }
    roots.push_back(root);
    std::filesystem::directory_iterator it(root, error), end;
    for (; !error && it != end; it.increment(error)) {
        if (cancelled && cancelled()) break;
        const auto child = it->path();
        if (!isDirectoryWithoutReparsePoint(child)) {
            continue;
        }
        roots.push_back(child);
        const auto legacy = child / L"clips";
        if (isDirectoryWithoutReparsePoint(legacy)) {
            roots.push_back(legacy);
        }
    }
    return roots;
}

} // namespace backtrack::ClipStorage
