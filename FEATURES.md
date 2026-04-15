# AOReloaded — Features

Client mod for Anarchy Online. Drop `version.dll` into your client directory and go.

## Installation

1. Copy `version.dll` into the same folder as `AnarchyOnline.exe`
2. Launch the game normally
3. Check `AOReloaded.log` in the client folder to confirm it loaded

## Features

### WoW-style Camera Auto-Follow
In 3rd person, the camera smoothly returns to a position behind your character whenever you move (and you're not holding LMB). LMB-drag still orbits the camera freely; on release it stays put until you start moving again. The "behind" position is based on which way your character is facing, so walking backwards or strafing won't flip the camera around.

**Config:** Options panel (F10) → **AOReloaded** tab → **Camera** → "WoW-style camera (auto-recenter after LMB drag)". Lerp speed is controlled by the `AOR_CYawSpd` DValue (default 5; higher = snappier follow).

<!-- 
Template for adding features:

### Feature Name
Brief description of what it does from a player's perspective.

**Config:** How to configure it (options panel, /command, ini file, or "none — always on").

-->

## Options Panel

AOReloaded adds an **AOReloaded** tab to the in-game options panel (F10). Mod settings will appear here as features are added.
