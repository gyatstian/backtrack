#pragma once

#include <cstdint>
#include <string>

// Name/service-based anti-cheat detection. Backtrack must never inject
// BacktrackGameCapture.dll into a title protected by a kernel anti-cheat:
// doing so is detected as tampering and can ban the player. This guard only
// inspects process image names, loaded module names, and running services via
// the SCM. It performs NO memory reads, driver probing, or kernel access.

namespace backtrack {

enum class AntiCheatKind : uint32_t {
    None = 0,
    Vanguard = 1, // Riot Vanguard (vgc/vgk)
    EAC = 2,      // Easy Anti-Cheat
    BattlEye = 3, // BattlEye (BEClient/BEService/BEDaisy)
};

// Human-readable label for status lines and logs.
const wchar_t* antiCheatDisplayName(AntiCheatKind kind);

// Inspect a target process by its loaded module names. Returns the first
// anti-cheat matched, or None. Requires only PROCESS_QUERY_LIMITED_INFORMATION;
// falls back to the process image base name when module enumeration is blocked.
AntiCheatKind detectForProcess(uint32_t pid);

// Query the Service Control Manager for known anti-cheat drivers/services that
// are installed and running system-wide (e.g. Vanguard's vgk kernel driver,
// which loads at boot independent of the game process).
AntiCheatKind detectSystemDrivers();

// Convenience: anti-cheat present either in the target process or system-wide.
AntiCheatKind detectAntiCheat(uint32_t pid);

} // namespace backtrack
