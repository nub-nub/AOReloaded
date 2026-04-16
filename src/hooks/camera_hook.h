#pragma once

// Camera system hooks for AOReloaded.
//
// Replaces ActionViewMouseHandler_c's state machine with one that tracks
// LMB and RMB independently, enabling:
//   - LMB hold: camera orbit (stock behavior)
//   - RMB hold: character + camera steering (stock behavior)
//   - LMB + RMB: run forward + steer (WoW-style, new)
//   - Seamless single-button transitions when releasing one of two held buttons
//
// Also hooks CameraVehicleFixedThird_t::CalcSteering for per-frame camera
// behaviors: yaw follow during movement, RMB-align (snap character facing
// to camera direction on RMB press).

namespace aor {

// Resolve APIs from N3.dll, Gamecode.dll, and GUI.dll, then install hooks
// on CalcSteering + ActionViewMouseHandler_c callbacks.
// Call after all three DLLs are loaded (game world active).
// Returns true if all critical hooks installed successfully.
bool InitCameraHooks();

}  // namespace aor
