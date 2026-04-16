#pragma once

// Numpad text input fix — translates numpad keys to characters in chat.
//
// The AO client binds numpad keys to camera/movement actions. When typing
// in a text field (chat, search, etc.), pressing numpad keys triggers those
// actions instead of inserting text. This hook intercepts numpad keys at
// InputConfig_t::CheckInput, translates them to their character equivalents,
// and injects them via WindowController_c::HandleTextInput.
//
// Toggled by the AOR_NumpadFix DValue (default: on).

namespace aor {

// Install the CheckInput hook for numpad translation.
// Call after GUI.dll is loaded and the game world is up.
// Returns true if the hook was installed successfully.
bool InitNumpadFix();

}  // namespace aor
