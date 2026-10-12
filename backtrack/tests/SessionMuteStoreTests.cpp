#include "audio/SessionMuteStore.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using backtrack::SessionMuteCandidate;
using backtrack::SessionMuteRecord;
using backtrack::SessionMuteStore;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path testDir(const char* name) {
    static int counter = 0;
    auto dir = std::filesystem::temp_directory_path() / "backtrack-session-mute-tests" /
        (std::string(name) + "-" + std::to_string(++counter));
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

SessionMuteCandidate makeSession(
    uint32_t pid, const std::wstring& key, bool muted, std::vector<bool>& calls) {
    SessionMuteCandidate session;
    session.processId = pid;
    session.pathKey = key;
    session.muted = muted;
    session.setMute = [&](bool mute) {
        calls.push_back(mute);
        return true;
    };
    return session;
}

void testSerializeParseRoundTrip() {
    const std::vector<SessionMuteRecord> records = {
        {1234, L"c:\\apps\\game.exe", false},
        {5678, L"c:\\apps\\music player.exe", true},
    };
    const std::string content = SessionMuteStore::serialize(records);
    expect(content == "1234\t0\tc:\\apps\\game.exe\n5678\t1\tc:\\apps\\music player.exe\n",
        "journal line format changed");
    const auto parsed = SessionMuteStore::parse(content);
    expect(parsed.size() == 2, "round trip lost a record");
    expect(parsed[0].processId == 1234 && !parsed[0].previousMute && parsed[0].pathKey == L"c:\\apps\\game.exe",
        "round trip corrupted record 0");
    expect(parsed[1].processId == 5678 && parsed[1].previousMute && parsed[1].pathKey == L"c:\\apps\\music player.exe",
        "round trip corrupted record 1");
}

void testParseSkipsMalformedLines() {
    const std::string content = "no-tabs-here\n"
                                "0\t0\tc:\\apps\\x.exe\n"
                                "42\t0\t\n"
                                "77\t1\tc:\\apps\\ok.exe\r\n";
    const auto parsed = SessionMuteStore::parse(content);
    expect(parsed.size() == 1, "malformed lines must not poison the journal");
    expect(parsed[0].processId == 77 && parsed[0].previousMute, "only good line survives");
}

void testApplyMutesWantedAndJournals() {
    const auto dir = testDir("apply");
    SessionMuteStore store(dir / L"audio-session-mutes.txt");
    std::vector<bool> calls;
    std::vector<SessionMuteCandidate> sessions;
    sessions.push_back(makeSession(111, L"c:\\apps\\game.exe", false, calls));
    sessions.push_back(makeSession(222, L"c:\\apps\\browser.exe", false, calls));

    const uint32_t muted = store.apply(
        {L"c:\\apps\\game.exe"},
        [&](std::vector<SessionMuteCandidate>& out) {
            out = sessions;
            return true;
        },
        [](uint32_t) { return true; });
    expect(muted == 1, "apply must mute exactly the wanted session");
    expect(calls == std::vector<bool>{true}, "wanted session must be muted");
    expect(std::filesystem::exists(dir / L"audio-session-mutes.txt"), "every applied mute must be journaled");
    expect(!std::filesystem::exists(dir / L"audio-session-mutes.txt.tmp"), "no torn journal temp may be left");
    const auto journaled = SessionMuteStore::parse([&] {
        std::ifstream stream(dir / L"audio-session-mutes.txt", std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }());
    expect(journaled.size() == 1 && journaled[0].processId == 111, "journal must hold the applied mute");
}

void testApplyEmptyRestoresAndDeletesJournal() {
    const auto dir = testDir("clear");
    SessionMuteStore store(dir / L"audio-session-mutes.txt");
    std::vector<bool> calls;
    std::vector<SessionMuteCandidate> sessions;
    sessions.push_back(makeSession(111, L"c:\\apps\\game.exe", false, calls));
    store.apply(
        {L"c:\\apps\\game.exe"},
        [&](std::vector<SessionMuteCandidate>& out) {
            out = sessions;
            return true;
        },
        [](uint32_t) { return true; });
    expect(std::filesystem::exists(dir / L"audio-session-mutes.txt"), "setup must journal the mute");

    const uint32_t muted = store.apply({}, nullptr, nullptr);
    expect(muted == 0, "empty apply must restore everything");
    expect(calls == (std::vector<bool>{true, false}), "previous unmuted state must be restored");
    expect(!std::filesystem::exists(dir / L"audio-session-mutes.txt"), "clean restore must delete the journal");
    store.clear();
    expect(calls.size() == 2, "clear with nothing applied must be a no-op");
}

void testApplyPrunesExitedProcess() {
    const auto dir = testDir("prune");
    SessionMuteStore store(dir / L"audio-session-mutes.txt");
    std::vector<bool> calls;
    std::vector<SessionMuteCandidate> sessions;
    sessions.push_back(makeSession(111, L"c:\\apps\\game.exe", false, calls));
    std::atomic<bool> alive{true};
    auto enumerate = [&](std::vector<SessionMuteCandidate>& out) {
        out = sessions;
        return true;
    };
    store.apply({L"c:\\apps\\game.exe"}, enumerate, [&](uint32_t) { return alive.load(); });
    sessions.clear();
    alive = false;
    const uint32_t muted = store.apply({L"c:\\apps\\game.exe"}, enumerate, [&](uint32_t) { return alive.load(); });
    expect(muted == 0, "exited process must be pruned");
    expect(calls == (std::vector<bool>{true, false}), "pruned mute must be restored");
    expect(!std::filesystem::exists(dir / L"audio-session-mutes.txt"), "pruned journal must be deleted");
}

void testApplyEnumerationFailureLeavesEverything() {
    const auto dir = testDir("enumfail");
    SessionMuteStore store(dir / L"audio-session-mutes.txt");
    std::vector<bool> calls;
    const uint32_t muted = store.apply(
        {L"c:\\apps\\game.exe"},
        [](std::vector<SessionMuteCandidate>&) { return false; },
        [](uint32_t) { return true; });
    expect(muted == 0, "failed enumeration must mute nothing");
    expect(calls.empty(), "failed enumeration must touch nothing");
    expect(!std::filesystem::exists(dir / L"audio-session-mutes.txt"), "failed enumeration must journal nothing");
}

void testSweepRestoresOnlyStillMuted() {
    const auto dir = testDir("sweep");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\n222\t0\tc:\\apps\\dj.exe\n333\t1\tc:\\apps\\gone.exe\n";
    }
    SessionMuteStore store(journal);
    std::vector<bool> gameCalls;
    std::vector<bool> djCalls;
    std::vector<SessionMuteCandidate> sessions;
    sessions.push_back(makeSession(111, L"c:\\apps\\game.exe", true, gameCalls));
    sessions.push_back(makeSession(222, L"c:\\apps\\dj.exe", false, djCalls)); // user unmuted by hand

    const auto result = store.sweepOrphans([&](std::vector<SessionMuteCandidate>& out) {
        out = sessions;
        return true;
    });
    expect(result.has_value(), "readable journal must run the sweep");
    expect(result->restored == 1, "only the still-muted session must be restored");
    expect(result->unmatched == 2, "unreached entries must be reported, not dropped");
    expect(gameCalls == std::vector<bool>{false}, "still-muted session restored to its previous state");
    expect(djCalls.empty(), "session the user unmuted by hand must be left alone");

    const auto remaining = SessionMuteStore::parse([&] {
        std::ifstream stream(journal, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }());
    expect(remaining.size() == 2, "unreached entries must be rewritten for the next launch");
}

void testSweepWithoutJournalIsNoop() {
    const auto dir = testDir("nosweep");
    SessionMuteStore store(dir / L"audio-session-mutes.txt");
    const auto result = store.sweepOrphans([](std::vector<SessionMuteCandidate>&) { return true; });
    expect(!result.has_value(), "missing journal must not run a sweep");
    expect(!std::filesystem::exists(dir / L"audio-session-mutes.txt"), "missing journal must not be created");
}

void testSweepEnumerationFailureKeepsJournal() {
    const auto dir = testDir("sweepfail");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\n";
    }
    SessionMuteStore store(journal);
    const auto result = store.sweepOrphans([](std::vector<SessionMuteCandidate>&) { return false; });
    expect(!result.has_value(), "failed enumeration must not run a sweep");
    expect(std::filesystem::exists(journal), "failed enumeration must leave the journal for the next launch");
}

void testEmergencyRestoreSkipsWhenLocked() {
    const auto dir = testDir("emergency");
    SessionMuteStore store(dir / L"audio-session-mutes.txt");
    std::vector<bool> calls;
    std::vector<SessionMuteCandidate> sessions;
    sessions.push_back(makeSession(111, L"c:\\apps\\game.exe", false, calls));
    store.apply(
        {L"c:\\apps\\game.exe"},
        [&](std::vector<SessionMuteCandidate>& out) {
            out = sessions;
            return true;
        },
        [](uint32_t) { return true; });

    // The crash path only try_locks: hold the lock here and prove a concurrent
    // restore skips instead of blocking or touching anything.
    auto held = store.tryAcquire();
    expect(held.owns_lock(), "test setup must hold the store lock");
    std::thread crasher([&] { store.emergencyRestore(); });
    crasher.join();
    expect(calls == std::vector<bool>{true}, "locked emergency restore must touch nothing");
    held.unlock();

    store.emergencyRestore();
    expect(calls == (std::vector<bool>{true, false}), "unlocked emergency restore must restore");
    // The journal stays: entries the crash path could not reach are the next
    // launch's sweep to undo.
    expect(std::filesystem::exists(dir / L"audio-session-mutes.txt"), "emergency restore must leave the journal");
}

} // namespace

int main() {
    try {
        testSerializeParseRoundTrip();
        testParseSkipsMalformedLines();
        testApplyMutesWantedAndJournals();
        testApplyEmptyRestoresAndDeletesJournal();
        testApplyPrunesExitedProcess();
        testApplyEnumerationFailureLeavesEverything();
        testSweepRestoresOnlyStillMuted();
        testSweepWithoutJournalIsNoop();
        testSweepEnumerationFailureKeepsJournal();
        testEmergencyRestoreSkipsWhenLocked();
        std::cout << "SessionMuteStore tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SessionMuteStore test failure: " << error.what() << '\n';
        return 1;
    }
}
