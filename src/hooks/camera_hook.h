#pragma once

// Camera behavior hooks for AOReloaded.
//
// Hooks CameraVehicleFixedThird_t::CalcSteering in N3.dll for per-frame
// camera behaviors:
//   - Yaw follow: camera smoothly returns behind character during movement
//   - RMB-align: snap character facing to camera direction on RMB press
//   - Both-buttons char-align: continuous alignment during BOTH_HELD
//   - Unified forward evaluation: delegates to input_handler per frame
//
// Depends on input_handler for InputState and forward movement dispatch.
// Call InitInputHandler() before InitCameraHooks().

namespace aor {

// Resolve N3.dll camera functions and install the CalcSteering hook.
// Call after InitInputHandler() and after N3.dll is loaded.
// Returns true if hook installed successfully.
bool InitCameraHooks();

}  // namespace aor
