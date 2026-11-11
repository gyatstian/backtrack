#include "integrations/WinHttpJson.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace backtrack {
std::optional<std::string> offlineHttpGetBody(
    HINTERNET, HINTERNET, const wchar_t*, const wchar_t*, bool, const wchar_t*);
}

// Include the worker once so the test can replace only its HTTP GET calls.
#define httpGetBody offlineHttpGetBody
#include "integrations/LeagueOfLegendsIntegration.cpp"
#undef httpGetBody

namespace {

constexpr std::array<std::string_view, 10> kSnapshots = {
    R"({"Events":[{"EventID":100,"EventTime":1000,"EventName":"ChampionKill","KillerName":"Example"}]})",
    R"({"Events":[{"EventID":0,"EventTime":0,"EventName":"ChampionKill","KillerName":"Example"}]})",
    R"({"Events":[{"EventID":0,"EventTime":0,"EventName":"ChampionKill","KillerName":"Example"},{"EventID":1,"EventTime":10,"EventName":"ChampionKill","KillerName":"Example"}]})",
    R"({"Events":[]})",
    R"({"Events":[{"EventID":0,"EventTime":0,"EventName":"ChampionKill","KillerName":"Example"},{"EventID":1,"EventTime":10,"EventName":"ChampionKill","KillerName":"Example"}]})",
    R"({"Events":[{"EventID":0,"EventTime":10,"EventName":"ChampionKill","KillerName":"Example"}]})",
    R"({"Events":[{"EventID":0,"EventTime":10,"EventName":"ChampionKill","KillerName":"Example"},{"EventID":1,"EventTime":10,"EventName":"ChampionKill","KillerName":"Example"}]})",
    R"({"Events":[{"EventID":1,"EventTime":0,"EventName":"ChampionKill","KillerName":"Example"}]})",
    R"({"Events":[{"EventID":1,"EventTime":1,"EventName":"ChampionKill","KillerName":"Example"}]})",
    R"({"Events":[{"EventID":1,"EventTime":1,"EventName":"ChampionKill","KillerName":"Example"}]})",
};

struct OfflineFeed {
    std::mutex mutex;
    std::condition_variable polled;
    std::vector<int> killsBeforePoll;
    int kills = 0;
} feed;

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

namespace backtrack {

void setThreadDescriptionSafe(const wchar_t*) {
}

std::optional<std::string> offlineHttpGetBody(
    HINTERNET, HINTERNET, const wchar_t* path, const wchar_t*, bool, const wchar_t*) {
    if (std::wstring_view(path) == L"/liveclientdata/activeplayer") {
        return std::string(R"({"riotId":"Example#NA1"})");
    }
    if (std::wstring_view(path) != L"/liveclientdata/eventdata") {
        return std::nullopt;
    }

    std::scoped_lock lock(feed.mutex);
    const size_t index = (std::min)(feed.killsBeforePoll.size(), kSnapshots.size() - 1);
    if (feed.killsBeforePoll.size() < kSnapshots.size()) {
        feed.killsBeforePoll.push_back(feed.kills);
        feed.polled.notify_one();
    }
    return std::string(kSnapshots[index]);
}

} // namespace backtrack

namespace {

void testNumericFields() {
    constexpr std::string_view object = "{\"EventID\" \t: \r\n -12, \"EventTime\": \t1.25e+2}";
    expect(backtrack::jsonIntField(object, "EventID") == -12, "integer field whitespace or sign changed");
    expect(backtrack::jsonDoubleField(object, "EventTime") == 125.0, "double field whitespace or exponent changed");
    expect(backtrack::jsonIntField("{\"EventID\":0}", "EventID") == 0, "zero integer was lost");
    expect(!backtrack::jsonIntField(object, "missing"), "missing integer was accepted");
    expect(!backtrack::jsonDoubleField(object, "missing"), "missing double was accepted");
    expect(!backtrack::jsonIntField("{\"EventID\"}", "EventID"), "integer without a colon was accepted");
    expect(!backtrack::jsonDoubleField("{\"EventTime\"}", "EventTime"), "double without a colon was accepted");
    expect(!backtrack::jsonIntField("\"EventID\": \t", "EventID"), "empty integer was accepted");
    expect(!backtrack::jsonDoubleField("\"EventTime\": \t", "EventTime"), "empty double was accepted");
    expect(!backtrack::jsonIntField("{\"EventID\":-}", "EventID"), "bare minus was accepted");
    expect(!backtrack::jsonIntField("{\"EventID\":9223372036854775808}", "EventID"), "integer overflow was accepted");
    expect(!backtrack::jsonDoubleField("{\"EventTime\":1e+}", "EventTime"), "incomplete exponent was accepted");
    expect(!backtrack::jsonDoubleField("{\"EventTime\":1e9999}", "EventTime"), "nonfinite double was accepted");
}

void testFeedResets() {
    backtrack::LeagueOfLegendsIntegration integration([] {
        std::scoped_lock lock(feed.mutex);
        ++feed.kills;
    });
    integration.start();

    bool completed = false;
    {
        std::unique_lock lock(feed.mutex);
        completed = feed.polled.wait_for(lock, std::chrono::seconds(30), [] {
            return feed.killsBeforePoll.size() == kSnapshots.size();
        });
    }
    integration.stop();

    expect(completed, "the offline worker did not finish polling");
    const auto& counts = feed.killsBeforePoll;
    expect(counts[1] == 0, "initial history was replayed");
    expect(counts[2] == 0, "reset history was replayed");
    expect(counts[3] == 1, "the first post-reset kill was missed or repeated");
    expect(counts[5] == 1, "an empty snapshot caused history to be replayed");
    expect(counts[6] == 1, "an ID-only reset replayed its baseline");
    expect(counts[7] == 2, "a kill after an ID-only reset was missed or repeated");
    expect(counts[8] == 2, "a time-only reset replayed its baseline");
    expect(counts[9] == 3, "a kill after a time-only reset was missed or repeated");
    expect(feed.kills == 3, "a repeated snapshot replayed a kill");
}

} // namespace

int main() {
    try {
        testNumericFields();
        testFeedResets();
        std::cout << "LeagueOfLegendsIntegrationTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "LeagueOfLegendsIntegrationTests failed: " << error.what() << '\n';
        return 1;
    }
}
