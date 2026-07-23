#include "clips/ClipManager.h"

#include "core/Logger.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <propsys.h>
#include <shobjidl.h>
#include <wrl/client.h>

namespace backtrack {

namespace {

constexpr PROPERTYKEY kPkeyMediaDuration = {
    {0x64440490, 0x4c8b, 0x11d1, {0x8b, 0x70, 0x08, 0x00, 0x36, 0xb1, 0x1a, 0x03}},
    3};

uint64_t readMediaDuration(const std::filesystem::path& path) {
    Microsoft::WRL::ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
        return 0;
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    uint64_t duration = 0;
    if (SUCCEEDED(store->GetValue(kPkeyMediaDuration, &value))) {
        if (value.vt == VT_UI8) {
            duration = value.uhVal.QuadPart;
        } else if (value.vt == VT_I8) {
            duration = static_cast<uint64_t>(value.hVal.QuadPart);
        }
    }
    PropVariantClear(&value);
    return duration;
}

bool hasMp4Extension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return extension == L".mp4";
}

bool isValidClipStem(const std::wstring& value) {
    if (value.empty() || value.find_first_not_of(L" \t\r\n") == std::wstring::npos) {
        return false;
    }
    if (value.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        return false;
    }
    if (value.back() == L'.' || iswspace(value.back())) {
        return false;
    }

    std::wstring upper = value;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towupper(ch));
    });
    const auto dot = upper.find(L'.');
    if (dot != std::wstring::npos) {
        upper.resize(dot);
    }

    return upper != L"CON" &&
           upper != L"PRN" &&
           upper != L"AUX" &&
           upper != L"NUL" &&
           !(upper.size() == 4 && upper.starts_with(L"COM") && upper[3] >= L'1' && upper[3] <= L'9') &&
           !(upper.size() == 4 && upper.starts_with(L"LPT") && upper[3] >= L'1' && upper[3] <= L'9');
}

std::wstring trimStem(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring normalizeTag(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && iswspace(value.back())) {
        value.pop_back();
    }
    value.erase(std::remove_if(value.begin(), value.end(), [](wchar_t ch) {
        return ch == L'\r' || ch == L'\n' || ch == L'\t' || (ch >= 0 && ch < 0x20);
    }), value.end());
    if (value.size() > 64) {
        value.resize(64);
        while (!value.empty() && iswspace(value.back())) {
            value.pop_back();
        }
    }
    return value;
}

std::wstring normalizedPathText(const std::filesystem::path& path) {
    std::wstring value = path.native();
    std::replace(value.begin(), value.end(), L'/', L'\\');
    while (!value.empty() && value.back() == L'\\') {
        value.pop_back();
    }
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(towlower(ch));
    });
    return value;
}

bool isPathInsideDirectory(const std::filesystem::path& path, const std::filesystem::path& directory) {
    const auto pathText = normalizedPathText(path);
    const auto directoryText = normalizedPathText(directory);
    if (pathText.empty() || directoryText.empty() || pathText == directoryText) {
        return false;
    }
    return pathText.size() > directoryText.size() &&
           pathText.starts_with(directoryText) &&
           pathText[directoryText.size()] == L'\\';
}

} // namespace

ClipManager::ClipManager(std::filesystem::path directory)
    : directory_(std::move(directory)) {
}

void ClipManager::setDirectory(std::filesystem::path directory) {
    directory_ = std::move(directory);
}

std::vector<ClipInfo> ClipManager::listClips() const {
    std::vector<ClipInfo> clips;
    std::error_code error;
    if (directory_.empty() || !std::filesystem::exists(directory_, error)) {
        if (error) {
            Logger::instance().warning(L"clips", L"Could not access clips directory: " + directory_.wstring() + L" (" + utf8ToWide(error.message()) + L")");
        }
        return clips;
    }

    std::filesystem::directory_iterator iterator(directory_, error);
    const std::filesystem::directory_iterator end;
    if (error) {
        Logger::instance().warning(L"clips", L"Could not enumerate clips directory: " + directory_.wstring() + L" (" + utf8ToWide(error.message()) + L")");
        return clips;
    }
    for (; iterator != end; iterator.increment(error)) {
        const auto& entry = *iterator;
        if (!entry.is_regular_file(error) || error || !hasMp4Extension(entry.path())) {
            error = {};
            continue;
        }
        if (!isManagedClipPath(entry.path())) {
            error = {};
            continue;
        }
        ClipInfo info;
        info.path = entry.path();
        info.bytes = entry.file_size(error);
        if (error) {
            Logger::instance().warning(L"clips", L"Could not read clip file size: " + entry.path().wstring() + L" (" + utf8ToWide(error.message()) + L")");
            error = {};
            continue;
        }
        info.modifiedTime = entry.last_write_time(error);
        if (error) {
            Logger::instance().warning(L"clips", L"Could not read clip modified time: " + entry.path().wstring() + L" (" + utf8ToWide(error.message()) + L")");
            error = {};
            continue;
        }
        info.duration100ns = readMediaDuration(entry.path());
        info.favorite = std::filesystem::exists(favoriteMarker(entry.path()), error);
        error = {};
        info.tags = readTags(entry.path());
        clips.push_back(std::move(info));
    }
    if (error) {
        Logger::instance().warning(L"clips", L"Could not finish enumerating clips directory: " + directory_.wstring() + L" (" + utf8ToWide(error.message()) + L")");
    }

    std::sort(clips.begin(), clips.end(), [](const ClipInfo& left, const ClipInfo& right) {
        if (left.modifiedTime != right.modifiedTime) {
            return left.modifiedTime > right.modifiedTime;
        }
        return left.path.filename().wstring() > right.path.filename().wstring();
    });
    return clips;
}

bool ClipManager::removeClip(const std::filesystem::path& path) const {
    if (!isManagedClipPath(path)) {
        Logger::instance().warning(L"clips", L"Rejected delete outside the clip directory: " + path.wstring());
        return false;
    }

    std::error_code error;
    std::filesystem::remove(favoriteMarker(path), error);
    error = {};
    std::filesystem::remove(tagFile(path), error);
    error = {};
    const bool removed = std::filesystem::remove(path, error);
    if (!removed || error) {
        Logger::instance().warning(L"clips", L"Could not delete clip: " + path.wstring() + (error ? L" (" + utf8ToWide(error.message()) + L")" : L""));
    }
    return removed && !error;
}

bool ClipManager::renameClip(const std::filesystem::path& path, const std::wstring& newStem) const {
    if (!isManagedClipPath(path)) {
        Logger::instance().warning(L"clips", L"Rejected rename outside the clip directory: " + path.wstring());
        return false;
    }

    const std::wstring trimmedStem = trimStem(newStem);
    if (!isValidClipStem(trimmedStem)) {
        return false;
    }
    auto target = path.parent_path() / (trimmedStem + path.extension().wstring());
    std::error_code error;
    if (std::filesystem::exists(target, error)) {
        Logger::instance().warning(L"clips", L"Could not rename clip because the target already exists: " + target.wstring());
        return false;
    }
    std::filesystem::rename(path, target, error);
    if (error) {
        Logger::instance().warning(L"clips", L"Could not rename clip: " + path.wstring() + L" (" + utf8ToWide(error.message()) + L")");
        return false;
    }

    const auto oldMarker = favoriteMarker(path);
    if (std::filesystem::exists(oldMarker)) {
        std::filesystem::rename(oldMarker, favoriteMarker(target), error);
        if (error) {
            Logger::instance().warning(L"clips", L"Could not rename favorite marker for clip: " + path.wstring() + L" (" + utf8ToWide(error.message()) + L")");
        }
    }
    error = {};
    const auto oldTags = tagFile(path);
    if (std::filesystem::exists(oldTags)) {
        std::filesystem::rename(oldTags, tagFile(target), error);
        if (error) {
            Logger::instance().warning(L"clips", L"Could not rename tag file for clip: " + path.wstring() + L" (" + utf8ToWide(error.message()) + L")");
        }
    }
    return true;
}

bool ClipManager::setFavorite(const std::filesystem::path& path, bool favorite) const {
    if (!isManagedClipPath(path)) {
        Logger::instance().warning(L"clips", L"Rejected favorite marker outside the clip directory: " + path.wstring());
        return false;
    }

    const auto marker = favoriteMarker(path);
    std::error_code error;
    if (favorite) {
        std::ofstream stream(marker, std::ios::out | std::ios::trunc);
        if (!stream.is_open()) {
            Logger::instance().warning(L"clips", L"Could not create favorite marker: " + marker.wstring());
        }
        return stream.is_open();
    }
    std::filesystem::remove(marker, error);
    if (error) {
        Logger::instance().warning(L"clips", L"Could not remove favorite marker: " + marker.wstring() + L" (" + utf8ToWide(error.message()) + L")");
    }
    return !error;
}

bool ClipManager::addTag(const std::filesystem::path& path, const std::wstring& tag) const {
    if (!isManagedClipPath(path)) {
        Logger::instance().warning(L"clips", L"Rejected tag update outside the clip directory: " + path.wstring());
        return false;
    }

    const std::wstring normalized = normalizeTag(tag);
    if (normalized.empty()) {
        return false;
    }

    auto tags = readTags(path);
    if (std::find(tags.begin(), tags.end(), normalized) == tags.end()) {
        tags.push_back(normalized);
    }

    const auto filePath = tagFile(path);
    std::ofstream stream(filePath, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        Logger::instance().warning(L"clips", L"Could not write clip tags: " + filePath.wstring());
        return false;
    }
    for (const auto& existing : tags) {
        stream << wideToUtf8(existing) << "\n";
    }
    if (!stream) {
        Logger::instance().warning(L"clips", L"Could not finish writing clip tags: " + filePath.wstring());
        return false;
    }
    return true;
}

bool ClipManager::isManagedClipPath(const std::filesystem::path& path) const {
    if (directory_.empty() || path.empty() || !hasMp4Extension(path)) {
        return false;
    }

    std::error_code error;
    const auto root = std::filesystem::weakly_canonical(directory_, error);
    if (error || root.empty()) {
        return false;
    }

    const auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error || resolved.empty() || !isPathInsideDirectory(resolved, root)) {
        return false;
    }

    return std::filesystem::is_regular_file(resolved, error) && !error;
}

std::filesystem::path ClipManager::favoriteMarker(const std::filesystem::path& path) const {
    return path.parent_path() / (path.filename().wstring() + L".favorite");
}

std::filesystem::path ClipManager::tagFile(const std::filesystem::path& path) const {
    return path.parent_path() / (path.filename().wstring() + L".tags");
}

std::vector<std::wstring> ClipManager::readTags(const std::filesystem::path& path) const {
    std::vector<std::wstring> tags;
    std::ifstream stream(tagFile(path), std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        return tags;
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::wstring tag = normalizeTag(utf8ToWide(line));
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) {
            tags.push_back(std::move(tag));
        }
    }
    return tags;
}

} // namespace backtrack
