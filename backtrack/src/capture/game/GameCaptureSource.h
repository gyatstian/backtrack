#pragma once

#include "capture/ICaptureSource.h"
#include "capture/game/AntiCheatGuard.h"
#include "capture/game/GameCaptureProtocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace backtrack {

// Host-side exclusive/game capture: injects BacktrackGameCapture.dll into the
// foreground process and opens shared D3D11 textures published from Present.
class GameCaptureSource final : public ICaptureSource {
public:
    GameCaptureSource();
    ~GameCaptureSource() override;

    bool initialize(D3DDevice& device, const AppSettings& settings, const CaptureTarget& target) override;
    bool acquireNextFrame(GpuFrame& frame, uint32_t timeoutMs) override;
    void shutdown() override;
    bool isDeviceLost() const override;
    CaptureBackend backend() const override { return CaptureBackend::GameCapture; }
    uint32_t width() const override { return width_; }
    uint32_t height() const override { return height_; }

    bool targetProcessExited() const;
    // True when injection was refused because a kernel anti-cheat was detected
    // and the user has not enabled the advanced override. Callers use this to
    // surface an anti-cheat-specific status and fall back to WGC.
    bool antiCheatBlocked() const { return antiCheatBlocked_; }
    AntiCheatKind antiCheatKind() const { return antiCheatKind_; }
    const std::wstring& targetImageName() const { return targetImageName_; }

private:
    struct TextureSlot {
        ComPtr<ID3D11Texture2D> texture;
    };

    bool injectIntoProcess(uint32_t pid);
    bool openSharedMapping(uint32_t hostPid);
    bool openSharedTexture(
        uint32_t sharedSlot,
        HANDLE remoteHandle,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        bool ntHandle);
    bool ensurePool(uint32_t width, uint32_t height, DXGI_FORMAT format);
    // Open/reopen the CPU-copy ("shmem") pixel mapping when the producer bumps
    // dataMappingGeneration. Returns false only on hard failure (device/mapping).
    bool ensureDataMapping(const gamecap::GameCaptureSharedHeader* header);
    // Consume one shmem frame: seqlock-read latestDataBuffer, UpdateSubresource
    // into a pool texture, publish it in frame. Returns false when no new frame.
    bool acquireNextFrameShmem(GpuFrame& frame, uint32_t timeoutMs);
    std::shared_ptr<TextureSlot> acquireSlot();
    void setError(const wchar_t* text);

    D3DDevice* device_ = nullptr;
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    HANDLE frameEvent_ = nullptr;
    HANDLE process_ = nullptr;
    HANDLE remoteThread_ = nullptr;
    uint32_t targetPid_ = 0;
    uint32_t hostPid_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    uint32_t lastSequence_ = 0;
    // Initial shared frame already owns keyed-mutex key 1. Consume it before
    // waiting for a newer sequence so producer can reacquire key 0.
    bool pendingInitialFrame_ = false;
    uint64_t frameIndex_ = 0;
    bool deviceLost_ = false;
    bool injected_ = false;
    bool targetExited_ = false;
    // User opt-in to inject into anti-cheat-protected titles. Default false so
    // the guard fails closed and falls back to Windows Graphics Capture.
    bool allowAntiCheatInject_ = false;
    // Set when inject was refused due to a detected anti-cheat (override off).
    bool antiCheatBlocked_ = false;
    AntiCheatKind antiCheatKind_ = AntiCheatKind::None;
    std::wstring targetImageName_;

    ComPtr<ID3D11Texture2D> sharedTexture_[gamecap::kGameCaptureMaxSharedTextures];
    ComPtr<IDXGIKeyedMutex> sharedMutex_[gamecap::kGameCaptureMaxSharedTextures];
    HANDLE localSharedHandle_[gamecap::kGameCaptureMaxSharedTextures] = {};
    uint64_t openSharedHandleValue_[gamecap::kGameCaptureMaxSharedTextures] = {};

    std::vector<std::shared_ptr<TextureSlot>> pool_;
    uint32_t poolSize_ = 6;

    // --- CPU-copy ("shmem") transport reader state ---------------------------
    HANDLE dataMapping_ = nullptr;
    void* dataView_ = nullptr;
    long dataMappingGeneration_ = 0; // last generation opened; 0 = none
    uint32_t dataBufferCount_ = 0;
    uint32_t dataStride_ = 0;
    uint32_t dataBufferBytes_ = 0;
    uint32_t dataMappingBytes_ = 0;
};

} // namespace backtrack
