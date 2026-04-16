// input_handler.cpp — Replacement ActionViewMouseHandler_c state machine.
//
// Hooks 4 GUI.dll callbacks + N3Msg_MovementChanged in Gamecode.dll.
// See input_handler.h for the public interface and design overview.
//
// Stock OnMouseRelease is NOT hooked (unhookable prologue). It delegates
// to EndDrag which we DO hook, so all release logic flows through us.

#include "hooks/input_handler.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace aor {

// ── Gamecode.dll API ────────────────────────────────────────────────────

namespace GamecodeAPI {

using FnN3MsgMovementChanged =
    void(__thiscall*)(void* engineAnarchy, int action,
                      float f1, float f2, bool b);

static void* N3Msg_MovementChanged_Addr = nullptr;

// Trampoline to the ORIGINAL N3Msg_MovementChanged. Bypasses our filter.
static FnN3MsgMovementChanged  N3Msg_MovementChanged   = nullptr;

static bool Init() {
    HMODULE gc = GetModuleHandleA("Gamecode.dll");
    if (!gc) {
        Log("[input] Gamecode.dll not loaded");
        return false;
    }
    Log("[input] Gamecode.dll at %p", gc);

    void* p = reinterpret_cast<void*>(GetProcAddress(gc,
        "?N3Msg_MovementChanged@n3EngineClientAnarchy_t@@"
        "QAEXW4MovementAction_e@Movement_n@@MM_N@Z"));
    if (!p) {
        Log("[input] FAILED to resolve N3Msg_MovementChanged");
        return false;
    }
    N3Msg_MovementChanged_Addr = p;
    N3Msg_MovementChanged = reinterpret_cast<FnN3MsgMovementChanged>(p);
    Log("[input] resolved N3Msg_MovementChanged -> %p", p);
    return true;
}

}  // namespace GamecodeAPI

// ── GUI.dll API ─────────────────────────────────────────────────────────

namespace GUIAPI {

using FnGetInstance     = void*(__cdecl*)();
using FnN3MsgLook      = void(__thiscall*)(void* n3im, float dx, float dy);
using FnN3MsgEnd       = void(__thiscall*)(void* n3im);
using FnAFCMSend       = void(__thiscall*)(void* afcm, int cmd, int param);
using FnWCGetInstance   = void*(__cdecl*)();
using FnWCGetDragObj    = void*(__thiscall*)(void* wc);
using FnWCSetMousePos   = void(__thiscall*)(void* wc, const void* point, bool clamp);

static uintptr_t g_guiBase = 0;

static FnWCGetInstance  WC_GetInstance     = nullptr;
static FnWCGetDragObj   WC_GetDragObject   = nullptr;
static FnWCSetMousePos  WC_SetMousePosition = nullptr;

template<typename T>
static T ReadCachedPtr(uint32_t rva) {
    return *reinterpret_cast<T*>(g_guiBase + rva);
}

static void* GetN3IM() {
    auto fn = ReadCachedPtr<FnGetInstance>(0x1a772c);
    return fn ? fn() : nullptr;
}

static void CameraMouseLookMovement(float dx, float dy) {
    void* n3im = GetN3IM();
    auto fn = ReadCachedPtr<FnN3MsgLook>(0x1a7604);
    if (n3im && fn) fn(n3im, dx, dy);
}

static void MouseMovement(float dx, float dy) {
    void* n3im = GetN3IM();
    auto fn = ReadCachedPtr<FnN3MsgLook>(0x1a7600);
    if (n3im && fn) fn(n3im, dx, dy);
}

static void EndCameraMouseLook() {
    void* n3im = GetN3IM();
    auto fn = ReadCachedPtr<FnN3MsgEnd>(0x1a760c);
    if (n3im && fn) fn(n3im);
}

static void EndMouseLook() {
    void* n3im = GetN3IM();
    auto fn = ReadCachedPtr<FnN3MsgEnd>(0x1a7608);
    if (n3im && fn) fn(n3im);
}

static void AFCMSend(int cmd, int param) {
    auto getInst = ReadCachedPtr<FnGetInstance>(0x1a70e0);
    auto send    = ReadCachedPtr<FnAFCMSend>(0x1a70e4);
    if (getInst && send) {
        void* afcm = getInst();
        if (afcm) send(afcm, cmd, param);
    }
}

static bool Init() {
    HMODULE gui = GetModuleHandleA("GUI.dll");
    if (!gui) {
        Log("[input] GUI.dll not loaded");
        return false;
    }
    g_guiBase = reinterpret_cast<uintptr_t>(gui);
    Log("[input] GUI.dll at %p", gui);

    WC_GetInstance      = reinterpret_cast<FnWCGetInstance>(g_guiBase + 0xb454);
    WC_GetDragObject    = reinterpret_cast<FnWCGetDragObj>(g_guiBase + 0x156e29);
    WC_SetMousePosition = reinterpret_cast<FnWCSetMousePos>(g_guiBase + 0x156abf);
    return true;
}

}  // namespace GUIAPI

// ── Constants ───────────────────────────────────────────────────────────

constexpr int kActionStartForward = 1;
constexpr int kActionStopForward  = 2;

// ── State ───────────────────────────────────────────────────────────────

static InputState g_input;

// ── Helpers ─────────────────────────────────────────────────────────────

bool IsCameraEnabled() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_CamOn", v) &&
        v.type == static_cast<uint32_t>(VariantType::Bool)) {
        return v.as_bool;
    }
    return true;
}

bool IsMouseRunEnabled() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_MouseRun", v) &&
        v.type == static_cast<uint32_t>(VariantType::Bool)) {
        return v.as_bool;
    }
    return true;
}

static bool IsForwardKeyHeld() {
    return (GetAsyncKeyState('W') & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_UP) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
}

static AOString MakeString(const char* str) {
    AOString s;
    std::memset(&s, 0, sizeof(s));
    uint32_t len = static_cast<uint32_t>(std::strlen(str));
    if (len < 16) {
        std::memcpy(s.sso_buf, str, len + 1);
        s.length = len;
        s.capacity = 15;
    } else {
        s.heap_ptr = const_cast<char*>(str);
        s.length = len;
        s.capacity = len;
    }
    return s;
}

static float GetMouseSensitivity() {
    if (!GameAPI::GetDValue) return 1.0f;
    static const char kName[] = "MouseTurnSensitivity";
    AOString name = MakeString(kName);
    AOVariant result{};
    GameAPI::GetDValue(&result, name, false);
    if (result.type == static_cast<uint32_t>(VariantType::Float))
        return result.as_float;
    if (result.type == static_cast<uint32_t>(VariantType::Int))
        return static_cast<float>(result.as_int);
    return 1.0f;
}

static void BeginDragVisuals() {
    if (g_input.cursor_hidden) return;
    void* wc = GUIAPI::WC_GetInstance();
    if (wc) {
        auto* cursor = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(wc) + 0x48);
        g_input.saved_cursor_x = cursor[0];
        g_input.saved_cursor_y = cursor[1];
    }
    g_input.accum_dist = 0.0f;
    g_input.cursor_hidden = true;
    GUIAPI::AFCMSend(0x12, 0x3d);
}

static void EndDragVisuals() {
    if (!g_input.cursor_hidden) return;
    g_input.cursor_hidden = false;
    GUIAPI::AFCMSend(0x12, 6);
}

// ── Public accessors ────────────────────────────────────────────────────

const InputState& GetInputState() {
    return g_input;
}

// ── Forward movement evaluation ─────────────────────────────────────────

static bool g_wasMovingForward = false;

void UpdateForwardMovement(void* engine) {
    bool mouseForward = (g_input.state == MouseState::BOTH_HELD) &&
                         IsMouseRunEnabled();
    bool shouldForward = mouseForward || IsForwardKeyHeld();

    if (shouldForward && !g_wasMovingForward) {
        if (GamecodeAPI::N3Msg_MovementChanged)
            GamecodeAPI::N3Msg_MovementChanged(engine,
                kActionStartForward, 0.0f, 0.0f, true);
        g_wasMovingForward = true;
    } else if (!shouldForward && g_wasMovingForward) {
        if (GamecodeAPI::N3Msg_MovementChanged)
            GamecodeAPI::N3Msg_MovementChanged(engine,
                kActionStopForward, 0.0f, 0.0f, true);
        g_wasMovingForward = false;
    }
}

// ── N3Msg_MovementChanged filter ────────────────────────────────────────

static void __fastcall MovementChangedDetour(
        void* engine, void* /*edx*/, int action, float f1, float f2, bool sync) {
    if (g_input.state == MouseState::BOTH_HELD && IsMouseRunEnabled() &&
        (action == kActionStartForward || action == kActionStopForward)) {
        return;
    }
    GamecodeAPI::N3Msg_MovementChanged(engine, action, f1, f2, sync);
}

// ── ActionViewMouseHandler_c detours ────────────────────────────────────

constexpr uint32_t kOnMouseDownRVA = 0x0002c2ee;
using FnOnMouseDown = void(__thiscall*)(void* this_ecx, const float* pos, int button, int clickFlags);
static FnOnMouseDown g_origOnMouseDown = nullptr;

constexpr uint32_t kOnMouseMoveRVA = 0x0002c17b;
using FnOnMouseMove = void(__thiscall*)(void* this_ecx, const float* delta);
static FnOnMouseMove g_origOnMouseMove = nullptr;

constexpr uint32_t kOnMouseUpRVA = 0x0002c469;
using FnOnMouseUp = void(__thiscall*)(void* this_ecx, const float* pos, int button);
static FnOnMouseUp g_origOnMouseUp = nullptr;

constexpr uint32_t kEndDragRVA = 0x0002c0e5;
using FnEndDrag = void(__thiscall*)(void* this_ecx);
static FnEndDrag g_origEndDrag = nullptr;

// ── OnMouseDown ─────────────────────────────────────────────────────────

static void __fastcall OnMouseDownDetour(void* this_ecx, void* /*edx*/,
                                          const float* pos, int button,
                                          int clickFlags) {
    g_origOnMouseDown(this_ecx, pos, button, clickFlags);

    if (!IsCameraEnabled()) return;

    void* wc = GUIAPI::WC_GetInstance();
    if (wc && GUIAPI::WC_GetDragObject(wc)) return;

    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<int*>(handler + 0x0C) = 0;
    *reinterpret_cast<int*>(handler + 0x10) = static_cast<int>(0xFFFFFFFF);

    bool mouseRunOn = IsMouseRunEnabled();

    if (button == 1) {
        switch (g_input.state) {
        case MouseState::PENDING_RMB:
        case MouseState::DRAGGING_RMB:
            if (mouseRunOn) {
                if (g_input.state == MouseState::PENDING_RMB)
                    BeginDragVisuals();
                g_input.state = MouseState::BOTH_HELD;
            }
            break;
        case MouseState::DRAGGING_LMB:
        case MouseState::BOTH_HELD:
            break;
        default:
            g_input.state = MouseState::PENDING_LMB;
            break;
        }
    } else if (button == 2) {
        switch (g_input.state) {
        case MouseState::PENDING_LMB:
        case MouseState::DRAGGING_LMB:
            if (mouseRunOn) {
                if (g_input.state == MouseState::PENDING_LMB)
                    BeginDragVisuals();
                if (g_input.state == MouseState::DRAGGING_LMB)
                    GUIAPI::EndCameraMouseLook();
                g_input.state = MouseState::BOTH_HELD;
            }
            break;
        case MouseState::DRAGGING_RMB:
        case MouseState::BOTH_HELD:
            break;
        default:
            g_input.state = MouseState::PENDING_RMB;
            break;
        }
    }
}

// ── OnMouseMove ─────────────────────────────────────────────────────────

static void __fastcall OnMouseMoveDetour(void* this_ecx, void* /*edx*/,
                                          const float* delta) {
    if (!IsCameraEnabled()) {
        g_origOnMouseMove(this_ecx, delta);
        return;
    }

    if (g_input.state == MouseState::PENDING_LMB) {
        BeginDragVisuals();
        g_input.state = MouseState::DRAGGING_LMB;
    } else if (g_input.state == MouseState::PENDING_RMB) {
        BeginDragVisuals();
        g_input.state = MouseState::DRAGGING_RMB;
    }

    if (g_input.state == MouseState::IDLE) return;

    float mag = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1]);
    g_input.accum_dist += std::fabs(mag);

    float sens = GetMouseSensitivity();
    float dx = delta[0] * sens;
    float dy = delta[1] * sens;

    switch (g_input.state) {
    case MouseState::DRAGGING_LMB:
        GUIAPI::CameraMouseLookMovement(dx, dy);
        break;
    case MouseState::DRAGGING_RMB:
    case MouseState::BOTH_HELD:
        GUIAPI::MouseMovement(dx, dy);
        break;
    default:
        break;
    }

    float saved[2] = { g_input.saved_cursor_x, g_input.saved_cursor_y };
    void* wc = GUIAPI::WC_GetInstance();
    if (wc) GUIAPI::WC_SetMousePosition(wc, saved, true);
}

// ── OnMouseUp ───────────────────────────────────────────────────────────

static void __fastcall OnMouseUpDetour(void* this_ecx, void* /*edx*/,
                                        const float* pos, int button) {
    if (!IsCameraEnabled()) {
        g_origOnMouseUp(this_ecx, pos, button);
        return;
    }

    float threshold = 0.0f;
    if (GUIAPI::g_guiBase) {
        threshold = *reinterpret_cast<float*>(GUIAPI::g_guiBase + 0x1aeaf4);
    }

    bool wasDragging = (g_input.state == MouseState::DRAGGING_LMB ||
                        g_input.state == MouseState::DRAGGING_RMB ||
                        g_input.state == MouseState::BOTH_HELD);
    if (wasDragging && g_input.accum_dist >= threshold) return;

    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<float*>(handler + 0x1C) = 0.0f;

    g_origOnMouseUp(this_ecx, pos, button);
}

// ── EndDrag ─────────────────────────────────────────────────────────────

static void __fastcall EndDragDetour(void* this_ecx, void* /*edx*/) {
    if (!IsCameraEnabled()) {
        g_origEndDrag(this_ecx);
        return;
    }

    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    switch (g_input.state) {
    case MouseState::BOTH_HELD:
        if (rmb && !lmb) {
            g_input.state = MouseState::DRAGGING_RMB;
        } else if (lmb && !rmb) {
            GUIAPI::EndMouseLook();
            g_input.state = MouseState::DRAGGING_LMB;
        } else {
            GUIAPI::EndMouseLook();
            EndDragVisuals();
            g_input.state = MouseState::IDLE;
        }
        break;

    case MouseState::DRAGGING_LMB:
        if (!lmb) {
            GUIAPI::EndCameraMouseLook();
            EndDragVisuals();
            g_input.state = MouseState::IDLE;
        }
        break;

    case MouseState::DRAGGING_RMB:
        if (!rmb) {
            GUIAPI::EndMouseLook();
            EndDragVisuals();
            g_input.state = MouseState::IDLE;
        }
        break;

    case MouseState::PENDING_LMB:
    case MouseState::PENDING_RMB:
        g_input.state = MouseState::IDLE;
        break;

    case MouseState::IDLE:
        break;
    }

    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<int*>(handler + 0x0C) = 0;
    *reinterpret_cast<int*>(handler + 0x10) = static_cast<int>(0xFFFFFFFF);
}

// ── Init ────────────────────────────────────────────────────────────────

bool InitInputHandler() {
    if (!GamecodeAPI::Init()) {
        Log("[input] Gamecode API init failed — mouse-run disabled");
    }

    if (!GUIAPI::Init()) {
        Log("[input] GUI API init failed — input handler disabled");
        return false;
    }

    // Hook N3Msg_MovementChanged (non-critical).
    if (GamecodeAPI::N3Msg_MovementChanged_Addr) {
        auto* bytes = static_cast<uint8_t*>(GamecodeAPI::N3Msg_MovementChanged_Addr);
        Log("[input] MovementChanged at %p, prologue: %02X %02X %02X %02X %02X",
            GamecodeAPI::N3Msg_MovementChanged_Addr,
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

        void* tramp = nullptr;
        if (InstallHook(GamecodeAPI::N3Msg_MovementChanged_Addr,
                        reinterpret_cast<void*>(&MovementChangedDetour),
                        &tramp)) {
            GamecodeAPI::N3Msg_MovementChanged =
                reinterpret_cast<GamecodeAPI::FnN3MsgMovementChanged>(tramp);
            Log("[input] MovementChanged hook installed");
        } else {
            Log("[input] MovementChanged hook failed — forward filter disabled");
        }
    }

    // Hook OnMouseDown.
    {
        void* addr = ResolveRVA("GUI.dll", kOnMouseDownRVA);
        if (!addr) { Log("[input] OnMouseDown RVA invalid"); return false; }
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseDownDetour), &tramp)) {
            Log("[input] failed to hook OnMouseDown"); return false;
        }
        g_origOnMouseDown = reinterpret_cast<FnOnMouseDown>(tramp);
        Log("[input] OnMouseDown hook installed");
    }

    // Hook OnMouseMove.
    {
        void* addr = ResolveRVA("GUI.dll", kOnMouseMoveRVA);
        if (!addr) { Log("[input] OnMouseMove RVA invalid"); return false; }
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseMoveDetour), &tramp)) {
            Log("[input] failed to hook OnMouseMove"); return false;
        }
        g_origOnMouseMove = reinterpret_cast<FnOnMouseMove>(tramp);
        Log("[input] OnMouseMove hook installed");
    }

    // Hook OnMouseUp.
    {
        void* addr = ResolveRVA("GUI.dll", kOnMouseUpRVA);
        if (!addr) { Log("[input] OnMouseUp RVA invalid"); return false; }
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseUpDetour), &tramp)) {
            Log("[input] failed to hook OnMouseUp"); return false;
        }
        g_origOnMouseUp = reinterpret_cast<FnOnMouseUp>(tramp);
        Log("[input] OnMouseUp hook installed");
    }

    // Hook EndDrag.
    {
        void* addr = ResolveRVA("GUI.dll", kEndDragRVA);
        if (!addr) { Log("[input] EndDrag RVA invalid"); return false; }
        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&EndDragDetour), &tramp)) {
            Log("[input] failed to hook EndDrag"); return false;
        }
        g_origEndDrag = reinterpret_cast<FnEndDrag>(tramp);
        Log("[input] EndDrag hook installed");
    }

    Log("[input] All input hooks installed");
    return true;
}

}  // namespace aor
