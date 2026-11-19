#include "clips/ClipManager.h"

#include <Windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace backtrack {

struct ClipManagerTestAccess {
    static bool renameWithRace(const ClipManager& manager, const std::filesystem::path& source,
                               const std::wstring& stem, void (*beforeMove)(const std::filesystem::path&)) {
        return manager.renameClipImpl(source, stem, beforeMove);
    }
};

} // namespace backtrack

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestRoot {
    std::filesystem::path path;
    TestRoot() {
        path = std::filesystem::temp_directory_path() /
            (L"backtrack_clip_manager_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        expect(std::filesystem::create_directory(path), "could not create clip test root");
    }
    ~TestRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

void writeFile(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << contents;
    expect(static_cast<bool>(stream), "could not write clip fixture");
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    expect(stream.is_open(), "could not read clip fixture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::filesystem::path sidecar(const std::filesystem::path& clip, const wchar_t* suffix) {
    return clip.wstring() + suffix;
}

std::wstring stagingName(unsigned long long offset) {
    return L"BT" + std::to_wstring((GetCurrentProcessId() + offset) % 1000000) + L".TMP";
}

void expectNoStaging(const std::filesystem::path& directory, const std::filesystem::path& allowed = {}) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path() == allowed) continue;
        const auto name = entry.path().filename().wstring();
        expect(!(name.starts_with(L"BT") && name.ends_with(L".TMP")), "clip staging file was left behind");
    }
}

bool createSymlink(const std::filesystem::path& link, const std::filesystem::path& target) {
    if (CreateSymbolicLinkW(link.c_str(), target.c_str(), 0x2)) return true;
    DWORD error = GetLastError();
    if (error == ERROR_INVALID_PARAMETER) {
        if (CreateSymbolicLinkW(link.c_str(), target.c_str(), 0)) return true;
        error = GetLastError();
    }
    if (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION) {
        std::cout << "clip symbolic link tests skipped: unavailable (" << error << ")\n";
        return false;
    }
    throw std::runtime_error("could not create clip symbolic link fixture");
}

void testHardLinkedSidecarsAndStaging() {
    TestRoot root;
    const auto clip = root.path / L"library" / L"Game" / L"clips" / L"clip.mp4";
    const auto sentinel = root.path / L"external.txt";
    const auto tags = sidecar(clip, L".tags");
    const auto favorite = sidecar(clip, L".favorite");
    const auto collision = clip.parent_path() / stagingName(0);
    const std::string original = "original\r\nalpha\noriginal\r\n";
    writeFile(clip, "media");
    writeFile(sentinel, original);
    expect(CreateHardLinkW(tags.c_str(), sentinel.c_str(), nullptr) != FALSE, "could not hard-link tags");
    expect(CreateHardLinkW(favorite.c_str(), sentinel.c_str(), nullptr) != FALSE, "could not hard-link favorite");
    expect(CreateHardLinkW(collision.c_str(), sentinel.c_str(), nullptr) != FALSE, "could not hard-link staging collision");

    const backtrack::ClipManager manager(root.path / L"library");
    expect(manager.setFavorite(clip, true), "existing hard-linked favorite was rejected");
    expect(manager.addTag(clip, L"  new tag  "), "could not replace hard-linked tags");
    expect(readFile(sentinel) == original && readFile(collision) == original && readFile(favorite) == original,
           "sidecar or staging write changed the external hard-link target");
    expect(readFile(tags) == "original\nalpha\nnew tag\n", "tags were not normalized and published");
    expect(!std::filesystem::equivalent(tags, sentinel), "tag replacement retained the hard link");
    expectNoStaging(clip.parent_path(), collision);
    expect(manager.setFavorite(clip, false), "could not remove hard-linked favorite");
    expect(readFile(sentinel) == original, "removing favorite changed its hard-link target");
}

void testSymbolicLinkedSidecarsAndStaging() {
    TestRoot root;
    const auto clip = root.path / L"library" / L"clip.mp4";
    const auto sentinel = root.path / L"external.txt";
    const auto tags = sidecar(clip, L".tags");
    const auto favorite = sidecar(clip, L".favorite");
    // The hard-link collision and its successful retry consumed names 0 and 1.
    const auto collision = clip.parent_path() / stagingName(2);
    const std::string original = "symbolic\r\n";
    writeFile(clip, "media");
    writeFile(sentinel, original);
    if (!createSymlink(collision, sentinel)) return;
    expect(createSymlink(tags, sentinel), "could not link tags");
    expect(createSymlink(favorite, sentinel), "could not link favorite");

    const backtrack::ClipManager manager(root.path / L"library");
    expect(manager.setFavorite(clip, true), "existing symbolic favorite was rejected");
    expect(manager.addTag(clip, L"next"), "could not replace symbolic tags");
    expect(readFile(sentinel) == original && readFile(collision) == original && readFile(favorite) == original,
           "sidecar or staging write changed the symbolic-link target");
    expect(readFile(tags) == "symbolic\nnext\n", "symbolic tag entry was not replaced");
    expect(!std::filesystem::equivalent(tags, sentinel), "tag replacement retained the symbolic link");
    expectNoStaging(clip.parent_path(), collision);
    expect(manager.setFavorite(clip, false), "could not remove symbolic favorite");
    expect(readFile(sentinel) == original, "removing favorite changed its symbolic-link target");

    const auto missing = root.path / L"missing.txt";
    expect(createSymlink(favorite, missing), "could not create dangling favorite");
    expect(!manager.setFavorite(clip, true), "dangling favorite was reported as present");
    expect(!std::filesystem::exists(missing), "favorite created a dangling link target");
}

void testFailedTagPublishPreservesOldTags() {
    TestRoot root;
    const auto clip = root.path / L"clip.mp4";
    const auto tags = sidecar(clip, L".tags");
    writeFile(clip, "media");
    writeFile(tags, "old\r\n");
    const HANDLE lock = CreateFileW(tags.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    expect(lock != INVALID_HANDLE_VALUE, "could not lock tags");
    const bool saved = backtrack::ClipManager(root.path).addTag(clip, L"new");
    CloseHandle(lock);
    expect(!saved, "locked tags were reported as saved");
    expect(readFile(tags) == "old\r\n", "failed tag publish changed old tags");
    expectNoStaging(root.path);
}

void testRenameRacePreservesMediaAndMetadata() {
    TestRoot root;
    const auto source = root.path / L"source.mp4";
    const auto target = root.path / L"target.mp4";
    writeFile(source, "source media");
    writeFile(sidecar(source, L".favorite"), "source favorite");
    writeFile(sidecar(source, L".tags"), "source tags");
    writeFile(sidecar(target, L".favorite"), "target favorite");
    writeFile(sidecar(target, L".tags"), "target tags");
    const backtrack::ClipManager manager(root.path);

    expect(!std::filesystem::exists(target), "race precheck did not see a free target");
    expect(!backtrack::ClipManagerTestAccess::renameWithRace(manager, source, L"target",
        [](const std::filesystem::path& destination) { writeFile(destination, "arriving media"); }),
        "rename overwrote a destination created after the precheck");
    expect(readFile(source) == "source media" && readFile(target) == "arriving media",
           "rename race changed media");
    expect(readFile(sidecar(source, L".favorite")) == "source favorite" &&
           readFile(sidecar(source, L".tags")) == "source tags" &&
           readFile(sidecar(target, L".favorite")) == "target favorite" &&
           readFile(sidecar(target, L".tags")) == "target tags", "rename race changed metadata");
    expect(!manager.renameClip(source, L"target"), "existing target passed the public rename");
}

void testNormalRenameAndStorageRoots() {
    TestRoot root;
    const auto library = root.path / L"library";
    const auto flat = library / L"flat.mp4";
    const auto game = library / L"Game" / L"game.mp4";
    const auto source = library / L"Game" / L"clips" / L"legacy.mp4";
    const auto target = source.parent_path() / L"renamed.mp4";
    writeFile(flat, "flat");
    writeFile(game, "game");
    writeFile(source, "legacy");
    const backtrack::ClipManager manager(library);
    expect(manager.setFavorite(source, true) && manager.addTag(source, L"  tag  ") && manager.addTag(source, L"tag"),
           "could not prepare normal sidecars");
    expect(readFile(sidecar(source, L".tags")) == "tag\n", "duplicate tag was not normalized");
    writeFile(sidecar(target, L".favorite"), "stale favorite");
    writeFile(sidecar(target, L".tags"), "stale tags");
    expect(!manager.renameClip(source, L"CON"), "invalid clip name was accepted");
    expect(manager.renameClip(source, L"renamed"), "normal clip rename failed");
    expect(!std::filesystem::exists(source) && readFile(target) == "legacy", "normal rename changed media");
    expect(readFile(sidecar(target, L".favorite")).empty() && readFile(sidecar(target, L".tags")) == "tag\n" &&
           !std::filesystem::exists(sidecar(source, L".favorite")) && !std::filesystem::exists(sidecar(source, L".tags")),
           "normal rename did not move sidecars");
    const auto clips = manager.listClips();
    for (const auto& expected : {flat, game, target}) {
        expect(std::any_of(clips.begin(), clips.end(), [&](const auto& clip) { return clip.path == expected; }),
               "supported storage root was not listed");
    }
}

void testSidecarRenameFailuresStayNonfatal() {
    for (const wchar_t* blocked : {L".favorite", L".tags"}) {
        TestRoot root;
        const auto source = root.path / L"source.mp4";
        const auto target = root.path / L"target.mp4";
        writeFile(source, "media");
        writeFile(sidecar(source, L".favorite"), "favorite");
        writeFile(sidecar(source, L".tags"), "tags");
        writeFile(sidecar(target, blocked) / L"blocker", "keep");
        expect(backtrack::ClipManager(root.path).renameClip(source, L"target"),
            "a failed sidecar move failed the media rename");
        expect(!std::filesystem::exists(source) && readFile(target) == "media",
            "a sidecar failure rolled back or changed media");
        expect(readFile(sidecar(target, blocked) / L"blocker") == "keep" &&
               std::filesystem::exists(sidecar(source, blocked)),
            "a failed sidecar move lost its source or destination");
        const wchar_t* moved = std::wstring(blocked) == L".favorite" ? L".tags" : L".favorite";
        expect(std::filesystem::exists(sidecar(target, moved)) &&
               !std::filesystem::exists(sidecar(source, moved)),
            "one sidecar failure prevented the other sidecar from moving");
    }
}

void testMixedCaseClipPathsAndStagingNames() {
    TestRoot root;
    const auto library = root.path / L"Library";
    const auto clip = library / L"Clip.Mp4";
    writeFile(clip, "media");
    writeFile(library / L".BACKTRACK_TMP" / L"Clip.PUBLISH.MP4", "staging");
    writeFile(library / L".BACKTRACK_TMP" / L"Clip.PuBlIsH.2.Mp4", "staging");
    const backtrack::ClipManager manager(library);
    const auto clips = manager.listClips();
    expect(clips.size() == 1 && clips.front().path == clip,
        "case folding changed clip extensions or staged-file exclusion");
    expect(manager.setFavorite(root.path / L"lIbRaRy" / L"cLiP.mP4", true),
        "case folding rejected a managed path with different capitalization");
    expect(manager.renameClip(clip, L"renamed"), "mixed-case MP4 extension was rejected");
    const auto target = library / L"renamed.Mp4";
    expect(readFile(target) == "media" && std::filesystem::exists(sidecar(target, L".favorite")),
        "mixed-case rename changed media or lost its sidecar");
}

void testRenameNameRulesAndOrphanSidecars() {
    TestRoot root;
    const auto clip = root.path / L"Clip.mp4";
    writeFile(clip, "media");
    const backtrack::ClipManager manager(root.path);
    expect(manager.renameClip(clip, L"Clip") && readFile(clip) == "media", "same-name rename was not a no-op");
    expect(backtrack::clipRenameTarget(clip, L"bad\x01name").empty(), "control character was accepted");
    expect(backtrack::clipRenameTarget(clip, L"named.MP4") == root.path / L"named.mp4", "typed extension was kept");
    expect(manager.renameClip(clip, L"clip"), "case-only rename was rejected");
    const auto lower = root.path / L"clip.mp4";
    bool caseChanged = false;
    for (const auto& entry : std::filesystem::directory_iterator(root.path)) caseChanged |= entry.path().filename() == L"clip.mp4";
    expect(caseChanged && readFile(lower) == "media", "case-only rename did not change the name on disk");

    const auto target = root.path / L"next.mp4";
    writeFile(sidecar(target, L".favorite"), "");
    writeFile(sidecar(target, L".tags"), "orphan\n");
    expect(manager.renameClip(lower, L"next.mp4"), "rename with a typed extension failed");
    expect(readFile(target) == "media" && !std::filesystem::exists(sidecar(target, L".favorite")) &&
           !std::filesystem::exists(sidecar(target, L".tags")), "renamed clip inherited orphan sidecars");

    const auto other = root.path / L"other.mp4";
    writeFile(other, "other");
    expect(backtrack::clipRenameCollides(target, other) && !manager.renameClip(target, L"other"),
           "rename onto another clip was not refused");
    expect(manager.setFavorite(other, true) && manager.addTag(other, L"kept"), "could not prepare scan sidecars");
    const auto clips = manager.listClips();
    const auto listed = std::find_if(clips.begin(), clips.end(), [&](const auto& info) { return info.path == other; });
    const auto plain = std::find_if(clips.begin(), clips.end(), [&](const auto& info) { return info.path == target; });
    expect(clips.size() == 2 && listed != clips.end() && listed->favorite && listed->tags == std::vector<std::wstring>{L"kept"} &&
           plain != clips.end() && !plain->favorite && plain->tags.empty(), "scan misread sidecars");
}

void testCancelledScanStopsBeforeReadingClips() {
    TestRoot root;
    const auto library = root.path / L"library";
    writeFile(library / L"clip.mp4", "clip");
    const backtrack::ClipManager manager(library);
    bool cancelled = true;
    const auto clips = manager.listClips([&] { return cancelled; });
    expect(clips.empty(), "a cancelled scan still returned clips");
    cancelled = false;
    expect(manager.listClips([&] { return cancelled; }).size() == 1,
           "a scan that was not cancelled skipped the clip");
}

} // namespace

int main() {
    try {
        testHardLinkedSidecarsAndStaging();
        testSymbolicLinkedSidecarsAndStaging();
        testFailedTagPublishPreservesOldTags();
        testRenameRacePreservesMediaAndMetadata();
        testNormalRenameAndStorageRoots();
        testCancelledScanStopsBeforeReadingClips();
        testSidecarRenameFailuresStayNonfatal();
        testMixedCaseClipPathsAndStagingNames();
        testRenameNameRulesAndOrphanSidecars();
        std::cout << "ClipManagerTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
