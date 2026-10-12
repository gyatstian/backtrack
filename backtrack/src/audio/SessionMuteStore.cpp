#include "audio/SessionMuteStore.h"

#include "core/Logger.h"

#include <objbase.h>

#include <cstdlib>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace backtrack {

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

void SessionMuteStore::writeJournalLocked(const std::string& content) {
    if (content == lastWritten_) {
        return;
    }

    std::error_code error;
    if (content.empty()) {
        // Journal removal is journal state too: an empty journal tells the
        // startup sweep there is nothing orphaned to undo.
        std::filesystem::remove(journalPath_, error);
        lastWritten_.clear();
        return;
    }

    std::filesystem::create_directories(journalPath_.parent_path(), error);
    error = {};
    const std::filesystem::path tempPath = std::filesystem::path(journalPath_.wstring() + L".tmp");
    {
        std::ofstream stream(tempPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            return;
        }
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
        stream.flush();
        if (!stream) {
            return;
        }
    }
    // Atomic replace so a crash mid-write never leaves a torn journal behind.
    std::filesystem::rename(tempPath, journalPath_, error);
    if (!error) {
        lastWritten_ = content;
    }
}

void SessionMuteStore::persistLocked() {
    std::vector<SessionMuteRecord> records;
    records.reserve(mutes_.size());
    for (const auto& mute : mutes_) {
        records.push_back(mute.record);
    }
    writeJournalLocked(serialize(records));
}

void SessionMuteStore::restoreEntriesLocked() {
    for (auto& mute : mutes_) {
        if (mute.setMute) {
            try {
                mute.setMute(mute.record.previousMute);
            } catch (...) {
                // Best effort on every path that funnels here, crash included:
                // one failed restore must not stop the rest.
            }
        }
    }
    mutes_.clear();
}

std::optional<std::vector<SessionMuteRecord>> SessionMuteStore::readJournal() const {
    std::error_code error;
    if (!std::filesystem::exists(journalPath_, error) || error) {
        return std::nullopt;
    }
    std::ifstream stream(journalPath_, std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        return std::nullopt;
    }
    return parse(std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()));
}

uint32_t SessionMuteStore::apply(const std::vector<std::wstring>& wantedKeys,
                                 std::function<bool(std::vector<SessionMuteCandidate>&)> enumerateSessions,
                                 std::function<bool(uint32_t)> isProcessAlive) {
    std::scoped_lock lock(mutex_);
    if (wantedKeys.empty()) {
        restoreEntriesLocked();
        writeJournalLocked({});
        return 0;
    }

    std::vector<SessionMuteCandidate> sessions;
    if (!enumerateSessions || !enumerateSessions(sessions)) {
        return 0;
    }

    const std::unordered_set<std::wstring> wanted(wantedKeys.begin(), wantedKeys.end());

    // Drop stale mutes for processes that exited or are no longer requested.
    for (auto it = mutes_.begin(); it != mutes_.end();) {
        const bool stillWanted = wanted.contains(it->record.pathKey);
        const bool alive = stillWanted && isProcessAlive && isProcessAlive(it->record.processId);
        if (!stillWanted || !alive || !it->setMute) {
            if (it->setMute) {
                try {
                    it->setMute(it->record.previousMute);
                } catch (...) {
                }
            }
            it = mutes_.erase(it);
        } else {
            ++it;
        }
    }

    std::unordered_set<uint32_t> alreadyMuted;
    for (const auto& mute : mutes_) {
        alreadyMuted.insert(mute.record.processId);
    }

    for (auto& session : sessions) {
        if (session.pathKey.empty() || !wanted.contains(session.pathKey) ||
            alreadyMuted.contains(session.processId)) {
            continue;
        }
        if (!session.setMute || !session.setMute(true)) {
            continue;
        }
        AppliedMute mute;
        mute.record.processId = session.processId;
        mute.record.pathKey = session.pathKey;
        mute.record.previousMute = session.muted;
        mute.setMute = std::move(session.setMute);
        mutes_.push_back(std::move(mute));
        alreadyMuted.insert(session.processId);
        // Journal this mute before walking the next session: a kill inside
        // the walk is precisely what the journal exists for, and the lock the
        // crash path try_locks is held across the whole enumeration, so there
        // is no second chance to record it. Unchanged content short-circuits
        // the write, so this costs one small file per newly muted session and
        // nothing on the no-op sweeps after it.
        persistLocked();
    }

    // Covers the stale-entry removals above, which unmute sessions too. No-op
    // when the loop already persisted this exact content.
    persistLocked();
    return static_cast<uint32_t>(mutes_.size());
}

void SessionMuteStore::clear() {
    std::scoped_lock lock(mutex_);
    restoreEntriesLocked();
    writeJournalLocked({});
}

std::optional<SessionMuteSweepResult> SessionMuteStore::sweepOrphans(
    std::function<bool(std::vector<SessionMuteCandidate>&)> enumerateSessions) {
    std::optional<std::vector<SessionMuteRecord>> journal = readJournal();
    if (!journal) {
        // No journal, or one that could not be read: no sweep ran, and the
        // file is left for the next launch.
        return std::nullopt;
    }
    // A readable journal sweeps even when no line parsed: the sweep consumes
    // it (deleting the file) and reports 0/0, as before.
    std::vector<SessionMuteRecord> orphans = std::move(*journal);

    std::vector<SessionMuteCandidate> sessions;
    if (!enumerateSessions || !enumerateSessions(sessions)) {
        return std::nullopt;
    }

    SessionMuteSweepResult result;
    for (const auto& session : sessions) {
        if (session.pathKey.empty() || !session.setMute) {
            continue;
        }
        const auto orphan = std::find_if(orphans.begin(), orphans.end(), [&](const SessionMuteRecord& candidate) {
            return candidate.processId == session.processId && candidate.pathKey == session.pathKey;
        });
        if (orphan == orphans.end()) {
            continue;
        }
        // Only touch sessions still muted: if the user already unmuted the app
        // after the crash, respect that instead of re-applying the journaled
        // state blindly.
        if (session.muted && session.setMute(orphan->previousMute)) {
            ++result.restored;
            orphans.erase(orphan);
        }
    }

    // Consumed only after a sweep actually ran: the enumeration failures above
    // return early, and entries the loop could not reach are rewritten, not dropped.
    {
        std::scoped_lock lock(mutex_);
        writeJournalLocked(serialize(orphans));
    }
    result.unmatched = orphans.size();
    return result;
}

void SessionMuteStore::emergencyRestore() noexcept {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    // COM is per-thread and none of the threads that reach here have it: the
    // console control handler runs on an OS-created thread, the SEH filter and
    // the terminate handler on whatever faulted. Un-initialized, the SetMute
    // below fails with CO_E_NOTINITIALIZED and the discarded result hides it,
    // leaving the application muted with only the next launch's sweep to undo
    // it. A mismatched apartment (RPC_E_CHANGED_MODE) is still a usable COM
    // thread, so the result is deliberately not checked. Uninitialize only
    // what this call initialized: unbalancing someone else's apartment on the
    // crash path would be worse than the leak it avoids.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static_cast<void>(comResult);
    restoreEntriesLocked();
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
}

} // namespace backtrack
