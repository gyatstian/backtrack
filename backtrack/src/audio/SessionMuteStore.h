#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace backtrack {

// One live render session seen during a mute sweep. `muted` is the mute state
// at enumeration time: the previous state apply() records, and the current
// state sweepOrphans() checks before touching anything.
struct SessionMuteCandidate {
    uint32_t processId = 0;
    std::wstring pathKey;
    bool muted = false;
    // Sets the session mute, true on success. Holds the session volume alive:
    // the production callback captures the COM pointer, so the session cannot
    // die between enumeration and restore.
    std::function<bool(bool muted)> setMute;
};

// Journaled record of one applied foreign-session mute. One line per record:
// "<pid>\t<previousMute 0|1>\t<pathKey>" with the key as UTF-8.
struct SessionMuteRecord {
    uint32_t processId = 0;
    std::wstring pathKey;
    bool previousMute = false;
};

struct SessionMuteSweepResult {
    uint32_t restored = 0;
    size_t unmatched = 0;
};

// Crash-safe mirror of applied foreign-session mutes. Mutes are global system
// state: they outlive this process if it dies between muting a foreign session
// and restoring it, so every applied mute is journaled to disk before the next
// session is walked, and a later start sweeps what a dead session left muted.
class SessionMuteStore {
public:
    explicit SessionMuteStore(std::filesystem::path journalPath);
    SessionMuteStore(const SessionMuteStore&) = delete;
    SessionMuteStore& operator=(const SessionMuteStore&) = delete;

    // Pure journal codec, no I/O. Malformed lines (missing tabs, pid 0, empty
    // key) are skipped so one torn line cannot poison the whole journal.
    static std::string serialize(const std::vector<SessionMuteRecord>& records);
    static std::vector<SessionMuteRecord> parse(const std::string& content);

    // Mutes live sessions in wantedKeys, restores any previously applied mute
    // that is stale (no longer wanted, process exited, handle lost). Returns
    // the applied mute count. Holds the lock across the whole enumeration and
    // journals each new mute before moving on, so the crash path (which only
    // try_locks) skips a mid-walk sweep and the next launch replays the
    // journal instead. Silent 0 when enumeration itself fails: nothing was
    // pruned and nothing was applied.
    uint32_t apply(const std::vector<std::wstring>& wantedKeys,
                   std::function<bool(std::vector<SessionMuteCandidate>&)> enumerateSessions,
                   std::function<bool(uint32_t)> isProcessAlive);

    // Restores every applied mute and deletes the journal (clean-restore path).
    void clear();

    // Startup sweep for mutes a previous crashed session left behind. Reads
    // the journal without the lock (the single-instance mutex is the guard
    // here, as a concurrent session could otherwise re-mute mid-sweep) and
    // locks only for the final journal delete/rewrite. Restores only sessions
    // still muted, so a user who unmuted one by hand keeps that. nullopt when
    // no sweep ran: no journal, unreadable journal, or failed enumeration. In
    // all three cases the journal is left untouched for the next launch.
    std::optional<SessionMuteSweepResult> sweepOrphans(
        std::function<bool(std::vector<SessionMuteCandidate>&)> enumerateSessions);

    // Crash-path restore (unhandled exception / terminate / ctrl handler).
    // Best effort and noexcept: skips when another thread holds the lock, and
    // never lets a failure escape into a crashing process. Leaves the journal
    // file alone: entries it could not reach stay journaled for the next
    // launch's sweep.
    void emergencyRestore() noexcept;

    // Exposes the lock so tests can hold it and prove the crash path skips.
    std::unique_lock<std::mutex> tryAcquire() {
        return std::unique_lock<std::mutex>(mutex_, std::try_to_lock);
    }

    const std::filesystem::path& journalPath() const {
        return journalPath_;
    }

private:
    struct AppliedMute {
        SessionMuteRecord record;
        std::function<bool(bool)> setMute;
    };

    void persistLocked();
    void writeJournalLocked(const std::string& content);
    void restoreEntriesLocked();
    // nullopt when the journal is missing or unreadable. A readable journal
    // with zero usable lines still returns an empty vector: the sweep runs
    // and consumes it, exactly like the pre-extraction code did.
    std::optional<std::vector<SessionMuteRecord>> readJournal() const;

    std::filesystem::path journalPath_;
    std::mutex mutex_;
    std::vector<AppliedMute> mutes_;
    std::string lastWritten_;
};
} // namespace backtrack
