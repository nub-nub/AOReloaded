#pragma once

// Replacement ActionViewMouseHandler_c — mouse input state machine.
//
// Hooks 4 of the stock handler's callbacks (OnMouseDown, OnMouseMove,
// OnMouseUp, EndDrag) in GUI.dll plus N3Msg_MovementChanged in
// Gamecode.dll. Tracks LMB and RMB independently, enabling:
//   - LMB hold: camera orbit (stock behavior, dispatches via N3Msg)
//   - RMB hold: character + camera steering (stock behavior)
//   - LMB + RMB: run forward + steer (separately toggleable)
//   - Seamless single-button transitions when releasing one of two
//
// Forward movement from BOTH_HELD is dispatched at state transitions
// (enter/exit BOTH_HELD), not per-frame. Keyboard/autorun events are
// suppressed during BOTH_HELD and replayed on exit if appropriate.

#include <cstdint>

namespace aor {

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

// Read-only access to the current input state.
const InputState& GetInputState();

// Check if our camera system is enabled (AOR_CamOn DValue).
bool IsCameraEnabled();

// Check if LMB+RMB mouse-run is enabled (AOR_MouseRun DValue).
bool IsMouseRunEnabled();

// Install input handler hooks on GUI.dll callbacks and Gamecode.dll
// movement dispatcher. Call after all game DLLs are loaded.
// Returns true if critical hooks installed successfully.
bool InitInputHandler();

// ── Mouse event filter system ──────────────────────────────────────────
//
// Allows other modules to intercept mouse events before the camera
// system processes them. Filters are called in registration order;
// the first filter that returns true consumes the event and prevents
// both the stock handler and our camera logic from seeing it.

enum class MouseEventType : uint8_t { Down, Move, Up, EndDrag };

// pos_or_delta: screen-space position (Down/Up) or pixel delta (Move).
//               nullptr for EndDrag.
// button: 1=LMB, 2=RMB (Down/Up only), 0 for Move/EndDrag.
// flags:  clickFlags from Down, 0 otherwise.
using MouseEventFilter = bool(*)(MouseEventType type, const float* pos_or_delta,
                                  int button, int flags);

void RegisterMouseFilter(MouseEventFilter filter);
void UnregisterMouseFilter(MouseEventFilter filter);

}  // namespace aor
