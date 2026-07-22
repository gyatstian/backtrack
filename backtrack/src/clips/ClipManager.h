#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace backtrack {

struct ClipInfo {
    std::filesystem::path path;
    std::filesystem::file_time_type modifiedTime{};
    uintmax_t bytes = 0;
    uint64_t duration100ns = 0;
    bool favorite = false;
    std::vector<std::wstring> tags;
};

class ClipManager {
public:
    explicit ClipManager(std::filesystem::path directory = {});

    void setDirectory(std::filesystem::path directory);
    std::vector<ClipInfo> listClips() const;
    bool removeClip(const std::filesystem::path& path) const;
    bool renameClip(const std::filesystem::path& path, const std::wstring& newStem) const;
    bool setFavorite(const std::filesystem::path& path, bool favorite) const;
    bool addTag(const std::filesystem::path& path, const std::wstring& tag) const;

private:
    bool isManagedClipPath(const std::filesystem::path& path) const;
    std::filesystem::path favoriteMarker(const std::filesystem::path& path) const;
    std::filesystem::path tagFile(const std::filesystem::path& path) const;
    std::vector<std::wstring> readTags(const std::filesystem::path& path) const;

    std::filesystem::path directory_;
};

} // namespace backtrack
