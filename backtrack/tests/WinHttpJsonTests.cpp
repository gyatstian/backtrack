#include <winsock2.h>
#include <ws2tcpip.h>

#include "integrations/WinHttpJson.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

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
    HttpServer(std::string body, std::chrono::milliseconds interval, bool stall = false)
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
    int sent() const { return sent_.load(); }

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
        char request[2048];
        if (recv(client, request, sizeof(request), 0) > 0) {
            const std::string header = "HTTP/1.1 200 OK\r\nContent-Length: " +
                std::to_string(body_.size()) + "\r\nConnection: close\r\n\r\n";
            size_t headerSent = 0;
            while (headerSent < header.size() && !stopping_.load()) {
                const int written = send(client, header.data() + headerSent,
                    static_cast<int>(header.size() - headerSent), 0);
                if (written <= 0) break;
                headerSent += static_cast<size_t>(written);
            }
            if (headerSent == header.size()) {
                size_t offset = 0;
                while (offset < body_.size() && !stopping_.load()) {
                    const size_t chunk = interval_ == 0ms && !stall_
                        ? (std::min)(size_t{8192}, body_.size() - offset) : 1;
                    const int written = send(client, body_.data() + offset, static_cast<int>(chunk), 0);
                    if (written <= 0) break;
                    offset += static_cast<size_t>(written);
                    {
                        std::scoped_lock lock(mutex_);
                        sent_ += written;
                        changed_.notify_all();
                    }
                    if (stall_) {
                        while (!stopping_.load()) std::this_thread::sleep_for(20ms);
                        break;
                    }
                    const auto next = std::chrono::steady_clock::now() + interval_;
                    while (!stopping_.load() && std::chrono::steady_clock::now() < next) std::this_thread::sleep_for(10ms);
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
    std::thread worker_;
};

backtrack::CancellableHttpSession makeSession(INTERNET_PORT port, const std::atomic<bool>* stop,
    std::chrono::milliseconds deadline) {
    auto handles = std::make_shared<backtrack::AsyncHttpHandles>();
    handles->session.reset(WinHttpOpen(L"Backtrack-test", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    expect(static_cast<bool>(handles->session), "WinHttpOpen failed");
    WinHttpSetTimeouts(handles->session.value, 2000, 2000, 2000, 2000);
    handles->connection.reset(WinHttpConnect(handles->session.value, L"127.0.0.1", port, 0));
    expect(static_cast<bool>(handles->connection), "WinHttpConnect failed");
    return {handles, stop, deadline, false};
}

void testSuccessfulResponse() {
    HttpServer server("hello", 10ms);
    auto session = makeSession(server.port(), nullptr, 3s);
    auto body = backtrack::httpGetBody(session, session.handles->connection.value, L"/", nullptr, false, L"test");
    expect(body && *body == "hello", "successful response changed");
}

void testResponseCap() {
    HttpServer server(std::string(backtrack::kMaxHttpResponseBytes + 1, 'x'), 0ms);
    auto session = makeSession(server.port(), nullptr, 10s);
    auto body = backtrack::httpGetBody(session, session.handles->connection.value, L"/", nullptr, false, L"test");
    const bool sent = server.waitForBytes(static_cast<int>(backtrack::kMaxHttpResponseBytes + 1), 2s);
    expect(!body && sent, "oversized response was not refused");
}

void testProgressDoesNotResetDeadline() {
    HttpServer server(std::string(100, 'x'), 80ms);
    auto session = makeSession(server.port(), nullptr, 700ms);
    const auto start = std::chrono::steady_clock::now();
    auto body = backtrack::httpGetBody(session, session.handles->connection.value, L"/", nullptr, false, L"test");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(!body, "slow response escaped the total deadline");
    expect(server.sent() >= 3, "server did not make progress before the deadline");
    expect(elapsed >= 400ms && elapsed < 2500ms, "total deadline was not enforced promptly");
}

void testStopCancelsPendingResponse() {
    HttpServer server(std::string(100, 'x'), 0ms, true);
    std::atomic<bool> stop{false};
    auto session = makeSession(server.port(), &stop, 5s);
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    std::optional<std::string> body;
    std::thread worker([&] {
        body = backtrack::httpGetBody(session, session.handles->connection.value, L"/", nullptr, false, L"test");
        std::scoped_lock lock(mutex);
        done = true;
        changed.notify_one();
    });
    const bool received = server.waitForBytes(1, 3s);
    stop.store(true, std::memory_order_release);
    bool cancelled = false;
    {
        std::unique_lock lock(mutex);
        cancelled = changed.wait_for(lock, 1500ms, [&] { return done; });
    }
    if (!cancelled) server.stop();
    worker.join();
    expect(received, "server did not start the response");
    expect(cancelled && !body, "stop did not cancel the pending response");
}

void testClosingCallbackOwnsBuffers() {
    auto state = std::make_shared<backtrack::AsyncHttpRequestState>();
    state->handles = std::make_shared<backtrack::AsyncHttpHandles>();
    std::weak_ptr<backtrack::AsyncHttpRequestState> weakState = state;
    std::weak_ptr<backtrack::AsyncHttpHandles> weakHandles = state->handles;
    auto* owner = new std::shared_ptr<backtrack::AsyncHttpRequestState>(state);
    const auto context = reinterpret_cast<DWORD_PTR>(owner);
    state.reset();
    backtrack::httpRequestCallback(nullptr, context, WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, nullptr, 0);
    expect(!weakState.expired() && !weakHandles.expired(), "error callback released outstanding storage");
    backtrack::httpRequestCallback(nullptr, context, WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, nullptr, 0);
    expect(weakState.expired() && weakHandles.expired(), "closing callback retained completed storage");
}

} // namespace

int main() {
    try {
        Winsock winsock;
        testSuccessfulResponse();
        testResponseCap();
        testProgressDoesNotResetDeadline();
        testStopCancelsPendingResponse();
        testClosingCallbackOwnsBuffers();
        std::cout << "WinHttpJsonTests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WinHttpJsonTests failed: " << error.what() << '\n';
        return 1;
    }
}
