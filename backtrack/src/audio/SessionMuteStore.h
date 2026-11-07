#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace backtrack {

// One live render session seen during a mute sweep. A process can own several
// sessions at once - one per render endpoint, plus a replacement after it
// recreates its stream - so the live identity is the session instance, not the
// PID. PID-only dedupe would skip a same-PID session and leave its audio in the
// recording.
struct SessionMuteCandidate {
    uint32_t processId = 0;
    std::wstring pathKey;
    // IMMDevice::GetId of the endpoint the session renders on.
    std::wstring endpointId;
    // IAudioSessionControl2::GetSessionInstanceIdentifier. Live-only: Windows
    // does not keep it stable across an application restart, so the journal
    // stores the durable (PID, path key) identity instead.
    std::wstring instanceId;
    bool muted = false;
    // False when GetMute failed. The state to restore to is unknown, so the
    // store refuses to mute the session rather than make an unverifiable
    // change; the caller then has to treat the exclusion as not established.
    bool muteStateKnown = true;
    // Sets the session mute, true on success. Only valid during the
    // enumeration call that produced it: the store never retains the callback,
    // so no COM interface outlives the apartment the enumeration ran on.
    std::function<bool(bool muted)> setMute;
};

// Journaled record of one applied foreign-session mute. One line per record:
// "<pid>\t<previousMute 0|1>\t<pathKey>" with the key as UTF-8. The identity is
// the durable one: session instance identifiers do not survive an application
// restart, so a crash sweep matches records to live sessions by process id and
// image path, one record per still-muted session.
struct SessionMuteRecord {
    uint32_t processId = 0;
    std::wstring pathKey;
    bool previousMute = false;
};

struct SessionMuteApplyResult {
    // Live foreign sessions this store currently holds muted for the request.
    uint32_t muted = 0;
    // Wanted sessions that could not be muted: SetMute failed, the previous
    // state was unknown, the session had no live identity, or its mute could
    // not be journaled first.
    size_t muteFailures = 0;
    // Records (journal obligations or failed live restores) still waiting for
    // a retry. Retrying them never blocks exclusion; they are about restoring
    // the foreign app's own state.
    size_t unresolved = 0;
    bool enumerationFailed = false;
    bool journalFailed = false;

    // True only when every wanted session is verifiably muted and every
    // journal write succeeded. The system-audio path must not record while it
    // is false: the user asked to keep those apps out of the recording.
    bool exclusionEstablished() const {
        return !enumerationFailed && !journalFailed && muteFailures == 0;
    }
};

struct SessionMuteRestoreResult {
    uint32_t restored = 0;
    size_t unresolved = 0;
    bool journalFailed = false;
};

struct SessionMuteSweepResult {
    uint32_t restored = 0;
    size_t unmatched = 0;
    bool journalFailed = false;
};

// Crash-safe mirror of applied foreign-session mutes. Mutes are global system
// state: they outlive this process if it dies between muting a foreign session
// and restoring it, so the intent is journaled to disk before the session is
// touched, and a later start sweeps what a dead session left muted.
//
// The store holds no COM interface. Every enumeration callback is invoked
// inside the call that produced it, so the caller keeps COM initialized across
// enumeration, callback invocation, and release of the temporary interfaces
// (see the WasapiCapture session-mute entry points).
class SessionMuteStore {
public:
    using SessionEnumerator = std::function<bool(std::vector<SessionMuteCandidate>&)>;

    explicit SessionMuteStore(std::filesystem::path journalPath);
    SessionMuteStore(const SessionMuteStore&) = delete;
    SessionMuteStore& operator=(const SessionMuteStore&) = delete;

    // Pure journal codec, no I/O. Malformed lines (missing tabs, pid 0, empty
    // key) are skipped so one torn line cannot poison the whole journal.
    static std::string serialize(const std::vector<SessionMuteRecord>& records);
    static std::vector<SessionMuteRecord> parse(const std::string& content);

    // Mutes every live session in wantedKeys, restores live mutes that are no
    // longer wanted, and re-asserts a mute the app or the user cleared. Each
    // intent is journaled before the session is touched. Restores every
    // journal obligation that can be resolved against this enumeration and
    // keeps the rest for the next call. The lock is held across enumeration,
    // journal writes, and every callback, so the crash path (which only
    // try_locks) skips a mid-walk sweep and the next launch replays the
    // journal instead.
    SessionMuteApplyResult apply(const std::vector<std::wstring>& wantedKeys,
                                 SessionEnumerator enumerateSessions,
                                 std::function<bool(uint32_t)> isProcessAlive);

    // Restores every live mute and journal obligation that can be resolved
    // against this enumeration. Records that could not be verified stay
    // journaled for the next launch. A fully restored journal is deleted.
    SessionMuteRestoreResult clear(SessionEnumerator enumerateSessions,
                                   std::function<bool(uint32_t)> isProcessAlive);

    // Startup sweep for mutes a previous crashed session left behind. Reads the
    // journal, restores every still-muted session whose record matches, and
    // retires records whose process is gone. nullopt when no sweep ran: no
    // journal, unreadable journal, or failed enumeration. In all three cases
    // the journal is left untouched for the next launch.
    std::optional<SessionMuteSweepResult> sweepOrphans(SessionEnumerator enumerateSessions,
                                                       std::function<bool(uint32_t)> isProcessAlive);

    // Crash-path restore (unhandled exception / terminate / ctrl handler).
    // Best effort and noexcept: skips when another thread holds the lock, and
    // never lets a failure escape into a crashing process. Restored records are
    // retired on a best-effort journal write; entries it could not reach stay
    // journaled for the next launch's sweep.
    void emergencyRestore(SessionEnumerator enumerateSessions) noexcept;

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
        std::wstring endpointId;
        std::wstring instanceId;
    };

    // Reads the journal into orphans_ once. False when the journal exists but
    // cannot be read: overwriting it would drop obligations this process never
    // saw, so every mutating path refuses to continue.
    bool loadOrphansLocked();
    bool persistLocked();
    bool writeJournalLocked(const std::string& content);
    // Restores one live mute against its freshly enumerated session. Returns
    // true when the record can be retired: the previous state was set, the
    // session is already unmuted, or SetMute succeeded. False keeps the record
    // for the next attempt. `session` must not be null.
    static bool restoreMuteLocked(const AppliedMute& mute, const SessionMuteCandidate& session);

    std::filesystem::path journalPath_;
    std::mutex mutex_;
    std::vector<AppliedMute> mutes_;
    // Journal records from a previous process that this process has not
    // resolved yet. They stay journaled until a restore is verified, so a later
    // unrelated mute cannot overwrite them.
    std::vector<SessionMuteRecord> orphans_;
    bool orphansLoaded_ = false;
    bool journalReadable_ = true;
    bool journalPresent_ = false;
    std::string lastWritten_;
};
} // namespace backtrack
