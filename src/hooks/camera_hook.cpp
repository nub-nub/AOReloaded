// camera_hook.cpp — Replacement ActionViewMouseHandler_c state machine
//                  + per-frame camera behaviors (yaw follow, RMB-align).
//
// Hooks 5 functions:
//   1. CameraVehicleFixedThird_t::CalcSteering (N3.dll)  — per-frame camera
//   2. ActionViewMouseHandler_c::OnMouseDown   (GUI.dll)  — button press
//   3. ActionViewMouseHandler_c::OnMouseMove   (GUI.dll)  — drag dispatch
//   4. ActionViewMouseHandler_c::OnMouseUp     (GUI.dll)  — click passthrough
//   5. ActionViewMouseHandler_c::EndDrag        (GUI.dll)  — cleanup/transitions
//
// The stock handler's single drag_mode field is replaced with an InputState
// struct that tracks LMB and RMB independently, enabling combined-button
// states (BOTH_HELD → forward movement + char steering).
//
// Stock OnMouseRelease is NOT hooked (unhookable prologue). It delegates to
// EndDrag which we DO hook, so all release logic flows through our detour.

#include "hooks/camera_hook.h"
#include "hooks/hook_engine.h"
#include "ao/game_api.h"
#include "core/logging.h"

#include <windows.h>
#include <cstdint>
#include <cmath>

namespace aor {

// ── N3.dll API ──────────────────────────────────────────────────────────

namespace N3API {

using FnGetEngineInstance    = void*(__cdecl*)();
using FnGetActiveCamera      = void*(__thiscall*)(void* engine);
using FnIsFirstPerson        = bool(__thiscall*)(void* camera);
using FnGetClientControlDynel = void*(__thiscall*)(void* engine);
using FnGetGlobalPos = float*(__thiscall*)(void* dynel);
using FnSetRelRot = void(__thiscall*)(void* dynel, const void* quat);

static FnGetEngineInstance     GetEngineInstance     = nullptr;
static FnGetActiveCamera       GetActiveCamera       = nullptr;
static FnIsFirstPerson         IsFirstPerson         = nullptr;
static FnGetClientControlDynel GetClientControlDynel = nullptr;
static FnGetGlobalPos          GetGlobalPos          = nullptr;
static FnSetRelRot             SetRelRot             = nullptr;

static bool Init() {
    HMODULE n3 = GetModuleHandleA("N3.dll");
    if (!n3) {
        Log("[camera] N3.dll not loaded");
        return false;
    }
    Log("[camera] N3.dll at %p", n3);

    auto resolve = [&](const char* mangled, void** out, const char* name) -> bool {
        *out = reinterpret_cast<void*>(GetProcAddress(n3, mangled));
        if (!*out) {
            Log("[camera] FAILED to resolve %s", name);
            return false;
        }
        Log("[camera] resolved %s -> %p", name, *out);
        return true;
    };

    bool ok = true;
    ok &= resolve("?GetInstance@n3EngineClient_t@@SAPAV1@XZ",
        reinterpret_cast<void**>(&GetEngineInstance), "GetInstance");
    ok &= resolve("?GetActiveCamera@n3EngineClient_t@@QBEPAVn3Camera_t@@XZ",
        reinterpret_cast<void**>(&GetActiveCamera), "GetActiveCamera");
    ok &= resolve("?IsFirstPerson@n3Camera_t@@QBE_NXZ",
        reinterpret_cast<void**>(&IsFirstPerson), "IsFirstPerson");
    ok &= resolve("?GetClientControlDynel@n3EngineClient_t@@QBEPAVn3VisualDynel_t@@XZ",
        reinterpret_cast<void**>(&GetClientControlDynel), "GetClientControlDynel");
    ok &= resolve("?GetGlobalPos@n3Dynel_t@@QBEABVVector3_t@@XZ",
        reinterpret_cast<void**>(&GetGlobalPos), "GetGlobalPos");
    ok &= resolve("?SetRelRot@n3Dynel_t@@QAEXABVQuaternion_t@@@Z",
        reinterpret_cast<void**>(&SetRelRot), "SetRelRot");
    return ok;
}

}  // namespace N3API

// ── Gamecode.dll API ────────────────────────────────────────────────────

namespace GamecodeAPI {

using FnN3MsgMovementChanged =
    void(__thiscall*)(void* engineAnarchy, int action,
                      float f1, float f2, bool b);

static FnN3MsgMovementChanged  N3Msg_MovementChanged   = nullptr;

static bool Init() {
    HMODULE gc = GetModuleHandleA("Gamecode.dll");
    if (!gc) {
        Log("[camera] Gamecode.dll not loaded");
        return false;
    }
    Log("[camera] Gamecode.dll at %p", gc);

    void* p = reinterpret_cast<void*>(GetProcAddress(gc,
        "?N3Msg_MovementChanged@n3EngineClientAnarchy_t@@"
        "QAEXW4MovementAction_e@Movement_n@@MM_N@Z"));
    if (!p) {
        Log("[camera] FAILED to resolve N3Msg_MovementChanged");
        return false;
    }
    N3Msg_MovementChanged = reinterpret_cast<FnN3MsgMovementChanged>(p);
    Log("[camera] resolved N3Msg_MovementChanged -> %p", p);
    return true;
}

}  // namespace GamecodeAPI

// ── GUI.dll API ─────────────────────────────────────────────────────────
//
// Functions resolved from GUI.dll by two methods:
//   1. Direct RVA — for functions defined in GUI.dll itself
//   2. Cached pointer read — for functions imported lazily by GUI.dll
//      (stored in data section, populated before mouse events fire)

namespace GUIAPI {

// Function types
using FnGetInstance     = void*(__cdecl*)();
using FnN3MsgLook      = void(__thiscall*)(void* n3im, float dx, float dy);
using FnN3MsgEnd       = void(__thiscall*)(void* n3im);
using FnAFCMSend       = void(__thiscall*)(void* afcm, int cmd, int param);
using FnWCGetInstance   = void*(__cdecl*)();
using FnWCGetDragObj    = void*(__thiscall*)(void* wc);
using FnWCSetMousePos   = void(__thiscall*)(void* wc, const void* point, bool clamp);

// Module base
static uintptr_t g_guiBase = 0;

// Direct GUI.dll functions (by RVA)
static FnWCGetInstance  WC_GetInstance     = nullptr;   // 0xb454
static FnWCGetDragObj   WC_GetDragObject   = nullptr;   // 0x156e29
static FnWCSetMousePos  WC_SetMousePosition = nullptr;  // 0x156abf

// Read a cached function pointer from GUI.dll's data section.
template<typename T>
static T ReadCachedPtr(uint32_t rva) {
    return *reinterpret_cast<T*>(g_guiBase + rva);
}

// ── N3InterfaceModule_t dispatch ────────────────────────────────────
// These read the lazy-resolved pointers every call (matches stock code).

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

// ── AFCM dispatch ───────────────────────────────────────────────────

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
        Log("[camera] GUI.dll not loaded");
        return false;
    }
    g_guiBase = reinterpret_cast<uintptr_t>(gui);
    Log("[camera] GUI.dll at %p", gui);

    WC_GetInstance      = reinterpret_cast<FnWCGetInstance>(g_guiBase + 0xb454);
    WC_GetDragObject    = reinterpret_cast<FnWCGetDragObj>(g_guiBase + 0x156e29);
    WC_SetMousePosition = reinterpret_cast<FnWCSetMousePos>(g_guiBase + 0x156abf);

    Log("[camera] WC::GetInstance -> %p", WC_GetInstance);
    Log("[camera] WC::GetDragObject -> %p", WC_GetDragObject);
    Log("[camera] WC::SetMousePosition -> %p", WC_SetMousePosition);
    return true;
}

}  // namespace GUIAPI

// ── Constants ───────────────────────────────────────────────────────────

constexpr int kActionStartForward = 1;
constexpr int kActionStopForward  = 2;
constexpr float kDefaultFollowSpeed = 5.0f;
constexpr float kMovementThreshold = 0.01f;

// ── Math helpers ────────────────────────────────────────────────────────

static float Vec3LenXZ(float x, float z) {
    return std::sqrt(x * x + z * z);
}

// ── Input State Machine ─────────────────────────────────────────────────

enum class MouseState : uint8_t {
    IDLE,
    PENDING_LMB,
    PENDING_RMB,
    DRAGGING_LMB,
    DRAGGING_RMB,
    BOTH_HELD,
};

struct InputState {
    MouseState state        = MouseState::IDLE;
    bool       cursor_hidden = false;
    float      saved_cursor_x = 0.0f;
    float      saved_cursor_y = 0.0f;
    float      accum_dist   = 0.0f;
};

static InputState g_input;

// Check if a forward-movement key is physically held. Covers common
// bindings (W, Up arrow, Numpad 8). Not perfect for exotic rebinds,
// but handles the vast majority of configurations.
static bool IsForwardKeyHeld() {
    return (GetAsyncKeyState('W') & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_UP) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_NUMPAD8) & 0x8000) != 0;
}

// Check if our camera system is enabled.
static bool IsEnabled() {
    AOVariant v{};
    if (GameAPI::GetVariant("AOR_CamOn", v) &&
        v.type == static_cast<uint32_t>(VariantType::Bool)) {
        return v.as_bool;
    }
    return true;  // default enabled
}

// Construct an AOString for any length. For len < 16, uses SSO inline
// buffer. For len >= 16, points heap_ptr to the provided (must-outlive)
// C string. No destructor runs on our AOString, so no ownership issues.
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

// Read MouseTurnSensitivity DValue. The name is 20 chars — exceeds the
// 15-char SSO limit, so we construct a heap-mode AOString pointing to
// a static const. GetDValue takes const& so this is read-safe.
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

// Begin a drag: save cursor, hide it, zero distance accumulator.
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
    GUIAPI::AFCMSend(0x12, 0x3d);  // hide cursor
}

// End drag visuals: show cursor.
static void EndDragVisuals() {
    if (!g_input.cursor_hidden) return;
    g_input.cursor_hidden = false;
    GUIAPI::AFCMSend(0x12, 6);  // show cursor
}

// ── CalcSteering hook ───────────────────────────────────────────────────
//
// CameraVehicleFixedThird_t::CalcSteering (N3.dll RVA 0x1f752)
// Per-frame: yaw follow during movement, RMB-align, both-buttons alignment.

constexpr uint32_t kCalcSteeringRVA = 0x0001f752;

using FnCalcSteering = int(__thiscall*)(void* vehicle, void* result);
static FnCalcSteering g_origCalcSteering = nullptr;

static float g_lastPlayerX = 0.0f;
static float g_lastPlayerZ = 0.0f;
static bool  g_hasLastPos  = false;

// Edge detection for RMB-align (one-shot per press).
static bool  g_rmbWasHeld  = false;

// Unified forward-movement state. Evaluated every frame from the
// combination of mouse buttons and keyboard keys. Only CalcSteering
// dispatches StartForward/StopForward — OnMouseDown and EndDrag do not.
static bool  g_wasMovingForward = false;

static int __fastcall CalcSteeringDetour(void* vehicle, void* /*edx*/, void* result) {
    void* engine = N3API::GetEngineInstance ? N3API::GetEngineInstance() : nullptr;
    if (!engine) goto call_original;

    // Check master toggle.
    if (!IsEnabled()) {
        g_rmbWasHeld = false;
        goto call_original;
    }

    // ── RMB-align & both-buttons continuous alignment ───────────────
    {
        // Read button state from our InputState instead of GetAsyncKeyState.
        bool rmbHeld = (g_input.state == MouseState::DRAGGING_RMB ||
                        g_input.state == MouseState::BOTH_HELD);
        // lmbHeld not currently used but reserved for future LMB-specific
        // per-frame behaviors (e.g., orbit dampening).
        (void)(g_input.state == MouseState::DRAGGING_LMB);
        bool rmbEdge = rmbHeld && !g_rmbWasHeld;
        g_rmbWasHeld = rmbHeld;

        // Continuous char-align during both-held or one-shot on RMB edge.
        bool doAlign = (rmbEdge || g_input.state == MouseState::BOTH_HELD)
                        && N3API::SetRelRot;
        if (doAlign) {
            auto base = reinterpret_cast<uintptr_t>(vehicle);
            float hx = *reinterpret_cast<float*>(base + 0x1F8);
            float hz = *reinterpret_cast<float*>(base + 0x200);
            float lenXZ = Vec3LenXZ(hx, hz);

            if (lenXZ >= 0.001f) {
                constexpr float kPi = 3.14159265358979323846f;
                float beta  = std::atan2(hz, hx);
                float delta = -kPi * 0.5f - beta;
                while (delta >  kPi) delta -= 2.0f * kPi;
                while (delta < -kPi) delta += 2.0f * kPi;

                if (std::fabs(delta) > 0.001f) {
                    float qy = *reinterpret_cast<float*>(base + 0x170);
                    float qw = *reinterpret_cast<float*>(base + 0x178);
                    float currentAngle = 2.0f * std::atan2(qy, qw);
                    float newAngle = currentAngle + delta;

                    struct Q { float x, y, z, w; };
                    Q newQ{ 0.0f,
                            std::sin(newAngle * 0.5f),
                            0.0f,
                            std::cos(newAngle * 0.5f) };

                    // Reset local heading BEFORE SetRelRot (order matters —
                    // see ao/CLAUDE.md for the synchronous cascade issue).
                    *reinterpret_cast<float*>(base + 0x1F8) = 0.0f;
                    *reinterpret_cast<float*>(base + 0x200) = -lenXZ;

                    if (void* dynel = N3API::GetClientControlDynel(engine)) {
                        N3API::SetRelRot(dynel, &newQ);
                    }
                }
            }
        }
    }

    // ── Unified forward-movement evaluation ────────────────────────
    //
    // Forward movement is a derived signal: move forward if EITHER both
    // mouse buttons are held OR a forward key is physically pressed.
    // This is the sole dispatch point for StartForward/StopForward —
    // OnMouseDown and EndDrag only manage mouse state, not movement.
    {
        bool shouldForward = (g_input.state == MouseState::BOTH_HELD) ||
                             IsForwardKeyHeld();

        if (shouldForward && !g_wasMovingForward) {
            // Rising edge: begin forward movement.
            if (GamecodeAPI::N3Msg_MovementChanged)
                GamecodeAPI::N3Msg_MovementChanged(engine,
                    kActionStartForward, 0.0f, 0.0f, true);
            g_wasMovingForward = true;
        } else if (!shouldForward && g_wasMovingForward) {
            // Falling edge: stop forward movement.
            if (GamecodeAPI::N3Msg_MovementChanged)
                GamecodeAPI::N3Msg_MovementChanged(engine,
                    kActionStopForward, 0.0f, 0.0f, true);
            g_wasMovingForward = false;
        } else if (shouldForward && g_wasMovingForward
                   && g_input.state == MouseState::BOTH_HELD) {
            // Self-healing: while BOTH_HELD is active, re-assert forward
            // with sync=false (local only, no network packet). This
            // overrides any external StopForward the keyboard may have
            // sent (e.g., user released W while both buttons are held).
            if (GamecodeAPI::N3Msg_MovementChanged)
                GamecodeAPI::N3Msg_MovementChanged(engine,
                    kActionStartForward, 0.0f, 0.0f, false);
        }
    }

    // ── Yaw follow during movement ──────────────────────────────────
    {
        void* dynel = N3API::GetClientControlDynel(engine);
        if (!dynel) goto call_original;

        float* pos = N3API::GetGlobalPos(dynel);
        if (!pos) goto call_original;

        float px = pos[0];
        float pz = pos[2];

        if (!g_hasLastPos) {
            g_lastPlayerX = px;
            g_lastPlayerZ = pz;
            g_hasLastPos = true;
            goto call_original;
        }

        float dx = px - g_lastPlayerX;
        float dz = pz - g_lastPlayerZ;
        float moveDist = Vec3LenXZ(dx, dz);

        g_lastPlayerX = px;
        g_lastPlayerZ = pz;

        if (moveDist < kMovementThreshold) goto call_original;

        // Don't follow while LMB-only is held (player is orbiting camera).
        if (g_input.state == MouseState::DRAGGING_LMB) goto call_original;

        auto base = reinterpret_cast<uintptr_t>(vehicle);

        float behindX = 0.0f;
        float behindZ = -1.0f;

        float* headingX = reinterpret_cast<float*>(base + 0x1F8);
        // headingY preserved at +0x1FC — pitch is never modified by yaw follow.
        float* headingZ = reinterpret_cast<float*>(base + 0x200);

        float dt = *reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(engine) + 0x68);

        float followSpeed = kDefaultFollowSpeed;
        AOVariant speedVal{};
        if (GameAPI::GetVariant("AOR_CYawSpd", speedVal)) {
            if (speedVal.type == static_cast<uint32_t>(VariantType::Float))
                followSpeed = speedVal.as_float;
            else if (speedVal.type == static_cast<uint32_t>(VariantType::Int))
                followSpeed = static_cast<float>(speedVal.as_int);
        }

        float t = dt * followSpeed;
        if (t > 1.0f) t = 1.0f;

        float oldLenXZ = Vec3LenXZ(*headingX, *headingZ);
        if (oldLenXZ < 0.001f) goto call_original;

        float currentAngle = std::atan2(*headingZ, *headingX);
        float targetAngle  = std::atan2(behindZ, behindX);
        float diff = targetAngle - currentAngle;
        constexpr float kPi = 3.14159265358979323846f;
        if (diff >  kPi) diff -= 2.0f * kPi;
        if (diff < -kPi) diff += 2.0f * kPi;

        float newAngle = currentAngle + diff * t;
        *headingX = std::cos(newAngle) * oldLenXZ;
        *headingZ = std::sin(newAngle) * oldLenXZ;
    }

call_original:
    return g_origCalcSteering(vehicle, result);
}

// ── ActionViewMouseHandler_c detours ────────────────────────────────────
//
// Hook targets in GUI.dll. Each callback receives the handler instance in
// ECX via slot dispatch. Detours use __fastcall(this, edx, ...stack_args).
// Trampolines typed as __thiscall to preserve RET N cleanup.

// OnMouseDown: void __thiscall(Point const& pos, int button, int clickFlags)
// RET 0xC (3 stack args)
constexpr uint32_t kOnMouseDownRVA = 0x0002c2ee;
using FnOnMouseDown = void(__thiscall*)(void* this_ecx, const float* pos, int button, int clickFlags);
static FnOnMouseDown g_origOnMouseDown = nullptr;

// OnMouseMove: void __thiscall(Point const& delta)
// RET 0x4 (1 stack arg)
constexpr uint32_t kOnMouseMoveRVA = 0x0002c17b;
using FnOnMouseMove = void(__thiscall*)(void* this_ecx, const float* delta);
static FnOnMouseMove g_origOnMouseMove = nullptr;

// OnMouseUp: void __thiscall(Point const& pos, int button)
// RET 0x8 (2 stack args)
constexpr uint32_t kOnMouseUpRVA = 0x0002c469;
using FnOnMouseUp = void(__thiscall*)(void* this_ecx, const float* pos, int button);
static FnOnMouseUp g_origOnMouseUp = nullptr;

// EndDrag: void __thiscall()
// RET 0 (no stack args)
constexpr uint32_t kEndDragRVA = 0x0002c0e5;
using FnEndDrag = void(__thiscall*)(void* this_ecx);
static FnEndDrag g_origEndDrag = nullptr;

// ── OnMouseDown detour ──────────────────────────────────────────────────

static void __fastcall OnMouseDownDetour(void* this_ecx, void* /*edx*/,
                                          const float* pos, int button,
                                          int clickFlags) {
    // Always call original first — it handles double-click actions and the
    // DragObject guard. This is safe: the original just sets handler fields
    // (+0x0C, +0x10) which we overwrite below.
    g_origOnMouseDown(this_ecx, pos, button, clickFlags);

    if (!IsEnabled()) return;  // stock behavior stands

    // If original returned because DragObject was active, we should too.
    void* wc = GUIAPI::WC_GetInstance();
    if (wc && GUIAPI::WC_GetDragObject(wc)) return;

    // Zero out stock handler fields so stock OnMouseMove (if it ever runs
    // via a code path we didn't anticipate) won't activate a stock drag.
    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<int*>(handler + 0x0C) = 0;
    *reinterpret_cast<int*>(handler + 0x10) = static_cast<int>(0xFFFFFFFF);

    if (button == 1) {  // LMB
        switch (g_input.state) {
        case MouseState::PENDING_RMB:
        case MouseState::DRAGGING_RMB:
            // RMB already active → enter BOTH_HELD.
            if (g_input.state == MouseState::PENDING_RMB)
                BeginDragVisuals();
            // Forward movement is handled by CalcSteering's unified
            // evaluation — not dispatched here.
            g_input.state = MouseState::BOTH_HELD;
            break;

        case MouseState::DRAGGING_LMB:
        case MouseState::BOTH_HELD:
            break;  // already in LMB or both mode, ignore

        default:  // IDLE or PENDING_LMB
            g_input.state = MouseState::PENDING_LMB;
            break;
        }
    } else if (button == 2) {  // RMB
        switch (g_input.state) {
        case MouseState::PENDING_LMB:
        case MouseState::DRAGGING_LMB:
            // LMB already active → enter BOTH_HELD.
            if (g_input.state == MouseState::PENDING_LMB)
                BeginDragVisuals();
            if (g_input.state == MouseState::DRAGGING_LMB) {
                // End camera orbit mode before switching to char steer.
                GUIAPI::EndCameraMouseLook();
            }
            // Forward movement is handled by CalcSteering's unified
            // evaluation — not dispatched here.
            g_input.state = MouseState::BOTH_HELD;
            break;

        case MouseState::DRAGGING_RMB:
        case MouseState::BOTH_HELD:
            break;  // already in RMB or both mode, ignore

        default:  // IDLE or PENDING_RMB
            g_input.state = MouseState::PENDING_RMB;
            break;
        }
    }
}

// ── OnMouseMove detour ──────────────────────────────────────────────────

static void __fastcall OnMouseMoveDetour(void* this_ecx, void* /*edx*/,
                                          const float* delta) {
    if (!IsEnabled()) {
        g_origOnMouseMove(this_ecx, delta);
        return;
    }

    // Transition from PENDING → DRAGGING on first mouse movement.
    if (g_input.state == MouseState::PENDING_LMB) {
        BeginDragVisuals();
        g_input.state = MouseState::DRAGGING_LMB;
    } else if (g_input.state == MouseState::PENDING_RMB) {
        BeginDragVisuals();
        g_input.state = MouseState::DRAGGING_RMB;
    }

    if (g_input.state == MouseState::IDLE) return;

    // Accumulate drag distance (for click-vs-drag in OnMouseUp).
    float mag = std::sqrt(delta[0] * delta[0] + delta[1] * delta[1]);
    g_input.accum_dist += std::fabs(mag);

    // Scale delta by mouse sensitivity.
    float sens = GetMouseSensitivity();
    float dx = delta[0] * sens;
    float dy = delta[1] * sens;

    // Dispatch based on our state.
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

    // Reset cursor to saved position (mouselook behavior).
    float saved[2] = { g_input.saved_cursor_x, g_input.saved_cursor_y };
    void* wc = GUIAPI::WC_GetInstance();
    if (wc) GUIAPI::WC_SetMousePosition(wc, saved, true);
}

// ── OnMouseUp detour (click handler) ────────────────────────────────────

static void __fastcall OnMouseUpDetour(void* this_ecx, void* /*edx*/,
                                        const float* pos, int button) {
    if (!IsEnabled()) {
        g_origOnMouseUp(this_ecx, pos, button);
        return;
    }

    // Read the drag threshold from GUI.dll data section.
    float threshold = 0.0f;
    if (GUIAPI::g_guiBase) {
        threshold = *reinterpret_cast<float*>(GUIAPI::g_guiBase + 0x1aeaf4);
    }

    // If a drag was active and moved far enough, suppress click handling.
    bool wasDragging = (g_input.state == MouseState::DRAGGING_LMB ||
                        g_input.state == MouseState::DRAGGING_RMB ||
                        g_input.state == MouseState::BOTH_HELD);
    if (wasDragging && g_input.accum_dist >= threshold) return;

    // Otherwise, it's a click. Set stock handler fields so the original
    // OnMouseUp sees drag_mode=0 and accum_dist=0 (clean click state).
    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<float*>(handler + 0x1C) = 0.0f;

    g_origOnMouseUp(this_ecx, pos, button);
}

// ── EndDrag detour ──────────────────────────────────────────────────────
//
// Called by: (a) stock OnMouseRelease on any button release,
//            (b) 3 GlobalSignals_c cleanup signals (window deactivate, etc.)
// Handles ALL release transitions since OnMouseRelease is NOT hooked.

static void __fastcall EndDragDetour(void* this_ecx, void* /*edx*/) {
    if (!IsEnabled()) {
        g_origEndDrag(this_ecx);
        return;
    }

    // Check physical button state to determine which button was released.
    bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    switch (g_input.state) {
    case MouseState::BOTH_HELD:
        // Forward movement is NOT stopped here — CalcSteering's unified
        // evaluation handles it. When state leaves BOTH_HELD, the next
        // CalcSteering frame sees shouldForward change and dispatches
        // StopForward only if no forward key is held either.

        if (rmb && !lmb) {
            // LMB released, RMB still held → continue char+cam steering.
            g_input.state = MouseState::DRAGGING_RMB;
            // Already in MouseMovement mode, no N3Msg change needed.
        } else if (lmb && !rmb) {
            // RMB released, LMB still held → switch to camera orbit.
            GUIAPI::EndMouseLook();
            g_input.state = MouseState::DRAGGING_LMB;
        } else {
            // Both released (or cleanup signal) → full teardown.
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

    // Always reset stock handler fields so any code reading them sees IDLE.
    auto* handler = reinterpret_cast<uint8_t*>(this_ecx);
    *reinterpret_cast<int*>(handler + 0x08) = 0;
    *reinterpret_cast<int*>(handler + 0x0C) = 0;
    *reinterpret_cast<int*>(handler + 0x10) = static_cast<int>(0xFFFFFFFF);
}

// ── Public init ─────────────────────────────────────────────────────────

bool InitCameraHooks() {
    if (!N3API::Init()) {
        Log("[camera] N3 API init failed — camera hooks disabled");
        return false;
    }

    if (!GamecodeAPI::Init()) {
        Log("[camera] Gamecode API init failed — both-mouse-forward disabled");
    }

    if (!GUIAPI::Init()) {
        Log("[camera] GUI API init failed — input handler hooks disabled");
        return false;
    }

    // ── Hook 1: CalcSteering (N3.dll) ───────────────────────────────
    {
        void* addr = ResolveRVA("N3.dll", kCalcSteeringRVA);
        if (!addr) {
            Log("[camera] CalcSteering RVA invalid");
            return false;
        }
        auto* bytes = static_cast<uint8_t*>(addr);
        Log("[camera] CalcSteering at %p, prologue: %02X %02X %02X %02X %02X",
            addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&CalcSteeringDetour), &tramp)) {
            Log("[camera] failed to hook CalcSteering");
            return false;
        }
        g_origCalcSteering = reinterpret_cast<FnCalcSteering>(tramp);
        Log("[camera] CalcSteering hook installed");
    }

    // ── Hook 2: OnMouseDown (GUI.dll) ───────────────────────────────
    {
        void* addr = ResolveRVA("GUI.dll", kOnMouseDownRVA);
        if (!addr) {
            Log("[camera] OnMouseDown RVA invalid");
            return false;
        }
        auto* bytes = static_cast<uint8_t*>(addr);
        Log("[camera] OnMouseDown at %p, prologue: %02X %02X %02X %02X %02X",
            addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseDownDetour), &tramp)) {
            Log("[camera] failed to hook OnMouseDown");
            return false;
        }
        g_origOnMouseDown = reinterpret_cast<FnOnMouseDown>(tramp);
        Log("[camera] OnMouseDown hook installed");
    }

    // ── Hook 3: OnMouseMove (GUI.dll) ───────────────────────────────
    {
        void* addr = ResolveRVA("GUI.dll", kOnMouseMoveRVA);
        if (!addr) {
            Log("[camera] OnMouseMove RVA invalid");
            return false;
        }
        auto* bytes = static_cast<uint8_t*>(addr);
        Log("[camera] OnMouseMove at %p, prologue: %02X %02X %02X %02X %02X",
            addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseMoveDetour), &tramp)) {
            Log("[camera] failed to hook OnMouseMove");
            return false;
        }
        g_origOnMouseMove = reinterpret_cast<FnOnMouseMove>(tramp);
        Log("[camera] OnMouseMove hook installed");
    }

    // ── Hook 4: OnMouseUp (GUI.dll) ─────────────────────────────────
    {
        void* addr = ResolveRVA("GUI.dll", kOnMouseUpRVA);
        if (!addr) {
            Log("[camera] OnMouseUp RVA invalid");
            return false;
        }
        auto* bytes = static_cast<uint8_t*>(addr);
        Log("[camera] OnMouseUp at %p, prologue: %02X %02X %02X %02X %02X",
            addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&OnMouseUpDetour), &tramp)) {
            Log("[camera] failed to hook OnMouseUp");
            return false;
        }
        g_origOnMouseUp = reinterpret_cast<FnOnMouseUp>(tramp);
        Log("[camera] OnMouseUp hook installed");
    }

    // ── Hook 5: EndDrag (GUI.dll) ───────────────────────────────────
    {
        void* addr = ResolveRVA("GUI.dll", kEndDragRVA);
        if (!addr) {
            Log("[camera] EndDrag RVA invalid");
            return false;
        }
        auto* bytes = static_cast<uint8_t*>(addr);
        Log("[camera] EndDrag at %p, prologue: %02X %02X %02X %02X %02X",
            addr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

        void* tramp = nullptr;
        if (!InstallHook(addr, reinterpret_cast<void*>(&EndDragDetour), &tramp)) {
            Log("[camera] failed to hook EndDrag");
            return false;
        }
        g_origEndDrag = reinterpret_cast<FnEndDrag>(tramp);
        Log("[camera] EndDrag hook installed");
    }

    Log("[camera] All hooks installed — replacement input handler active");
    return true;
}

}  // namespace aor
