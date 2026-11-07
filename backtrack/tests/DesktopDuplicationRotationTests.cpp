#include "capture/DesktopDuplicationCapture.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace backtrack {

// Grants the rotation suite access to the private header-inline helpers so it
// exercises the exact production coordinate math without a D3D device.
struct DesktopDuplicationRotationTestAccess {
    static void unrotatedDimensions(DXGI_MODE_ROTATION rotation, uint32_t& width, uint32_t& height) {
        DesktopDuplicationCapture::unrotatedDimensions(rotation, width, height);
    }
    static void transformCursorPosition(DXGI_MODE_ROTATION rotation,
        int32_t physX, int32_t physY,
        uint32_t texW, uint32_t texH,
        int32_t& outX, int32_t& outY) {
        DesktopDuplicationCapture::transformCursorPosition(rotation, physX, physY, texW, texH, outX, outY);
    }
};

} // namespace backtrack

namespace {

using namespace backtrack;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testDimensionNormalization() {
    // A 1080x1920 physical (portrait) display captured as an unrotated
    // 1920x1080 texture: 90 and 270 swap the mode dimensions.
    uint32_t w = 1080, h = 1920;
    DesktopDuplicationRotationTestAccess::unrotatedDimensions(DXGI_MODE_ROTATION_ROTATE90, w, h);
    expect(w == 1920 && h == 1080, "ROTATE90 did not swap dimensions");

    w = 1080;
    h = 1920;
    DesktopDuplicationRotationTestAccess::unrotatedDimensions(DXGI_MODE_ROTATION_ROTATE270, w, h);
    expect(w == 1920 && h == 1080, "ROTATE270 did not swap dimensions");

    w = 1080;
    h = 1920;
    DesktopDuplicationRotationTestAccess::unrotatedDimensions(DXGI_MODE_ROTATION_ROTATE180, w, h);
    expect(w == 1080 && h == 1920, "ROTATE180 must not swap dimensions");

    w = 1080;
    h = 1920;
    DesktopDuplicationRotationTestAccess::unrotatedDimensions(DXGI_MODE_ROTATION_IDENTITY, w, h);
    expect(w == 1080 && h == 1920, "IDENTITY must not swap dimensions");

    w = 1080;
    h = 1920;
    DesktopDuplicationRotationTestAccess::unrotatedDimensions(DXGI_MODE_ROTATION_UNSPECIFIED, w, h);
    expect(w == 1080 && h == 1920, "UNSPECIFIED must not swap dimensions");
}

void testCursorTransform90() {
    // Texture 1920x1080, physical display 1080x1920 (90 deg clockwise).
    int32_t x = 0, y = 0;
    // Top-left of the physical display is the bottom-left of the texture.
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE90, 0, 0, 1920, 1080, x, y);
    expect(x == 0 && y == 1079, "ROTATE90 top-left physical mapped to wrong corner");

    // Bottom-right of the physical display (1079, 1919) is the top-right.
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE90, 1079, 1919, 1920, 1080, x, y);
    expect(x == 1919 && y == 0, "ROTATE90 bottom-right physical mapped to wrong corner");

    // Mid position: physical (100, 200) -> texture (200, 1080-100-1).
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE90, 100, 200, 1920, 1080, x, y);
    expect(x == 200 && y == 979, "ROTATE90 mid position mapped incorrectly");
}

void testCursorTransform270() {
    // Texture 1920x1080, physical display 1080x1920 (90 deg counterclockwise).
    int32_t x = 0, y = 0;
    // Top-left of the physical display is the top-right of the texture.
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE270, 0, 0, 1920, 1080, x, y);
    expect(x == 1919 && y == 0, "ROTATE270 top-left physical mapped to wrong corner");

    // Bottom-right of the physical display (1079, 1919) is the bottom-left.
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE270, 1079, 1919, 1920, 1080, x, y);
    expect(x == 0 && y == 1079, "ROTATE270 bottom-right physical mapped to wrong corner");

    // Mid position: physical (100, 200) -> texture (1920-200-1, 100).
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE270, 100, 200, 1920, 1080, x, y);
    expect(x == 1719 && y == 100, "ROTATE270 mid position mapped incorrectly");
}

void testCursorTransform180() {
    int32_t x = 0, y = 0;
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE180, 0, 0, 1920, 1080, x, y);
    expect(x == 1919 && y == 1079, "ROTATE180 top-left physical mapped to wrong corner");

    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE180, 1919, 1079, 1920, 1080, x, y);
    expect(x == 0 && y == 0, "ROTATE180 bottom-right physical mapped to wrong corner");

    // The -1 in the 180 mapping shifts the center by one pixel.
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_ROTATE180, 960, 540, 1920, 1080, x, y);
    expect(x == 959 && y == 539, "ROTATE180 center mapping changed");
}

void testCursorTransformIdentity() {
    int32_t x = 123, y = 456;
    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_IDENTITY, 123, 456, 1920, 1080, x, y);
    expect(x == 123 && y == 456, "IDENTITY moved the cursor");

    DesktopDuplicationRotationTestAccess::transformCursorPosition(
        DXGI_MODE_ROTATION_UNSPECIFIED, 123, 456, 1920, 1080, x, y);
    expect(x == 123 && y == 456, "UNSPECIFIED moved the cursor");
}

void testCursorTransformAllCorners() {
    // Every rotation maps each physical corner to the correct texture corner.
    const uint32_t texW = 1920, texH = 1080;
    const int32_t rotW = static_cast<int32_t>(texH); // 1080
    const int32_t rotH = static_cast<int32_t>(texW); // 1920
    const struct {
        DXGI_MODE_ROTATION rotation;
        int32_t px;
        int32_t py;
        int32_t ex;
        int32_t ey;
    } cases[] = {
        // top-left, top-right, bottom-left, bottom-right of the physical display
        {DXGI_MODE_ROTATION_ROTATE90, 0, 0, 0, 1079},
        {DXGI_MODE_ROTATION_ROTATE90, rotW - 1, 0, 0, 0},
        {DXGI_MODE_ROTATION_ROTATE90, 0, rotH - 1, 1919, 1079},
        {DXGI_MODE_ROTATION_ROTATE90, rotW - 1, rotH - 1, 1919, 0},
        {DXGI_MODE_ROTATION_ROTATE270, 0, 0, 1919, 0},
        {DXGI_MODE_ROTATION_ROTATE270, rotW - 1, 0, 1919, 1079},
        {DXGI_MODE_ROTATION_ROTATE270, 0, rotH - 1, 0, 0},
        {DXGI_MODE_ROTATION_ROTATE270, rotW - 1, rotH - 1, 0, 1079},
        // 180 leaves the physical dimensions equal to the texture's.
        {DXGI_MODE_ROTATION_ROTATE180, 0, 0, 1919, 1079},
        {DXGI_MODE_ROTATION_ROTATE180, 1919, 0, 0, 1079},
        {DXGI_MODE_ROTATION_ROTATE180, 0, 1079, 1919, 0},
        {DXGI_MODE_ROTATION_ROTATE180, 1919, 1079, 0, 0},
    };
    for (const auto& c : cases) {
        int32_t x = 0, y = 0;
        DesktopDuplicationRotationTestAccess::transformCursorPosition(
            c.rotation, c.px, c.py, texW, texH, x, y);
        expect(x == c.ex && y == c.ey, "corner mapping mismatch");
    }
}

// A pipeline recreate builds a new source on the same output. DXGI reports the
// pointer shape only when it changes, so dropping the carried shape blanks the
// cursor until the next pointer update. A different output or a cleared entry
// (device loss) must not restore a stale cursor.
void testCursorCarryAcrossRebuild() {
    DesktopDuplicationCursorCarry carry;
    DesktopDuplicationCursorSnapshot restored;
    const HMONITOR outputA = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(1));
    const HMONITOR outputB = reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(2));
    expect(!carry.restore(outputA, restored), "an empty carry must not restore a cursor");

    DesktopDuplicationCursorSnapshot saved;
    saved.shapeBuffer = {1, 2, 3, 4};
    saved.shapeInfo.Width = 32;
    saved.shapeInfo.Height = 32;
    saved.shapeInfo.Pitch = 128;
    saved.shapeInfo.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    saved.position = POINT{100, 200};
    saved.visible = true;
    carry.store(outputA, saved);

    expect(carry.restore(outputA, restored), "the same output must keep the cursor state");
    expect(restored.shapeBuffer == saved.shapeBuffer &&
           restored.shapeInfo.Width == 32 && restored.shapeInfo.Pitch == 128 &&
           restored.position.x == 100 && restored.position.y == 200 && restored.visible,
           "the restored cursor state differs from the saved one");
    expect(!carry.restore(outputB, restored), "a different output must not restore a stale cursor");
    expect(!carry.restore(nullptr, restored), "a null output must not restore a cursor");

    carry.clear();
    expect(!carry.restore(outputA, restored), "device loss must not leave a restorable cursor");

    carry.store(nullptr, saved);
    expect(!carry.restore(outputA, restored), "a source without an output must not store a cursor");
}

} // namespace

int main() {
    try {
        testDimensionNormalization();
        testCursorTransform90();
        testCursorTransform270();
        testCursorTransform180();
        testCursorTransformIdentity();
        testCursorTransformAllCorners();
        testCursorCarryAcrossRebuild();
    } catch (const std::exception& e) {
        std::cerr << "DesktopDuplicationRotationTests FAILED: " << e.what() << "\n";
        return 1;
    }
    std::cout << "DesktopDuplicationRotationTests passed\n";
    return 0;
}
