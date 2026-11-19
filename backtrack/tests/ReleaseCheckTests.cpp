#include <winsock2.h>
#include <ws2tcpip.h>

#include "core/AppVersion.h"
#include "core/Logger.h"
#include "platform/Win32Util.h"
#include "ui_common/RecorderActions.h"

#include <winhttp.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace backtrack {

// Stands in for the window's release-check worker: run the shared fetch on its
// own thread, hold the result, and cancel through the same stop flag.
struct Check {
    Check() = default;
    Check(const Check&) = delete;
    Check& operator=(const Check&) = delete;
    ~Check() { stop(); }

    void start(INTERNET_PORT port, std::chrono::milliseconds timeout) {
        worker = std::thread([this, port, timeout] {
            version = fetchReleaseVersion({L"127.0.0.1", port, WINHTTP_ACCESS_TYPE_NO_PROXY, false, timeout}, stopping);
        });
    }
    bool wait(DWORD timeoutMs) {
        return WaitForSingleObject(worker.native_handle(), timeoutMs) == WAIT_OBJECT_0;
    }
    void stop() {
        stopping.store(true);
        if (worker.joinable()) worker.join();
    }

    std::atomic<bool> stopping{false};
    std::optional<std::wstring> version;
    std::thread worker;
};

} // namespace backtrack

namespace {

using namespace std::chrono_literals;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Winsock {
    Winsock() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
    }
    ~Winsock() { WSACleanup(); }
};

class HttpServer {
public:
    HttpServer(std::string body, std::chrono::milliseconds interval, bool stall)
        : body_(std::move(body)), interval_(interval), stall_(stall) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            listen(listener_, 1) == SOCKET_ERROR) {
            closesocket(listener_);
            throw std::runtime_error("listen failed");
        }
        int length = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
            closesocket(listener_);
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~HttpServer() {
        stop();
        closesocket(listener_);
    }
    INTERNET_PORT port() const { return port_; }
    std::string request() {
        std::scoped_lock lock(mutex_);
        return request_;
    }
    bool waitForBytes(int count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return sent_.load() >= count; });
    }
    void stop() {
        stopping_.store(true);
        if (worker_.joinable()) worker_.join();
    }

private:
    void serve() {
        SOCKET client = INVALID_SOCKET;
        while (!stopping_.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener_, &set);
            timeval timeout{0, 50000};
            if (select(0, &set, nullptr, nullptr, &timeout) > 0) {
                client = accept(listener_, nullptr, nullptr);
                break;
            }
        }
        if (client == INVALID_SOCKET) return;
        DWORD timeout = 1000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string request;
        char buffer[2048];
        while (!stopping_.load() && request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
            const int read = recv(client, buffer, sizeof(buffer), 0);
            if (read <= 0) break;
            request.append(buffer, static_cast<size_t>(read));
        }
        {
            std::scoped_lock lock(mutex_);
            request_ = std::move(request);
        }
        if (request_.find("\r\n\r\n") != std::string::npos) {
            const std::string header = "HTTP/1.1 200 OK\r\nContent-Length: " +
                std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n";
            size_t offset = 0;
            while (offset < header.size() && !stopping_.load()) {
                const int written = send(client, header.data() + offset, static_cast<int>(header.size() - offset), 0);
                if (written <= 0) break;
                offset += static_cast<size_t>(written);
            }
            if (offset == header.size()) {
                offset = 0;
                while (offset < body_.size() && !stopping_.load()) {
                    const size_t chunk = interval_ == 0ms && !stall_ ? body_.size() - offset : 1;
                    const int written = send(client, body_.data() + offset, static_cast<int>(chunk), 0);
                    if (written <= 0) break;
                    offset += static_cast<size_t>(written);
                    {
                        std::scoped_lock lock(mutex_);
                        sent_ += written;
                        changed_.notify_all();
                    }
                    if (stall_) {
                        while (!stopping_.load()) std::this_thread::sleep_for(10ms);
                        break;
                    }
                    const auto next = std::chrono::steady_clock::now() + interval_;
                    while (!stopping_.load() && std::chrono::steady_clock::now() < next) {
                        std::this_thread::sleep_for(10ms);
                    }
                }
            }
        }
        closesocket(client);
    }

    SOCKET listener_ = INVALID_SOCKET;
    INTERNET_PORT port_ = 0;
    std::string body_;
    std::chrono::milliseconds interval_;
    bool stall_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> sent_{0};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string request_;
    std::thread worker_;
};

void testSuccessfulCheck(bool current) {
    const auto currentVersion = std::wstring(backtrack::kCurrentAppVersion);
    const auto tag = current ? backtrack::wideToUtf8(currentVersion) : std::string("test-update-version");
    HttpServer server("{\"tag_name\":\"" + tag + "\"}", 0ms, false);
    backtrack::Check check;
    check.start(server.port(), 3s);
    const bool finished = check.wait(5000);
    if (!finished) server.stop();
    expect(finished && check.version.has_value(), "successful update check did not return the release tag");
    expect((*check.version != currentVersion) == !current,
           "successful update check returned the wrong version comparison");
    const auto request = server.request();
    expect(request.find("GET /repos/gyatstian/backtrack/releases/latest ") != std::string::npos,
           "update check requested the wrong path");
    expect(request.find("application/vnd.github+json") != std::string::npos,
           "update check omitted the GitHub accept header");
    expect(request.find("Backtrack/" + backtrack::wideToUtf8(currentVersion)) != std::string::npos,
           "update check did not identify the current build");
}

void testSlowProgressHasTotalDeadline() {
    HttpServer server(std::string(100, 'x'), 80ms, false);
    backtrack::Check check;
    const auto start = std::chrono::steady_clock::now();
    check.start(server.port(), 700ms);
    const bool progress = server.waitForBytes(3, 2s);
    const bool finished = check.wait(2500);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    if (!finished) server.stop();
    check.stop();
    expect(progress, "slow server did not make progress");
    expect(finished && elapsed >= 400ms && elapsed < 2500ms, "update deadline did not bound slow progress");
    expect(!check.version.has_value(), "timed-out check returned a version");
}

void testStopUnblocksStalledCheck() {
    HttpServer server(std::string(100, 'x'), 0ms, true);
    backtrack::Check check;
    check.start(server.port(), 5s);
    const bool received = server.waitForBytes(1, 3s);
    const auto start = std::chrono::steady_clock::now();
    check.stop();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    server.stop();
    expect(received, "server did not start the pending response");
    expect(elapsed < 1500ms, "stop waited for the stalled server");
    expect(!check.version.has_value(), "stopped update check returned a version");
}

} // namespace

void testVersionComparison() {
    using backtrack::isNewerVersion;
    const auto current = backtrack::kCurrentAppVersion;
    expect(isNewerVersion(L"v1.98", current), "a later release was not detected as newer");
    expect(!isNewerVersion(current, current), "the running version read as newer than itself");
    expect(!isNewerVersion(L"v1.9", L"v1.97"), "1.9 must not compare as newer than 1.97");
    expect(isNewerVersion(L"1.97.1", L"v1.97"), "a patch release was not detected as newer");
    expect(!isNewerVersion(L"v0.9", L"v1.97"), "an older release compared as newer");
    expect(isNewerVersion(L"V2", L"v1.97"), "an uppercase prefix or missing parts broke the comparison");
    expect(!isNewerVersion(L"test-update-version", L"v1.97"), "a non-numeric tag compared as newer");
}

int main() {
    try {
        Winsock winsock;
        testVersionComparison();
        testSuccessfulCheck(false);
        testSuccessfulCheck(true);
        testSlowProgressHasTotalDeadline();
        testStopUnblocksStalledCheck();
        std::cout << "ReleaseCheckTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ReleaseCheckTests failed: " << error.what() << '\n';
        return 1;
    }
}
