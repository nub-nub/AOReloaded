#pragma once

// Timer bar drag system — makes the cast/action timer bars draggable.
//
// Hooks TimerSystemModule_t::CreateTimer in GUI.dll to reposition each
// new timer bar using a saved pixel offset. Registers a mouse event
// filter with the input handler so that LMB click-drag on any active
// timer bar repositions the entire group.
//
// The offset is persisted via the AOR_TBarX / AOR_TBarY DValues
// (written to AOReloaded.ini by the settings hook).
//
// Depends on: InitInputHandler() must have been called first (for the
// mouse event filter system).

namespace aor {

// Install the CreateTimer hook and register the mouse event filter.
// Returns true if the hook was installed successfully.
bool InitTimerBarDrag();

}  // namespace aor
