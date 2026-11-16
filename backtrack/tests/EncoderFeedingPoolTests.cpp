// Regression for the deep-lookahead encoder starvation (audit finding #04):
// NVENC Custom lookahead can retain many accepted inputs before producing
// output, pinning every encoder-feeding pool surface. The feeding pool must
// be sized from the negotiated input-retention budget (IEncoder::
// inputRetentionBudget) plus the queue and held-frame slack, so a fake
// deep-lookahead encoder backed by a real GpuTexturePool reaches its first
// output without any earlier lease being released. Intermediate queue-sized
// pools are left untouched. This file builds its pool through the same
// two-argument GpuPoolBuilder clamp the scaler uses, so it pins the clamp
// arithmetic; the end-to-end proof that D3D11FrameScaler::setPoolSize keeps a
// negotiated 33-slot pool lives in D3D11FrameScalerTests.
#include "capture/GpuPoolBuilder.h"
#include "capture/GpuTexturePool.h"
#include "encoder/IEncoder.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace backtrack {
namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct TestSlot {
    int tag = 0;
};

// Fake deep-lookahead encoder backed by a real pool: it retains every fed
// lease until its retention fills, then releases the oldest one on its first
// output. Reports the same retention through IEncoder::inputRetentionBudget so
// the test drives the negotiation through the production contract.
struct FakeRetainingEncoder final : IEncoder {
    uint32_t retention = 0;
    std::vector<std::shared_ptr<void>> held;
    uint32_t outputs = 0;

    uint32_t inputRetentionBudget() const override { return retention; }

    bool initialize(D3DDevice&, const VideoSettings&) override { return true; }
    void requestKeyFrame() override {}
    bool resetInputResources() override { return true; }
    bool encodeFrame(const GpuFrame&, EncodedPacket&) override { return false; }
    void drain(std::vector<EncodedPacket>&) override {}
    void shutdown() override {}
    EncoderCapabilities capabilities() const override { return {}; }
    EncoderStats stats() const override { return {}; }
    bool encoderAvailable() const noexcept override { return true; }
    DXGI_FORMAT preferredInputFormat() const override {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    // Feeds one pool-backed frame. Returns true when an output became ready.
    bool feed(std::shared_ptr<void> lease) {
        if (outputs == 0 && held.size() >= retention) {
            // First output: release the oldest retained input's lease.
            held.erase(held.begin());
            ++outputs;
            return true;
        }
        held.push_back(std::move(lease));
        return false;
    }
};

void testDeepLookaheadReachesFirstOutputWithoutEarlierRelease() {
    const uint32_t queueLimit = 4;
    // lookahead depth 24, no B-frames: the deepest real NVENC budget that is
    // not the 31-frame ceiling. Retained inputs alone exceed the intermediate
    // 16-texture cap, so a pool built through the queue-derived clamp starves
    // here.
    const uint32_t retention = 28;
    GpuTexturePool<TestSlot> pool;
    const uint32_t poolSize = GpuPoolBuilder::normalizePoolSize(
        encoderFeedingPoolSizeFor(queueLimit, retention), kMaxEncoderFeedingPoolSize);
    expect(poolSize == queueLimit + retention + 1,
        "the scaler-side clamp dropped part of the negotiated feeding pool");
    for (uint32_t i = 0; i < poolSize; ++i) {
        pool.push_back(std::make_shared<TestSlot>());
    }

    FakeRetainingEncoder encoder;
    encoder.retention = retention;

    // Feed `retention` inputs; every one is retained, none released.
    for (uint32_t i = 0; i < retention; ++i) {
        auto slot = pool.acquire();
        expect(slot != nullptr, "each retained input must get a pool slot");
        expect(!encoder.feed(std::move(slot)),
            "no output may arrive before the retention fills");
    }

    // The (retention+1)-th input must reach the first output while every
    // earlier lease is still held. A pool sized only from the queue depth
    // (5..12) is exhausted here and would starve the producer.
    auto finalSlot = pool.acquire();
    expect(finalSlot != nullptr,
        "feeding pool sized from retention must not starve before the first output");
    expect(encoder.feed(std::move(finalSlot)),
        "deep-lookahead encoder must reach its first output without an earlier lease release");
    expect(encoder.outputs == 1, "exactly one output after the retention fills");
    expect(encoder.held.size() == retention - 1,
        "the first output released exactly one retained input");
}

void testNegotiationBounds() {
    // Feeding pool = queued + retained + one held/scale slot.
    expect(encoderFeedingPoolSizeFor(4, 20) == 4 + 20 + 1,
        "queued + retained + held-frame slack");
    // Deepest real NVENC budget (31 lookahead + 2 B-frame + 4 buffers, 8
    // queue) fits inside the feeding-pool ceiling.
    expect(encoderFeedingPoolSizeFor(8, 40) == 8 + 40 + 1,
        "deepest real NVENC budget is not over-capped");
    // The ceiling is a safety net, never hit by real settings.
    expect(encoderFeedingPoolSizeFor(8, 100) == kMaxEncoderFeedingPoolSize,
        "safety ceiling clamps a synthetic oversized budget");
    // Floor applies even with a tiny queue and no retention.
    expect(encoderFeedingPoolSizeFor(0, 0) == kMinTexturePoolSize,
        "floor applies for an empty queue");
    // Intermediate pools are NOT enlarged by the negotiation.
    expect(texturePoolSizeForQueueLimit(8) <= kMaxTexturePoolSize,
        "queue-derived pools stay within the intermediate cap");
    expect(kMaxEncoderFeedingPoolSize > kMaxTexturePoolSize,
        "only the feeding pool may exceed the intermediate cap");
}

// The scaler normalizes its pool with the feeding ceiling; a pool it clamps
// back to the intermediate cap is the starvation the deep-lookahead case
// above shows. Both ceilings go through the one GpuPoolBuilder clamp.
void testScalerSideClampKeepsFeedingCeiling() {
    const uint32_t negotiated = encoderFeedingPoolSizeFor(4, 28);
    expect(negotiated == 33u, "lookahead-24 no-B-frame budget negotiates 33 textures");
    expect(GpuPoolBuilder::normalizePoolSize(negotiated, kMaxEncoderFeedingPoolSize) == negotiated,
        "scaler-side clamp must keep a 33-texture feeding pool");
    expect(GpuPoolBuilder::normalizePoolSize(kMaxEncoderFeedingPoolSize, kMaxEncoderFeedingPoolSize) ==
            kMaxEncoderFeedingPoolSize,
        "scaler-side clamp accepts its own ceiling");
    expect(GpuPoolBuilder::normalizePoolSize(kMaxEncoderFeedingPoolSize + 1, kMaxEncoderFeedingPoolSize) ==
            kMaxEncoderFeedingPoolSize,
        "scaler-side clamp still caps above the ceiling");
    expect(GpuPoolBuilder::normalizePoolSize(33u) == kMaxTexturePoolSize,
        "queue-derived clamp must still cap at the intermediate size");
    expect(GpuPoolBuilder::normalizePoolSize(0, kMaxEncoderFeedingPoolSize) == kMinTexturePoolSize,
        "the feeding ceiling keeps the shared floor");
}

void testNoRetentionStillServesQueueBacklog() {
    // AMF-style encoder (copies inputs, retains nothing, budget 0): the
    // feeding pool still covers a full queue plus the next in-flight scale.
    const uint32_t queueLimit = 4;
    GpuTexturePool<TestSlot> pool;
    const uint32_t poolSize = GpuPoolBuilder::normalizePoolSize(
        encoderFeedingPoolSizeFor(queueLimit, 0), kMaxEncoderFeedingPoolSize);
    for (uint32_t i = 0; i < poolSize; ++i) {
        pool.push_back(std::make_shared<TestSlot>());
    }

    std::vector<std::shared_ptr<void>> queued;
    for (uint32_t i = 0; i < queueLimit; ++i) {
        auto slot = pool.acquire();
        expect(slot != nullptr, "a full queue gets its slots");
        queued.push_back(std::move(slot));
    }
    auto scaleSlot = pool.acquire();
    expect(scaleSlot != nullptr, "a free slot must remain for the in-flight scale");
}

} // namespace
} // namespace backtrack

using namespace backtrack;

int main() {
    try {
        testDeepLookaheadReachesFirstOutputWithoutEarlierRelease();
        testNegotiationBounds();
        testScalerSideClampKeepsFeedingCeiling();
        testNoRetentionStillServesQueueBacklog();
        return 0;
    } catch (const std::exception& error) {
        std::fputs(error.what(), stderr);
        std::fputc('\n', stderr);
        return 1;
    }
}
