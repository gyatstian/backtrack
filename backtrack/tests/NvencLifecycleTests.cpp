#include "encoder/NvencApiTier.h"
#include "encoder/NvencEncoder13.cpp"
#include "app/CaptureFrameHandoff.h"
#include "capture/GpuTexturePool.h"

#if !BACKTRACK_HAS_NVENC
#error NvencLifecycleTests requires the NVENC SDK header
#endif

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace backtrack {

// The tier translation unit is included rather than linked so this suite can
// reach the private implementation. NvencEncoder dispatches between tiers, so
// the name points at the one every current driver uses.
using NvencEncoder = nvenc13::Encoder;
using namespace nvenc13;

struct NvencEncoderTestAccess {
    static void attach(NvencEncoder& encoder, void* session,
                       const NV_ENCODE_API_FUNCTION_LIST& api,
                       std::array<unsigned char, 4>& buffers) {
        auto& impl = *encoder.impl_;
        impl.session = session;
        impl.api = api;
        impl.settings.width = 2;
        impl.settings.height = 2;
        impl.settings.fps = 60;
        impl.caps.available = true;
        impl.publishAvailability(true);
        for (auto& buffer : buffers) {
            impl.bitstreamBuffers.push_back(&buffer);
            impl.availableBitstreams.push_back(&buffer);
        }
    }

    static bool harvest(NvencEncoder& encoder) {
        return encoder.impl_->harvestPendingOutputs(false);
    }

    static bool popReady(NvencEncoder& encoder, EncodedPacket& packet) {
        return encoder.impl_->popReadyPacket(packet);
    }

    static bool inputMappingFaulted(const NvencEncoder& encoder) {
        return encoder.impl_->inputMappingFaulted;
    }

    // B-frame reorder delay, so an early output is not plausibly complete yet.
    static void setBFrames(NvencEncoder& encoder, bool enabled) {
        encoder.impl_->caps.effective.bFrames = enabled;
    }

    static void setLookahead(NvencEncoder& encoder, bool enabled, uint32_t depth) {
        encoder.impl_->caps.effective.lookahead = enabled;
        encoder.impl_->caps.effective.lookaheadDepth = depth;
    }

    static void queueReadyAndDisarmKeyframe(NvencEncoder& encoder) {
        EncodedPacket packet;
        packet.bytes = {0x42};
        packet.pts100ns = 999;
        encoder.impl_->readyPackets.push_back(std::move(packet));
        encoder.impl_->forceKeyFrame = false;
    }

    static bool keyframeArmed(const NvencEncoder& encoder) {
        return encoder.impl_->forceKeyFrame.load();
    }

    static void retainMapping(NvencEncoder& encoder, ID3D11Texture2D* texture) {
        encoder.impl_->retainedMappings.push_back({texture, texture, {}});
    }

    static void exhaustOutputBuffers(NvencEncoder& encoder) {
        encoder.impl_->availableBitstreams.clear();
    }

    // Simulates an output that has been waiting past the lock deadline.
    static void agePendingOutputs(NvencEncoder& encoder, std::chrono::seconds age) {
        for (auto& pending : encoder.impl_->pendingOutputs) {
            pending.submittedAt -= age;
        }
    }
};

} // namespace backtrack

namespace {

using namespace backtrack;

enum class Event { Write, Map, Submit, Complete, Unmap, Release, Unregister, Destroy };

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testPureMappings() {
    const struct { EncoderPreset preset; GUID guid; } presets[] = {
        {EncoderPreset::P1, NV_ENC_PRESET_P1_GUID},
        {EncoderPreset::P2, NV_ENC_PRESET_P2_GUID},
        {EncoderPreset::P3, NV_ENC_PRESET_P3_GUID},
        {EncoderPreset::P4, NV_ENC_PRESET_P4_GUID},
        {EncoderPreset::P5, NV_ENC_PRESET_P5_GUID},
        {EncoderPreset::P6, NV_ENC_PRESET_P6_GUID},
        {EncoderPreset::P7, NV_ENC_PRESET_P7_GUID},
    };
    for (const auto& entry : presets) {
        expect(presetGuid(entry.preset) == entry.guid, "NVENC preset GUID changed");
    }
    const struct { EncoderMode mode; NV_ENC_TUNING_INFO tuning; } modes[] = {
        {EncoderMode::HighQuality, NV_ENC_TUNING_INFO_HIGH_QUALITY},
        {EncoderMode::LowLatency, NV_ENC_TUNING_INFO_LOW_LATENCY},
        {EncoderMode::UltraLowLatency, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY},
        {EncoderMode::Lossless, NV_ENC_TUNING_INFO_LOSSLESS},
        {EncoderMode::UltraHighQuality, NV_ENC_TUNING_INFO_ULTRA_HIGH_QUALITY},
    };
    for (const auto& entry : modes) {
        expect(tuningInfo(entry.mode) == entry.tuning, "NVENC tuning info changed");
    }
    const struct { EncoderMultipass mode; NV_ENC_MULTI_PASS pass; } passes[] = {
        {EncoderMultipass::Disabled, NV_ENC_MULTI_PASS_DISABLED},
        {EncoderMultipass::QuarterResolution, NV_ENC_TWO_PASS_QUARTER_RESOLUTION},
        {EncoderMultipass::FullResolution, NV_ENC_TWO_PASS_FULL_RESOLUTION},
    };
    for (const auto& entry : passes) {
        expect(nvencMultipass(entry.mode) == entry.pass, "NVENC multipass changed");
    }
    for (int value : {(std::numeric_limits<int>::min)(), -1, 7, 999, (std::numeric_limits<int>::max)()}) {
        expect(presetGuid(static_cast<EncoderPreset>(value)) == NV_ENC_PRESET_P4_GUID,
            "invalid NVENC preset did not fall back to P4");
    }
    for (int value : {(std::numeric_limits<int>::min)(), -1, 5, 999, (std::numeric_limits<int>::max)()}) {
        expect(tuningInfo(static_cast<EncoderMode>(value)) == NV_ENC_TUNING_INFO_HIGH_QUALITY,
            "invalid NVENC mode did not fall back to high quality");
    }
    for (int value : {(std::numeric_limits<int>::min)(), -1, 3, 999, (std::numeric_limits<int>::max)()}) {
        expect(nvencMultipass(static_cast<EncoderMultipass>(value)) == NV_ENC_MULTI_PASS_DISABLED,
            "invalid NVENC multipass did not fall back to disabled");
    }
    const struct { uint32_t count; NV_ENC_NUM_REF_FRAMES frames; } references[] = {
        {0, NV_ENC_NUM_REF_FRAMES_1},
        {1, NV_ENC_NUM_REF_FRAMES_1},
        {2, NV_ENC_NUM_REF_FRAMES_2},
        {3, NV_ENC_NUM_REF_FRAMES_3},
        {4, NV_ENC_NUM_REF_FRAMES_4},
        {5, NV_ENC_NUM_REF_FRAMES_4},
        {(std::numeric_limits<uint32_t>::max)(), NV_ENC_NUM_REF_FRAMES_4},
    };
    for (const auto& entry : references) {
        expect(nvencReferenceFrames(entry.count) == entry.frames, "NVENC reference-frame clamp changed");
    }
}

struct FakeNvenc {
    FakeNvenc() { events.reserve(128); }

    std::array<unsigned char, 4> buffers{};
    std::array<uint8_t, 6> bytes{0, 0, 0, 1, 0x65, 0x88};
    std::unordered_map<NV_ENC_OUTPUT_PTR, uint64_t> timestamps;
    std::unordered_map<NV_ENC_INPUT_PTR, uint64_t> inputTokens;
    std::unordered_set<NV_ENC_INPUT_PTR> mappedResources;
    std::unordered_set<uint64_t> completedTokens;
    // Handles in submission order, and the input token the driver encoded into
    // each of them. The driver fills buffers in encode order, so a handle's
    // picture is decided by its submission position, not by the handle.
    std::vector<NV_ENC_OUTPUT_PTR> submissionOrder;
    std::vector<uint64_t> encodeOrder;
    std::vector<NV_ENC_OUTPUT_PTR> lockHandles;
    bool lockedUnsubmitted = false;
    std::vector<Event> events;
    NVENCSTATUS submitStatus = NV_ENC_SUCCESS;
    int completionCredits = 0;
    int outstanding = 0;
    int accepted = 0;
    int unmapAttempts = 0;
    int mapAttempts = 0;
    bool mapped = false;
    bool submittedSinceMap = false;
    bool submittedWithoutFreshMap = false;
    bool failLock = false;
    bool failMap = false;
    bool failRegister = false;
    // This driver answers a not-yet-complete non-blocking lock with
    // NV_ENC_ERR_OUT_OF_MEMORY instead of NV_ENC_ERR_LOCK_BUSY.
    bool outOfMemoryWhenIncomplete = false;
    int lockAttempts = 0;
    bool failUnmap = false;
    bool completeOnEos = false;
    bool invalidUnmap = false;
    bool invalidDestroy = false;
    bool releasedWhileMapped = false;

    static FakeNvenc& from(void* session) { return *static_cast<FakeNvenc*>(session); }

    static NVENCSTATUS NVENCAPI registerResource(void* session, NV_ENC_REGISTER_RESOURCE* resource) {
        if (from(session).failRegister) return NV_ENC_ERR_RESOURCE_REGISTER_FAILED;
        resource->registeredResource = resource->resourceToRegister;
        return NV_ENC_SUCCESS;
    }
    static NVENCSTATUS NVENCAPI map(void* session, NV_ENC_MAP_INPUT_RESOURCE* resource) {
        auto& fake = from(session);
        ++fake.mapAttempts;
        if (fake.failMap) return NV_ENC_ERR_MAP_FAILED;
        fake.mappedResources.insert(resource->registeredResource);
        fake.mapped = true;
        fake.submittedSinceMap = false;
        fake.events.push_back(Event::Map);
        resource->mappedResource = resource->registeredResource;
        resource->mappedBufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
        return NV_ENC_SUCCESS;
    }
    static NVENCSTATUS NVENCAPI submit(void* session, NV_ENC_PIC_PARAMS* pic) {
        auto& fake = from(session);
        if (pic->encodePicFlags & NV_ENC_PIC_FLAG_EOS) {
            if (fake.completeOnEos) fake.completionCredits += fake.outstanding;
            return NV_ENC_SUCCESS;
        }
        fake.events.push_back(Event::Submit);
        fake.submittedWithoutFreshMap |= !fake.mapped || fake.submittedSinceMap;
        fake.submittedSinceMap = true;
        if (fake.submitStatus == NV_ENC_SUCCESS || fake.submitStatus == NV_ENC_ERR_NEED_MORE_INPUT) {
            ++fake.accepted;
            ++fake.outstanding;
            fake.timestamps[pic->outputBitstream] = pic->inputTimeStamp;
            fake.inputTokens[pic->inputBuffer] = pic->inputTimeStamp;
            fake.submissionOrder.push_back(pic->outputBitstream);
        }
        return fake.submitStatus;
    }
    static NVENCSTATUS NVENCAPI lock(void* session, NV_ENC_LOCK_BITSTREAM* output) {
        auto& fake = from(session);
        ++fake.lockAttempts;
        if (fake.failLock) return NV_ENC_ERR_GENERIC;
        if (fake.completionCredits == 0) {
            return fake.outOfMemoryWhenIncomplete ? NV_ENC_ERR_OUT_OF_MEMORY
                                                  : NV_ENC_ERR_LOCK_BUSY;
        }
        const auto position = std::find(fake.submissionOrder.begin(), fake.submissionOrder.end(),
            output->outputBitstream);
        if (position == fake.submissionOrder.end()) {
            fake.lockedUnsubmitted = true;
            return NV_ENC_ERR_INVALID_PARAM;
        }
        --fake.completionCredits;
        --fake.outstanding;
        const size_t submittedIndex = static_cast<size_t>(position - fake.submissionOrder.begin());
        const uint64_t token = submittedIndex < fake.encodeOrder.size()
            ? fake.encodeOrder[submittedIndex]
            : fake.timestamps.at(output->outputBitstream);
        fake.events.push_back(Event::Complete);
        fake.lockHandles.push_back(output->outputBitstream);
        fake.bytes[5] = static_cast<uint8_t>(token);
        output->bitstreamBufferPtr = fake.bytes.data();
        output->bitstreamSizeInBytes = static_cast<uint32_t>(fake.bytes.size());
        fake.completedTokens.insert(token);
        output->outputTimeStamp = token;
        output->outputDuration = 166667;
        output->pictureType = NV_ENC_PIC_TYPE_IDR;
        return NV_ENC_SUCCESS;
    }
    static NVENCSTATUS NVENCAPI unlock(void*, NV_ENC_OUTPUT_PTR) { return NV_ENC_SUCCESS; }
    static NVENCSTATUS NVENCAPI unmap(void* session, NV_ENC_INPUT_PTR input) {
        auto& fake = from(session);
        ++fake.unmapAttempts;
        const auto token = fake.inputTokens.find(input);
        if (token != fake.inputTokens.end()) {
            if (fake.completedTokens.erase(token->second) == 0) {
                fake.invalidUnmap = true;
                return NV_ENC_ERR_GENERIC;
            }
            fake.inputTokens.erase(token);
        }
        if (fake.failUnmap) return NV_ENC_ERR_GENERIC;
        fake.mappedResources.erase(input);
        fake.mapped = !fake.mappedResources.empty();
        fake.events.push_back(Event::Unmap);
        return NV_ENC_SUCCESS;
    }
    static NVENCSTATUS NVENCAPI unregisterResource(void* session, NV_ENC_REGISTERED_PTR) {
        auto& fake = from(session);
        if (fake.mapped) return NV_ENC_ERR_GENERIC;
        fake.events.push_back(Event::Unregister);
        return NV_ENC_SUCCESS;
    }
    static NVENCSTATUS NVENCAPI destroyBuffer(void* session, NV_ENC_OUTPUT_PTR) {
        auto& fake = from(session);
        if (fake.outstanding != 0) fake.invalidDestroy = true;
        return fake.outstanding == 0 ? NV_ENC_SUCCESS : NV_ENC_ERR_GENERIC;
    }
    static NVENCSTATUS NVENCAPI destroyEncoder(void* session) {
        auto& fake = from(session);
        if (fake.outstanding != 0 || fake.mapped) fake.invalidDestroy = true;
        fake.events.push_back(Event::Destroy);
        return fake.invalidDestroy ? NV_ENC_ERR_GENERIC : NV_ENC_SUCCESS;
    }

    NV_ENCODE_API_FUNCTION_LIST api() {
        NV_ENCODE_API_FUNCTION_LIST result{};
        result.nvEncRegisterResource = registerResource;
        result.nvEncMapInputResource = map;
        result.nvEncEncodePicture = submit;
        result.nvEncLockBitstream = lock;
        result.nvEncUnlockBitstream = unlock;
        result.nvEncUnmapInputResource = unmap;
        result.nvEncUnregisterResource = unregisterResource;
        result.nvEncDestroyBitstreamBuffer = destroyBuffer;
        result.nvEncDestroyEncoder = destroyEncoder;
        return result;
    }
};

class InstrumentedPool {
    struct Slot { ComPtr<ID3D11Texture2D> texture; };
    struct Lease {
        std::shared_ptr<Slot> slot;
        FakeNvenc* fake = nullptr;
        ~Lease() {
            // Per texture: several inputs may be mapped at once, so only a
            // release of a texture the encoder still owns is a defect.
            fake->releasedWhileMapped |= fake->mappedResources.count(slot->texture.Get()) != 0;
            fake->events.push_back(Event::Release);
        }
    };

public:
    explicit InstrumentedPool(FakeNvenc& fake, uint32_t slots = 1) : fake_(fake) {
        expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device_, nullptr, &context_)), "create WARP device failed");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = 2;
        desc.Height = 2;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        for (uint32_t index = 0; index < slots; ++index) {
            auto slot = std::make_shared<Slot>();
            expect(SUCCEEDED(device_->CreateTexture2D(&desc, nullptr, &slot->texture)),
                   "create texture failed");
            pool_.push_back(std::move(slot));
        }
    }

    bool available() { return static_cast<bool>(pool_.acquire()); }

    GpuFrame write() {
        auto slot = pool_.acquire();
        expect(static_cast<bool>(slot), "pool slot reused while leased");
        expect(!fake_.mapped, "graphics accessed a mapped input");
        const uint32_t pixels[4] = {1, 2, 3, 4};
        context_->UpdateSubresource(slot->texture.Get(), 0, nullptr, pixels, 2 * sizeof(uint32_t), 0);
        fake_.events.push_back(Event::Write);
        GpuFrame frame;
        frame.texture = slot->texture;
        auto lease = std::make_shared<Lease>();
        lease->slot = std::move(slot);
        lease->fake = &fake_;
        frame.lease = std::move(lease);
        frame.width = 2;
        frame.height = 2;
        frame.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        frame.duration100ns = 166667;
        return frame;
    }

private:
    FakeNvenc& fake_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    GpuTexturePool<Slot> pool_;
};

size_t eventAfter(const FakeNvenc& fake, Event event, size_t start = 0) {
    for (size_t index = start; index < fake.events.size(); ++index) {
        if (fake.events[index] == event) return index;
    }
    throw std::runtime_error("required lifecycle event was missing or out of order");
}

void attach(NvencEncoder& encoder, FakeNvenc& fake) {
    NvencEncoderTestAccess::attach(encoder, &fake, fake.api(), fake.buffers);
}

void testCompletionBeforeReuse() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    frame = {};
    expect(!pool.available(), "pending input lease was released");
    fake.completionCredits = 1;
    expect(NvencEncoderTestAccess::harvest(encoder), "completed output was not harvested");
    expect(!fake.mapped && !fake.releasedWhileMapped, "input was released while still mapped");
    expect(pool.available(), "completed input did not return to the pool");
    auto reused = pool.write();
    expect(reused.lease != nullptr, "reused frame has no lease");
    size_t position = 0;
    for (Event event : {Event::Write, Event::Map, Event::Submit, Event::Complete,
                        Event::Unmap, Event::Release, Event::Write}) {
        position = eventAfter(fake, event, position) + 1;
    }
}

void testRemapAfterCompletion() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    fake.completionCredits = 1;
    expect(NvencEncoderTestAccess::harvest(encoder), "first output was not harvested");
    expect(!fake.mapped, "first input remained mapped after completion");
    frame.pts100ns = 166667;
    encoder.encodeFrame(frame, packets);
    expect(fake.accepted == 2 && fake.mapAttempts == 2,
           "a completed input was resubmitted without a fresh mapping");
    fake.completionCredits = 1;
    expect(NvencEncoderTestAccess::harvest(encoder), "second output was not harvested");
    frame = {};
    expect(pool.available() && !fake.mapped && !fake.releasedWhileMapped && !fake.invalidUnmap,
           "resubmitted input was not safely released");
}

void testReorderedInputRetirement() {
    FakeNvenc fake;
    InstrumentedPool poolA(fake);
    InstrumentedPool poolB(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    auto first = poolA.write();
    auto second = poolB.write();
    // O1 carries the second input's picture, O2 the first one's.
    fake.encodeOrder = {2, 1};
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(first, packets);
    encoder.encodeFrame(second, packets);
    first = {};
    second = {};
    fake.completionCredits = 1;
    expect(NvencEncoderTestAccess::harvest(encoder), "reordered output was not harvested");
    expect(!fake.invalidUnmap, "an input was unmapped before its own output completed");
    expect(poolB.available() && !poolA.available(),
           "reordered output retired the wrong input");
    fake.completionCredits = 1;
    expect(NvencEncoderTestAccess::harvest(encoder), "second reordered output was not harvested");
    expect(poolA.available() && poolB.available() && !fake.mapped,
           "reordered inputs were not retired in output-timestamp order");
}

// The SDK requires the output buffers to be locked in submission order: the
// driver writes encode-order pictures into them, so buffer N does not
// necessarily carry submission N's input. The buffers stay in submission order
// while retirement follows the timestamp.
void testOutputBuffersLockInSubmissionOrder() {
    FakeNvenc fake;
    InstrumentedPool poolA(fake);
    InstrumentedPool poolB(fake);
    InstrumentedPool poolC(fake);
    InstrumentedPool poolD(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    std::vector<GpuFrame> frames{poolA.write(), poolB.write(), poolC.write(), poolD.write()};
    for (size_t index = 0; index < frames.size(); ++index) {
        frames[index].pts100ns = 1000 * static_cast<int64_t>(index + 1);
    }
    std::vector<EncodedPacket> packets;
    for (auto& frame : frames) {
        encoder.encodeFrame(frame, packets);
    }
    frames.clear();
    expect(fake.accepted == 4, "not every frame was submitted");

    // Encode order: pictures 1, 4, 2, 3 land in buffers O1, O2, O3, O4.
    fake.encodeOrder = {1, 4, 2, 3};
    fake.completionCredits = 4;
    expect(NvencEncoderTestAccess::harvest(encoder), "the reordered outputs were not harvested");

    expect(!fake.lockedUnsubmitted, "a buffer that was never submitted was locked");
    expect(fake.lockHandles == fake.submissionOrder,
           "output buffers were not locked in submission order");

    std::vector<EncodedPacket> published;
    EncodedPacket packet;
    while (NvencEncoderTestAccess::popReady(encoder, packet)) {
        published.push_back(std::move(packet));
    }
    const int64_t expectedPts[] = {1000, 4000, 2000, 3000};
    const uint8_t expectedToken[] = {1, 4, 2, 3};
    expect(published.size() == 4, "a reordered output was lost or duplicated");
    for (size_t index = 0; index < published.size(); ++index) {
        expect(published[index].pts100ns == expectedPts[index],
               "packets did not leave in encode order");
        expect(published[index].bytes.size() == fake.bytes.size() &&
               published[index].bytes.back() == expectedToken[index],
               "a packet carried another input's payload");
    }
    expect(poolA.available() && poolB.available() && poolC.available() && poolD.available() &&
           !fake.mapped && !fake.invalidUnmap && !fake.releasedWhileMapped,
           "a reordered input was retired at the wrong time");
}

void testMapFailureReleasesInput() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    fake.failMap = true;
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    expect(fake.accepted == 0 && fake.mapAttempts == 2, "map failure was not retried once");
    frame = {};
    expect(!fake.mapped && !fake.releasedWhileMapped && pool.available(),
           "map failure released or retained the producer input");
}

void testRepeatedInput() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    fake.completeOnEos = true;
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    frame.pts100ns = 166667;
    encoder.encodeFrame(frame, packets);
    expect(fake.accepted == 1, "a texture already mapped by an in-flight submission was submitted again");
    expect(!fake.submittedWithoutFreshMap, "repeated input was submitted without a fresh mapping");
    frame = {};
    fake.completionCredits = 1;
    expect(NvencEncoderTestAccess::harvest(encoder), "first repeated output was not harvested");
    expect(pool.available() && !fake.mapped && !fake.releasedWhileMapped &&
           !fake.invalidUnmap && !fake.invalidDestroy,
           "repeated input was not safely released");
}

// A same-size producer reuses one texture for idle heartbeats, so a repeat
// submission must first give the encoder one bounded chance to retire the
// earlier one instead of dropping itself out of the pipeline.
void testRepeatedInputHarvestsBeforeDropping() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    auto frame = pool.write();
    frame.pts100ns = 1000;
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    expect(fake.accepted == 1, "the first submission was not accepted");

    fake.completionCredits = 1;
    frame.pts100ns = 2000;
    encoder.encodeFrame(frame, packets);
    expect(fake.accepted == 2,
           "a repeated input was dropped without harvesting its pending output");
    expect(!fake.submittedWithoutFreshMap, "the repeat was submitted without a fresh mapping");
    expect(!fake.invalidUnmap && !fake.releasedWhileMapped,
           "the repeat was submitted over a texture the encoder still owned");
    expect(packets.size() == 1 && packets.front().pts100ns == 1000,
           "the harvest that unblocked the repeat did not publish the first output");
    expect(encoder.stats().droppedFrames == 0, "the harvestable repeat was counted as a drop");

    fake.completionCredits = 1;
    expect(NvencEncoderTestAccess::harvest(encoder), "the repeated output was not harvested");
    frame = {};
    expect(pool.available() && !fake.mapped && !fake.releasedWhileMapped && !fake.invalidUnmap,
           "the repeated input was not safely released");
}

void testDroppedInputsRearmAndReturnReadyTail() {
    enum class DropCase { Repeated, Retained, Registration, OutputPool };
    for (const auto test : {DropCase::Repeated, DropCase::Retained, DropCase::Registration, DropCase::OutputPool}) {
        FakeNvenc fake;
        InstrumentedPool pool(fake);
        NvencEncoder encoder;
        attach(encoder, fake);
        fake.completeOnEos = true;
        auto frame = pool.write();
        std::vector<EncodedPacket> packets;
        const wchar_t* expectedLog = nullptr;
        switch (test) {
        case DropCase::Repeated:
            encoder.encodeFrame(frame, packets);
            expectedLog = L"NVENC frame dropped: input texture still mapped; keyframe re-armed";
            break;
        case DropCase::Retained:
            NvencEncoderTestAccess::retainMapping(encoder, frame.texture.Get());
            fake.mappedResources.insert(frame.texture.Get());
            fake.mapped = true;
            expectedLog = L"NVENC frame dropped: retained input mapping; keyframe re-armed";
            break;
        case DropCase::Registration:
            fake.failRegister = true;
            expectedLog = L"NVENC frame dropped: no registered input resource; keyframe re-armed";
            break;
        case DropCase::OutputPool:
            NvencEncoderTestAccess::exhaustOutputBuffers(encoder);
            expectedLog = L"NVENC output buffer pool is full; dropping frame until delayed output drains";
            break;
        }
        NvencEncoderTestAccess::queueReadyAndDisarmKeyframe(encoder);
        encoder.encodeFrame(frame, packets);
        expect(packets.size() == 1 && packets.front().pts100ns == 999 &&
               packets.front().bytes == std::vector<uint8_t>{0x42},
            "a dropped input stranded or changed the ready output tail");
        expect(encoder.stats().droppedFrames == 1 && NvencEncoderTestAccess::keyframeArmed(encoder),
            "a dropped input did not count exactly once and re-arm the next keyframe");
        const int submitted = test == DropCase::Repeated ? 1 : 0;
        expect(fake.accepted == submitted && fake.mapAttempts == submitted && !fake.submittedWithoutFreshMap,
            "a dropped input was mapped or submitted again");
        const auto lines = Logger::instance().recentLines(1);
        expect(!lines.empty() && lines.back().find(expectedLog) != std::wstring::npos,
            "a drop path changed its diagnostic text or emitted an extra warning");
        expect(encoder.resetInputResources(), "drop-path cleanup could not retire inputs");
        frame = {};
        expect(!fake.mapped && !fake.releasedWhileMapped && pool.available(),
            "drop-path cleanup released a still-mapped input");
    }
}

void testRetentionBudgetUsesPipelineDelay() {
    NvencEncoder encoder;
    for (const bool bFrames : {false, true}) {
        NvencEncoderTestAccess::setBFrames(encoder, bFrames);
        for (const bool lookahead : {false, true}) {
            for (const uint32_t depth : {0u, 24u, 31u, 100u}) {
                NvencEncoderTestAccess::setLookahead(encoder, lookahead, depth);
                const uint32_t delay = (bFrames ? 2u : 0u) + (lookahead ? depth : 0u);
                expect(encoder.inputRetentionBudget() == std::min(delay + 4, 40u),
                    "the retention budget no longer includes the active pipeline delay and headroom");
            }
        }
    }
}

void testRejectedSubmission() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    fake.submitStatus = NV_ENC_ERR_ENCODER_BUSY;
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    frame = {};
    expect(fake.accepted == 0 && !fake.mapped && !fake.releasedWhileMapped && pool.available(),
           "rejected input was returned to its producer before unmap");
}

void testResetAndCaptureHandoff() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    fake.submitStatus = NV_ENC_ERR_NEED_MORE_INPUT;
    auto last = pool.write();
    GpuFrame acquired;
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(last, packets);
    fake.completionCredits = 1;
    expect(releaseCaptureFrames(last, acquired, [&] {
        expect(encoder.resetInputResources(), "input reset failed");
        expect(last.lease && !fake.mapped, "capture lease was not held through reset unmap");
        return encoder.encoderAvailable();
    }), "input reset failed");
    expect(pool.available() && !fake.releasedWhileMapped, "reset released a mapped input");
    size_t position = 0;
    for (Event event : {Event::Write, Event::Map, Event::Submit, Event::Complete,
                        Event::Unmap, Event::Unregister, Event::Release}) {
        position = eventAfter(fake, event, position) + 1;
    }
}

void testFailedResetKeepsCaptureFrame() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    fake.submitStatus = NV_ENC_ERR_NEED_MORE_INPUT;
    auto last = pool.write();
    GpuFrame acquired;
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(last, packets);
    fake.completionCredits = 1;
    fake.failUnmap = true;
    const bool released = releaseCaptureFrames(last, acquired, [&] {
        encoder.resetInputResources();
        return encoder.encoderAvailable();
    });
    expect(!released && last.lease && !pool.available() && !fake.invalidDestroy,
           "failed reset released capture ownership");
}

void testFailedLockRetainsInput() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    frame = {};
    fake.failLock = true;
    NvencEncoderTestAccess::harvest(encoder);
    expect(!pool.available() && !fake.invalidUnmap && !fake.invalidDestroy,
           "lock failure released or destroyed an input with work outstanding");
    expect(encoder.stats().droppedFrames == 1 && !encoder.encoderAvailable(),
           "lock failure did not count the dropped frame or fault the encoder");
}

void testShutdownRetainsUnfinishedInput() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    frame = {};
    fake.failLock = true;
    encoder.shutdown();
    expect(!pool.available() && !fake.invalidUnmap && !fake.invalidDestroy,
           "shutdown released or destroyed an unfinished input");
}

void testFailedUnmapRetainsInput() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    frame = {};
    fake.failUnmap = true;
    fake.completionCredits = 1;
    NvencEncoderTestAccess::harvest(encoder);
    expect(fake.unmapAttempts > 0 && !pool.available() && !fake.releasedWhileMapped,
           "unmap failure was not handled or released a mapped input");
}

// Regression: this driver answers a not-yet-complete non-blocking lock with
// NV_ENC_ERR_OUT_OF_MEMORY. That is a "not ready" status, not a fault: the
// encoder must stay available and publish the output once it completes.
void testIncompleteLockStatusIsNotAFault() {
    FakeNvenc fake;
    InstrumentedPool pool(fake, 4);
    NvencEncoder encoder;
    attach(encoder, fake);
    // B-frames: nothing is complete until the reorder pipeline is full.
    NvencEncoderTestAccess::setBFrames(encoder, true);
    fake.outOfMemoryWhenIncomplete = true;
    std::vector<GpuFrame> frames;
    for (int index = 0; index < 4; ++index) {
        frames.push_back(pool.write());
        frames.back().pts100ns = 166667 * (index + 1);
    }
    std::vector<EncodedPacket> packets;
    for (auto& frame : frames) {
        encoder.encodeFrame(frame, packets);
        expect(encoder.encoderAvailable(), "an incomplete non-blocking lock faulted the encoder");
        expect(!NvencEncoderTestAccess::inputMappingFaulted(encoder),
               "a not-ready lock latched the input-mapping fault");
    }
    expect(fake.lockAttempts > 0, "the early lock status was never exercised");
    expect(fake.accepted == 4, "submissions stopped after a not-ready lock");

    // The same picture completes later: the encoder must still publish it.
    fake.completionCredits = 1;
    expect(NvencEncoderTestAccess::harvest(encoder), "the completed output was not harvested");
    EncodedPacket packet;
    expect(NvencEncoderTestAccess::popReady(encoder, packet), "no packet was published");
    expect(!packet.bytes.empty(), "the published packet is empty");
    expect(encoder.encoderAvailable(), "the encoder stayed unavailable after publishing");

    // The remaining reordered outputs complete in order; every input must be
    // retired before its lease is released.
    fake.completionCredits = 3;
    expect(NvencEncoderTestAccess::harvest(encoder), "the remaining outputs were not harvested");
    frames.clear();
    expect(!fake.mapped && !fake.invalidUnmap && !fake.releasedWhileMapped,
           "an input was released while still mapped");
}

// A driver that never completes the picture must still escalate after the lock
// deadline, or a real out-of-memory/stuck session would wedge the pipeline.
void testStuckLockStillEscalates() {
    FakeNvenc fake;
    InstrumentedPool pool(fake);
    NvencEncoder encoder;
    attach(encoder, fake);
    fake.outOfMemoryWhenIncomplete = true;
    auto frame = pool.write();
    std::vector<EncodedPacket> packets;
    encoder.encodeFrame(frame, packets);
    expect(encoder.encoderAvailable(), "an early lock status faulted the encoder");

    NvencEncoderTestAccess::agePendingOutputs(encoder, std::chrono::seconds(6));
    expect(!NvencEncoderTestAccess::harvest(encoder),
           "a permanently stuck lock must escalate to a fault");
    expect(!encoder.encoderAvailable(),
           "a stuck lock must publish unavailable for the recreate path");
    expect(encoder.capabilities().detail.find(L"OUT_OF_MEMORY") != std::wstring::npos,
           "the fault detail must name the lock status for Diagnostics");
    // A lock deadline is not an input-binding fault: the session can still
    // retire its inputs, which is what lets the recreate path release them.
    expect(!NvencEncoderTestAccess::inputMappingFaulted(encoder),
           "a stuck lock latched the input-mapping fault");
    fake.outOfMemoryWhenIncomplete = false;
    fake.completionCredits = 1;
    expect(encoder.resetInputResources(), "a faulted session could not retire its inputs");
    frame = {};
    expect(pool.available() && !fake.mapped, "the input was not released after the fault");
    encoder.shutdown();
}

} // namespace

void testApiTierSelection() {
    // Boundaries of NvEncodeAPIGetMaxSupportedVersion's (major << 4) | minor.
    expect(selectNvencApiTier(kNvencApiVersion13) == NvencApiTier::Api13, "13.0 did not select the 13.0 tier");
    expect(selectNvencApiTier(nvencApiVersion(13, 1)) == NvencApiTier::Api13, "13.1 did not select the 13.0 tier");
    expect(selectNvencApiTier(nvencApiVersion(12, 2)) == NvencApiTier::Api11, "12.2 did not fall back to the 11.1 tier");
    expect(selectNvencApiTier(kNvencApiVersion11) == NvencApiTier::Api11, "11.1 did not select the 11.1 tier");
    expect(selectNvencApiTier(nvencApiVersion(11, 0)) == NvencApiTier::Api11, "a driver below every tier did not select the 11.1 tier");
    expect(selectNvencApiTier(0) == NvencApiTier::Api11, "an unreadable driver version did not select the 11.1 tier");
}

int main() {
    try {
        testApiTierSelection();
        testPureMappings();
        testCompletionBeforeReuse();
        testRemapAfterCompletion();
        testReorderedInputRetirement();
        testOutputBuffersLockInSubmissionOrder();
        testMapFailureReleasesInput();
        testRepeatedInput();
        testRepeatedInputHarvestsBeforeDropping();
        testDroppedInputsRearmAndReturnReadyTail();
        testRetentionBudgetUsesPipelineDelay();
        testRejectedSubmission();
        testResetAndCaptureHandoff();
        testFailedResetKeepsCaptureFrame();
        testFailedLockRetainsInput();
        testShutdownRetainsUnfinishedInput();
        testFailedUnmapRetainsInput();
        testIncompleteLockStatusIsNotAFault();
        testStuckLockStillEscalates();
    } catch (const std::exception& error) {
        std::cerr << "NvencLifecycleTests: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
