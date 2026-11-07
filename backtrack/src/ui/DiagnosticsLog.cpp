#include "ui/DiagnosticsLog.h"

#include "core/Logger.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace backtrack {
namespace {

struct FileHandle {
    FileHandle() = default;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() {
        if (value != INVALID_HANDLE_VALUE) {
            CloseHandle(value);
        }
    }
};

// the exclusive handle prevents a substituted path from being published or deleted.
struct StagedFile {
    FileHandle file;
    bool published = false;
    ~StagedFile() {
        if (!published && file.value != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};

std::error_code lastError() {
    return {static_cast<int>(GetLastError()), std::system_category()};
}

std::error_code writeAll(HANDLE file, const char* data, size_t size) {
    while (size != 0) {
        const DWORD requested = static_cast<DWORD>((std::min)(size, static_cast<size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(file, data, requested, &written, nullptr)) {
            return lastError();
        }
        if (written == 0) {
            return {ERROR_WRITE_FAULT, std::system_category()};
        }
        data += written;
        size -= written;
    }
    return {};
}

std::error_code publish(HANDLE file, const std::filesystem::path& destination) {
    const std::wstring name = destination.wstring();
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
    if (!SetFileInformationByHandle(file, FileRenameInfo, info, static_cast<DWORD>(bufferSize))) {
        return lastError();
    }
    return {};
}

} // namespace

std::error_code saveDiagnosticsLog(const std::filesystem::path& destination, std::string_view tail) {
    auto& logger = Logger::instance();
    const auto source = logger.currentPath();
    if (source.empty()) {
        return {ERROR_FILE_NOT_FOUND, std::system_category()};
    }

    std::error_code error;
    const auto target = std::filesystem::absolute(destination, error);
    if (error) {
        return error;
    }
    if (std::filesystem::equivalent(source, target, error)) {
        return {ERROR_SHARING_VIOLATION, std::system_category()};
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        return error;
    }

    static std::atomic<unsigned long long> nextName{0};
    const auto targetName = target.filename().wstring();
    StagedFile staging;
    for (int attempt = 0; attempt < 128; ++attempt) {
        // an 8.3 name cannot acquire a different short-name alias for the target.
        const auto name = L"BT" + std::to_wstring(
            (GetCurrentProcessId() + nextName.fetch_add(1, std::memory_order_relaxed)) % 1000000) + L".TMP";
        if (CompareStringOrdinal(name.c_str(), -1, targetName.c_str(), -1, TRUE) == CSTR_EQUAL) {
            continue;
        }
        const auto candidate = target.parent_path() / name;
        staging.file.value = CreateFileW(candidate.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (staging.file.value != INVALID_HANDLE_VALUE) {
            break;
        }
        error = lastError();
        if (error.value() != ERROR_FILE_EXISTS && error.value() != ERROR_ALREADY_EXISTS) {
            return error;
        }
    }
    if (staging.file.value == INVALID_HANDLE_VALUE) {
        return error;
    }

    if (!logger.flush()) {
        return {ERROR_WRITE_FAULT, std::system_category()};
    }
    FileHandle input;
    // deny deletion while copying so an alias cannot replace the live file.
    input.value = CreateFileW(source.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input.value == INVALID_HANDLE_VALUE) {
        return lastError();
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(input.value, &size)) {
        return lastError();
    }
    if (size.QuadPart < 0) {
        return {ERROR_INVALID_DATA, std::system_category()};
    }
    auto remaining = static_cast<ULONGLONG>(size.QuadPart);
    std::array<char, 64 * 1024> buffer{};
    while (remaining != 0) {
        const DWORD requested = static_cast<DWORD>((std::min)(remaining, static_cast<ULONGLONG>(buffer.size())));
        DWORD read = 0;
        if (!ReadFile(input.value, buffer.data(), requested, &read, nullptr)) {
            return lastError();
        }
        if (read == 0) {
            return {ERROR_HANDLE_EOF, std::system_category()};
        }
        if (error = writeAll(staging.file.value, buffer.data(), read); error) {
            return error;
        }
        remaining -= read;
    }

    // the diagnostic tail remains best-effort, like the former post-copy append.
    (void)writeAll(staging.file.value, tail.data(), tail.size());
    if (error = publish(staging.file.value, target); error) {
        return error;
    }
    staging.published = true;
    return {};
}

} // namespace backtrack
