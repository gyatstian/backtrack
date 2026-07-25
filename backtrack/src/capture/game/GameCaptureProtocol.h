#pragma once

#include <cstdint>
#include <cstdio>

// Shared memory contract between Backtrack.exe and BacktrackGameCapture.dll.
// POD fixed layout; both sides must match kGameCaptureProtocolVersion.

namespace backtrack::gamecap {

constexpr uint32_t kGameCaptureMagic = 0x42544743u; // 'BTGC'
constexpr uint32_t kGameCaptureProtocolVersion = 6;
constexpr uint32_t kGameCaptureMaxSharedTextures = 3;
// CPU-copy fallback ("shmem" transport) is double-buffered: producer writes the
// buffer the reader is not consuming, then publishes its index via seqlock.
constexpr uint32_t kGameCaptureMaxDataBuffers = 2;

// Single-instance host names (Backtrack creates; DLL opens).
constexpr wchar_t kGameCaptureShmName[] = L"Local\\BacktrackGameCap";
constexpr wchar_t kGameCaptureEventName[] = L"Local\\BacktrackGameCapEvt";
constexpr wchar_t kGameCaptureDllFileName[] = L"BacktrackGameCapture.dll";
// Pixel-data mapping (shmem transport) is a separate, resizable mapping. Its
// name is suffixed with a generation counter so a resize creates a fresh
// section object rather than colliding with the reader's stale view. The DLL
// producer bumps GameCaptureSharedHeader::dataMappingGeneration when it recreates it.
constexpr wchar_t kGameCaptureDataShmPrefix[] = L"Local\\BacktrackGameCapData";

// Build the generation-suffixed data-mapping name into caller storage.
inline void gameCaptureDataMappingName(wchar_t (&out)[64], uint32_t generation) {
    swprintf_s(out, L"%s%u", kGameCaptureDataShmPrefix, generation);
}

// header->transportMode selector.
enum class GameCaptureTransport : uint32_t {
    SharedTexture = 0, // GPU shared texture + keyed mutex (preferred, zero-copy)
    SharedMemory = 1,  // CPU staging copy through a separate pixel mapping
};

enum class GameCaptureSlotState : uint32_t {
    Idle = 0,
    Active = 1,
    Error = 2,
    Stopping = 3,
};

enum class GameCaptureGraphicsApi : uint32_t {
    Unknown = 0,
    D3D11 = 1,
    D3D12 = 2,
    OpenGL = 3,
    Vulkan = 4,
    D3D9 = 5,
};

// header->flags bits
constexpr uint32_t kGameCapFlagNtHandle = 1u << 0;
constexpr uint32_t kGameCapFlagPresentHooked = 1u << 1;
constexpr uint32_t kGameCapFlagFactoryHooked = 1u << 2;
constexpr uint32_t kGameCapFlagPublishException = 1u << 3;

#pragma pack(push, 8)
struct GameCaptureFrameDescriptor {
    uint64_t sharedHandle = 0; // HANDLE in target process
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0; // DXGI_FORMAT
    uint32_t flags = 0;
    int64_t presentQpc = 0;
};

struct GameCaptureSharedHeader {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t headerBytes = 0;
    GameCaptureSlotState state = GameCaptureSlotState::Idle;
    // publishEpoch is a cross-process seqlock: odd while producer writes a
    // descriptor, even when complete. Readers must acquire matching even values.
    volatile long publishEpoch = 0;
    uint32_t sequence = 0;
    uint32_t latestReadySlot = 0;
    uint32_t sharedTextureCount = kGameCaptureMaxSharedTextures;
    GameCaptureFrameDescriptor frames[kGameCaptureMaxSharedTextures] = {};
    // Compatibility mirror of frames[latestReadySlot]. New readers use frames.
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint64_t sharedHandle = 0;
    uint32_t targetPid = 0;
    uint32_t hostPid = 0;
    uint32_t errorCode = 0;
    alignas(64) uint32_t framesPublished = 0;
    int64_t lastPresentQpc = 0;
    wchar_t errorText[128] = {};
    // Hook and frame-pipeline diagnostics. QPC durations are accumulated ticks;
    // divide by qpcFrequency for seconds. Counters are incremented atomically
    // by their owning process (DLL producer or host consumer).
    uint32_t flags = 0;
    uint32_t graphicsApi = 0; // GameCaptureGraphicsApi
    uint32_t lastExceptionCode = 0;
    uint32_t hookInstallMs = 0;
    uint64_t qpcFrequency = 0;
    volatile int64_t hookInstallQpc = 0;
    volatile int64_t producerCopyQpc = 0;
    volatile int64_t hostCopyQpc = 0;
    volatile long producerKeyedMutexMisses = 0;
    volatile long hostKeyedMutexMisses = 0;
    volatile long droppedByCadence = 0;
    volatile long droppedByContention = 0;
    volatile long resizeCount = 0;
    volatile long sharedTextureRecreateCount = 0;
    // Host-selected surface and output cadence. HWND is valid only in target process.
    uint64_t targetWindow = 0;
    uint32_t captureFps = 60;

    // --- CPU-copy ("shmem") transport (protocol v6) ---------------------------
    // Active transport. Producer switches to SharedMemory when it cannot publish
    // a shareable GPU texture (e.g. OpenSharedResource / keyed-mutex unavailable).
    uint32_t transportMode = 0; // GameCaptureTransport
    // Generation of the pixel-data mapping the producer currently writes. The
    // producer owns creation (it learns the backbuffer dimensions at Present);
    // a bump forces the reader to close its stale view and reopen the fresh
    // section by name. The reader compares against its last-opened generation.
    // Producer sets the layout fields below, then publishes the generation last.
    volatile long dataMappingGeneration = 0;
    // Layout of the data mapping. dataBufferBytes is per-buffer (row-padded);
    // total mapping is dataBufferCount * dataBufferBytes. dataStride is the
    // row pitch chosen by the producer's staging texture (>= width*bpp).
    uint32_t dataBufferCount = 0;
    uint32_t dataStride = 0;
    uint32_t dataBufferBytes = 0;
    uint32_t dataMappingBytes = 0;
    uint32_t dataWidth = 0;
    uint32_t dataHeight = 0;
    uint32_t dataFormat = 0; // DXGI_FORMAT of the shmem pixels
    // Index of the most recently published data buffer, guarded by publishEpoch.
    volatile long latestDataBuffer = 0;
    uint32_t reserved = 0;
};
#pragma pack(pop)

static_assert(sizeof(GameCaptureSharedHeader) <= 4096, "keep SHM header small");

} // namespace backtrack::gamecap
