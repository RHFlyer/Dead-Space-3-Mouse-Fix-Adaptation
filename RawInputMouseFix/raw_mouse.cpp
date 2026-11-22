// Raw Input Mouse Fix, an add-on for the DS3 Mod Loader made with the mod SDK.
//
// Copyright (c) 2026 slopkhayzo (the Raw Input Mouse Fix); SDK port Copyright (c) 2026 RHFlyer.
// MIT licence: the original's notice and permission text are in LICENSE-RawInputMouseFix.txt, the port's in the
// project's LICENSE.txt; both must stay with copies of this file.
//
// Based on the Raw Input Mouse Fix by slopkhayzo (MIT), https://github.com/slopkhayzo/DeadSpace3-RawInputMouseFix,
// version 1.1.2 (upstream commit 33225cd). All credit for the fix itself, and for finding where the game filters and
// smooths the mouse, goes to its author; their licence is LICENSE-RawInputMouseFix.txt next to this file. This file
// is that fix moved onto the SDK: the same two game functions, the same maths, the same settings and hotkeys. What
// changed, and why, is in CHANGELOG.txt.
//
// What it does: the game turns mouse movement into a turn rate and then filters it (acceleration, a rate limit) in
// one function, and eases the camera after the aim in another. This mod
//   - reads the mouse itself through Windows raw input (the counts the mouse really sent),
//   - hooks the game's mouse rate filter and replaces its output with those counts times a sensitivity: 1:1 aim,
//   - hooks the camera smoothing function and gives it factors of 1, so the camera follows the aim at once.
//
// What it shows about the SDK (an input mod, not only items and menus):
//   - two hooks made with host->hook, with long expected-byte checks, one of them a 6-line naked gate that keeps
//     the registers the game passes in (the SDK's hooks keep EAX, ECX and EDX for you; see HookChaseOrientationDamp);
//   - reading raw mouse input by subclassing the game's window, and putting the window back on unload;
//   - the frame callback (host->onFrame) as a mod's polling loop: hotkeys, the game's own options, a 5 s report,
//     with no thread of its own;
//   - settings that apply at once (DS3ModSettingsChanged), and hot reload (DS3ModShutdown undoes all of the above).

#include "../ds3mod.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <intrin.h>
#include <malloc.h>

#define RAW_MOUSE_VERSION "1.0.0"

namespace {

// ---- The game (Dead Space 3 1.0.0.1, Steam/Origin retail; addresses as Ghidra shows them) ----

constexpr std::uintptr_t kImageBase = 0x00400000;
constexpr unsigned kMouseRateFilter = 0x0040D820;     // __thiscall: (frame time, &horizontal, &vertical) -> accepted
constexpr unsigned kChaseOrientationDamp = 0x0073EF10;  // eases the rendered camera towards the aim
constexpr std::uintptr_t kFrameStamp = 0x012A9A54;    // a counter the game bumps once per frame (.data)
constexpr std::uintptr_t kInputManager = 0x011D1914;  // read by the rate filter; part of its expected bytes
// The options object (built by FUN_0065a740). Its general-settings part at +0x80 holds Control.MouseSensitivity,
// the in-game mouse slider / 100, as a float from 0 to 1, at +0x88.
constexpr std::uintptr_t kOptionsPointer = 0x011D191C;
constexpr std::uintptr_t kGeneralOptionsOffset = 0x80;
constexpr std::uintptr_t kGeneralOptionsVtable = 0x00D7AD00;
constexpr std::uintptr_t kMouseSliderOffset = 0x88;
// Every call of the two functions in this game build. Read only, as a check that this is the build the fix knows.
constexpr std::uintptr_t kRateFilterCalls[] = {0x00549F29, 0x0054F801, 0x00590720, 0x0064788D, 0x006788A9,
                                               0x00679327, 0x0067A4B5, 0x0073F402, 0x0074E6A0};
constexpr std::uintptr_t kOrientationDampCalls[] = {0x0074DA6D, 0x0074DB89, 0x0074DC60};

// The expected bytes for host->hook: the rate filter's first 33 bytes (with the input manager's address inside
// them) and the smoothing function's first 32, as upstream checks them.
constexpr std::uint8_t kRateFilterBytes[] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0xD9, 0xEE, 0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x0D,
    static_cast<std::uint8_t>(kInputManager), static_cast<std::uint8_t>(kInputManager >> 8),
    static_cast<std::uint8_t>(kInputManager >> 16), static_cast<std::uint8_t>(kInputManager >> 24),
    0x8A, 0x81, 0x74, 0x05, 0x00, 0x00, 0x33, 0xDB, 0x88, 0x45, 0xFC, 0x3C, 0x10, 0x73, 0x23};
constexpr std::uint8_t kOrientationDampBytes[] = {
    0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4, 0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04,
    0x89, 0x6C, 0x24, 0x04, 0x8B, 0xEC, 0xD9, 0x43, 0x0C, 0x81, 0xEC, 0x1C, 0x02, 0x00, 0x00, 0xD9};

// With UseGameSlider, SensitivityMultiplier is the multiplier at slider 50; the slider scales it linearly.
constexpr float kSliderCenter = 0.5f;
// Raw counts older than this were collected while nothing read the camera (scripted scenes, menus, cutscenes) and
// must not be replayed afterwards.
constexpr DWORD kStaleRawDeltaMs = 250;

using MouseRateFilter = unsigned(__thiscall*)(void* self, float frameTime, float* horizontal, float* vertical);

// ---- State ----

const DS3ModHost* g_host = nullptr;
wchar_t g_logPath[MAX_PATH] = {};
HANDLE g_instanceMutex = nullptr;

// Set by host->hook: the game's own code. The naked gate below jumps through g_originalOrientationDamp.
MouseRateFilter g_originalFilter = nullptr;
void* g_originalOrientationDamp = nullptr;
bool g_rateHooked = false;
bool g_dampHooked = false;

std::atomic<LONG> g_rawX{0};
std::atomic<LONG> g_rawY{0};
std::atomic<ULONG> g_rawPackets{0};
std::atomic<ULONG> g_filterCalls{0};
std::atomic<ULONG> g_rateReplacements{0};
std::atomic<ULONG> g_thirdPersonCalls{0};
std::atomic<ULONG> g_aimCalls{0};
std::atomic<ULONG> g_staleDrops{0};
// Radians per mouse count, in millionths. The scales are the ones in the ini upstream ships. The multiplier (0.50)
// and UseGameSlider (on) are this add-on's defaults, the setup it was tested with: at the game's slider 50 the
// fix runs at 0.50. A player without ds3_raw_mouse.ini aims exactly as with the file it writes (kDefaultIni).
std::atomic<LONG> g_thirdPersonXMicros{700};
std::atomic<LONG> g_thirdPersonYMicros{1850};
std::atomic<LONG> g_aimXMicros{1100};
std::atomic<LONG> g_aimYMicros{1100};
std::atomic<LONG> g_sensitivityPermille{500};
std::atomic<LONG> g_baseSensitivityPermille{500};
volatile LONG g_useGameSlider = 1;
LONG g_lastSliderPermille = -1;  // frame callback / settings callback only: both run on the game's render thread
volatile LONG g_dampBypasses = 0;
volatile LONG g_rateEnabled = 0;
volatile LONG g_dampEnabled = 0;  // read by the naked gate: a 32-bit int, 0 or 1
volatile LONG g_aimHeld = 0;
volatile LONG g_hotkeysEnabled = 1;
volatile LONG g_diagnosticsEnabled = 1;
// Set on unload when another program's window procedure sits on top of ours and ours can't be taken out: the old
// build then only passes messages on.
volatile LONG g_passThrough = 0;

CRITICAL_SECTION g_frameLock;
std::uint32_t g_cachedFrame = 0xffffffffu;
LONG g_cachedX = 0;
LONG g_cachedY = 0;
DWORD g_lastDrainTick = 0;

struct WindowHook {
    HWND window = nullptr;
    WNDPROC original = nullptr;
    bool othersOnTop = false;  // another procedure went on top of ours (logged once)
};
CRITICAL_SECTION g_windowLock;
WindowHook g_windows[8] = {};

// ---- Logging: DS3RawMouse.log in the mod's folder (the 5 s report goes only there), key lines also to ds3_mods.log

void WriteLogLine(const char* text)
{
    if (!g_logPath[0]) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, g_logPath, L"a") != 0 || !file) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::fprintf(file, "%02u:%02u:%02u.%03u %s\n", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, text);
    std::fclose(file);
}

void Log(const char* format, ...)
{
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments);
    va_end(arguments);
    WriteLogLine(text);
}

// Both logs: for what a player looks for in ds3_mods.log (loaded, refused, hooks).
void Report(const char* format, ...)
{
    char text[512];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments);
    va_end(arguments);
    WriteLogLine(text);
    if (g_host) g_host->log(g_host, text);
}

// ---- Settings: ds3_raw_mouse.ini (mod.ini's ini=), the same file and keys as the standalone fix ----

// Written once if the player has no ds3_raw_mouse.ini yet (an add-on zip never holds the settings file, so an update
// can't reset it). It lists every key with its default, as the fix's own download did.
const char kDefaultIni[] =
    "[Patch]\r\n"
    "; Both patches are on while the mod is switched on (Mods panel, or raw_mouse in ds3_mods.ini).\r\n"
    "RawInput=true\r\n"
    "RemoveCameraSmoothing=true\r\n"
    "; F6 reloads this file. F7/F8 toggle the patches. F9/F10 adjust SensitivityMultiplier.\r\n"
    "Hotkeys=true\r\n"
    "; Writes activity counters to DS3RawMouse.log every five seconds.\r\n"
    "Diagnostics=true\r\n"
    "\r\n"
    "[Mouse]\r\n"
    "; Overall scale applied to every sensitivity. Valid range: 0.10 to 5.00.\r\n"
    "; F9/F10 adjust this value by 0.05 at runtime; F6 restores the value saved here.\r\n"
    "SensitivityMultiplier=0.50\r\n"
    "; true = the in-game mouse sensitivity slider (settings > controls) sets the multiplier:\r\n"
    "; slider 50 = SensitivityMultiplier above, and the slider scales it linearly (25 = half,\r\n"
    "; 100 = double), within 0.10 to 5.00. F9/F10 are then ignored.\r\n"
    "UseGameSlider=true\r\n"
    "; Base radians-per-count scales.\r\n"
    "; The aim profile is active while the right mouse button is held. Other special\r\n"
    "; camera modes use the third-person profile unless the right mouse button is held.\r\n"
    "ThirdPersonSensitivityX=0.000700\r\n"
    "ThirdPersonSensitivityY=0.001850\r\n"
    "AimSensitivityX=0.001100\r\n"
    "AimSensitivityY=0.001100\r\n";

const wchar_t* IniPath()
{
    return g_host && g_host->iniPath ? g_host->iniPath : L"";
}

void WriteDefaultIniIfMissing()
{
    const wchar_t* path = IniPath();
    if (!path[0]) return;
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;  // it exists (the usual case), or can't be made: defaults apply
    DWORD written = 0;
    WriteFile(file, kDefaultIni, static_cast<DWORD>(sizeof(kDefaultIni) - 1), &written, nullptr);
    CloseHandle(file);
    Report("wrote ds3_raw_mouse.ini with the default settings");
}

void ReadIniText(const wchar_t* section, const wchar_t* key, const wchar_t* fallback, char (&out)[64])
{
    wchar_t text[64] = {};
    GetPrivateProfileStringW(section, key, fallback, text, 64, IniPath());
    int i = 0;
    for (; text[i] && i < 63; ++i) out[i] = text[i] < 128 ? static_cast<char>(text[i]) : '?';
    out[i] = '\0';
}

bool ReadBoolean(const wchar_t* key, bool fallback, const wchar_t* section = L"Patch")
{
    char text[64];
    ReadIniText(section, key, fallback ? L"1" : L"0", text);
    return _stricmp(text, "1") == 0 || _stricmp(text, "true") == 0 || _stricmp(text, "yes") == 0 ||
           _stricmp(text, "on") == 0;
}

bool ReadSensitivity(const wchar_t* key, LONG& micros)
{
    char text[64];
    ReadIniText(L"Mouse", key, L"", text);
    if (text[0] == '\0') return false;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value < 0.000000001f || value > 0.1f) return false;
    micros = static_cast<LONG>(value * 1000000.0f + 0.5f);
    return true;
}

bool ReadMultiplier(const wchar_t* key, LONG& permille)
{
    char text[64];
    ReadIniText(L"Mouse", key, L"", text);
    if (text[0] == '\0') return false;
    char* end = nullptr;
    const float value = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value < 0.1f || value > 5.0f) return false;
    permille = static_cast<LONG>(value * 1000.0f + 0.5f);
    return true;
}

void LoadConfig()
{
    LONG thirdX = g_thirdPersonXMicros.load(std::memory_order_relaxed);
    LONG thirdY = g_thirdPersonYMicros.load(std::memory_order_relaxed);
    LONG aimX = g_aimXMicros.load(std::memory_order_relaxed);
    LONG aimY = g_aimYMicros.load(std::memory_order_relaxed);
    LONG multiplier = g_sensitivityPermille.load(std::memory_order_relaxed);
    const bool gotThirdX = ReadSensitivity(L"ThirdPersonSensitivityX", thirdX);
    const bool gotThirdY = ReadSensitivity(L"ThirdPersonSensitivityY", thirdY);
    const bool gotAimX = ReadSensitivity(L"AimSensitivityX", aimX);
    const bool gotAimY = ReadSensitivity(L"AimSensitivityY", aimY);
    const bool gotMultiplier = ReadMultiplier(L"SensitivityMultiplier", multiplier);
    if (gotThirdX) g_thirdPersonXMicros.store(thirdX, std::memory_order_relaxed);
    if (gotThirdY) g_thirdPersonYMicros.store(thirdY, std::memory_order_relaxed);
    if (gotAimX) g_aimXMicros.store(aimX, std::memory_order_relaxed);
    if (gotAimY) g_aimYMicros.store(aimY, std::memory_order_relaxed);
    if (gotMultiplier) {
        g_sensitivityPermille.store(multiplier, std::memory_order_relaxed);
        g_baseSensitivityPermille.store(multiplier, std::memory_order_relaxed);
    }
    InterlockedExchange(&g_useGameSlider, ReadBoolean(L"UseGameSlider", true, L"Mouse") ? 1 : 0);
    g_lastSliderPermille = -1;  // re-apply the slider on the next poll
    InterlockedExchange(&g_rateEnabled, g_rateHooked && ReadBoolean(L"RawInput", true) ? 1 : 0);
    InterlockedExchange(&g_dampEnabled, g_dampHooked && ReadBoolean(L"RemoveCameraSmoothing", true) ? 1 : 0);
    InterlockedExchange(&g_hotkeysEnabled, ReadBoolean(L"Hotkeys", true) ? 1 : 0);
    InterlockedExchange(&g_diagnosticsEnabled, ReadBoolean(L"Diagnostics", true) ? 1 : 0);
    const bool complete = gotThirdX && gotThirdY && gotAimX && gotAimY && gotMultiplier;
    // Only the file name: the full path would show the game folder, which can hold the Windows user name.
    Log("config %s: multiplier=%.2f%s; third X=%.6f Y=%.6f; aim X=%.6f Y=%.6f (ds3_raw_mouse.ini)",
        complete ? "loaded" : "defaults/partial", multiplier / 1000.0,
        InterlockedCompareExchange(&g_useGameSlider, 0, 0) != 0 ? " at slider 50" : "", thirdX / 1000000.0,
        thirdY / 1000000.0, aimX / 1000000.0, aimY / 1000000.0);
}

void LogEffectiveSensitivity(const char* source)
{
    const double multiplier = g_sensitivityPermille.load(std::memory_order_relaxed) / 1000.0;
    Log("%s: multiplier=%.2f third X=%.6f Y=%.6f; aim X=%.6f Y=%.6f", source, multiplier,
        g_thirdPersonXMicros.load(std::memory_order_relaxed) / 1000000.0 * multiplier,
        g_thirdPersonYMicros.load(std::memory_order_relaxed) / 1000000.0 * multiplier,
        g_aimXMicros.load(std::memory_order_relaxed) / 1000000.0 * multiplier,
        g_aimYMicros.load(std::memory_order_relaxed) / 1000000.0 * multiplier);
}

// ---- Reading game memory without crashing ----

bool ReadMemory(std::uintptr_t address, void* destination, std::size_t length)
{
    SIZE_T bytesRead = 0;
    return length != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, length,
                             &bytesRead) != FALSE &&
           bytesRead == length;
}

bool RelativeCallTargets(std::uintptr_t call, std::uintptr_t target)
{
    std::uint8_t instruction[5] = {};
    if (!ReadMemory(call, instruction, sizeof(instruction)) || instruction[0] != 0xE8) return false;
    std::int32_t displacement = 0;
    std::memcpy(&displacement, instruction + 1, sizeof(displacement));
    return static_cast<std::int64_t>(call) + 5 + displacement == static_cast<std::int64_t>(target);
}

bool WritableMemoryAt(std::uintptr_t address, std::size_t length)
{
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = memory.Protect & 0xffu;
    const bool writable = protection == PAGE_READWRITE || protection == PAGE_WRITECOPY ||
                          protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return writable && address + length <= regionEnd;
}

// The game build check that host->hook's expected bytes don't cover: every caller of the two functions, and the
// frame counter's storage. Upstream checks the same.
bool GameBuildMatches()
{
    if (reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) != kImageBase) {
        Report("refused: deadspace3.exe is not at 0x%08X; an unknown game build", static_cast<unsigned>(kImageBase));
        return false;
    }
    for (const std::uintptr_t call : kRateFilterCalls) {
        if (!RelativeCallTargets(call, kMouseRateFilter)) {
            Report("refused: the rate filter call at 0x%08X is not there; an unknown game build",
                   static_cast<unsigned>(call));
            return false;
        }
    }
    for (const std::uintptr_t call : kOrientationDampCalls) {
        if (!RelativeCallTargets(call, kChaseOrientationDamp)) {
            Report("refused: the camera smoothing call at 0x%08X is not there; an unknown game build",
                   static_cast<unsigned>(call));
            return false;
        }
    }
    if (!WritableMemoryAt(kFrameStamp, sizeof(std::uint32_t))) {
        Report("refused: the frame counter at 0x%08X can't be read; an unknown game build",
               static_cast<unsigned>(kFrameStamp));
        return false;
    }
    return true;
}

// One copy per game: the standalone fix (through an ASI loader) and this add-on use the same name, so whichever
// starts second stays off instead of filtering the mouse twice.
bool AcquireInstanceGuard()
{
    wchar_t name[64];
    swprintf_s(name, L"Local\\DS3RawMouseFix-%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexW(nullptr, FALSE, name);
    if (!mutex) {
        Report("refused: could not create its one-copy guard (error %lu)", GetLastError());
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        Report("refused: another Raw Input Mouse Fix is already running in the game (the standalone DS3RawMouse.asi "
               "through an ASI loader?); remove one of them");
        return false;
    }
    g_instanceMutex = mutex;
    return true;
}

// ---- The game's mouse slider (UseGameSlider) ----

bool ReadGameSlider(float& value)
{
    std::uint32_t options = 0;
    std::uint32_t vtable = 0;
    if (!ReadMemory(kOptionsPointer, &options, sizeof(options)) || options == 0 ||
        !ReadMemory(options + kGeneralOptionsOffset, &vtable, sizeof(vtable)) || vtable != kGeneralOptionsVtable ||
        !ReadMemory(options + kMouseSliderOffset, &value, sizeof(value))) {
        return false;
    }
    return std::isfinite(value) && value >= 0.0f && value <= 1.0f;
}

void PollGameSlider()
{
    if (InterlockedCompareExchange(&g_useGameSlider, 0, 0) == 0) return;
    float slider = 0.0f;
    if (!ReadGameSlider(slider)) return;  // options not created yet, or an unexpected layout
    const LONG sliderPermille = static_cast<LONG>(slider * 1000.0f + 0.5f);
    if (sliderPermille == g_lastSliderPermille) return;
    g_lastSliderPermille = sliderPermille;
    const float base = static_cast<float>(g_baseSensitivityPermille.load(std::memory_order_relaxed));
    const LONG permille = std::clamp(static_cast<LONG>(base * slider / kSliderCenter + 0.5f), 100L, 5000L);
    g_sensitivityPermille.store(permille, std::memory_order_relaxed);
    char source[48];
    sprintf_s(source, "game slider %.0f", slider * 100.0f);
    LogEffectiveSensitivity(source);
}

// ---- The two hooks ----

bool GameHasForeground()
{
    DWORD processId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &processId);
    return processId == GetCurrentProcessId();
}

bool RateEnabled() { return InterlockedCompareExchange(&g_rateEnabled, 0, 0) != 0; }
bool DampEnabled() { return InterlockedCompareExchange(&g_dampEnabled, 0, 0) != 0; }
bool AimHeld() { return InterlockedCompareExchange(&g_aimHeld, 0, 0) != 0; }

void ClearRawDelta()
{
    g_rawX.store(0, std::memory_order_relaxed);
    g_rawY.store(0, std::memory_order_relaxed);
    EnterCriticalSection(&g_frameLock);
    g_cachedFrame = 0xffffffffu;
    g_cachedX = 0;
    g_cachedY = 0;
    LeaveCriticalSection(&g_frameLock);
}

// The raw counts for this game frame: collected once per frame (the game may call the filter several times in one),
// and dropped if nothing read them for a while.
void GetFrameRawDelta(LONG& x, LONG& y)
{
    const auto frame = *reinterpret_cast<volatile std::uint32_t*>(kFrameStamp);
    EnterCriticalSection(&g_frameLock);
    if (frame != g_cachedFrame) {
        g_cachedFrame = frame;
        g_cachedX = g_rawX.exchange(0, std::memory_order_acq_rel);
        g_cachedY = g_rawY.exchange(0, std::memory_order_acq_rel);
        const DWORD now = GetTickCount();
        if (now - g_lastDrainTick > kStaleRawDeltaMs) {
            if (g_cachedX != 0 || g_cachedY != 0) g_staleDrops.fetch_add(1, std::memory_order_relaxed);
            g_cachedX = 0;
            g_cachedY = 0;
        }
        g_lastDrainTick = now;
    }
    x = g_cachedX;
    y = g_cachedY;
    LeaveCriticalSection(&g_frameLock);
}

// The game's rate filter is __thiscall (this in ECX); __fastcall with an unused second argument (EDX) matches it.
unsigned __fastcall HookMouseRateFilter(void* self, void*, float frameTime, float* horizontal, float* vertical)
{
    const unsigned accepted = g_originalFilter(self, frameTime, horizontal, vertical);
    g_filterCalls.fetch_add(1, std::memory_order_relaxed);
    if (!RateEnabled()) return accepted;

    // Drain on every call: counts from a frame the game rejects are dropped here instead of being carried into the
    // next accepted frame.
    LONG x = 0;
    LONG y = 0;
    GetFrameRawDelta(x, y);
    if ((accepted & 0xffu) == 0 || horizontal == nullptr || vertical == nullptr || frameTime <= 0.000001f ||
        !GameHasForeground()) {
        return accepted;
    }

    const bool aimProfile = AimHeld();
    if (aimProfile) g_aimCalls.fetch_add(1, std::memory_order_relaxed);
    else g_thirdPersonCalls.fetch_add(1, std::memory_order_relaxed);
    const float multiplier = static_cast<float>(g_sensitivityPermille.load(std::memory_order_relaxed)) / 1000.0f;
    const LONG xMicros = aimProfile ? g_aimXMicros.load(std::memory_order_relaxed)
                                    : g_thirdPersonXMicros.load(std::memory_order_relaxed);
    const LONG yMicros = aimProfile ? g_aimYMicros.load(std::memory_order_relaxed)
                                    : g_thirdPersonYMicros.load(std::memory_order_relaxed);
    const float sensitivityX = static_cast<float>(xMicros) / 1000000.0f * multiplier;
    const float sensitivityY = static_cast<float>(yMicros) / 1000000.0f * multiplier;
    // The game applies its own inversion setting after this. Raw input's positive-right / positive-down signs are
    // the right input here.
    *horizontal = static_cast<float>(x) * sensitivityX / frameTime;
    *vertical = static_cast<float>(y) * sensitivityY / frameTime;
    g_rateReplacements.fetch_add(1, std::memory_order_relaxed);
    return accepted;
}

// The smoothing function takes four stack arguments plus values the game passes in EAX, ECX and EDI (its compiler
// made up its own calling convention). A C++ detour would overwrite those registers before calling the game's code,
// so this gate is written in assembly: it changes only stack arguments 2 and 3 (the pitch and yaw easing factors) and
// jumps on to the game's code without touching a general register. The SDK's hook stubs keep EAX, ECX and EDX on the
// way in and EAX and EDX on the way out; the three callers set ECX again before they use it (checked in Ghidra), so
// the gate works through host->hook, hot reload included.
__declspec(naked) void HookChaseOrientationDamp()
{
    __asm {
        cmp dword ptr [g_dampEnabled], 0
        je passthrough
        // A factor of 1 closes the whole gap between the aim and the rendered camera at once.
        mov dword ptr [esp + 8], 03f800000h
        mov dword ptr [esp + 12], 03f800000h
        lock inc dword ptr [g_dampBypasses]
    passthrough:
        jmp dword ptr [g_originalOrientationDamp]
    }
}

// Once a window can reach our procedure, this build must never leave memory: a message may still be inside it when
// the mod unloads, and if another procedure sits on top, messages keep being passed down to it. Pinning makes the
// loader's FreeLibrary ([mods] hot_reload_free=1) leave this build mapped; a hot reload still loads the new build
// from its own fresh copy.
void PinModule()
{
    static bool pinned = false;
    if (pinned) return;
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&PinModule), &self)) {
        pinned = true;
    } else {
        Log("could not pin this build in memory (error %lu)", GetLastError());
    }
}

// ---- Raw mouse input: a window procedure on the window the game registered for raw mouse input ----

WNDPROC FindOriginalWindowProc(HWND window)
{
    WNDPROC original = nullptr;
    EnterCriticalSection(&g_windowLock);
    for (const auto& entry : g_windows) {
        if (entry.window == window) {
            original = entry.original;
            break;
        }
    }
    LeaveCriticalSection(&g_windowLock);
    return original;
}

LRESULT CALLBACK CaptureWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_INPUT && InterlockedCompareExchange(&g_passThrough, 0, 0) == 0) {
        UINT size = 0;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size,
                            sizeof(RAWINPUTHEADER)) == 0 &&
            size >= sizeof(RAWINPUT)) {
            auto* buffer = static_cast<std::uint8_t*>(_alloca(size));
            UINT received = size;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &received,
                                sizeof(RAWINPUTHEADER)) == received) {
                const auto* input = reinterpret_cast<const RAWINPUT*>(buffer);
                if (input->header.dwType == RIM_TYPEMOUSE && (input->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    const USHORT buttons = input->data.mouse.usButtonFlags;
                    if ((buttons & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) InterlockedExchange(&g_aimHeld, 1);
                    if ((buttons & RI_MOUSE_RIGHT_BUTTON_UP) != 0) InterlockedExchange(&g_aimHeld, 0);
                    if (RateEnabled()) {
                        g_rawX.fetch_add(input->data.mouse.lLastX, std::memory_order_relaxed);
                        g_rawY.fetch_add(input->data.mouse.lLastY, std::memory_order_relaxed);
                    }
                    g_rawPackets.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    WNDPROC original = FindOriginalWindowProc(window);
    return original != nullptr ? CallWindowProcW(original, window, message, wParam, lParam)
                               : DefWindowProcW(window, message, wParam, lParam);
}

bool SubclassRawWindow(HWND window)
{
    if (window == nullptr) return false;
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != GetCurrentProcessId()) return false;

    EnterCriticalSection(&g_windowLock);
    for (auto& entry : g_windows) {
        if (entry.window != window) continue;
        const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
        if (current == CaptureWindowProc) {
            LeaveCriticalSection(&g_windowLock);
            return true;
        }
        if (current != entry.original) {
            // Another procedure went on top of ours. It may pass messages down to ours, so putting ours on top again
            // would make a loop (ours -> it -> ours ...). Leave the chain as it is.
            const bool first = !entry.othersOnTop;
            entry.othersOnTop = true;
            LeaveCriticalSection(&g_windowLock);
            if (first) {
                Log("raw-input window hwnd=%p: another procedure is on top of ours (%p); leaving it there", window,
                    current);
            }
            return true;
        }
        // The game put its own procedure back, taking ours out: go on top again.
        SetLastError(ERROR_SUCCESS);
        const auto prior = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CaptureWindowProc)));
        if (prior != nullptr || GetLastError() == ERROR_SUCCESS) {
            entry.original = prior;
            LeaveCriticalSection(&g_windowLock);
            Log("re-subclassed raw-input window hwnd=%p", window);
            return true;
        }
        LeaveCriticalSection(&g_windowLock);
        return false;
    }

    WindowHook* freeEntry = nullptr;
    for (auto& entry : g_windows) {
        if (entry.window == nullptr) {
            freeEntry = &entry;
            break;
        }
    }
    if (freeEntry == nullptr) {
        LeaveCriticalSection(&g_windowLock);
        return false;
    }
    // Filled before the swap: the window's thread may send a message as soon as the new procedure is in.
    freeEntry->window = window;
    freeEntry->original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(window, GWLP_WNDPROC));
    SetLastError(ERROR_SUCCESS);
    const auto prior = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(CaptureWindowProc)));
    if (prior == nullptr && GetLastError() != ERROR_SUCCESS) {
        freeEntry->window = nullptr;
        freeEntry->original = nullptr;
        LeaveCriticalSection(&g_windowLock);
        return false;
    }
    freeEntry->original = prior;
    freeEntry->othersOnTop = false;
    LeaveCriticalSection(&g_windowLock);
    PinModule();
    Log("subclassed raw-input window hwnd=%p original=%p", window, prior);
    return true;
}

// Unload: puts every window's procedure back. If something else has put its own procedure on top of ours since,
// ours can't be taken out of the middle; it then only passes messages on. This build stays in memory either way
// (PinModule), so a message still inside our procedure, or one passed down to it later, never runs freed code.
void RestoreWindows()
{
    EnterCriticalSection(&g_windowLock);
    for (auto& entry : g_windows) {
        if (entry.window == nullptr) continue;
        if (IsWindow(entry.window)) {
            const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(entry.window, GWLP_WNDPROC));
            if (current == CaptureWindowProc) {
                SetWindowLongPtrW(entry.window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(entry.original));
                Log("restored the window procedure of hwnd=%p", entry.window);
            } else {
                InterlockedExchange(&g_passThrough, 1);
                Report("window hwnd=%p: another procedure is on top of ours; ours now only passes messages on",
                       entry.window);
                continue;  // keep entry.original: the pass-through still needs it
            }
        }
        entry.window = nullptr;
        entry.original = nullptr;
    }
    LeaveCriticalSection(&g_windowLock);
}

bool FindRegisteredMouse(HWND& target)
{
    UINT count = 0;
    if (GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1) || count == 0)
        return false;
    RAWINPUTDEVICE devices[16] = {};
    count = (std::min)(count, static_cast<UINT>(_countof(devices)));
    const UINT result = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (result == static_cast<UINT>(-1)) return false;
    for (UINT index = 0; index < result; ++index) {
        if (devices[index].usUsagePage == 0x01 && devices[index].usUsage == 0x02 && devices[index].hwndTarget) {
            target = devices[index].hwndTarget;
            return true;
        }
    }
    return false;
}

void EnsureRawCapture()
{
    if (!GameHasForeground()) InterlockedExchange(&g_aimHeld, 0);
    HWND target = nullptr;
    if (FindRegisteredMouse(target)) SubclassRawWindow(target);
}

// ---- Hotkeys and the polling loop: the frame callback ----

void PollHotkeys()
{
    static bool f6WasDown = false, f7WasDown = false, f8WasDown = false, f9WasDown = false, f10WasDown = false;
    const bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    const bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    // Keys held while the Mods panel has the keyboard, or pressed in another program, are not for this mod.
    const bool active = InterlockedCompareExchange(&g_hotkeysEnabled, 0, 0) != 0 && !g_host->inputBlocked() &&
                        GameHasForeground();
    if (active) {
        if (f6 && !f6WasDown) {
            LoadConfig();
            LogEffectiveSensitivity("F6 reload");
        }
        if (f7 && !f7WasDown) {
            if (!g_dampHooked) {
                Log("F7 ignored: the camera smoothing hook is not installed");
            } else {
                const bool enabled = !DampEnabled();
                InterlockedExchange(&g_dampEnabled, enabled ? 1 : 0);
                Log("F7: chase orientation bypass %s", enabled ? "enabled" : "disabled");
            }
        }
        if (f8 && !f8WasDown) {
            if (!g_rateHooked) {
                Log("F8 ignored: the mouse rate hook is not installed");
            } else {
                const bool enabled = !RateEnabled();
                InterlockedExchange(&g_rateEnabled, enabled ? 1 : 0);
                ClearRawDelta();
                Log("F8: raw rate replacement %s", enabled ? "enabled" : "disabled");
            }
        }
        if (InterlockedCompareExchange(&g_useGameSlider, 0, 0) != 0 && ((f9 && !f9WasDown) || (f10 && !f10WasDown))) {
            Log("F9/F10 ignored: UseGameSlider is on, so the in-game mouse slider sets the multiplier");
        } else if (f9 && !f9WasDown) {
            g_sensitivityPermille.store((std::max)(100L, g_sensitivityPermille.load() - 50L));
            LogEffectiveSensitivity("F9");
        } else if (f10 && !f10WasDown) {
            g_sensitivityPermille.store((std::min)(5000L, g_sensitivityPermille.load() + 50L));
            LogEffectiveSensitivity("F10");
        }
    }
    f6WasDown = f6;
    f7WasDown = f7;
    f8WasDown = f8;
    f9WasDown = f9;
    f10WasDown = f10;
}

struct Report5s {
    ULONG packets = 0, calls = 0, replacements = 0, third = 0, aim = 0, stale = 0, bypasses = 0;
};
Report5s g_prior;
DWORD g_lastCaptureCheck = 0;
DWORD g_lastReport = 0;

void WriteReport(DWORD now)
{
    Report5s n;
    n.packets = g_rawPackets.load();
    n.calls = g_filterCalls.load();
    n.replacements = g_rateReplacements.load();
    n.third = g_thirdPersonCalls.load();
    n.aim = g_aimCalls.load();
    n.stale = g_staleDrops.load();
    n.bypasses = static_cast<ULONG>(InterlockedCompareExchange(&g_dampBypasses, 0, 0));
    // A quiet 5 s (menus, a loading screen, the game in the background) writes nothing.
    if (n.packets != g_prior.packets || n.calls != g_prior.calls || n.bypasses != g_prior.bypasses) {
        Log("5s stats: raw=%lu filter=%lu replaced=%lu profiles(third/aim)=%lu/%lu damp_bypass=%lu stale_drops=%lu "
            "rate=%u damp=%u aim_held=%u",
            n.packets - g_prior.packets, n.calls - g_prior.calls, n.replacements - g_prior.replacements,
            n.third - g_prior.third, n.aim - g_prior.aim, n.bypasses - g_prior.bypasses, n.stale - g_prior.stale,
            RateEnabled() ? 1u : 0u, DampEnabled() ? 1u : 0u, AimHeld() ? 1u : 0u);
    }
    g_prior = n;
    g_lastReport = now;
}

// Once per frame on the game's render thread: what upstream's own thread did every 2 ms (hotkeys) and every 100 ms
// (the raw-input window, the game's slider).
void __cdecl OnFrame(const DS3Frame*, void*)
{
    PollHotkeys();
    const DWORD now = GetTickCount();
    if (now - g_lastCaptureCheck >= 100) {
        EnsureRawCapture();
        PollGameSlider();
        g_lastCaptureCheck = now;
    }
    if (now - g_lastReport >= 5000 && InterlockedCompareExchange(&g_diagnosticsEnabled, 0, 0) != 0) WriteReport(now);
}

bool Hook(const char* name, unsigned address, const std::uint8_t* bytes, unsigned length, void* detour,
          void** original)
{
    return g_host->hook(g_host, name, address, bytes, length, detour, original) != 0;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl DS3ModInit(const DS3ModHost* host)
{
    // hook and onFrame came with API 2.
    if (!host || host->apiVersion < 2 || !DS3MOD_HAS(host, onFrame)) {
        if (host) host->log(host, "needs DS3 Mod Loader API 2 or later (hook, onFrame); not started");
        return 1;
    }
    g_host = host;
    InitializeCriticalSection(&g_frameLock);
    InitializeCriticalSection(&g_windowLock);
    // Find files through modDir, never GetModuleFileName: with hot reload the DLL runs from a copy.
    swprintf_s(g_logPath, L"%ls\\DS3RawMouse.log", host->modDir);
    Log("Raw Input Mouse Fix " RAW_MOUSE_VERSION " (SDK add-on, based on slopkhayzo's fix) loading");

    if (!AcquireInstanceGuard()) return 1;
    if (!GameBuildMatches()) return 1;
    WriteDefaultIniIfMissing();

    g_rateHooked = Hook("mouse rate filter", kMouseRateFilter, kRateFilterBytes, sizeof(kRateFilterBytes),
                        reinterpret_cast<void*>(&HookMouseRateFilter), reinterpret_cast<void**>(&g_originalFilter));
    g_dampHooked = Hook("camera smoothing", kChaseOrientationDamp, kOrientationDampBytes,
                        sizeof(kOrientationDampBytes), reinterpret_cast<void*>(&HookChaseOrientationDamp),
                        &g_originalOrientationDamp);
    if (!g_rateHooked && !g_dampHooked) {
        Report("neither hook could be installed (ds3_mods.log says why); the game's own mouse handling stays");
        return 1;
    }
    LoadConfig();
    host->onFrame(host, &OnFrame, nullptr);
    g_lastReport = GetTickCount();
    Report("initialized: raw=%u smoothing_bypass=%u hotkeys=%u diagnostics=%u; F6 reload, F7/F8 toggle, F9/F10 "
           "multiplier",
           RateEnabled() ? 1u : 0u, DampEnabled() ? 1u : 0u,
           InterlockedCompareExchange(&g_hotkeysEnabled, 0, 0) != 0 ? 1u : 0u,
           InterlockedCompareExchange(&g_diagnosticsEnabled, 0, 0) != 0 ? 1u : 0u);
    if (!g_rateHooked) Report("the mouse rate hook is missing: 1:1 aim is off, camera smoothing removal works");
    if (!g_dampHooked) Report("the camera smoothing hook is missing: smoothing stays, 1:1 aim works");
    LogEffectiveSensitivity("initial sensitivity");
    return 0;
}

// The Mods panel changed a setting (or the ini was edited while the game runs): apply it now, like F6.
extern "C" __declspec(dllexport) void __cdecl DS3ModSettingsChanged()
{
    if (!g_host || (!g_rateHooked && !g_dampHooked)) return;
    LoadConfig();
    LogEffectiveSensitivity("settings changed");
}

// Hot reload. The loader has already stopped OnFrame and both hooks and waited until neither runs; it removes the
// hooks after this returns. What is left is ours to undo: the window procedure and the one-copy guard.
extern "C" __declspec(dllexport) void __cdecl DS3ModShutdown()
{
    RestoreWindows();
    InterlockedExchange(&g_rateEnabled, 0);
    InterlockedExchange(&g_dampEnabled, 0);
    if (g_instanceMutex) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
    }
    Log("unloaded (hot reload)");
}
