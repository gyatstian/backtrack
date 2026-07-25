#include "capture/game/AntiCheatGuard.h"

#include <Windows.h>
#include <Psapi.h>

#include <array>
#include <filesystem>

namespace backtrack {
namespace {

struct AntiCheatSignature {
    AntiCheatKind kind;
    const wchar_t* token; // matched as a case-insensitive substring
};

// Module/image name fragments. Matched as substrings so we catch decorated
// names like "vgc.exe", "EasyAntiCheat_x64.dll", "BEClient_x64.dll".
constexpr std::array<AntiCheatSignature, 9> kModuleSignatures = {{
    {AntiCheatKind::Vanguard, L"vgc"},
    {AntiCheatKind::Vanguard, L"vgk"},
    {AntiCheatKind::Vanguard, L"vanguard"},
    {AntiCheatKind::EAC, L"easyanticheat"},
    {AntiCheatKind::EAC, L"eac_launcher"},
    {AntiCheatKind::BattlEye, L"beclient"},
    {AntiCheatKind::BattlEye, L"beservice"},
    {AntiCheatKind::BattlEye, L"bedaisy"},
    {AntiCheatKind::BattlEye, L"battleye"},
}};

// System service/driver names registered with the SCM. Vanguard's vgk kernel
// driver loads at boot, so it is detectable even before the game launches.
constexpr std::array<AntiCheatSignature, 6> kServiceSignatures = {{
    {AntiCheatKind::Vanguard, L"vgk"},
    {AntiCheatKind::Vanguard, L"vgc"},
    {AntiCheatKind::EAC, L"EasyAntiCheat"},
    {AntiCheatKind::EAC, L"EasyAntiCheat_EOS"},
    {AntiCheatKind::BattlEye, L"BEService"},
    {AntiCheatKind::BattlEye, L"BEDaisy"},
}};

std::wstring toLower(std::wstring value) {
    for (wchar_t& ch : value) {
        ch = static_cast<wchar_t>(::towlower(ch));
    }
    return value;
}

AntiCheatKind matchModuleName(const std::wstring& loweredName) {
    for (const AntiCheatSignature& sig : kModuleSignatures) {
        if (loweredName.find(sig.token) != std::wstring::npos) {
            return sig.kind;
        }
    }
    return AntiCheatKind::None;
}

std::wstring processImageBaseName(HANDLE process) {
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        return std::filesystem::path(path).filename().wstring();
    }
    return {};
}

// Returns true if the named service/driver exists and is currently running.
bool serviceRunning(SC_HANDLE scm, const wchar_t* name) {
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    if (!svc) {
        return false;
    }
    SERVICE_STATUS status = {};
    bool running = false;
    if (QueryServiceStatus(svc, &status)) {
        running = status.dwCurrentState == SERVICE_RUNNING ||
                  status.dwCurrentState == SERVICE_START_PENDING;
    }
    CloseServiceHandle(svc);
    return running;
}

} // namespace

const wchar_t* antiCheatDisplayName(AntiCheatKind kind) {
    switch (kind) {
        case AntiCheatKind::Vanguard:
            return L"Riot Vanguard";
        case AntiCheatKind::EAC:
            return L"Easy Anti-Cheat";
        case AntiCheatKind::BattlEye:
            return L"BattlEye";
        case AntiCheatKind::None:
        default:
            return L"None";
    }
}

// Inspects a target process. On return, *inspected is set true only if we were
// able to enumerate the process's loaded modules. A successful enumeration that
// finds no anti-cheat module is a strong all-clear: the target is not a
// protected title, even if a kernel anti-cheat is running system-wide for some
// other game. When enumeration fails (access denied, common for genuinely
// protected titles) *inspected stays false and the caller applies the
// conservative system-driver backstop.
AntiCheatKind detectForProcessImpl(uint32_t pid, bool* inspected) {
    if (inspected) {
        *inspected = false;
    }
    if (pid == 0) {
        return AntiCheatKind::None;
    }
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        // Try the lighter right; module enumeration will likely fail but the
        // image name check below can still run.
        process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) {
            return AntiCheatKind::None;
        }
    }

    AntiCheatKind result = AntiCheatKind::None;

    HMODULE modules[1024] = {};
    DWORD needed = 0;
    if (EnumProcessModulesEx(process, modules, sizeof(modules), &needed,
                             LIST_MODULES_ALL)) {
        if (inspected) {
            *inspected = true;
        }
        const DWORD count = needed / sizeof(HMODULE);
        wchar_t name[MAX_PATH] = {};
        for (DWORD i = 0; i < count && i < 1024 && result == AntiCheatKind::None; ++i) {
            if (GetModuleBaseNameW(process, modules[i], name, MAX_PATH) == 0) {
                continue;
            }
            result = matchModuleName(toLower(name));
        }
    }

    if (result == AntiCheatKind::None) {
        // Module enumeration blocked (common for protected titles) or no match:
        // fall back to the process image name.
        const std::wstring image = processImageBaseName(process);
        if (!image.empty()) {
            result = matchModuleName(toLower(image));
        }
    }

    CloseHandle(process);
    return result;
}

AntiCheatKind detectForProcess(uint32_t pid) {
    return detectForProcessImpl(pid, nullptr);
}

AntiCheatKind detectSystemDrivers() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return AntiCheatKind::None;
    }
    AntiCheatKind result = AntiCheatKind::None;
    for (const AntiCheatSignature& sig : kServiceSignatures) {
        if (serviceRunning(scm, sig.token)) {
            result = sig.kind;
            break;
        }
    }
    CloseServiceHandle(scm);
    return result;
}

AntiCheatKind detectAntiCheat(uint32_t pid) {
    bool inspected = false;
    const AntiCheatKind fromProcess = detectForProcessImpl(pid, &inspected);
    if (fromProcess != AntiCheatKind::None) {
        return fromProcess;
    }
    // Strong all-clear: we successfully enumerated the target's modules and
    // found no anti-cheat. Do NOT block on a system-wide driver that belongs to
    // a different game (e.g. Vanguard's vgk left resident after League ran).
    // The system-driver backstop only applies when the target refused
    // inspection, which is itself a hallmark of a protected title.
    if (inspected) {
        return AntiCheatKind::None;
    }
    return detectSystemDrivers();
}

} // namespace backtrack
