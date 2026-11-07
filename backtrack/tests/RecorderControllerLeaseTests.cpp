#include "app/CaptureFrameHandoff.h"
#include "capture/ICaptureSource.h"
#include "core/SpscQueue.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class LeaseCheckingSource final : public backtrack::ICaptureSource {
public:
    bool initialize(backtrack::D3DDevice&, const backtrack::AppSettings&,
                    const backtrack::CaptureTarget&) override { return true; }
    bool acquireNextFrame(backtrack::GpuFrame& frame, uint32_t) override {
        if (resizePending) {
            if (!resizeReported) {
                resizeReported = true;
                ++resolutionChanges;
                return false;
            }
            expect(leases == 0, "resize with a raw capture lease");
            resizePending = false;
            ++resizes;
        }
        ++leases;
        frame.lease = std::shared_ptr<void>(this, [this](void*) { --leases; });
        return true;
    }
    void requestResize() { resizePending = true; }
    void shutdown() override {
        expect(leases == 0, "shutdown with a raw capture lease");
        ++shutdowns;
    }
    bool isDeviceLost() const override { return false; }
    backtrack::CaptureBackend backend() const override {
        return backtrack::CaptureBackend::WindowsGraphicsCapture;
    }
    uint32_t width() const override { return 1; }
    uint32_t height() const override { return 1; }
    uint64_t resolutionChangeCount() const override { return resolutionChanges; }

    int leases = 0;
    int resizes = 0;
    int shutdowns = 0;
    uint64_t resolutionChanges = 0;
    bool resizePending = false;
    bool resizeReported = false;
};

void testResizeDrainsEveryOwner() {
    LeaseCheckingSource source;
    backtrack::GpuFrame last, acquired;
    source.acquireNextFrame(last, 0);
    source.acquireNextFrame(acquired, 0);
    backtrack::SpscQueue<backtrack::GpuFrame> queue(2);
    backtrack::GpuFrame queued, popped, encoderInput;
    source.acquireNextFrame(queued, 0);
    source.acquireNextFrame(popped, 0);
    source.acquireNextFrame(encoderInput, 0);
    expect(queue.tryPush(std::move(queued)), "could not queue a frame");
    bool mapped = true;
    source.requestResize();
    backtrack::GpuFrame next;
    expect(!source.acquireNextFrame(next, 0) && source.resolutionChangeCount() == 1,
           "source did not signal its pending resize");

    expect(backtrack::releaseCaptureFrames(last, acquired, [&] {
        expect(last.lease && acquired.lease, "capture leases released before encoder unmap");
        std::vector<backtrack::GpuFrame> discarded;
        backtrack::retainQueuedCaptureFrames(queue, discarded, &popped);
        expect(discarded.size() == 2 && source.leases == 5,
               "queue discard released a raw capture lease");
        expect(mapped && encoderInput.lease, "in-flight input missing before drain");
        mapped = false;
        encoderInput = {};
        discarded.clear();
        return true;
    }), "resize drain failed");
    expect(!mapped, "resize left an encoder mapping");
    expect(source.acquireNextFrame(next, 0), "source did not acquire after resize");
    expect(source.resizes == 1, "resize was not reached");
    next = {};
    source.shutdown();
}

void testFailedDrainKeepsFrames() {
    LeaseCheckingSource source;
    backtrack::GpuFrame last, acquired;
    source.acquireNextFrame(last, 0);
    source.acquireNextFrame(acquired, 0);
    expect(!backtrack::releaseCaptureFrames(last, acquired, [] { return false; }),
           "failed drain was accepted");
    expect(source.leases == 2 && last.lease && acquired.lease,
           "failed drain released capture frames");
    expect(backtrack::releaseCaptureFrames(last, acquired, [] { return true; }),
           "retry failed");
    source.shutdown();
}

void testRemovedDeviceReleaseBeforeShutdown() {
    LeaseCheckingSource source;
    backtrack::GpuFrame last, acquired;
    source.acquireNextFrame(last, 0);
    source.acquireNextFrame(acquired, 0);
    backtrack::GpuFrame encoderInput = last;
    bool mapped = true;
    expect(backtrack::releaseCaptureFrames(last, acquired, [&] {
        expect(last.lease && mapped, "removed-device input was dropped before shutdown");
        mapped = false;
        encoderInput = {};
        return true;
    }), "removed-device shutdown failed");
    source.shutdown();
    expect(source.shutdowns == 1 && !mapped, "removed-device shutdown order failed");
}

} // namespace

int main() {
    try {
        testResizeDrainsEveryOwner();
        testFailedDrainKeepsFrames();
        testRemovedDeviceReleaseBeforeShutdown();
    } catch (const std::exception& error) {
        std::cerr << "RecorderControllerLeaseTests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
