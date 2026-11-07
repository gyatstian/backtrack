#include "audio/SessionMuteStore.h"

#include "core/Logger.h"

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

// A journal record only carries the durable identity, so several records can
// match the same PID and path. `claimed` keeps the pairing one record to one
// session instance, which is what restores each session's own previous state.
const SessionMuteCandidate* findSessionForRecord(
    const std::vector<SessionMuteCandidate>& sessions,
    const SessionMuteRecord& record,
    const std::unordered_set<std::wstring>& claimed) {
    const auto match = std::find_if(sessions.begin(), sessions.end(), [&](const SessionMuteCandidate& session) {
        return session.processId == record.processId && session.pathKey == record.pathKey &&
               !claimed.contains(sessionKey(session.endpointId, session.instanceId));
    });
    return match == sessions.end() ? nullptr : &*match;
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
        const auto firstTab = line.find('\t');
        const auto secondTab =
            firstTab == std::string::npos ? std::string::npos : line.find('\t', firstTab + 1);
        if (firstTab == std::string::npos || secondTab == std::string::npos) {
            continue;
        }
        SessionMuteRecord record;
        record.processId = static_cast<uint32_t>(_strtoui64(line.c_str(), nullptr, 10));
        record.previousMute = line[firstTab + 1] == '1';
        record.pathKey = utf8ToWide(line.substr(secondTab + 1));
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
    const std::filesystem::path tempPath = std::filesystem::path(journalPath_.wstring() + L".tmp");
    {
        std::ofstream stream(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            return false;
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(tempPath, error);
            return false;
        }
    }
    // Atomic replace so a crash mid-write never leaves a torn journal behind.
    std::filesystem::rename(tempPath, journalPath_, error);
    if (error) {
        std::error_code cleanup;
        std::filesystem::remove(tempPath, cleanup);
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
    std::unordered_set<std::wstring> claimed;
    for (const auto& mute : mutes_) {
        claimed.insert(sessionKey(mute.endpointId, mute.instanceId));
    }
    for (auto it = orphans_.begin(); it != orphans_.end();) {
        const SessionMuteCandidate* session = findSessionForRecord(sessions, *it, claimed);
        if (!session) {
            if (isProcessAlive && !isProcessAlive(it->processId)) {
                // The process is gone, and its sessions and their mutes with it.
                it = orphans_.erase(it);
            } else {
                ++it;
            }
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

    std::unordered_set<std::wstring> claimed;
    for (const auto& mute : mutes_) {
        claimed.insert(sessionKey(mute.endpointId, mute.instanceId));
    }
    for (auto it = orphans_.begin(); it != orphans_.end();) {
        const SessionMuteCandidate* session = findSessionForRecord(sessions, *it, claimed);
        if (!session) {
            if (isProcessAlive && !isProcessAlive(it->processId)) {
                it = orphans_.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        if (!session->muteStateKnown) {
            ++it;
            continue;
        }
        if (!session->muted) {
            it = orphans_.erase(it);
            continue;
        }
        if (!session->setMute || !session->setMute(it->previousMute)) {
            ++it;
            continue;
        }
        claimed.insert(sessionKey(session->endpointId, session->instanceId));
        ++result.restored;
        it = orphans_.erase(it);
    }

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
    std::unordered_set<std::wstring> claimed;
    for (const auto& mute : mutes_) {
        claimed.insert(sessionKey(mute.endpointId, mute.instanceId));
    }
    for (auto it = orphans_.begin(); it != orphans_.end();) {
        const SessionMuteCandidate* session = findSessionForRecord(sessions, *it, claimed);
        if (!session) {
            if (isProcessAlive && !isProcessAlive(it->processId)) {
                // The process is gone, so its sessions and their mutes are too.
                it = orphans_.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        // Only touch sessions still muted: if the user already unmuted the app
        // after the crash, respect that instead of re-applying the journaled
        // state blindly.
        if (!session->muteStateKnown) {
            ++it;
            continue;
        }
        if (!session->muted) {
            it = orphans_.erase(it);
            continue;
        }
        if (!session->setMute || !session->setMute(it->previousMute)) {
            ++it;
            continue;
        }
        claimed.insert(sessionKey(session->endpointId, session->instanceId));
        ++result.restored;
        it = orphans_.erase(it);
    }

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
        std::unordered_set<std::wstring> claimed;
        for (auto it = orphans_.begin(); it != orphans_.end();) {
            const SessionMuteCandidate* session = findSessionForRecord(sessions, *it, claimed);
            if (!session) {
                ++it;
                continue;
            }
            if (!session->muteStateKnown) {
                ++it;
                continue;
            }
            if (!session->muted) {
                it = orphans_.erase(it);
                continue;
            }
            if (!session->setMute || !session->setMute(it->previousMute)) {
                ++it;
                continue;
            }
            claimed.insert(sessionKey(session->endpointId, session->instanceId));
            it = orphans_.erase(it);
        }

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
