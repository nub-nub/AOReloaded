#pragma once

// Camera system hooks for AOReloaded.
//
// Phase 1: Hooks ActionViewMouseHandler_c::EndDrag in GUI.dll to fix the
// LMB drag offset persistence bug. When a small accidental LMB drag ends
// in 3rd person, auto-recenters camera behind the character.

namespace aor {

// Resolve N3.dll camera functions and install the EndDrag hook.
// Call after both GUI.dll and N3.dll are loaded (game world active).
// Returns true if hook installed successfully.
bool InitCameraHooks();

}  // namespace aor
