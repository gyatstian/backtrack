#pragma once

#include <Windows.h>

#include <cstdint>

namespace backtrack::gamecap {

// OpenGL exclusive capture via wglSwapBuffers inline hook.
// Publishes into the same SHM header / shared D3D11 texture as DXGI path.

struct GlCaptureDeps {
    // Filled by host DLL globals; GL path creates its own D3D device if null.
    void* header = nullptr; // GameCaptureSharedHeader*
    HANDLE event = nullptr;
    bool* activeFlag = nullptr; // atomic bool storage not used; caller checks active
};

// Returns true if opengl32 present and wglSwapBuffers hooked.
bool installGlCapture();
void uninstallGlCapture();
bool glCaptureInstalled();
void resetGlCaptureIpc();

// Called from DXGI install path when GL-only: skip DXGI refuse.
bool processHasOpenGl();

} // namespace backtrack::gamecap
