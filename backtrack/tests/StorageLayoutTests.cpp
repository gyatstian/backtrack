#include "audio/AudioConvert.h"
#include "clips/ClipManager.h"
#include "clips/ClipStorage.h"
#include "mux/Mp4Muxer.h"
#include "mux/RecoveryStore.h"
#include "replay/ReplayBuffer.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace backtrack {

static bool failMux = false;
static unsigned muxCalls = 0;
static bool muxHadAudio = false;
static std::vector<std::filesystem::path> muxOutputs;

bool removeEmptyOutputReservation(const std::filesystem::path& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    std::error_code error;
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_HIDDEN) == 0 ||
        !std::filesystem::is_regular_file(path, error) || error ||
        std::filesystem::file_size(path, error) != 0 || error) return false;
    return std::filesystem::remove(path, error) && !error;
}

bool Mp4Muxer::muxToMp4(const MuxedInputs& inputs, const std::filesystem::path& output,
                        std::wstring*, unsigned, DWORD*) {
    ++muxCalls;
    muxHadAudio = !inputs.systemAudioPath.empty() || !inputs.microphoneAudioPath.empty();
    muxOutputs.push_back(output);
    if (failMux) return false;
    if (!inputs.systemAudioPath.empty()) {
        std::ofstream mixed(inputs.systemAudioPath.parent_path() / L"mixed_audio.wav", std::ios::binary);
        mixed << "new mix";
    }
    // The real muxer stages the MP4 and moves it onto the output path, dropping
    // the empty hidden reservation first. The CRT cannot open a hidden file, so
    // the stub has to do the same.
    removeEmptyOutputReservation(output);
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    stream << "published";
    return stream.good();
}

} // namespace backtrack

namespace {

using namespace backtrack;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestRoot {
    std::filesystem::path path;
    TestRoot() {
        path = std::filesystem::temp_directory_path() /
            (L"backtrack_layout_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        std::error_code error;
        expect(std::filesystem::create_directory(path, error) && !error, "could not create test directory");
    }
    ~TestRoot() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    expect(!error, "could not create fixture directory");
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    expect(stream.good(), "could not write fixture file");
}

// Recovery detail is ASCII in practice; anything else becomes '?' so the
// assertion message itself cannot depend on the locale.
std::string asciiFromWide(const std::wstring& wide) {
    std::string text;
    text.reserve(wide.size());
    for (wchar_t character : wide) {
        text.push_back(character >= 0 && character < 0x80 ? static_cast<char>(character) : '?');
    }
    return text;
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    expect(stream.is_open(), "could not read fixture file");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void testOutputReservationPreservesCollisions() {
    TestRoot root;
    const auto initial = root.path / L"clip.mp4";
    const auto second = root.path / L"clip_2.mp4";
    writeFile(initial, "first clip");
    writeFile(second, "second clip");
    const auto tags = root.path / L"clip_3.mp4.tags";
    const auto favorite = root.path / L"clip_4.mp4.favorite";
    writeFile(tags, "user tags");
    writeFile(favorite, "user favorite");
    const auto claimed = ClipStorage::reserveClipOutput(initial);
    expect(claimed == root.path / L"clip_5.mp4", "reservation did not choose a free name");
    const DWORD attributes = GetFileAttributesW(claimed.c_str());
    expect(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_HIDDEN) != 0,
           "reservation was not hidden");
    expect(readFile(initial) == "first clip" && readFile(second) == "second clip", "reservation changed existing clips");
    expect(readFile(tags) == "user tags" && readFile(favorite) == "user favorite", "reservation changed orphaned sidecars");
    expect(ClipStorage::reserveClipOutput(root.path / L"missing" / L"clip.mp4").empty(),
           "reservation returned an unclaimed name on failure");
}

void testClipRootsAndTags() {
    TestRoot root;
    expect(ClipStorage::outputDirectory(root.path, L"Game", false) == root.path, "flat output changed");
    expect(ClipStorage::outputDirectory(root.path, L"Game", true) == root.path / L"Game", "game output changed");
    expect(ClipStorage::outputDirectory(root.path, L".BACKTRACK_TMP", true) == root.path / L"_.BACKTRACK_TMP",
           "temporary namespace was not escaped");
    expect(ClipStorage::outputDirectory(root.path, L".backtrack_ring", true) == root.path / L"_.backtrack_ring",
           "ring namespace was not escaped");
    expect(ClipStorage::outputDirectory(root.path, L".other", true) == root.path / L"_.other",
           "dot-prefixed game was not escaped");

    const auto flat = root.path / L"flat.mp4";
    const auto game = root.path / L"Game" / L"game.mp4";
    const auto kill = root.path / L"Game" / L"kill.mp4";
    const auto oldKill = root.path / L"Game" / L"clips" / L"old-kill.mp4";
    const auto reserved = root.path / L".backtrack_tmp" / L"old-game.mp4";
    const auto reservedKill = root.path / L".backtrack_tmp" / L"clips" / L"old-kill.mp4";
    const auto ringGame = root.path / L".backtrack_ring" / L"old-game.mp4";
    const auto staged = root.path / L".backtrack_tmp" / L"clip.publish.2.mp4";
    const auto escapedTemp = ClipStorage::outputDirectory(root.path, L".backtrack_tmp", true) / L"new.mp4";
    const auto escapedRing = ClipStorage::outputDirectory(root.path, L".backtrack_ring", true) / L"new.mp4";
    for (const auto& path : {flat, oldKill, reserved, reservedKill, ringGame, staged}) writeFile(path, "clip");
    ReplayBuffer buffer;
    ReplaySettings settings;
    settings.diskBackedVideo = false;
    settings.diskBackedAudio = false;
    buffer.configure(settings);
    EncodedPacket packet;
    packet.bytes = {0, 0, 0, 1, 0x65};
    packet.keyFrame = true;
    packet.duration100ns = 333333;
    buffer.pushVideo(std::move(packet));
    for (const auto& path : {game, kill, escapedTemp, escapedRing}) {
        expect(buffer.saveTo(path, VideoSettings{}), "could not save game replay");
    }
    ClipManager manager(root.path);
    expect(manager.addTag(kill, L"Kill clip"), "could not tag per-game replay");
    const auto clips = manager.listClips();
    const auto contains = [&](const auto& path) {
        return std::any_of(clips.begin(), clips.end(), [&](const auto& clip) { return clip.path == path; });
    };
    for (const auto& path : {flat, game, kill, oldKill, reserved, reservedKill, ringGame, escapedTemp, escapedRing}) {
        expect(contains(path), "supported clip root was not discovered");
    }
    expect(!contains(staged), "legacy staged MP4 appeared as a clip");
    const auto found = std::find_if(clips.begin(), clips.end(), [&](const auto& clip) { return clip.path == kill; });
    expect(found != clips.end() && found->tags == std::vector<std::wstring>{L"Kill clip"}, "kill tag missing");
    expect(!std::filesystem::exists(root.path / L"Game" / L"clips" / L"kill.mp4"), "tagging moved the new clip");
}

MuxedInputs makeManifest(const std::filesystem::path& session) {
    MuxedInputs inputs;
    inputs.videoPath = session / L"video.h264";
    inputs.systemAudioPath = session / L"system.wav";
    inputs.codec = VideoCodec::H264;
    inputs.fps = 60;
    inputs.width = 1920;
    inputs.height = 1080;
    inputs.bitrateKbps = 8000;
    MuxedInputs::VideoSample sample{};
    sample.size = 4;
    sample.duration100ns = 333333;
    sample.keyFrame = true;
    inputs.videoSamples.push_back(sample);
    return inputs;
}

void writeSession(const std::filesystem::path& session) {
    writeFile(session / L"video.h264", "abcd");
    writeFile(session / L"system.wav", "audio");
    writeFile(session / L"mixed_audio.wav", "user mix");
    writeFile(session / L"unrelated.txt", "user content");
    expect(RecoveryStore::writeManifest(makeManifest(session), session), "could not write manifest");
}

void testFailedGameReplayAndDiscovery() {
    TestRoot root;
    const auto game = root.path / L"Game";
    ReplayBuffer buffer;
    ReplaySettings settings;
    settings.enabled = true;
    settings.seconds = 30;
    buffer.configure(settings);
    EncodedPacket packet;
    packet.bytes = {0, 0, 0, 1, 0x65};
    packet.keyFrame = true;
    packet.duration100ns = 333333;
    packet.codec = VideoCodec::H264;
    buffer.pushVideo(std::move(packet));
    failMux = true;
    std::wstring detail;
    const auto output = game / L"replay.mp4";
    const bool saved = buffer.saveTo(output, VideoSettings{}, &detail);
    failMux = false;
    expect(!saved, "failed mux was reported as saved");
    const auto session = game / L".backtrack_tmp" / L"replay";
    expect(std::filesystem::exists(session / RecoveryStore::kRecoveryManifestName), "failed replay not preserved");
    writeFile(session / L"unrelated.txt", "keep me");
    const auto originalVideo = readFile(session / L"replay.h264");
    const auto originalManifest = readFile(session / RecoveryStore::kRecoveryManifestName);
    expect(buffer.saveTo(output, VideoSettings{}), "same-name replay retry failed");
    expect(readFile(session / L"replay.h264") == originalVideo &&
           readFile(session / RecoveryStore::kRecoveryManifestName) == originalManifest,
           "same-name retry changed the preserved replay");
    const auto recovered = RecoveryStore::recoverLatestFailedRecording(root.path, detail);
    const std::string discovery = "manual recovery did not discover the game root: " + asciiFromWide(detail);
    expect(recovered.parent_path() == game, discovery.c_str());
    expect(readFile(session / L"unrelated.txt") == "keep me", "recovery deleted unrelated content");
    expect(std::filesystem::exists(session / L"replay.h264"), "recovery deleted preserved stream");
    const unsigned after = muxCalls;
    RecoveryStore::sweepOrphanedRecoveryData(root.path);
    expect(muxCalls == after, "completed recovery was repeated");
    ClipManager manager(root.path);
    const auto clips = manager.listClips();
    expect(std::any_of(clips.begin(), clips.end(), [&](const auto& clip) { return clip.path == recovered; }),
           "recovered game clip not listed");
}

void testNestedRecoveryAndUnrelatedCleanup() {
    TestRoot root;
    muxOutputs.clear();
    const auto first = root.path / L"Game" / L".backtrack_tmp" / L"first";
    const auto second = root.path / L"Game" / L"clips" / L".backtrack_tmp" / L"second";
    const auto collision = root.path / L".backtrack_tmp" / L".backtrack_tmp" / L"reserved";
    const auto ringCollision = root.path / L".backtrack_ring" / L".backtrack_tmp" / L"reserved-ring";
    writeSession(first);
    writeSession(second);
    writeSession(collision);
    writeSession(ringCollision);
    const auto outputCollision = root.path / L"Game" / L"first.mp4";
    const auto sidecarCollision = root.path / L"Game" / L"first_recovered.mp4.tags";
    writeFile(outputCollision, "existing clip");
    writeFile(sidecarCollision, "existing tags");
    writeFile(first / L".backtrack_recovered", "user marker");
    const auto workCollision = first.parent_path() / L".backtrack_recovery_work" / L"user.txt";
    writeFile(workCollision, "user work");
    const auto unrelated = root.path / L".backtrack_tmp" / L"unrecognized" / L"user.txt";
    const auto hidden = root.path / L"hidden-user.txt";
    const auto hiddenMp4 = root.path / L"hidden-user.mp4";
    writeFile(unrelated, "untouched");
    writeFile(hidden, "");
    writeFile(hiddenMp4, "");
    expect(SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0 &&
           SetFileAttributesW(hiddenMp4.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0, "could not hide fixture");
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    std::error_code error;
    std::filesystem::last_write_time(hidden, old, error);
    expect(!error, "could not age fixture");
    std::filesystem::last_write_time(hiddenMp4, old, error);
    expect(!error, "could not age MP4 fixture");
    const auto before = muxCalls;
    RecoveryStore::sweepOrphanedRecoveryData(root.path);
    expect(muxCalls == before + 4, "startup did not recover every nested session");
    for (const auto& session : {first, second, collision, ringCollision}) {
        expect(readFile(session / L"mixed_audio.wav") == "user mix", "recovery overwrote mixed audio collision");
        expect(readFile(session / L"unrelated.txt") == "user content", "recovery removed unrelated session content");
        expect(readFile(session / L"video.h264") == "abcd", "recovery changed preserved video");
    }
    expect(readFile(outputCollision) == "existing clip", "recovery overwrote an existing clip");
    expect(readFile(sidecarCollision) == "existing tags", "recovery changed an orphaned sidecar");
    expect(std::filesystem::exists(root.path / L"Game" / L"first_recovered_2.mp4"), "recovery did not choose a free output name");
    expect(readFile(first / L".backtrack_recovered") == "user marker", "recovery overwrote a marker collision");
    expect(readFile(workCollision) == "user work", "recovery removed a work-directory collision");
    expect(!std::filesystem::exists(first.parent_path() / L".backtrack_recovery_work_1"), "owned recovery work was not cleaned up");
    expect(readFile(unrelated) == "untouched" && std::filesystem::exists(hidden) && std::filesystem::exists(hiddenMp4),
           "sweep deleted unrelated content");
    const auto after = muxCalls;
    RecoveryStore::sweepOrphanedRecoveryData(root.path);
    expect(muxCalls == after, "startup recovered completed sessions twice");
    expect(std::any_of(muxOutputs.begin(), muxOutputs.end(), [&](const auto& p) { return p.parent_path() == root.path / L"Game" / L"clips"; }),
           "nested recovery output was misplaced");
    expect(std::any_of(muxOutputs.begin(), muxOutputs.end(), [&](const auto& p) { return p.parent_path() == root.path / L".backtrack_tmp"; }),
           "temporary-name recovery output was misplaced");
    expect(std::any_of(muxOutputs.begin(), muxOutputs.end(), [&](const auto& p) { return p.parent_path() == root.path / L".backtrack_ring"; }),
           "ring-name recovery output was misplaced");
}

// a clip the user deleted or renamed through the library must not come back.
void testDeletedOrRenamedRecoveryIsNotRepeated() {
    TestRoot root;
    const auto game = root.path / L"Game";
    writeSession(game / L".backtrack_tmp" / L"deleted");
    writeSession(game / L".backtrack_tmp" / L"renamed");
    const unsigned before = muxCalls;
    RecoveryStore::sweepOrphanedRecoveryData(root.path);
    expect(muxCalls == before + 2, "startup did not recover the failed sessions");
    const auto deleted = game / L"deleted.mp4";
    const auto renamed = game / L"renamed.mp4";
    expect(readFile(deleted) == "published" && readFile(renamed) == "published",
           "recovery did not publish the expected names");

    std::error_code error;
    expect(std::filesystem::remove(deleted, error) && !error, "could not delete recovered clip");
    const auto moved = game / L"User Rename.mp4";
    std::filesystem::rename(renamed, moved, error);
    expect(!error, "could not rename recovered clip");

    RecoveryStore::sweepOrphanedRecoveryData(root.path);
    expect(muxCalls == before + 2, "a deleted or renamed clip was recovered again");
    expect(!std::filesystem::exists(deleted), "deleted clip came back");
    expect(!std::filesystem::exists(game / L"deleted_recovered.mp4"), "deleted clip came back under a second name");
    expect(!std::filesystem::exists(renamed) && readFile(moved) == "published",
           "renamed clip was duplicated or lost");

    // a session that was never published stays recoverable after the sweep.
    writeSession(game / L".backtrack_tmp" / L"fresh");
    RecoveryStore::sweepOrphanedRecoveryData(root.path);
    expect(muxCalls == before + 3, "a fresh failed session was not recovered");
    expect(readFile(game / L"fresh.mp4") == "published", "fresh session published nothing");
}

void testManualRecoverySelectsNewestAcrossRoots() {
    TestRoot root;
    const auto older = root.path / L".backtrack_tmp" / L"older";
    const auto newer = root.path / L"Game" / L".backtrack_tmp" / L"newer";
    writeSession(older);
    writeSession(newer);
    std::error_code error;
    std::filesystem::last_write_time(older / RecoveryStore::kRecoveryManifestName,
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(2), error);
    expect(!error, "could not age older manifest");
    std::wstring detail;
    const auto first = RecoveryStore::recoverLatestFailedRecording(root.path, detail);
    expect(first.parent_path() == root.path / L"Game", "manual recovery did not choose the newest manifest");
    const auto second = RecoveryStore::recoverLatestFailedRecording(root.path, detail);
    expect(second.parent_path() == root.path, "manual recovery did not continue to the older manifest");
    expect(RecoveryStore::recoverLatestFailedRecording(root.path, detail).empty(), "completed recovery was selected again");
    expect(std::filesystem::remove(first, error) && !error, "could not replace recovered output fixture");
    writeFile(first, "replacement clip with different content");
    const auto retry = RecoveryStore::recoverLatestFailedRecording(root.path, detail);
    expect(retry.parent_path() == first.parent_path() && retry != first, "replaced output hid the preserved session");
    expect(readFile(first) == "replacement clip with different content", "recovery overwrote a replaced output");
    expect(RecoveryStore::recoverLatestFailedRecording(root.path, detail).empty(), "retry marker did not prevent a duplicate");
}

void testEscapingManifestIsPreserved() {
    TestRoot root;
    const auto outside = root.path / L"outside.h264";
    const auto session = root.path / L"Game" / L".backtrack_tmp" / L"bad";
    writeFile(outside, "user stream");
    writeFile(session / L"recovery.tsv",
              "backtrack-recovery-v1\nvideo\t..\\..\\..\\outside.h264\nsample\t0\t4\t0\t333333\t1\n");
    const auto before = muxCalls;
    RecoveryStore::sweepOrphanedRecoveryData(root.path);
    expect(muxCalls == before, "recovery followed a manifest path outside its session");
    expect(readFile(outside) == "user stream" && std::filesystem::exists(session / L"recovery.tsv"),
           "invalid recovery data was not preserved");
}

void testClearAndDisableRingLifecycle() {
    TestRoot root;
    for (const bool disable : {false, true}) {
        const auto directory = root.path / (disable ? L"disable" : L"clear");
        const auto ring = directory / L".backtrack_ring";
        writeFile(ring / L"replay_video.bin", "user video");
        writeFile(ring / L"replay_audio.bin", "user audio");
        {
            ReplayBuffer buffer;
            ReplaySettings settings;
            buffer.configure(settings, directory, 8ull * 1024 * 1024, 1024ull * 1024);
            expect(buffer.storageFallbackReason().empty(), "could not open alternate rings");
            EncodedPacket packet;
            packet.bytes = {0, 0, 0, 1, 0x65};
            packet.keyFrame = true;
            packet.duration100ns = 333333;
            buffer.pushVideo(packet);
            for (const auto track : {AudioTrack::System, AudioTrack::Microphone}) {
                AudioPacket audio;
                audio.track = track;
                audio.format = std::make_shared<WaveFormatBlob>(canonicalAudioFormat());
                audio.frameCount = 1;
                audio.bytes.assign(8, 0);
                buffer.pushAudio(std::move(audio));
            }
            expect(buffer.videoPacketCount() == 1 && buffer.videoKeyFrameCount() == 1,
                   "initial counters were not published");
            if (disable) {
                settings.enabled = false;
                buffer.configure(settings, directory, 8ull * 1024 * 1024, 1024ull * 1024);
                buffer.pushVideo(packet);
            } else {
                buffer.clear();
            }
            expect(buffer.videoPacketCount() == 0 && buffer.videoKeyFrameCount() == 0 &&
                   buffer.videoSpan100ns() == 0, "clear or disable left buffered video counters");
            expect(std::filesystem::exists(ring / L"replay_video_1.bin") == !disable &&
                   std::filesystem::exists(ring / L"replay_audio_1.bin") == !disable,
                   "clear closed rings or disable retained them");
            if (disable) {
                settings.enabled = true;
                buffer.configure(settings, directory, 8ull * 1024 * 1024, 1024ull * 1024);
            }
            buffer.pushVideo(std::move(packet));
            expect(buffer.videoPacketCount() == 1 && buffer.videoKeyFrameCount() == 1,
                   "buffer did not resume after clear or re-enable");
            muxHadAudio = true;
            expect(buffer.saveTo(directory / L"resumed.mp4", VideoSettings{}),
                   "resumed ring could not save");
            expect(!muxHadAudio, "resumed save included audio from before clear or disable");
        }
        expect(readFile(ring / L"replay_video.bin") == "user video" &&
               readFile(ring / L"replay_audio.bin") == "user audio", "lifecycle changed ring collisions");
        expect(!std::filesystem::exists(ring / L"replay_video_1.bin") &&
               !std::filesystem::exists(ring / L"replay_audio_1.bin"), "owned rings survived destruction");
    }
}

void testRingCollisionIsPreserved() {
    TestRoot root;
    const auto ring = root.path / L".backtrack_ring";
    writeFile(ring / L"replay_video.bin", "user video");
    writeFile(ring / L"replay_audio.bin", "user audio");
    writeFile(ring / L"clips" / L"kill.mp4", "user clip");
    {
        ReplayBuffer buffer;
        ReplaySettings settings;
        settings.enabled = true;
        settings.seconds = 30;
        settings.diskBackedVideo = true;
        settings.diskBackedAudio = true;
        buffer.configure(settings, root.path, 8ull * 1024 * 1024, 1024ull * 1024);
        expect(buffer.storageFallbackReason().empty(), "ring collision prevented alternate names");
        expect(readFile(ring / L"replay_video.bin") == "user video", "video ring overwrote user content");
        expect(readFile(ring / L"replay_audio.bin") == "user audio", "audio ring overwrote user content");
    }
    ReplayBuffer::removeOrphanedRing(root.path);
    expect(readFile(ring / L"replay_video.bin") == "user video", "ring sweep deleted user video");
    expect(readFile(ring / L"replay_audio.bin") == "user audio", "ring sweep deleted user audio");
    expect(readFile(ring / L"clips" / L"kill.mp4") == "user clip", "ring sweep deleted game clip");
    expect(!std::filesystem::exists(ring / L"replay_video_1.bin"), "owned video ring survived close");
    expect(!std::filesystem::exists(ring / L"replay_audio_1.bin"), "owned audio ring survived close");
}

} // namespace

int main() {
    try {
        testOutputReservationPreservesCollisions();
        testClipRootsAndTags();
        testFailedGameReplayAndDiscovery();
        testNestedRecoveryAndUnrelatedCleanup();
        testDeletedOrRenamedRecoveryIsNotRepeated();
        testManualRecoverySelectsNewestAcrossRoots();
        testEscapingManifestIsPreserved();
        testRingCollisionIsPreserved();
        testClearAndDisableRingLifecycle();
        std::cout << "StorageLayout tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "StorageLayout test failure: " << error.what() << '\n';
        return 1;
    }
}
