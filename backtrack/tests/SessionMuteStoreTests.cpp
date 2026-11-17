#include "audio/SessionMuteStore.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using backtrack::SessionMuteApplyResult;
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

std::string readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::vector<SessionMuteRecord> readJournal(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return {};
    }
    return SessionMuteStore::parse(readFile(path));
}

// Fake live session. The store only ever sees the candidate built here, which
// mirrors production COM ownership: the callback (and the interface it holds)
// is valid only for the enumeration that produced it.
struct FakeSession {
    uint32_t pid = 0;
    std::wstring key;
    std::wstring endpoint = L"{endpoint-0}";
    std::wstring instance;
    bool muted = false;
    bool muteStateKnown = true;
    bool failSetMute = false;
    std::vector<bool>* calls = nullptr;
    std::function<void()> beforeSetMute;
};

// Every enumeration gets a generation. A callback from an older generation is
// stale: in production that is a COM interface invoked or released outside the
// apartment the enumeration ran on, which is exactly what the store must not do.
int gEnumerationGeneration = 0;
int gStaleCallbackCalls = 0;
int gEnumerations = 0;

SessionMuteStore::SessionEnumerator enumeratorFor(std::vector<FakeSession>& sessions) {
    return [&sessions](std::vector<SessionMuteCandidate>& out) {
        ++gEnumerations;
        const int generation = ++gEnumerationGeneration;
        out.clear();
        for (auto& session : sessions) {
            out.push_back(SessionMuteCandidate{
                session.pid,
                session.key,
                session.endpoint,
                session.instance,
                session.muted,
                session.muteStateKnown,
                [&session, generation](bool mute) {
                    if (generation != gEnumerationGeneration) {
                        ++gStaleCallbackCalls;
                    }
                    if (session.beforeSetMute) {
                        session.beforeSetMute();
                    }
                    if (session.calls) {
                        session.calls->push_back(mute);
                    }
                    if (session.failSetMute) {
                        return false;
                    }
                    session.muted = mute;
                    return true;
                }});
        }
        return true;
    };
}

bool alwaysAlive(uint32_t) {
    return true;
}

void testSerializeParseRoundTrip() {
    const std::vector<SessionMuteRecord> records = {
        {1234, L"c:\\apps\\game.exe", false, L"{endpoint-0}", L"{session-a}"},
        {5678, L"c:\\apps\\music player.exe", true, L"{endpoint-1}", L"{session-b}"},
    };
    const std::string content = SessionMuteStore::serialize(records);
    expect(content ==
            "1234\t0\tc:\\apps\\game.exe\t{endpoint-0}\t{session-a}\n"
            "5678\t1\tc:\\apps\\music player.exe\t{endpoint-1}\t{session-b}\n",
        "journal line format changed");
    const auto parsed = SessionMuteStore::parse(content);
    expect(parsed.size() == 2, "round trip lost a record");
    expect(parsed[0].processId == 1234 && !parsed[0].previousMute && parsed[0].pathKey == L"c:\\apps\\game.exe",
        "round trip corrupted record 0");
    expect(parsed[0].endpointId == L"{endpoint-0}" && parsed[0].instanceId == L"{session-a}",
        "round trip lost record 0's live identity");
    expect(parsed[1].processId == 5678 && parsed[1].previousMute && parsed[1].pathKey == L"c:\\apps\\music player.exe",
        "round trip corrupted record 1");
    expect(parsed[1].endpointId == L"{endpoint-1}" && parsed[1].instanceId == L"{session-b}",
        "round trip lost record 1's live identity");

    // Legacy 3-column lines still load, with no live identity, and unknown
    // extra columns are ignored.
    const auto legacy = SessionMuteStore::parse("77\t1\tc:\\apps\\old.exe\n78\t0\tc:\\apps\\new.exe\t{ep}\t{id}\tignored\n");
    expect(legacy.size() == 2, "legacy and extended lines must both parse");
    expect(legacy[0].previousMute && legacy[0].pathKey == L"c:\\apps\\old.exe" &&
           legacy[0].endpointId.empty() && legacy[0].instanceId.empty(),
        "a 3-column line must parse with an empty live identity");
    expect(legacy[1].endpointId == L"{ep}" && legacy[1].instanceId == L"{id}",
        "the two known extra columns must parse and later ones must be ignored");
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
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(2);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;
    sessions[1].pid = 222;
    sessions[1].key = L"c:\\apps\\browser.exe";
    sessions[1].instance = L"{session-b}";
    sessions[1].calls = &calls;

    const auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.muted == 1, "apply must mute exactly the wanted session");
    expect(result.muteFailures == 0 && result.exclusionEstablished(), "a clean mute establishes exclusion");
    expect(calls == std::vector<bool>{true}, "wanted session must be muted");
    expect(!sessions[1].muted, "unwanted session must be left alone");
    const auto journaled = readJournal(journal);
    expect(journaled.size() == 1 && journaled[0].processId == 111 && !journaled[0].previousMute,
        "journal must hold the applied mute with its previous state");
    expect(journaled[0].endpointId == L"{endpoint-0}" && journaled[0].instanceId == L"{session-a}",
        "the journal must record which session instance was muted");
    expect(!std::filesystem::exists(dir / L"audio-session-mutes.txt.tmp"), "no torn journal temp may be left");
}

void testApplyJournalsIntentBeforeMutating() {
    const auto dir = testDir("intent");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    bool journaledBeforeMute = false;
    sessions[0].beforeSetMute = [&] {
        const auto records = readJournal(journal);
        journaledBeforeMute = records.size() == 1 && records[0].processId == 111;
    };

    const auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.exclusionEstablished(), "setup must mute");
    expect(journaledBeforeMute, "the intent must be on disk before the session is touched");
}

void testApplyEmptyRestoresAndDeletesJournal() {
    const auto dir = testDir("clear");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;

    store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(std::filesystem::exists(journal), "setup must journal the mute");

    const auto result = store.apply({}, enumeratorFor(sessions), alwaysAlive);
    expect(result.muted == 0, "empty apply must restore everything");
    expect(result.unresolved == 0, "a verified restore leaves nothing unresolved");
    expect(calls == (std::vector<bool>{true, false}), "previous unmuted state must be restored");
    expect(!std::filesystem::exists(journal), "clean restore must delete the journal");

    const auto clearResult = store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(clearResult.restored == 0 && clearResult.unresolved == 0, "clear with nothing applied must be a no-op");
}

void testTwoSessionsSamePidRestoreOwnState() {
    const auto dir = testDir("sweep-two-sessions");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\n"
               << "111\t1\tc:\\apps\\game.exe\n";
    }
    SessionMuteStore store(journal);
    std::vector<bool> callsA;
    std::vector<bool> callsB;
    std::vector<FakeSession> sessions(2);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].muted = true;
    sessions[0].calls = &callsA;
    sessions[1].pid = 111;
    sessions[1].key = L"c:\\apps\\game.exe";
    sessions[1].instance = L"{session-b}";
    sessions[1].muted = true;
    sessions[1].calls = &callsB;

    const auto result = store.sweepOrphans(enumeratorFor(sessions), alwaysAlive);
    expect(result.has_value() && result->restored == 2, "each record must restore one session");
    expect(result->unmatched == 0, "both records were matched");
    expect(callsA == std::vector<bool>{false}, "first session restored to its own previous state");
    expect(callsB == std::vector<bool>{true}, "second session restored to its own previous state");
    expect(!sessions[0].muted, "first session is unmuted");
    expect(sessions[1].muted, "second session keeps the user's own mute");
    expect(!std::filesystem::exists(journal), "a complete sweep retires the journal");
}

void testTwoSessionsSamePidWithReplacement() {
    const auto dir = testDir("replace");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> callsA;
    std::vector<bool> callsB;
    std::vector<bool> callsB2;
    std::vector<FakeSession> sessions(2);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &callsA;
    sessions[1].pid = 111;
    sessions[1].key = L"c:\\apps\\game.exe";
    sessions[1].instance = L"{session-b}";
    sessions[1].calls = &callsB;

    auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.muted == 2, "both same-PID sessions must be muted");
    expect(result.exclusionEstablished(), "both same-PID sessions are excluded");
    expect(callsA == std::vector<bool>{true} && callsB == std::vector<bool>{true}, "each session muted once");
    expect(readJournal(journal).size() == 2, "both sessions are journaled");

    // Session B restarts: same process and path, new instance. The old mute
    // died with the old session, and the replacement must be muted on its own.
    FakeSession replacement;
    replacement.pid = 111;
    replacement.key = L"c:\\apps\\game.exe";
    replacement.instance = L"{session-b2}";
    replacement.calls = &callsB2;
    sessions[1] = replacement;

    result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.muted == 2, "the replacement session must be muted too");
    expect(result.exclusionEstablished(), "the replacement must not leave a gap");
    expect(callsB2 == std::vector<bool>{true}, "replacement session muted");
    expect(callsB == std::vector<bool>{true}, "the retired session must not be restored");
    expect(readJournal(journal).size() == 2, "journal holds the live sessions, not the retired one");

    const auto restored = store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(restored.restored == 2 && restored.unresolved == 0, "both live sessions restore");
    expect(callsA == (std::vector<bool>{true, false}), "session A restored");
    expect(callsB2 == (std::vector<bool>{true, false}), "replacement restored");
    expect(!std::filesystem::exists(journal), "clean restore deletes the journal");
}

void testApplyReassertsMuteClearedByApp() {
    const auto dir = testDir("reassert");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;

    store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    // The app resets its own session mute while the exclusion is still wanted.
    sessions[0].muted = false;
    const auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.exclusionEstablished(), "a cleared mute must be re-asserted");
    expect(calls == (std::vector<bool>{true, true}), "the mute is written again");
    expect(readJournal(journal).size() == 1, "re-asserting must not duplicate the record");
    store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(calls == (std::vector<bool>{true, true, false}), "restore still uses the original previous state");
}

void testMuteFailureSurfacedAndRetried() {
    const auto dir = testDir("mute-fail");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;
    sessions[0].failSetMute = true;

    auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.muteFailures == 1, "a failed mute must be surfaced");
    expect(!result.exclusionEstablished(), "a failed mute must not claim exclusion");
    expect(calls == std::vector<bool>{true}, "the mute was attempted");
    expect(!std::filesystem::exists(journal), "a mute that never happened must not stay journaled");

    sessions[0].failSetMute = false;
    result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.muteFailures == 0 && result.exclusionEstablished(), "the retry must establish exclusion");
    expect(result.muted == 1, "the retry mutes the session");
    store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(calls == (std::vector<bool>{true, true, false}), "retry then restore");
}

void testUnknownMuteStateIsNotMuted() {
    const auto dir = testDir("unknown-state");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;
    sessions[0].muteStateKnown = false;

    const auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.muteFailures == 1, "an unknown previous state is an unestablished exclusion");
    expect(!result.exclusionEstablished(), "unknown state must not claim exclusion");
    expect(calls.empty(), "an unverifiable session must not be touched");
    expect(!std::filesystem::exists(journal), "nothing was applied, so nothing is journaled");

    // A journal obligation against an unreadable session state is preserved.
    std::vector<FakeSession> muted(1);
    muted[0].pid = 222;
    muted[0].key = L"c:\\apps\\dj.exe";
    muted[0].instance = L"{session-dj}";
    muted[0].muted = true;
    muted[0].muteStateKnown = false;
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "222\t0\tc:\\apps\\dj.exe\n";
    }
    SessionMuteStore second(journal);
    const auto swept = second.sweepOrphans(enumeratorFor(muted), alwaysAlive);
    expect(swept.has_value() && swept->restored == 0, "an unverifiable session is not restored");
    expect(swept->unmatched == 1, "the obligation stays unmatched");
    expect(readJournal(journal).size() == 1, "the obligation stays journaled");
}

void testJournalWriteFailureSurfaced() {
    const auto dir = testDir("journal-fail");
    {
        std::ofstream blocker(dir / L"blocked", std::ios::binary);
        blocker << "not a directory";
    }
    // The parent is a file, so create_directories and the journal write fail.
    SessionMuteStore store(dir / L"blocked" / L"audio-session-mutes.txt");
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;

    const auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.journalFailed, "a journal write failure must reach the caller");
    expect(!result.exclusionEstablished(), "an unjournaled mute must not claim exclusion");
    expect(calls.empty(), "the session must not be muted when its intent cannot be journaled");
    expect(!sessions[0].muted, "the foreign session is untouched");
}

void testUnreadableJournalBlocksMutation() {
    const auto dir = testDir("journal-unreadable");
    const auto journal = dir / L"audio-session-mutes.txt";
    std::filesystem::create_directories(journal); // a directory cannot be read as a journal
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;

    const auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.journalFailed, "an unreadable journal must be reported");
    expect(calls.empty(), "overwriting an unreadable journal would drop its obligations");
    expect(std::filesystem::is_directory(journal), "the unreadable journal is left untouched");
}

void testFailedRestoreThenRetry() {
    const auto dir = testDir("restore-retry");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;

    store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    sessions[0].failSetMute = true;
    auto result = store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(result.unresolved == 1, "a failed restore must stay unresolved");
    expect(result.restored == 0, "nothing was restored");
    expect(std::filesystem::exists(journal), "the failed restore must keep its journal record");
    expect(sessions[0].muted, "the session is still muted");

    sessions[0].failSetMute = false;
    result = store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(result.restored == 1 && result.unresolved == 0, "the retry restores the session");
    expect(calls == (std::vector<bool>{true, false, false}), "the restore is retried");
    expect(!sessions[0].muted, "the session is unmuted after the retry");
    expect(!std::filesystem::exists(journal), "a verified restore retires the journal");
}

void testPartialSweepThenUnrelatedApplyAndClear() {
    const auto dir = testDir("partial-sweep");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\n"
               << "222\t0\tc:\\apps\\dj.exe\n";
    }
    SessionMuteStore store(journal);
    std::vector<bool> gameCalls;
    std::vector<bool> otherCalls;
    std::vector<FakeSession> sessions(2);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-game}";
    sessions[0].muted = true;
    sessions[0].calls = &gameCalls;
    sessions[1].pid = 333;
    sessions[1].key = L"c:\\apps\\editor.exe";
    sessions[1].instance = L"{session-editor}";
    sessions[1].calls = &otherCalls;

    const auto swept = store.sweepOrphans(enumeratorFor(sessions), alwaysAlive);
    expect(swept.has_value() && swept->restored == 1, "the reachable orphan is restored");
    expect(swept->unmatched == 1, "the unreachable orphan is kept");
    expect(gameCalls == std::vector<bool>{false}, "reachable orphan restored to its previous state");
    expect(readJournal(journal).size() == 1, "the unmatched record is rewritten");

    // An unrelated apply must not overwrite the unresolved startup record.
    const auto applied = store.apply({L"c:\\apps\\editor.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(applied.muted == 1 && applied.exclusionEstablished(), "the unrelated mute is applied");
    auto journaled = readJournal(journal);
    expect(journaled.size() == 2, "apply must preserve the unresolved orphan");
    const bool keptOrphan = journaled[0].processId == 222 || journaled[1].processId == 222;
    const bool keptMute = journaled[0].processId == 333 || journaled[1].processId == 333;
    expect(keptOrphan && keptMute, "the journal holds the orphan and the new mute");

    const auto cleared = store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(cleared.restored == 1, "the unrelated mute is restored");
    expect(cleared.unresolved == 1, "the unreachable orphan stays");
    expect(otherCalls == (std::vector<bool>{true, false}), "the unrelated mute round trips");
    journaled = readJournal(journal);
    expect(journaled.size() == 1 && journaled[0].processId == 222, "only the orphan remains");

    // The process is gone now, so its session and mute are gone too.
    const auto retired = store.clear(enumeratorFor(sessions), [](uint32_t) { return false; });
    expect(retired.unresolved == 0, "a dead process's obligation is retired");
    expect(!std::filesystem::exists(journal), "a fully resolved journal is deleted");
}

void testCompleteSweepRetiresJournalAndFreshStore() {
    const auto dir = testDir("sweep-complete");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\n";
    }
    {
        SessionMuteStore store(journal);
        std::vector<bool> calls;
        std::vector<FakeSession> sessions(1);
        sessions[0].pid = 111;
        sessions[0].key = L"c:\\apps\\game.exe";
        sessions[0].instance = L"{session-a}";
        sessions[0].muted = true;
        sessions[0].calls = &calls;

        const auto swept = store.sweepOrphans(enumeratorFor(sessions), alwaysAlive);
        expect(swept.has_value() && swept->restored == 1 && swept->unmatched == 0, "the sweep restores the orphan");
        expect(!std::filesystem::exists(journal),
            "a complete sweep must retire the journal even though the write cache started empty");
    }

    // A fresh process sees no journal and starts from scratch.
    SessionMuteStore fresh(journal);
    expect(!fresh.sweepOrphans([](std::vector<SessionMuteCandidate>&) { return true; }, alwaysAlive).has_value(),
        "a retired journal must not run a sweep");
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;
    const auto applied = fresh.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(applied.exclusionEstablished(), "a fresh store still applies mutes");
    expect(std::filesystem::exists(journal), "the fresh store journals its own mute");
    fresh.clear(enumeratorFor(sessions), alwaysAlive);
    expect(!std::filesystem::exists(journal), "the fresh store cleans up its journal");
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
    std::vector<FakeSession> sessions(2);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-game}";
    sessions[0].muted = true;
    sessions[0].calls = &gameCalls;
    sessions[1].pid = 222;
    sessions[1].key = L"c:\\apps\\dj.exe";
    sessions[1].instance = L"{session-dj}";
    sessions[1].muted = false; // user unmuted by hand
    sessions[1].calls = &djCalls;

    const auto result = store.sweepOrphans(enumeratorFor(sessions), alwaysAlive);
    expect(result.has_value(), "readable journal must run the sweep");
    expect(result->restored == 1, "only the still-muted session must be restored");
    expect(result->unmatched == 1, "only the record with no live session stays unmatched");
    expect(gameCalls == std::vector<bool>{false}, "still-muted session restored to its previous state");
    expect(djCalls.empty(), "session the user unmuted by hand must be left alone");

    const auto remaining = readJournal(journal);
    expect(remaining.size() == 1 && remaining[0].processId == 333,
        "the user-unmuted record is retired; the unreachable one is rewritten");
}

void testSweepRetiresDeadProcess() {
    const auto dir = testDir("sweep-dead");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        // Live identity recorded, but neither the process nor a replacement
        // session survives it.
        stream << "333\t1\tc:\\apps\\gone.exe\t{endpoint-0}\t{session-gone}\n";
    }
    SessionMuteStore store(journal);
    const auto result = store.sweepOrphans(
        [](std::vector<SessionMuteCandidate>&) { return true; },
        [](uint32_t) { return false; });
    expect(result.has_value() && result->unmatched == 0, "a dead process's record is retired");
    expect(!std::filesystem::exists(journal), "the obsolete journal is deleted");
}

void testClearRestoresTheJournaledSessionOnly() {
    // One process, two render sessions: A muted by the user, B muted by
    // Backtrack. Recovery must unmute B, not the first session with that pid
    // and path.
    const auto dir = testDir("two-sessions");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\t{endpoint-0}\t{session-b}\n";
    }
    SessionMuteStore store(journal);
    std::vector<bool> callsA;
    std::vector<bool> callsB;
    std::vector<FakeSession> sessions(2);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].muted = true;
    sessions[0].calls = &callsA;
    sessions[1].pid = 111;
    sessions[1].key = L"c:\\apps\\game.exe";
    sessions[1].instance = L"{session-b}";
    sessions[1].muted = true;
    sessions[1].calls = &callsB;

    const auto result = store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(result.restored == 1 && result.unresolved == 0, "the journaled session is restored");
    expect(callsB == std::vector<bool>{false}, "the session Backtrack muted is unmuted");
    expect(callsA.empty() && sessions[0].muted, "the user's own mute must stay untouched");
    expect(!std::filesystem::exists(journal), "a verified restore retires the journal");
}

void testSweepRestoresInheritedMuteAfterRestart() {
    // The recorded process is gone; its replacement restarted into the same
    // path on the same endpoint and inherited the persisted mute.
    const auto dir = testDir("restart");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "999\t0\tc:\\apps\\game.exe\t{endpoint-0}\t{session-old}\n";
    }
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-new}";
    sessions[0].muted = true;
    sessions[0].calls = &calls;

    const auto result = store.sweepOrphans(enumeratorFor(sessions),
        [](uint32_t processId) { return processId != 999; });
    expect(result.has_value() && result->restored == 1, "the replacement session inherits the restore");
    expect(result->unmatched == 0, "the record is retired with the restore");
    expect(calls == std::vector<bool>{false}, "the inherited mute is restored to the recorded state");
    expect(!std::filesystem::exists(journal), "the retired journal is deleted");
}

void testSweepReleasesRecordWhenItsInstanceVanished() {
    // The process lives on with a sibling session, but the exact instance the
    // record names is gone. Windows keeps a mute per session, so the record is
    // retired instead of unmuting whichever session the pid and path match.
    const auto dir = testDir("instance-gone");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\t{endpoint-0}\t{session-gone}\n";
    }
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-sibling}";
    sessions[0].muted = true;
    sessions[0].calls = &calls;

    const auto result = store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(result.restored == 0 && result.unresolved == 0, "a vanished instance releases its record");
    expect(calls.empty() && sessions[0].muted, "a sibling session must not be unmuted");
    expect(!std::filesystem::exists(journal), "the released record is deleted");
}

void testJournalWriteKeepsForeignFileAtLegacyTempName() {
    const auto dir = testDir("temp-name");
    const auto journal = dir / L"audio-session-mutes.txt";
    const auto legacyTemp = dir / L"audio-session-mutes.txt.tmp";
    const auto linked = dir / L"unrelated.txt";
    {
        std::ofstream stream(legacyTemp, std::ios::binary);
        stream << "another file's contents";
    }
    std::error_code linkError;
    std::filesystem::create_hard_link(legacyTemp, linked, linkError);
    expect(!linkError, "test setup must create the hard link");
    const std::string legacyBytes = readFile(legacyTemp);
    const std::string linkedBytes = readFile(linked);

    SessionMuteStore store(journal);
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    const auto result = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(result.exclusionEstablished(), "setup must write the journal");
    expect(!readFile(journal).empty(), "the journal was written");
    expect(readFile(legacyTemp) == legacyBytes && readFile(linked) == linkedBytes,
        "a file (or its hard link) at the old .tmp name must survive a journal write");
}

void testSweepEnumerationFailureKeepsJournal() {
    const auto dir = testDir("sweepfail");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\n";
    }
    SessionMuteStore store(journal);
    const auto result = store.sweepOrphans(
        [](std::vector<SessionMuteCandidate>&) { return false; }, alwaysAlive);
    expect(!result.has_value(), "failed enumeration must not run a sweep");
    expect(std::filesystem::exists(journal), "failed enumeration must leave the journal for the next launch");
}

void testApplyEnumerationFailureLeavesEverything() {
    const auto dir = testDir("enumfail");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;

    const auto result = store.apply(
        {L"c:\\apps\\game.exe"},
        [](std::vector<SessionMuteCandidate>&) { return false; },
        alwaysAlive);
    expect(result.enumerationFailed, "failed enumeration must be reported");
    expect(!result.exclusionEstablished(), "failed enumeration cannot establish exclusion");
    expect(calls.empty(), "failed enumeration must touch nothing");
    expect(!std::filesystem::exists(journal), "failed enumeration must journal nothing");

    // With a live mute already applied, a failed enumeration must not overwrite
    // or restore it: the journal still describes it.
    store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    const auto before = readFile(journal);
    const auto failed = store.apply(
        {L"c:\\apps\\game.exe"},
        [](std::vector<SessionMuteCandidate>&) { return false; },
        alwaysAlive);
    expect(failed.enumerationFailed && failed.muted == 1, "the live mute is still reported");
    expect(readFile(journal) == before, "a failed enumeration must leave the journal byte-for-byte");
    expect(sessions[0].muted, "a failed enumeration must not unmute");
}

void testApplyAdoptsOrphanForWantedSession() {
    const auto dir = testDir("adopt");
    const auto journal = dir / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        // The user had this app muted before the previous process muted it too.
        stream << "111\t1\tc:\\apps\\game.exe\n";
    }
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].muted = true;
    sessions[0].calls = &calls;

    const auto applied = store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    expect(applied.muted == 1 && applied.exclusionEstablished(), "the obligation becomes a live mute");
    expect(applied.unresolved == 0, "adoption resolves the orphan");
    expect(calls.empty(), "an already muted session is not touched again");
    const auto journaled = readJournal(journal);
    expect(journaled.size() == 1 && journaled[0].previousMute,
        "the journaled previous state survives the adoption");

    const auto cleared = store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(cleared.restored == 1 && cleared.unresolved == 0, "the adopted mute restores");
    expect(calls == std::vector<bool>{true}, "the user's own mute is restored, not cleared");
    expect(sessions[0].muted, "the session is still muted as the user left it");
    expect(!std::filesystem::exists(journal), "the journal is retired");
}

void testEmergencyRestoreSkipsWhenLocked() {
    const auto dir = testDir("emergency");
    const auto journal = dir / L"audio-session-mutes.txt";
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;

    store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);

    // The crash path only try_locks: hold the lock here and prove a concurrent
    // restore skips instead of blocking or touching anything.
    auto held = store.tryAcquire();
    expect(held.owns_lock(), "test setup must hold the store lock");
    std::thread crasher([&] { store.emergencyRestore(enumeratorFor(sessions)); });
    crasher.join();
    expect(calls == std::vector<bool>{true}, "locked emergency restore must touch nothing");
    held.unlock();

    store.emergencyRestore(enumeratorFor(sessions));
    expect(calls == (std::vector<bool>{true, false}), "unlocked emergency restore must restore");
    // Restored records are retired; a failed write would leave the journal, so
    // either way the next launch is safe.
    expect(!std::filesystem::exists(journal), "a complete emergency restore retires the journal");
}

void testSurvivingLiveClaimsAndEmergencyPairing() {
    for (int path = 0; path < 4; ++path) {
        const auto journal = testDir("live-claims") / L"audio-session-mutes.txt";
        {
            std::ofstream stream(journal, std::ios::binary);
            stream << "111\t0\tc:\\apps\\game.exe\n111\t1\tc:\\apps\\game.exe\n";
        }
        SessionMuteStore store(journal);
        std::vector<bool> calls;
        std::vector<FakeSession> sessions(1);
        sessions[0].pid = 111;
        sessions[0].key = L"c:\\apps\\game.exe";
        sessions[0].instance = L"{session-a}";
        sessions[0].muted = true;
        sessions[0].calls = &calls;
        const auto adopted = store.apply({sessions[0].key}, enumeratorFor(sessions), alwaysAlive);
        expect(adopted.muted == 1 && adopted.unresolved == 1 && calls.empty(),
            "setup must adopt one record and leave the other orphan unclaimed");
        sessions[0].failSetMute = true;

        if (path == 0) {
            const auto result = store.apply({sessions[0].key}, enumeratorFor(sessions), alwaysAlive);
            expect(result.muted == 1 && result.unresolved == 1 && calls.empty(),
                "apply must seed claims from its live mutes");
        } else if (path == 1) {
            const auto result = store.clear(enumeratorFor(sessions), alwaysAlive);
            expect(result.restored == 0 && result.unresolved == 2 && calls == std::vector<bool>{false},
                "clear must not pair an orphan with a surviving failed live restore");
        } else if (path == 2) {
            const auto result = store.sweepOrphans(enumeratorFor(sessions), alwaysAlive);
            expect(result && result->restored == 0 && result->unmatched == 1 && calls.empty(),
                "sweep must seed claims from surviving live mutes");
        } else {
            store.emergencyRestore(enumeratorFor(sessions));
            expect(calls == (std::vector<bool>{false, true}),
                "emergency pairing must start empty even when a live restore failed");
        }
        const auto records = readJournal(journal);
        expect(records.size() == 2 && !records[0].previousMute && records[1].previousMute,
            "unresolved live and orphan records must retain their own previous states");
    }
}

void testClearAndEmergencyPairOrphansOneToOne() {
    for (bool emergency : {false, true}) {
        const auto journal = testDir("orphan-pairing") / L"audio-session-mutes.txt";
        {
            std::ofstream stream(journal, std::ios::binary);
            stream << "111\t0\tc:\\apps\\game.exe\n111\t1\tc:\\apps\\game.exe\n"
                   << "222\t0\tc:\\apps\\gone.exe\n";
        }
        SessionMuteStore store(journal);
        std::vector<bool> callsA;
        std::vector<bool> callsB;
        std::vector<FakeSession> sessions(2);
        for (auto& session : sessions) {
            session.pid = 111;
            session.key = L"c:\\apps\\game.exe";
            session.instance = L"{same-instance}";
            session.muted = true;
        }
        sessions[1].endpoint = L"{endpoint-1}";
        sessions[0].calls = &callsA;
        sessions[1].calls = &callsB;
        if (emergency) {
            store.emergencyRestore(enumeratorFor(sessions));
        } else {
            const auto result = store.clear(enumeratorFor(sessions), alwaysAlive);
            expect(result.restored == 2 && result.unresolved == 1, "clear must count both orphan restores");
        }
        expect(callsA == std::vector<bool>{false} && callsB == std::vector<bool>{true},
            "orphan claims must distinguish endpoints and restore each record's own state");
        const auto remaining = readJournal(journal);
        expect(remaining.size() == 1 && remaining[0].processId == 222,
            "an unreachable orphan must survive, including on the crash path without liveness pruning");
    }
}

void testEmergencyPreservesUnverifiedOrphans() {
    const auto journal = testDir("emergency-unverified") / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\unknown.exe\n222\t1\tc:\\apps\\unmuted.exe\n"
               << "333\t0\tc:\\apps\\failed.exe\n444\t0\tc:\\apps\\missing-callback.exe\n";
    }
    SessionMuteStore store(journal);
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(3);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\unknown.exe";
    sessions[0].muteStateKnown = false;
    sessions[1].pid = 222;
    sessions[1].key = L"c:\\apps\\unmuted.exe";
    sessions[2].pid = 333;
    sessions[2].key = L"c:\\apps\\failed.exe";
    sessions[2].muted = true;
    sessions[2].failSetMute = true;
    for (auto& session : sessions) {
        session.instance = session.key;
        session.calls = &calls;
    }
    auto enumerate = enumeratorFor(sessions);
    store.emergencyRestore([&](std::vector<SessionMuteCandidate>& out) {
        enumerate(out);
        out.push_back({444, L"c:\\apps\\missing-callback.exe", L"endpoint", L"instance", true, true, {}});
        return true;
    });
    expect(calls == std::vector<bool>{false}, "only the known still-muted orphan with a callback is attempted");
    const auto remaining = readJournal(journal);
    expect(remaining.size() == 3 && remaining[0].processId == 111 &&
           remaining[1].processId == 333 && remaining[2].processId == 444,
        "unknown state, failed restore and missing callback must stay journaled; user-unmuted must retire");
}

void testEmergencyContainsFailuresAndCanRetry() {
    const auto journal = testDir("emergency-exceptions") / L"audio-session-mutes.txt";
    {
        std::ofstream stream(journal, std::ios::binary);
        stream << "111\t0\tc:\\apps\\game.exe\n";
    }
    SessionMuteStore store(journal);
    const auto before = readFile(journal);
    store.emergencyRestore([](std::vector<SessionMuteCandidate>&) { return false; });
    expect(readFile(journal) == before, "failed emergency enumeration must leave the journal unchanged");
    store.emergencyRestore([](std::vector<SessionMuteCandidate>&) -> bool {
        throw std::runtime_error("enumeration failure");
    });
    expect(readFile(journal) == before, "throwing emergency enumeration must leave the journal unchanged");

    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].muted = true;
    sessions[0].beforeSetMute = [] { throw std::runtime_error("restore failure"); };
    store.emergencyRestore(enumeratorFor(sessions));
    expect(readFile(journal) == before && sessions[0].muted,
        "throwing emergency restore must preserve its obligation");
    {
        auto held = store.tryAcquire();
        expect(held.owns_lock(), "an emergency exception must release the store lock");
    }
    sessions[0].beforeSetMute = {};
    store.emergencyRestore(enumeratorFor(sessions));
    expect(!sessions[0].muted && !std::filesystem::exists(journal),
        "emergency restore must remain retryable after exceptions");
}

void testCallbacksNeverOutliveTheirEnumeration() {
    // Production candidates hold ISimpleAudioVolume proxies. Invoking one from
    // an older enumeration would run it outside the apartment that created it;
    // the store must instead re-enumerate for every restore.
    const auto dir = testDir("apartment");
    SessionMuteStore store(dir / L"audio-session-mutes.txt");
    std::vector<bool> calls;
    std::vector<FakeSession> sessions(1);
    sessions[0].pid = 111;
    sessions[0].key = L"c:\\apps\\game.exe";
    sessions[0].instance = L"{session-a}";
    sessions[0].calls = &calls;

    store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    store.apply({L"c:\\apps\\game.exe"}, enumeratorFor(sessions), alwaysAlive);
    const int enumerationsBeforeRestore = gEnumerations;
    store.clear(enumeratorFor(sessions), alwaysAlive);
    expect(gEnumerations == enumerationsBeforeRestore + 1,
        "a restore must enumerate the live sessions instead of reusing a stored callback");
    expect(gStaleCallbackCalls == 0, "a callback must never be invoked after its enumeration returned");
    expect(calls == (std::vector<bool>{true, false}), "the restore used the live enumeration");
}

} // namespace

int main() {
    try {
        testSerializeParseRoundTrip();
        testParseSkipsMalformedLines();
        testApplyMutesWantedAndJournals();
        testApplyJournalsIntentBeforeMutating();
        testApplyEmptyRestoresAndDeletesJournal();
        testTwoSessionsSamePidRestoreOwnState();
        testTwoSessionsSamePidWithReplacement();
        testApplyReassertsMuteClearedByApp();
        testMuteFailureSurfacedAndRetried();
        testUnknownMuteStateIsNotMuted();
        testJournalWriteFailureSurfaced();
        testUnreadableJournalBlocksMutation();
        testFailedRestoreThenRetry();
        testPartialSweepThenUnrelatedApplyAndClear();
        testCompleteSweepRetiresJournalAndFreshStore();
        testSweepRestoresOnlyStillMuted();
        testSweepRetiresDeadProcess();
        testClearRestoresTheJournaledSessionOnly();
        testSweepRestoresInheritedMuteAfterRestart();
        testSweepReleasesRecordWhenItsInstanceVanished();
        testJournalWriteKeepsForeignFileAtLegacyTempName();
        testSweepEnumerationFailureKeepsJournal();
        testApplyEnumerationFailureLeavesEverything();
        testApplyAdoptsOrphanForWantedSession();
        testEmergencyRestoreSkipsWhenLocked();
        testSurvivingLiveClaimsAndEmergencyPairing();
        testClearAndEmergencyPairOrphansOneToOne();
        testEmergencyPreservesUnverifiedOrphans();
        testEmergencyContainsFailuresAndCanRetry();
        testCallbacksNeverOutliveTheirEnumeration();
        std::cout << "SessionMuteStore tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SessionMuteStore test failure: " << error.what() << '\n';
        return 1;
    }
}
