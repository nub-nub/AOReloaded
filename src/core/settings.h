#pragma once

// AOReloaded settings system.
//
// Manages persistent settings stored in AOReloaded.ini (next to the exe).
// Each setting is backed by a DValue in the game's registry, so the options
// panel widgets bind to them directly. A detour on SetDValue catches all
// changes and writes them to the .ini immediately.
//
// Init flow (called from DeferredInit):
//   1. SettingsInit()         — resolve .ini path, load persisted values
//   2. SettingsRegisterAll()  — register DValues with loaded (or default) values
//   3. SettingsInstallHook()  — detour SetDValue for change persistence
//                               (call after game world is up, so we don't
//                               spam the .ini during the game's own init)

namespace aor {

// Step 1: Resolve the .ini file path and load any previously saved values
// into an internal table. Does NOT touch DValues or game state.
void SettingsInit();

// Step 2: Register each setting as a DValue via GameAPI::RegisterXxx,
// using the value loaded from .ini (or the compiled-in default).
void SettingsRegisterAll();

// Step 3: Install the SetDValue detour. Any subsequent SetDValue call
// whose name matches a registered setting will trigger an .ini write.
// Returns false if the hook couldn't be installed.
bool SettingsInstallHook();

}  // namespace aor
