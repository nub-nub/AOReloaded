// camera_hook.cpp — WoW-style camera: auto-follow behind character during movement.
//
// LMB drag orbits camera. On release, camera stays at new angle.
// When the character moves, camera gradually lerps back behind the character.
// When stationary, camera stays wherever you left it.
//
// Implementation: hook CameraVehicleFixedThird_t::CalcSteering (per-frame).
// Before the original steering runs, check if the player is moving. If so,
// lerp the camera heading direction toward "behind the character" using the
// player's facing quaternion. The original CalcSteering then steers toward
// the updated heading naturally.

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

// GetGlobalPos returns const Vector3_t& (pointer in EAX).
using FnGetGlobalPos = float*(__thiscall*)(void* dynel);

// n3Dynel_t::SetRelRot(Quaternion const&) — writes body rotation via Vehicle_t.
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

// n3EngineClientAnarchy_t::N3Msg_MovementChanged(MovementAction_e, float, float, bool).
using FnN3MsgMovementChanged =
    void(__thiscall*)(void* engineAnarchy, int action,
                      float f1, float f2, bool b);

// n3EngineClientAnarchy_t::N3Msg_EndMouseMovement() — ends the mouse-look
// mode (resets cursor capture, stops any in-progress RMB/LMB turn, fires
// FullStop). Hooked so we can suppress it when the OTHER mouse button is
// still held (fixing the "release one → both mode ends" issue).
using FnN3MsgEndMouseMovement = void(__thiscall*)(void* engineAnarchy);

static FnN3MsgMovementChanged  N3Msg_MovementChanged   = nullptr;
static FnN3MsgEndMouseMovement N3Msg_EndMouseMovement  = nullptr;

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

    void* pe = reinterpret_cast<void*>(GetProcAddress(gc,
        "?N3Msg_EndMouseMovement@n3EngineClientAnarchy_t@@QAEXXZ"));
    if (pe) {
        N3Msg_EndMouseMovement =
            reinterpret_cast<FnN3MsgEndMouseMovement>(pe);
        Log("[camera] resolved N3Msg_EndMouseMovement -> %p", pe);
    } else {
        Log("[camera] FAILED to resolve N3Msg_EndMouseMovement");
    }

    return true;
}

}  // namespace GamecodeAPI

// MovementAction_e enum values, confirmed by live-hooking the native
// N3Msg_MovementChanged dispatcher and observing W/S key presses.
constexpr int kActionStartForward = 1;
constexpr int kActionStopForward  = 2;


// ── Math helpers ────────────────────────────────────────────────────────

static float Vec3LenXZ(float x, float z) {
    return std::sqrt(x * x + z * z);
}

// ── CalcSteering hook ───────────────────────────────────────────────────
//
// CameraVehicleFixedThird_t::CalcSteering (N3.dll RVA 0x1f752)
// __thiscall, RET 4 (one stack param: Vector3_t& result)
//
// Called every frame for 3rd person camera. We inject yaw follow logic
// before the original steering computation.

constexpr uint32_t kCalcSteeringRVA = 0x0001f752;

using FnCalcSteering = int(__thiscall*)(void* vehicle, void* result);
static FnCalcSteering g_origCalcSteering = nullptr;

// Per-frame state for movement detection.
static float g_lastPlayerX = 0.0f;
static float g_lastPlayerZ = 0.0f;
static bool  g_hasLastPos  = false;

// Edge-detection for RMB-align (character → camera facing on RMB press).
static bool  g_rmbWasHeld  = false;
// Edge-detection for both-mouse-buttons-held → run forward.
static bool  g_bothWasHeld = false;

// Mouse-button state machine patch. The AO exe's WndProcImpl takes FIVE
// __stdcall args: (void* windowState, HWND, UINT msg, WPARAM, LPARAM).
// The leading object pointer is AO's internal window-state struct; the
// WndProc stub at 0x00405d4e loads it from [0x00411fb4] and pushes it as
// arg1 before forwarding to WndProcImpl. RET 0x14 in WndProcImpl confirms
// 5 dwords of callee cleanup.
using FnWndProc = LRESULT(__stdcall*)(void*, HWND, UINT, WPARAM, LPARAM);
static FnWndProc g_origWndProc = nullptr;
static bool      g_lmbSwallowed = false;

static LRESULT __stdcall WndProcDetour(void* windowState, HWND hwnd,
                                       UINT msg, WPARAM wParam,
                                       LPARAM lParam) {
    bool rmbHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    bool lmbHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;

    switch (msg) {
    case WM_LBUTTONDOWN:
        // LMB-down while RMB is already held would switch the game from
        // RMB-turn to LMB-orbit, killing mouse-turn. Swallow.
        if (rmbHeld) {
            g_lmbSwallowed = true;
            return 0;
        }
        break;

    case WM_LBUTTONUP: {
        // Case A: this release matches an LMB-down we swallowed earlier.
        // Swallow so the game stays blind to the whole LMB cycle.
        if (g_lmbSwallowed) {
            g_lmbSwallowed = false;
            return 0;
        }
        // Case B: real LMB release while RMB is still physically held.
        // The game will ReleaseCapture and end whichever mode it was in;
        // drop a synthetic WM_RBUTTONDOWN straight after so it re-enters
        // RMB-turn mode. SendMessage (synchronous) keeps ReleaseCapture
        // → new SetCapture back-to-back so the cursor doesn't flicker.
        if (rmbHeld) {
            LRESULT r = g_origWndProc(windowState, hwnd, msg,
                                      wParam, lParam);
            SendMessageA(hwnd, WM_RBUTTONDOWN, MK_RBUTTON, lParam);
            return r;
        }
        break;
    }

    case WM_RBUTTONUP:
        // RMB release while LMB is still physically held. Let the game
        // end RMB-turn, then synthesize a WM_LBUTTONDOWN so it re-enters
        // LMB-orbit. Clear g_lmbSwallowed — the game now genuinely holds
        // an LMB-down (from our synth) and the next real WM_LBUTTONUP
        // must pass through to end LMB-orbit cleanly.
        if (lmbHeld) {
            LRESULT r = g_origWndProc(windowState, hwnd, msg,
                                      wParam, lParam);
            g_lmbSwallowed = false;
            SendMessageA(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lParam);
            return r;
        }
        break;

    default:
        break;
    }

    return g_origWndProc(windowState, hwnd, msg, wParam, lParam);
}

// Default follow speed (DValue overridable).
constexpr float kDefaultFollowSpeed = 5.0f;

// Minimum movement per frame to count as "moving" (in world units).
constexpr float kMovementThreshold = 0.01f;

static int __fastcall CalcSteeringDetour(void* vehicle, void* /*edx*/, void* result) {
    // Get engine + player dynel.
    void* engine = N3API::GetEngineInstance ? N3API::GetEngineInstance() : nullptr;
    if (!engine) goto call_original;

    // Check master toggle.
    {
        AOVariant enabled{};
        if (GameAPI::GetVariant("AOR_CamOn", enabled) &&
            enabled.type == static_cast<uint32_t>(VariantType::Bool) &&
            !enabled.as_bool) {
            g_rmbWasHeld = false;  // reset edge detector so disable/enable is clean
            goto call_original;
        }
    }

    // ── RMB-align: on RMB press edge, rotate the player dynel so char
    // faces where the camera is pointing, then reset the camera's local
    // heading to "directly behind" (preserving pitch). WoW-style: right-
    // clicking realigns character with camera view instead of dragging
    // the character+camera offset together.
    {
        bool rmbHeld = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        bool lmbHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        bool rmbEdge = rmbHeld && !g_rmbWasHeld;
        g_rmbWasHeld = rmbHeld;

        // Both-mouse-buttons → run forward. Dispatch on rising/falling edge.
        bool bothHeld = rmbHeld && lmbHeld;
        if (bothHeld != g_bothWasHeld && GamecodeAPI::N3Msg_MovementChanged) {
            int action = bothHeld ? kActionStartForward : kActionStopForward;
            GamecodeAPI::N3Msg_MovementChanged(engine, action, 0.0f, 0.0f, true);
        }

        g_bothWasHeld = bothHeld;

        // Continuous char-align during both-held. Every frame, rotate
        // the player dynel so it faces where the camera is looking, and
        // reset local heading to "behind". Effect: any camera motion
        // (LMB orbit, RMB turn, or anything else) drags the character
        // along, so mouse drag during both-held always turns the
        // character regardless of which mode the game is internally in.
        bool doAlign = (rmbEdge || bothHeld) && N3API::SetRelRot;
        if (doAlign) {
            auto base = reinterpret_cast<uintptr_t>(vehicle);
            float hx = *reinterpret_cast<float*>(base + 0x1F8);
            float hz = *reinterpret_cast<float*>(base + 0x200);
            float lenXZ = Vec3LenXZ(hx, hz);

            if (lenXZ >= 0.001f) {
                constexpr float kPi = 3.14159265358979323846f;

                // delta = -π/2 - atan2(hz, hx). See derivation in commit note:
                // char_forward_world = (sin θ, 0, cos θ); camera look dir in
                // world = -rotate(local_heading, charQuat). Setting char to
                // face camera look yields this formula (zero when heading
                // already points locally "behind" (0, 0, -1)).
                float beta  = std::atan2(hz, hx);
                float delta = -kPi * 0.5f - beta;
                while (delta >  kPi) delta -= 2.0f * kPi;
                while (delta < -kPi) delta += 2.0f * kPi;

                if (std::fabs(delta) > 0.001f) {
                    // Char yaw quat at vehicle+0x16C is pure-Y: (0, qy, 0, qw).
                    float qy = *reinterpret_cast<float*>(base + 0x170);
                    float qw = *reinterpret_cast<float*>(base + 0x178);
                    float currentAngle = 2.0f * std::atan2(qy, qw);
                    float newAngle = currentAngle + delta;

                    struct Q { float x, y, z, w; };
                    Q newQ{ 0.0f,
                            std::sin(newAngle * 0.5f),
                            0.0f,
                            std::cos(newAngle * 0.5f) };

                    // ORDER MATTERS: reset local heading BEFORE SetRelRot.
                    // SetRelRot on the player dynel triggers a synchronous
                    // cascade (locality notify → recompute camera target)
                    // that reads camera heading × char-yaw. If heading is
                    // still at its pre-snap offset when that runs, the
                    // camera gets placed at a doubly-rotated (offset ×
                    // rotation delta) position for one rendered frame.
                    // Resetting heading first makes the cascade see the
                    // correct post-snap state.
                    *reinterpret_cast<float*>(base + 0x1F8) = 0.0f;
                    *reinterpret_cast<float*>(base + 0x200) = -lenXZ;

                    if (void* dynel = N3API::GetClientControlDynel(engine)) {
                        N3API::SetRelRot(dynel, &newQ);
                    }
                }
            }
        }
    }

    {
        void* dynel = N3API::GetClientControlDynel(engine);
        if (!dynel) goto call_original;

        // Get player position.
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

        // Compute movement distance in XZ plane.
        float dx = px - g_lastPlayerX;
        float dz = pz - g_lastPlayerZ;
        float moveDist = Vec3LenXZ(dx, dz);

        g_lastPlayerX = px;
        g_lastPlayerZ = pz;

        // Only follow when character is moving.
        if (moveDist < kMovementThreshold) goto call_original;

        // Don't follow while LMB is held (player is orbiting camera).
        if (GetAsyncKeyState(VK_LBUTTON) & 0x8000) goto call_original;

        auto base = reinterpret_cast<uintptr_t>(vehicle);

        // "Behind the character" in the heading's local space is always (0, -1).
        // RecalcOptimalPos rotates heading by the character yaw quaternion at
        // vehicle+0x16c, so local (0, -1) becomes world-space "behind character".
        // This means direction is based on character FACING, not movement direction
        // — walking backwards won't flip the camera.
        float behindX = 0.0f;
        float behindZ = -1.0f;

        // Read current camera heading from vehicle+0x1F8 (3D direction vector).
        float* headingX = reinterpret_cast<float*>(base + 0x1F8);
        float* headingY = reinterpret_cast<float*>(base + 0x1FC);
        float* headingZ = reinterpret_cast<float*>(base + 0x200);

        // Read delta time from engine+0x68.
        float dt = *reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(engine) + 0x68);

        // Follow speed — read from DValue or use default.
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

        // Angle-based lerp — uniform angular speed and correct shortest-path
        // for any angular distance (including near-180° where vector lerp
        // collapses to near-zero direction changes).
        float oldLenXZ = Vec3LenXZ(*headingX, *headingZ);
        if (oldLenXZ < 0.001f) goto call_original;

        float currentAngle = std::atan2(*headingZ, *headingX);
        float targetAngle  = std::atan2(behindZ, behindX);
        float diff = targetAngle - currentAngle;
        // Wrap to [-π, π] so we always rotate the short way.
        constexpr float kPi = 3.14159265358979323846f;
        if (diff >  kPi) diff -= 2.0f * kPi;
        if (diff < -kPi) diff += 2.0f * kPi;

        float newAngle = currentAngle + diff * t;
        *headingX = std::cos(newAngle) * oldLenXZ;
        *headingZ = std::sin(newAngle) * oldLenXZ;
        // headingY unchanged — preserves pitch.
    }

call_original:
    int ret = g_origCalcSteering(vehicle, result);
    return ret;
}

// ── Public init ─────────────────────────────────────────────────────────

bool InitCameraHooks() {
    if (!N3API::Init()) {
        Log("[camera] N3 API init failed — camera hooks disabled");
        return false;
    }

    // Gamecode API is optional — if resolution fails, the RMB-align and
    // follow-camera still work, but the "both mouse buttons → forward"
    // feature is disabled. Log and continue.
    if (!GamecodeAPI::Init()) {
        Log("[camera] Gamecode API init failed — both-mouse-forward disabled");
    }

    // Hook CalcSteering on CameraVehicleFixedThird_t.
    void* calcSteeringAddr = ResolveRVA("N3.dll", kCalcSteeringRVA);
    if (!calcSteeringAddr) {
        Log("[camera] N3.dll CalcSteering RVA invalid");
        return false;
    }
    auto* bytes = static_cast<uint8_t*>(calcSteeringAddr);
    Log("[camera] CalcSteering at %p, prologue: %02X %02X %02X %02X %02X",
        calcSteeringAddr, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* trampoline = nullptr;
    if (!InstallHook(calcSteeringAddr, reinterpret_cast<void*>(&CalcSteeringDetour),
                     &trampoline)) {
        Log("[camera] failed to hook CalcSteering");
        return false;
    }
    g_origCalcSteering = reinterpret_cast<FnCalcSteering>(trampoline);

    Log("[camera] CalcSteering hook installed — yaw follow active");

    // Hook the AO exe's WndProc to fix mouse-button state-machine:
    //   - Issue A: LMB-down while RMB-held shouldn't kick the game out
    //     of RMB-turn mode.
    //   - Issue B: releasing one button while the other stays held
    //     should fall back to the remaining button's mode, not idle.
    // WndProc lives at AnarchyOnline.exe RVA 0x00004a66. Prologue is
    // `B8 imm32; CALL __SEH_prolog4` — the MOV-EAX-imm32 pattern we
    // already whitelist.
    if (HMODULE exe = GetModuleHandleA(nullptr)) {
        void* wndProcAddr = reinterpret_cast<uint8_t*>(exe) + 0x4a66;
        auto* mb = static_cast<uint8_t*>(wndProcAddr);
        Log("[camera] AO WndProc at %p, prologue: "
            "%02X %02X %02X %02X %02X",
            wndProcAddr, mb[0], mb[1], mb[2], mb[3], mb[4]);
        void* wptramp = nullptr;
        if (InstallHook(wndProcAddr,
                        reinterpret_cast<void*>(&WndProcDetour),
                        &wptramp)) {
            g_origWndProc = reinterpret_cast<FnWndProc>(wptramp);
            Log("[camera] WndProc hook installed — mouse state fixes active");
        } else {
            Log("[camera] failed to hook WndProc");
        }
    }

    return true;
}

}  // namespace aor
