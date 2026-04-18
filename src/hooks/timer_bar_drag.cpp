// timer_bar_drag.cpp — Draggable timer bars (cast/action progress bars).
//
// The stock timer bars (nano cast, equip, reload, attack cooldown) are
// small fixed-position RenderWindow_t sprites. This module:
//   1. Hooks CreateTimer to reposition new bars using a saved offset
//   2. Registers a mouse filter so LMB-drag on any bar moves the group
//   3. Persists the offset to AOReloaded.ini via the DValue system
//
// Future extension points:
//   - ScaleAllBars(): call RenderWindow_t::Resize on each bar for
//     configurable dimensions (needs PowerBar_t resize too)
//   - Text overlay: TimerBarBase_c+0x0C already holds a TextLine_t*;
//     hook StartTimerBar to capture the actual nano name from the
//     TimerEntry_t and update the text after creation

#include "hooks/timer_bar_drag.h"
#include "hooks/input_handler.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "ao/types.h"
#include "core/logging.h"
#include "core/settings.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace aor {

// ── Constants ───────────────────────────────────────────────────────────

// Stock default position: X = 40, Y = 40.  Bars stack vertically at 20px spacing.
static constexpr int kDefaultX        = 0x28;   // 40
static constexpr int kDefaultBarW     = 0x80;   // 128
static constexpr int kDefaultBarH     = 0x10;   // 16
static constexpr int kSlotHeight      = 0x14;   // 20

// GUI.dll RVAs.
static constexpr uint32_t kCreateTimerRVA       = 0x518f0;   // TimerBar_c path (logout, camp)
static constexpr uint32_t kCreateGameTimerRVA   = 0x5195b;   // GameTimerBar_c path (equip, nano, attack, reload)
static constexpr uint32_t kRWRepositionRVA      = 0x20f6e;   // RenderWindow_t::Reposition
// RenderWindow_t::SetScale RVA resolved inline in init (0x20b0c).
// RenderWindow_t::Resize at 0x20938 only re-tiles background, doesn't scale content.

// TimerBarBase_c field offsets.
static constexpr int kBarRenderWindow = 0x04;
static constexpr int kBarSlotIndex    = 0x18;

// Future extension offsets (kept for documentation, see plan):
//   kBarPowerBar  = 0x08  — PowerBar_t* for resizing
//   kBarTextLine  = 0x0C  — TextLine_t* for nano name overlay
//   kBarIdentType = 0x10  — timer type (1-5) for type-specific behavior

// TimerSystemModule_t field offsets.
static constexpr int kTSMTimerList    = 0x28;

// ── N3InterfaceModule_t API — name resolution ───────────────────────────
// Function pointers are cached in GUI.dll's data section, filled at
// runtime by the game's module system.  Read via ReadCachedPtr.

// GUI.dll data-section RVAs for N3InterfaceModule_t function pointers.
static constexpr uint32_t kN3IMGetInstanceRVA = 0x1a772c;  // N3InterfaceModule_t::GetInstance
static constexpr uint32_t kN3MsgGetNameRVA    = 0x1a7724;  // N3Msg_GetName

using FnN3IMGetInstance = void*(__cdecl*)();
using FnN3MsgGetName = const char*(__thiscall*)(void* n3im,
                                                 const int* identity1,
                                                 const int* identity2);

// Unused API thunks kept for documentation / future use:
//   kN3MsgGetSkillRVA    = 0x1a7728  — N3Msg_GetSkill(Stat_e, int), 2-param local player overload
//   kN3MsgGetSkill4RVA   = 0x1a773c  — N3Msg_GetSkill(Identity_t&, Stat_e, int, Identity_t&), 4-param
//   kN3MsgTemplateIDRVA  = 0x1a771c  — N3Msg_TemplateIDToDynelID(Identity_t const&)
//   kLDBGetTextStrRVA    = 0x1a85c0  — LDBface::GetText(uint, char const*), string key overload
//   kLDBGetTextIdRVA     = 0x1a85dc  — LDBface::GetText(uint, uint), numeric ID overload
//   kStatCurrentNano     = 53        — player stat that holds some ID during nano cast

// ── CastNanoSpell hook — captures nano identity at cast time ────────────
// N3Msg_CastNanoSpell(Identity_t const&, Identity_t const&) is the function
// GUI.dll calls to initiate a nano cast.  We hook it from Interfaces.dll to
// capture the nano's real identity (type + AOID), which we then use for
// name lookup when the timer bar is created moments later.

using FnCastNanoSpell = void(__thiscall*)(void* n3im,
                                          const int* nanoIdentity,
                                          const int* targetIdentity);
static FnCastNanoSpell g_origCastNanoSpell = nullptr;
static int g_lastCastNanoId[2] = { 0, 0 };  // {type, instance} from last cast

static void __fastcall CastNanoSpellDetour(void* ecx_this, void* /*edx*/,
                                            const int* nanoId,
                                            const int* targetId) {
    if (nanoId) {
        g_lastCastNanoId[0] = nanoId[0];
        g_lastCastNanoId[1] = nanoId[1];
        Log("[timerbar] CastNanoSpell: nano={0x%X,%d} target={0x%X,%d}",
            nanoId[0], nanoId[1],
            targetId ? targetId[0] : 0, targetId ? targetId[1] : 0);
    }
    g_origCastNanoSpell(ecx_this, nanoId, targetId);
}

// LDB function pointers — see "Unused API thunks" comments above.

// ── GUI.dll function pointers ───────────────────────────────────────────

static uintptr_t g_guiBase = 0;

// RenderWindow_t::Reposition(IPoint const&) — writes x,y at +0x2c/+0x30,
// then repositions internal RenderSprite_t grid and refreshes HotSpot children.
using FnRWReposition = void(__thiscall*)(void* renderWindow, const int* ipoint);
static FnRWReposition g_rwReposition = nullptr;

// RenderWindow_t::SetScale(float const&, float const&) — stores scale at
// +0x3c/+0x40, propagates to RenderSprite_t tiles.  Updates dest dimensions
// but does NOT rebuild GPU quad vertices — must follow with Resize.
using FnRWSetScale = void(__thiscall*)(void* renderWindow,
                                        const float* scaleX, const float* scaleY);
static FnRWSetScale g_rwSetScale = nullptr;

// RenderWindow_t::Resize(IPoint const&) — re-tiles the grid AND rebuilds
// RenderSprite_t quad vertices using current scale values.
using FnRWResize = void(__thiscall*)(void* renderWindow, const int* ipoint);
static FnRWResize g_rwResize = nullptr;

// TimerSystemModule_t::GetInstance — lazy singleton accessor.
using FnTSMGetInstance = void*(__cdecl*)();
static FnTSMGetInstance g_tsmGetInstance = nullptr;

// WindowController_c::GetInstance — cursor position at WC+0x48.
using FnWCGetInstance = void*(__cdecl*)();
static FnWCGetInstance g_wcGetInstance = nullptr;

// ── CreateTimer hook types ──────────────────────────────────────────────

// Overload 1: TimerBar_c* __thiscall CreateTimer(int, Identity_t const&,
//             char const*, unsigned int).  RET 0x10.  Logout, camp bars.
using FnCreateTimer = void*(__thiscall*)(void* this_tsm, int parent,
                                         const void* identity,
                                         const char* name, uint32_t color);
static FnCreateTimer g_origCreateTimer = nullptr;

// Overload 2: GameTimerBar_c* __thiscall CreateTimer(
//             weak_ptr<TimerEntry_t const>, Identity_t const&,
//             char const*, unsigned int).  RET 0x14.
//   weak_ptr is 8 bytes by value → 2 dwords on stack.
using FnCreateGameTimer = void*(__thiscall*)(void* this_tsm,
                                             uint32_t wp_lo, uint32_t wp_hi,
                                             const void* identity,
                                             const char* name, uint32_t color);
static FnCreateGameTimer g_origCreateGameTimer = nullptr;

// ── State ───────────────────────────────────────────────────────────────

static int  g_posX = kDefaultX;      // absolute X position (AOR_TBarX)
static int  g_posY = kDefaultX;      // absolute Y of first bar (AOR_TBarY); default 40
static int  g_barW = kDefaultBarW;   // bar width (AOR_TBarW)
static int  g_barH = kDefaultBarH;   // bar height (AOR_TBarH)
static bool g_dragging = false;
static float g_dragStartCursorX = 0;
static float g_dragStartCursorY = 0;
static int   g_dragStartPosX = 0;
static int   g_dragStartPosY = 0;

// Captured from the CreateTimer hook's ECX, or resolved via GetInstance.
static void* g_timerSystemModule = nullptr;

// Forward declarations — called from GetTSM on first resolution.
static void RepositionAllBars();
static void ScaleAllBars();

// Get the TimerSystemModule_t singleton. Tries cached pointer first,
// then falls back to calling GetInstance().
static void* GetTSM() {
    if (g_timerSystemModule) return g_timerSystemModule;
    if (g_tsmGetInstance) {
        g_timerSystemModule = g_tsmGetInstance();
        if (g_timerSystemModule) {
            Log("[timerbar] resolved TSM singleton via GetInstance: %p",
                g_timerSystemModule);
            // Apply saved position/size to any bars that already exist.
            // These functions call GetTSM() but won't recurse because
            // g_timerSystemModule is already set above.
            if (g_posX != kDefaultX || g_posY != kDefaultX)
                RepositionAllBars();
            if (g_barW != kDefaultBarW || g_barH != kDefaultBarH)
                ScaleAllBars();
        }
    }
    return g_timerSystemModule;
}

// ── Position helpers ────────────────────────────────────────────────────

static void RepositionBar(void* barBase) {
    auto* rw = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(barBase) + kBarRenderWindow);
    int slot = *reinterpret_cast<int*>(
        reinterpret_cast<uintptr_t>(barBase) + kBarSlotIndex);

    int pos[2];
    pos[0] = g_posX;
    pos[1] = g_posY + slot * kSlotHeight;

    if (rw && g_rwReposition)
        g_rwReposition(rw, pos);
}

// Walk the game's std::list<TimerBarBase_c*> and reposition every bar.
static void RepositionAllBars() {
    void* tsm = GetTSM();
    if (!tsm) return;

    // The list head pointer lives at TSM+0x28.  It points to a heap-
    // allocated sentinel node for a doubly-linked list.
    auto** sentinel = *reinterpret_cast<void***>(
        reinterpret_cast<uintptr_t>(tsm) + kTSMTimerList);
    if (!sentinel) return;

    auto** node = reinterpret_cast<void**>(*sentinel);  // first real node
    while (node != sentinel) {
        void* barBase = node[2];  // data at list-node +0x08
        if (barBase)
            RepositionBar(barBase);
        node = reinterpret_cast<void**>(*node);  // next
    }
}

// ── Resize helpers ──────────────────────────────────────────────────────

// kBarPowerBar = 0x08  — PowerBar_t* (future: nano name overlay, text)

static void ScaleBar(void* barBase) {
    auto* rw = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(barBase) + kBarRenderWindow);
    if (!rw || !g_rwSetScale || !g_rwResize) return;

    float sx = static_cast<float>(g_barW) / static_cast<float>(kDefaultBarW);
    float sy = static_cast<float>(g_barH) / static_cast<float>(kDefaultBarH);

    // Step 1: Set scale factors on all tiles (updates dest dimensions but
    //         does NOT rebuild GPU quad vertices).
    g_rwSetScale(rw, &sx, &sy);

    // Step 2: Trigger a Resize with the CURRENT stored size.  This calls
    //         RenderSprite_t::Resize on each tile, which reads the scale
    //         values we just set and rebuilds the GPU quad vertices.
    int currentSize[2];
    currentSize[0] = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(rw) + 0x34);
    currentSize[1] = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(rw) + 0x38);
    g_rwResize(rw, currentSize);

    Log("[timerbar] ScaleBar: scale=(%.2f,%.2f) rwSize=(%d,%d)", sx, sy,
        currentSize[0], currentSize[1]);
}

// Walk the game's timer bar list and resize every bar.
static void ScaleAllBars() {
    void* tsm = GetTSM();
    if (!tsm) return;

    auto** sentinel = *reinterpret_cast<void***>(
        reinterpret_cast<uintptr_t>(tsm) + kTSMTimerList);
    if (!sentinel) return;

    auto** node = reinterpret_cast<void**>(*sentinel);
    while (node != sentinel) {
        void* barBase = node[2];
        if (barBase)
            ScaleBar(barBase);
        node = reinterpret_cast<void**>(*node);
    }
}

// ── Cursor helpers ──────────────────────────────────────────────────────

static bool GetCursorPos(float& outX, float& outY) {
    if (!g_wcGetInstance) return false;
    void* wc = g_wcGetInstance();
    if (!wc) return false;
    auto* cursor = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(wc) + 0x48);
    outX = cursor[0];
    outY = cursor[1];
    return true;
}

// ── Hit testing ─────────────────────────────────────────────────────────

// Info returned by a successful hit test — tells the caller where the
// clicked bar actually is on screen so the drag offset can be derived.
struct BarHitInfo {
    int actualX;   // RenderWindow_t +0x2c
    int actualY;   // RenderWindow_t +0x30
    int slot;      // TimerBarBase_c +0x18
};

// Check if (px, py) falls within any active timer bar's bounding rect.
// Reads the ACTUAL position from each bar's RenderWindow_t (+0x2c/+0x30)
// rather than computing from the formula, because bars may have been
// created through paths we don't hook.
// On hit, fills *outHit with the bar's actual position and slot.
static bool HitTestTimerBars(float px, float py, BarHitInfo* outHit) {
    void* tsm = GetTSM();
    if (!tsm) {
        Log("[timerbar] hit test: no TSM singleton");
        return false;
    }

    auto** sentinel = *reinterpret_cast<void***>(
        reinterpret_cast<uintptr_t>(tsm) + kTSMTimerList);
    if (!sentinel) {
        Log("[timerbar] hit test: null sentinel");
        return false;
    }

    int barCount = 0;
    auto** node = reinterpret_cast<void**>(*sentinel);
    while (node != sentinel) {
        void* barBase = node[2];
        if (barBase) {
            void* rw = *reinterpret_cast<void**>(
                reinterpret_cast<uintptr_t>(barBase) + kBarRenderWindow);
            if (rw) {
                int bx = *reinterpret_cast<int*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x2c);
                int by = *reinterpret_cast<int*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x30);
                int bw = *reinterpret_cast<int*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x34);
                int bh = *reinterpret_cast<int*>(
                    reinterpret_cast<uintptr_t>(rw) + 0x38);

                if (px >= bx && px < bx + bw &&
                    py >= by && py < by + bh) {
                    if (outHit) {
                        outHit->actualX = bx;
                        outHit->actualY = by;
                        outHit->slot = *reinterpret_cast<int*>(
                            reinterpret_cast<uintptr_t>(barBase) + kBarSlotIndex);
                    }
                    return true;
                }
            }
            barCount++;
        }
        node = reinterpret_cast<void**>(*node);
    }
    if (barCount == 0)
        Log("[timerbar] hit test: no active bars in list");
    return false;
}

// ── Persistence ─────────────────────────────────────────────────────────

static void LoadPosition() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_TBarX", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        g_posX = v.as_int;
    }
    if (GameAPI::GetVariant("AOR_TBarY", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        g_posY = v.as_int;
    }
    if (GameAPI::GetVariant("AOR_TBarW", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        g_barW = v.as_int;
    }
    if (GameAPI::GetVariant("AOR_TBarH", v) &&
        v.type == static_cast<uint32_t>(VariantType::Int)) {
        g_barH = v.as_int;
    }
    Log("[timerbar] loaded pos=(%d,%d) size=(%d,%d)", g_posX, g_posY, g_barW, g_barH);
}

static void SavePosition() {
    if (!GameAPI::SetDValue) return;

    AOString nameX = AOString::FromShort("AOR_TBarX");
    AOString nameY = AOString::FromShort("AOR_TBarY");
    AOVariant valX = AOVariant::FromInt(g_posX);
    AOVariant valY = AOVariant::FromInt(g_posY);

    GameAPI::SetDValue(nameX, valX);
    GameAPI::SetDValue(nameY, valY);
    Log("[timerbar] saved position: X=%d Y=%d", g_posX, g_posY);
}

// ── CreateTimer hook ────────────────────────────────────────────────────

static void* __fastcall CreateTimerDetour(void* ecx_this, void* /*edx*/,
                                           int parent, const void* identity,
                                           const char* name, uint32_t color) {
    // Capture the singleton pointer on first call.
    if (!g_timerSystemModule)
        g_timerSystemModule = ecx_this;

    // Call the original — creates the bar at the stock hardcoded position.
    void* bar = g_origCreateTimer(ecx_this, parent, identity, name, color);

    if (bar) {
        RepositionBar(bar);
        if (g_barW != kDefaultBarW || g_barH != kDefaultBarH)
            ScaleBar(bar);
    }

    return bar;
}

// ── Nano name lookup ────────────────────────────────────────────────────
// Read a cached function pointer from GUI.dll's data section.
template<typename T>
static T ReadCachedPtr(uint32_t rva) {
    return *reinterpret_cast<T*>(g_guiBase + rva);
}

// Try to get the nano name.  wp_lo is the raw TimerEntry_t* from the
// weak_ptr passed to CreateGameTimer.  identity is {type, instance} but
// for nano timers instance is always 0.  The actual nano AOID is
// somewhere in the TimerEntry_t.
static const char* TryGetNanoName(const void* identity) {
    if (!identity) return nullptr;

    auto* id = reinterpret_cast<const int*>(identity);
    if (id[0] != 3) return nullptr;  // only nano timers (type 3)

    // Use the nano identity captured from CastNanoSpell hook.
    if (g_lastCastNanoId[0] == 0 && g_lastCastNanoId[1] == 0) {
        Log("[timerbar] no captured nano identity");
        return nullptr;
    }

    auto getInst = ReadCachedPtr<FnN3IMGetInstance>(kN3IMGetInstanceRVA);
    auto getName = ReadCachedPtr<FnN3MsgGetName>(kN3MsgGetNameRVA);
    if (!getInst || !getName) return nullptr;

    void* n3im = getInst();
    if (!n3im) return nullptr;

    // Try GetName with the captured identity directly.
    int zeroId[2] = { 0, 0 };
    const char* result = getName(n3im, g_lastCastNanoId, zeroId);
    if (result && result[0] && std::strcmp(result, "NoName") != 0) {
        Log("[timerbar] nano name (from CastNanoSpell id): \"%s\"", result);
        return result;
    }

    // Try reversed parameter order.
    result = getName(n3im, zeroId, g_lastCastNanoId);
    if (result && result[0] && std::strcmp(result, "NoName") != 0) {
        Log("[timerbar] nano name (reversed): \"%s\"", result);
        return result;
    }

    Log("[timerbar] GetName failed for nano id={0x%X,%d}",
        g_lastCastNanoId[0], g_lastCastNanoId[1]);
    return nullptr;
}

// ── CreateGameTimer hook (overload 2 — equip, nano, attack, reload) ────

static void* __fastcall CreateGameTimerDetour(void* ecx_this, void* /*edx*/,
                                               uint32_t wp_lo, uint32_t wp_hi,
                                               const void* identity,
                                               const char* name, uint32_t color) {
    if (!g_timerSystemModule)
        g_timerSystemModule = ecx_this;

    // For nano timers, try to replace the generic "Nano program" label
    // with the actual nano program name.
    const char* actualName = name;
    const char* nanoName = TryGetNanoName(identity);
    if (nanoName)
        actualName = nanoName;

    void* bar = g_origCreateGameTimer(ecx_this, wp_lo, wp_hi,
                                       identity, actualName, color);

    if (bar) {
        RepositionBar(bar);
        if (g_barW != kDefaultBarW || g_barH != kDefaultBarH)
            ScaleBar(bar);
    }

    return bar;
}

// ── Mouse event filter ──────────────────────────────────────────────────

static bool TimerBarMouseFilter(MouseEventType type, const float* pos_or_delta,
                                 int button, int /*flags*/) {
    switch (type) {
    case MouseEventType::Down: {
        // Only handle LMB (button 1).
        if (button != 1) return false;
        if (!pos_or_delta) return false;

        float cx, cy;
        if (!GetCursorPos(cx, cy)) return false;

        BarHitInfo hit{};
        if (!HitTestTimerBars(cx, cy, &hit))
            return false;

        // Derive the actual position from where the bar really is, not from
        // the saved DValue.  Bars may have been created at the default
        // position through a path that ran before our hook was ready.
        g_posX = hit.actualX;
        g_posY = hit.actualY - hit.slot * kSlotHeight;

        // Start drag.
        g_dragging = true;
        g_dragStartCursorX = cx;
        g_dragStartCursorY = cy;
        g_dragStartPosX = g_posX;
        g_dragStartPosY = g_posY;
        Log("[timerbar] drag started at (%.0f, %.0f) pos=(%d,%d)",
            cx, cy, g_posX, g_posY);
        return true;
    }

    case MouseEventType::Move: {
        if (!g_dragging) return false;

        // Read current cursor and compute delta from drag start.
        float cx, cy;
        if (!GetCursorPos(cx, cy)) return false;

        g_posX = g_dragStartPosX + static_cast<int>(cx - g_dragStartCursorX);
        g_posY = g_dragStartPosY + static_cast<int>(cy - g_dragStartCursorY);

        RepositionAllBars();
        return true;
    }

    case MouseEventType::Up: {
        if (!g_dragging) return false;
        // Only end drag on LMB release.
        if (button != 1) return false;

        g_dragging = false;
        SavePosition();
        Log("[timerbar] drag ended, pos: X=%d Y=%d", g_posX, g_posY);
        return true;
    }

    case MouseEventType::EndDrag: {
        if (!g_dragging) return false;

        g_dragging = false;
        SavePosition();
        Log("[timerbar] drag ended (EndDrag), pos: X=%d Y=%d",
            g_posX, g_posY);
        return true;
    }
    }

    return false;
}

// ── Settings callback — live slider updates ─────────────────────────────

static void OnSettingChanged(const char* name, int newValue) {
    if (std::strcmp(name, "AOR_TBarX") == 0) {
        g_posX = newValue;
        RepositionAllBars();
    } else if (std::strcmp(name, "AOR_TBarY") == 0) {
        g_posY = newValue;
        RepositionAllBars();
    } else if (std::strcmp(name, "AOR_TBarW") == 0) {
        g_barW = newValue;
        ScaleAllBars();
    } else if (std::strcmp(name, "AOR_TBarH") == 0) {
        g_barH = newValue;
        ScaleAllBars();
    }
}

// ── Init ────────────────────────────────────────────────────────────────

bool InitTimerBarDrag() {
    HMODULE gui = GetModuleHandleA("GUI.dll");
    if (!gui) {
        Log("[timerbar] GUI.dll not loaded");
        return false;
    }
    g_guiBase = reinterpret_cast<uintptr_t>(gui);

    // Resolve GUI.dll function pointers.
    // N3Msg_GetName and N3InterfaceModule_t::GetInstance are cached
    // function pointers at known GUI.dll data section RVAs — resolved
    // lazily by ReadCachedPtr in TryGetNanoName.
    g_rwReposition = reinterpret_cast<FnRWReposition>(
        g_guiBase + kRWRepositionRVA);
    g_rwSetScale = reinterpret_cast<FnRWSetScale>(
        g_guiBase + 0x20b0c);  // RenderWindow_t::SetScale
    g_rwResize = reinterpret_cast<FnRWResize>(
        g_guiBase + 0x20938);  // RenderWindow_t::Resize
    g_tsmGetInstance = reinterpret_cast<FnTSMGetInstance>(
        g_guiBase + 0x51d05);  // TimerSystemModule_t::GetInstance
    g_wcGetInstance = reinterpret_cast<FnWCGetInstance>(
        g_guiBase + 0xb454);  // WindowController_c::GetInstance

    // Load saved position from DValues.
    LoadPosition();

    // Register for live slider updates from the options panel.
    RegisterSettingCallback(&OnSettingChanged);

    // Hook CreateTimer.
    void* target = reinterpret_cast<void*>(g_guiBase + kCreateTimerRVA);
    auto* bytes = static_cast<uint8_t*>(target);
    Log("[timerbar] CreateTimer at %p, prologue: %02X %02X %02X %02X %02X",
        target, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* trampoline = nullptr;
    if (!InstallHook(target, reinterpret_cast<void*>(&CreateTimerDetour),
                     &trampoline)) {
        Log("[timerbar] CreateTimer hook failed");
        return false;
    }
    g_origCreateTimer = reinterpret_cast<FnCreateTimer>(trampoline);
    Log("[timerbar] CreateTimer hook installed");

    // Hook CreateGameTimer (overload 2 — equip, nano, attack, reload).
    {
        void* target2 = reinterpret_cast<void*>(g_guiBase + kCreateGameTimerRVA);
        auto* bytes2 = static_cast<uint8_t*>(target2);
        Log("[timerbar] CreateGameTimer at %p, prologue: %02X %02X %02X %02X %02X",
            target2, bytes2[0], bytes2[1], bytes2[2], bytes2[3], bytes2[4]);

        void* tramp2 = nullptr;
        if (!InstallHook(target2, reinterpret_cast<void*>(&CreateGameTimerDetour),
                         &tramp2)) {
            Log("[timerbar] CreateGameTimer hook failed");
            // Non-fatal: some bar types won't be repositioned at creation.
        } else {
            g_origCreateGameTimer = reinterpret_cast<FnCreateGameTimer>(tramp2);
            Log("[timerbar] CreateGameTimer hook installed");
        }
    }

    // Hook CastNanoSpell from Interfaces.dll for nano name capture.
    {
        HMODULE iface = GetModuleHandleA("Interfaces.dll");
        if (iface) {
            void* castAddr = reinterpret_cast<void*>(GetProcAddress(iface,
                "?N3Msg_CastNanoSpell@N3InterfaceModule_t@@QBEXABVIdentity_t@@0@Z"));
            Log("[timerbar] CastNanoSpell at %p", castAddr);
            if (castAddr) {
                auto* bytes3 = static_cast<uint8_t*>(castAddr);
                Log("[timerbar] CastNanoSpell prologue: %02X %02X %02X %02X %02X",
                    bytes3[0], bytes3[1], bytes3[2], bytes3[3], bytes3[4]);
                void* tramp3 = nullptr;
                if (InstallHook(castAddr, reinterpret_cast<void*>(&CastNanoSpellDetour),
                                &tramp3)) {
                    g_origCastNanoSpell = reinterpret_cast<FnCastNanoSpell>(tramp3);
                    Log("[timerbar] CastNanoSpell hook installed");
                } else {
                    Log("[timerbar] CastNanoSpell hook failed");
                }
            }
        } else {
            Log("[timerbar] Interfaces.dll not loaded");
        }
    }

    // Register mouse filter with the input handler.
    RegisterMouseFilter(&TimerBarMouseFilter);
    Log("[timerbar] mouse filter registered");

    return true;
}

}  // namespace aor
