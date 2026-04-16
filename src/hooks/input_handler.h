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
// Forward movement is a derived signal: shouldForward = BOTH_HELD || W.
// Evaluated per-frame by UpdateForwardMovement() (called from camera hook).

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

// Per-frame forward movement evaluation. Call from the camera hook's
// per-frame update (CalcSteering). Evaluates the unified forward signal
// and dispatches StartForward/StopForward on edges.
void UpdateForwardMovement(void* engine);

// Check if our camera system is enabled (AOR_CamOn DValue).
bool IsCameraEnabled();

// Check if LMB+RMB mouse-run is enabled (AOR_MouseRun DValue).
bool IsMouseRunEnabled();

// Install input handler hooks on GUI.dll callbacks and Gamecode.dll
// movement dispatcher. Call after all game DLLs are loaded.
// Returns true if critical hooks installed successfully.
bool InitInputHandler();

}  // namespace aor
