#include "audio/SessionMuteStore.h"

#include "core/Logger.h"
#include "platform/AtomicWrite.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace backtrack {

namespace {

const SessionMuteCandidate* findSession(
    const std::vector<SessionMuteCandidate>& sessions,
    const std::wstring& endpointId,
    const std::wstring& instanceId) {
    if (instanceId.empty()) {
        return nullptr;
    }
    const auto match = std::find_if(sessions.begin(), sessions.end(), [&](const SessionMuteCandidate& session) {
        return session.instanceId == instanceId && session.endpointId == endpointId;
    });
    return match == sessions.end() ? nullptr : &*match;
}

std::wstring sessionKey(const std::wstring& endpointId, const std::wstring& instanceId) {
    return endpointId + L'\x1f' + instanceId;
}

// A record's live identity only narrows the search; it is never required, and
// the pairing still claims one session per record so each restores its own
// previous state.
struct RecordMatch {
    const SessionMuteCandidate* session = nullptr;
    // The record describes no session that can still be restored.
    bool retire = false;
};

bool claimedBy(const std::unordered_set<std::wstring>& claimed, const SessionMuteCandidate& session) {
    return claimed.contains(sessionKey(session.endpointId, session.instanceId));
}

// Pairs one journal record with one live session. `isProcessAlive` empty means
// the caller has no liveness knowledge (the crash path) and nothing is retired
// on it.
RecordMatch findSessionForRecord(
    const std::vector<SessionMuteCandidate>& sessions,
    const SessionMuteRecord& record,
    const std::unordered_set<std::wstring>& claimed,
    const std::function<bool(uint32_t)>& isProcessAlive) {
    const auto freeSession = [&](auto predicate) -> const SessionMuteCandidate* {
        const auto match = std::find_if(sessions.begin(), sessions.end(), [&](const SessionMuteCandidate& session) {
            return !claimedBy(claimed, session) && predicate(session);
        });
        return match == sessions.end() ? nullptr : &*match;
    };
    const bool haveLiveness = static_cast<bool>(isProcessAlive);
    const bool processAlive = !haveLiveness || isProcessAlive(record.processId);

    if (record.instanceId.empty()) {
        // Legacy record: the durable identity is all it has.
        if (const auto* session = freeSession([&](const SessionMuteCandidate& session) {
                return session.processId == record.processId && session.pathKey == record.pathKey;
            })) {
            return {session, false};
        }
        return {nullptr, !processAlive};
    }

    if (const auto* session = freeSession([&](const SessionMuteCandidate& session) {
            return session.processId == record.processId &&
                   session.pathKey == record.pathKey &&
                   session.endpointId == record.endpointId &&
                   session.instanceId == record.instanceId;
        })) {
        return {session, false};
    }
    if (!haveLiveness) {
        // Cannot tell a dead process from a recreated session here, so keep the
        // obligation instead of guessing.
        return {nullptr, false};
    }
    if (processAlive) {
        // The exact instance is gone and its process lives: Windows keeps a
        // mute per session, so the mute died with the session. Falling back to
        // another session of the same process would unmute the user's own
        // mute instead.
        return {nullptr, true};
    }
    // The process is gone. A session it restarted into can inherit the
    // persisted mute, so pair by the durable identity and let the caller check
    // that session's mute state.
    if (const auto* session = freeSession([&](const SessionMuteCandidate& session) {
            return session.pathKey == record.pathKey &&
                   (record.endpointId.empty() || session.endpointId == record.endpointId);
        })) {
        return {session, false};
    }
    // Nothing live carries the path: the app is not running, so nothing can be
    // muted by it. A path still live but claimed by another record is kept for
    // a later retry.
    const bool pathLive = std::any_of(sessions.begin(), sessions.end(),
        [&](const SessionMuteCandidate& session) { return session.pathKey == record.pathKey; });
    return {nullptr, !pathLive};
}

} // namespace

SessionMuteStore::SessionMuteStore(std::filesystem::path journalPath)
    : journalPath_(std::move(journalPath)) {
}

std::string SessionMuteStore::serialize(const std::vector<SessionMuteRecord>& records) {
    std::string content;
    for (const auto& record : records) {
        content += std::to_string(record.processId);
        content += '\t';
        content += record.previousMute ? '1' : '0';
        content += '\t';
        content += wideToUtf8(record.pathKey);
        content += '\t';
        content += wideToUtf8(record.endpointId);
        content += '\t';
        content += wideToUtf8(record.instanceId);
        content += '\n';
    }
    return content;
}

std::vector<SessionMuteRecord> SessionMuteStore::parse(const std::string& content) {
    std::vector<SessionMuteRecord> records;
    size_t begin = 0;
    while (begin < content.size()) {
        size_t end = content.find('\n', begin);
        if (end == std::string::npos) {
            end = content.size();
        }
        std::string line = content.substr(begin, end - begin);
        begin = end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Columns: pid, previousMute, pathKey, then the optional live identity
        // (endpoint id, session instance id). Anything past them is ignored.
        std::string fields[5];
        size_t columnBegin = 0;
        int columns = 0;
        while (columns < 5) {
            const size_t tab = line.find('\t', columnBegin);
            fields[columns++] = line.substr(columnBegin,
                tab == std::string::npos ? std::string::npos : tab - columnBegin);
            if (tab == std::string::npos) {
                break;
            }
            columnBegin = tab + 1;
        }
        if (columns < 3) {
            continue;
        }
        SessionMuteRecord record;
        record.processId = static_cast<uint32_t>(_strtoui64(line.c_str(), nullptr, 10));
        record.previousMute = fields[1].size() == 1 && fields[1][0] == '1';
        record.pathKey = utf8ToWide(fields[2]);
        record.endpointId = utf8ToWide(fields[3]);
        record.instanceId = utf8ToWide(fields[4]);
        if (record.processId != 0 && !record.pathKey.empty()) {
            records.push_back(std::move(record));
        }
    }
    return records;
}

bool SessionMuteStore::writeJournalLocked(const std::string& content) {
    if (content.empty()) {
        if (!journalPresent_) {
            return true;
        }
        // Journal removal is journal state too, and it must not short-circuit
        // on the content cache: an empty cache next to a journal left by a
        // crashed process is exactly the obsolete journal this removes.
        std::error_code error;
        std::filesystem::remove(journalPath_, error);
        if (error) {
            return false;
        }
        journalPresent_ = false;
        lastWritten_.clear();
        return true;
    }

    if (journalPresent_ && content == lastWritten_) {
        return true;
    }

    std::error_code error;
    std::filesystem::create_directories(journalPath_.parent_path(), error);
    if (error) {
        return false;
    }
    // A uniquely named staging file renamed into place: a fixed "<journal>.tmp"
    // name is a name another process can pre-create or hard-link to, and the
    // rename would replace that file instead of ours.
    if (atomicWriteSibling(journalPath_, content)) {
        return false;
    }
    journalPresent_ = true;
    lastWritten_ = content;
    return true;
}

bool SessionMuteStore::persistLocked() {
    std::vector<SessionMuteRecord> records;
    records.reserve(mutes_.size() + orphans_.size());
    for (const auto& mute : mutes_) {
        records.push_back(mute.record);
    }
    records.insert(records.end(), orphans_.begin(), orphans_.end());
    return writeJournalLocked(serialize(records));
}

bool SessionMuteStore::loadOrphansLocked() {
    if (orphansLoaded_) {
        return journalReadable_;
    }
    orphansLoaded_ = true;

    std::error_code error;
    journalPresent_ = std::filesystem::exists(journalPath_, error);
    if (error) {
        journalReadable_ = false;
        return false;
    }
    if (!journalPresent_) {
        return true;
    }

    std::ifstream stream(journalPath_, std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        journalReadable_ = false;
        return false;
    }
    orphans_ = parse(std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
    return true;
}

bool SessionMuteStore::restoreMuteLocked(const AppliedMute& mute, const SessionMuteCandidate& session) {
    if (session.muteStateKnown && !session.muted) {
        // Already unmuted (the user or the app did it): the state to restore to
        // is the current one, so retiring the record loses nothing.
        return true;
    }
    // SetMute success is the verification: an unknown read-back state still
    // lets the write settle the session, and a failed write keeps the record.
    return session.setMute && session.setMute(mute.record.previousMute);
}

std::unordered_set<std::wstring> SessionMuteStore::claimedSessionsLocked() const {
    std::unordered_set<std::wstring> claimed;
    for (const auto& mute : mutes_) {
        claimed.insert(sessionKey(mute.endpointId, mute.instanceId));
    }
    return claimed;
}

uint32_t SessionMuteStore::restoreOrphanRecordsLocked(
    const std::vector<SessionMuteCandidate>& sessions,
    const std::function<bool(uint32_t)>& isProcessAlive,
    std::unordered_set<std::wstring>& claimed) {
    uint32_t restored = 0;
    for (auto it = orphans_.begin(); it != orphans_.end();) {
        const RecordMatch match = findSessionForRecord(sessions, *it, claimed, isProcessAlive);
        if (match.retire) {
            it = orphans_.erase(it);
            continue;
        }
        if (!match.session || !match.session->muteStateKnown) {
            ++it;
            continue;
        }
        if (!match.session->muted) {
            // Live and unmuted: the persisted mute did not survive, so nothing
            // is left to restore.
            it = orphans_.erase(it);
            continue;
        }
        if (!match.session->setMute || !match.session->setMute(it->previousMute)) {
            ++it;
            continue;
        }
        claimed.insert(sessionKey(match.session->endpointId, match.session->instanceId));
        ++restored;
        it = orphans_.erase(it);
    }
    return restored;
}

SessionMuteApplyResult SessionMuteStore::apply(const std::vector<std::wstring>& wantedKeys,
                                               SessionEnumerator enumerateSessions,
                                               std::function<bool(uint32_t)> isProcessAlive) {
    std::scoped_lock lock(mutex_);
    SessionMuteApplyResult result;
    if (!loadOrphansLocked()) {
        // An unreadable journal is an obligation this process cannot see;
        // writing would erase it, so nothing is touched.
        result.journalFailed = true;
        return result;
    }

    const std::unordered_set<std::wstring> wanted(wantedKeys.begin(), wantedKeys.end());
    std::vector<SessionMuteCandidate> sessions;
    if (!wanted.empty() || !mutes_.empty() || !orphans_.empty()) {
        if (!enumerateSessions || !enumerateSessions(sessions)) {
            result.enumerationFailed = true;
            for (const auto& mute : mutes_) {
                if (wanted.contains(mute.record.pathKey)) {
                    ++result.muted;
                }
            }
            result.unresolved = mutes_.size() + orphans_.size() - result.muted;
            return result;
        }
    }

    // Journal obligations from a previous process come first: a session that is
    // still muted and still wanted becomes a live mute instead of being
    // restored and re-muted, so the state the user actually had is the one kept
    // for later.
    auto claimed = claimedSessionsLocked();
    for (auto it = orphans_.begin(); it != orphans_.end();) {
        const RecordMatch match = findSessionForRecord(sessions, *it, claimed, isProcessAlive);
        if (match.retire) {
            // No live session can still carry this obligation.
            it = orphans_.erase(it);
            continue;
        }
        const SessionMuteCandidate* session = match.session;
        if (!session) {
            ++it;
            continue;
        }
        if (!session->muteStateKnown) {
            // Cannot verify whether the journaled mute still applies; keep the
            // obligation and retry instead of guessing.
            ++it;
            continue;
        }
        if (!session->muted) {
            // The user already unmuted the session; there is nothing to restore.
            it = orphans_.erase(it);
            continue;
        }
        if (wanted.contains(it->pathKey)) {
            AppliedMute adopted;
            adopted.record = *it;
            adopted.record.endpointId = session->endpointId;
            adopted.record.instanceId = session->instanceId;
            adopted.endpointId = session->endpointId;
            adopted.instanceId = session->instanceId;
            if (!adopted.instanceId.empty() && !adopted.endpointId.empty()) {
                claimed.insert(sessionKey(adopted.endpointId, adopted.instanceId));
                mutes_.push_back(std::move(adopted));
                it = orphans_.erase(it);
            } else {
                // Still muted and still wanted, but with no live identity: the
                // obligation itself is what restores it later, so keep it.
                ++it;
            }
            continue;
        }
        if (!session->setMute || !session->setMute(it->previousMute)) {
            ++it;
            continue;
        }
        claimed.insert(sessionKey(session->endpointId, session->instanceId));
        it = orphans_.erase(it);
    }

    // Live mutes: keep what is still wanted and verifiably muted, restore what
    // is not, retire records whose exact session instance is gone.
    for (auto it = mutes_.begin(); it != mutes_.end();) {
        const SessionMuteCandidate* session = findSession(sessions, it->endpointId, it->instanceId);
        if (!session) {
            // Windows keeps a mute per session instance, so the mute went with
            // the session. Retiring cannot leave foreign audio muted and keeps
            // the journal from growing forever.
            it = mutes_.erase(it);
            continue;
        }
        if (!wanted.contains(it->record.pathKey)) {
            if (restoreMuteLocked(*it, *session)) {
                it = mutes_.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        if (session->muteStateKnown && session->muted) {
            ++it;
            continue;
        }
        // Still wanted but no longer verifiably muted: the app can reset its own
        // session mute and a replacement session starts unmuted. Re-assert,
        // otherwise excluded audio would reach the recording.
        if (!session->setMute || !session->setMute(true)) {
            ++result.muteFailures;
        }
        ++it;
    }

    for (auto& session : sessions) {
        if (session.pathKey.empty() || !wanted.contains(session.pathKey) || session.muted) {
            // `muted` already true means it is silent, whether by us, by a
            // journaled obligation, or by the user; there is nothing to change.
            continue;
        }
        if (claimed.contains(sessionKey(session.endpointId, session.instanceId))) {
            // Already tracked: the live-mute loop above re-asserted it and
            // reported a failure if that failed. Never add a second record.
            continue;
        }
        if (!session.muteStateKnown ||
            session.endpointId.empty() ||
            session.instanceId.empty() ||
            !session.setMute) {
            ++result.muteFailures;
            continue;
        }

        AppliedMute mute;
        mute.record.processId = session.processId;
        mute.record.pathKey = session.pathKey;
        mute.record.previousMute = session.muted;
        mute.record.endpointId = session.endpointId;
        mute.record.instanceId = session.instanceId;
        mute.endpointId = session.endpointId;
        mute.instanceId = session.instanceId;
        mutes_.push_back(mute);
        // Journal the intent before mutating: a crash between the two must leave
        // a record the next launch can undo, never a mute nobody knows about.
        if (!persistLocked()) {
            mutes_.pop_back();
            result.journalFailed = true;
            break;
        }
        if (!session.setMute(true)) {
            mutes_.pop_back();
            persistLocked(); // best effort: the intent is moot now
            ++result.muteFailures;
        }
    }

    for (const auto& mute : mutes_) {
        if (wanted.contains(mute.record.pathKey)) {
            ++result.muted;
        }
    }
    result.unresolved = mutes_.size() + orphans_.size() - result.muted;
    // Retires restored records too. Unchanged content short-circuits the write,
    // so a poll with nothing to change costs one comparison.
    if (!result.journalFailed && !persistLocked()) {
        result.journalFailed = true;
    }
    return result;
}

SessionMuteRestoreResult SessionMuteStore::clear(SessionEnumerator enumerateSessions,
                                                 std::function<bool(uint32_t)> isProcessAlive) {
    std::scoped_lock lock(mutex_);
    SessionMuteRestoreResult result;
    const bool journalReadable = loadOrphansLocked();

    std::vector<SessionMuteCandidate> sessions;
    if ((!mutes_.empty() || !orphans_.empty()) &&
        (!enumerateSessions || !enumerateSessions(sessions))) {
        // Nothing was touched, so the journal still describes the live mutes.
        result.unresolved = mutes_.size() + orphans_.size();
        result.journalFailed = !journalReadable;
        return result;
    }

    for (auto it = mutes_.begin(); it != mutes_.end();) {
        const SessionMuteCandidate* session = findSession(sessions, it->endpointId, it->instanceId);
        if (!session) {
            it = mutes_.erase(it);
            continue;
        }
        if (restoreMuteLocked(*it, *session)) {
            ++result.restored;
            it = mutes_.erase(it);
        } else {
            ++it;
        }
    }

    auto claimed = claimedSessionsLocked();
    result.restored += restoreOrphanRecordsLocked(sessions, isProcessAlive, claimed);

    result.unresolved = mutes_.size() + orphans_.size();
    if (!journalReadable) {
        // Restored what this process could see, but writing now would drop the
        // obligations it could not read. Leave the journal for the next launch.
        result.journalFailed = true;
        return result;
    }
    if (!persistLocked()) {
        result.journalFailed = true;
    }
    return result;
}

std::optional<SessionMuteSweepResult> SessionMuteStore::sweepOrphans(
    SessionEnumerator enumerateSessions,
    std::function<bool(uint32_t)> isProcessAlive) {
    std::scoped_lock lock(mutex_);
    if (!loadOrphansLocked() || !journalPresent_) {
        // No journal, or one that could not be read: no sweep ran, and the file
        // is left for the next launch.
        return std::nullopt;
    }
    // A readable journal sweeps even when no line parsed: the sweep consumes it
    // (deleting the file) and reports 0/0, as before.
    std::vector<SessionMuteCandidate> sessions;
    if (!enumerateSessions || !enumerateSessions(sessions)) {
        return std::nullopt;
    }

    SessionMuteSweepResult result;
    auto claimed = claimedSessionsLocked();
    result.restored = restoreOrphanRecordsLocked(sessions, isProcessAlive, claimed);

    // Consumed only after a sweep actually ran: the enumeration failure above
    // returns early, and entries the loop could not reach are rewritten, not
    // dropped.
    if (!persistLocked()) {
        result.journalFailed = true;
    }
    result.unmatched = orphans_.size();
    return result;
}

void SessionMuteStore::emergencyRestore(SessionEnumerator enumerateSessions) noexcept {
    try {
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) {
            return;
        }
        const bool journalReadable = loadOrphansLocked();
        if (mutes_.empty() && orphans_.empty()) {
            return;
        }
        std::vector<SessionMuteCandidate> sessions;
        if (!enumerateSessions || !enumerateSessions(sessions)) {
            return;
        }

        for (auto it = mutes_.begin(); it != mutes_.end();) {
            const SessionMuteCandidate* session = findSession(sessions, it->endpointId, it->instanceId);
            if (!session) {
                it = mutes_.erase(it);
                continue;
            }
            if (restoreMuteLocked(*it, *session)) {
                it = mutes_.erase(it);
            } else {
                ++it;
            }
        }
        // Preserve crash-path pairing: no live claims and no process-liveness pruning.
        std::unordered_set<std::wstring> claimed;
        restoreOrphanRecordsLocked(sessions, {}, claimed);

        // Retire what was restored so the next launch does not restore it a
        // second time. A failed write leaves the old journal, which is
        // idempotent, and is ignored: a crashing process must not care. An
        // unreadable journal is never rewritten, so its obligations survive.
        if (journalReadable) {
            persistLocked();
        }
    } catch (...) {
        // The crash path must never let a failure escape into the faulting
        // process.
    }
}

} // namespace backtrack
