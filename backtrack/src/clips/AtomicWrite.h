#pragma once

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <system_error>

namespace backtrack {

inline std::error_code atomicWriteSibling(const std::filesystem::path& destination, std::string_view bytes) {
    std::error_code error;
    const auto target = std::filesystem::absolute(destination, error);
    if (error) return error;

    struct StagingFile {
        HANDLE handle = INVALID_HANDLE_VALUE;
        bool published = false;
        ~StagingFile() {
            if (handle != INVALID_HANDLE_VALUE) {
                if (!published) {
                    FILE_DISPOSITION_INFO disposition{TRUE};
                    SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
                }
                CloseHandle(handle);
            }
        }
    } staging;

    static std::atomic<unsigned long long> nextName{0};
    const auto targetName = target.filename().wstring();
    for (int attempt = 0; attempt < 128; ++attempt) {
        // An 8.3 name cannot acquire a different short-name alias for the target.
        const auto name = L"BT" + std::to_wstring(
            (GetCurrentProcessId() + nextName.fetch_add(1, std::memory_order_relaxed)) % 1000000) + L".TMP";
        if (CompareStringOrdinal(name.c_str(), -1, targetName.c_str(), -1, TRUE) == CSTR_EQUAL) {
            continue;
        }
        const auto candidate = target.parent_path() / name;
        staging.handle = CreateFileW(candidate.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (staging.handle != INVALID_HANDLE_VALUE) break;
        error = {static_cast<int>(GetLastError()), std::system_category()};
        if (error.value() != ERROR_FILE_EXISTS && error.value() != ERROR_ALREADY_EXISTS) return error;
    }
    if (staging.handle == INVALID_HANDLE_VALUE) {
        return error ? error : std::error_code(ERROR_FILE_EXISTS, std::system_category());
    }

    while (!bytes.empty()) {
        const DWORD requested = static_cast<DWORD>((std::min)(bytes.size(), static_cast<size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(staging.handle, bytes.data(), requested, &written, nullptr)) {
            return {static_cast<int>(GetLastError()), std::system_category()};
        }
        if (written == 0) return {ERROR_WRITE_FAULT, std::system_category()};
        bytes.remove_prefix(written);
    }

    const auto name = target.wstring();
    if (name.size() > (MAXDWORD - sizeof(FILE_RENAME_INFO)) / sizeof(wchar_t)) {
        return {ERROR_FILENAME_EXCED_RANGE, std::system_category()};
    }
    const size_t nameBytes = name.size() * sizeof(wchar_t);
    const size_t bufferSize = (std::max)(sizeof(FILE_RENAME_INFO),
        offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t));
    auto buffer = std::make_unique<std::byte[]>(bufferSize);
    auto* info = new (buffer.get()) FILE_RENAME_INFO{};
    info->ReplaceIfExists = TRUE;
    info->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(info->FileName, name.c_str(), nameBytes + sizeof(wchar_t));
    if (!SetFileInformationByHandle(staging.handle, FileRenameInfo, info, static_cast<DWORD>(bufferSize))) {
        return {static_cast<int>(GetLastError()), std::system_category()};
    }
    staging.published = true;
    return {};
}

} // namespace backtrack
