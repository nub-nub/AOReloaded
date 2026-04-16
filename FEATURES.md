# AOReloaded — Features

Client mod for Anarchy Online. Drop `version.dll` into your client directory and go.

## Installation

1. Copy `version.dll` into the same folder as `AnarchyOnline.exe`
2. Launch the game normally
3. Check `AOReloaded.log` in the client folder to confirm it loaded

## Features

### WoW-style Camera Auto-Follow
In 3rd person, the camera smoothly returns to a position behind your character whenever you move (and you're not holding LMB). LMB-drag still orbits the camera freely; on release it stays put until you start moving again. The "behind" position is based on which way your character is facing, so walking backwards or strafing won't flip the camera around.

Additionally, any **right-click drag** realigns your character to face the direction the camera is currently looking and snaps the camera to directly behind — so after orbiting with LMB, your next RMB-drag starts from a clean "behind" view instead of preserving the orbit offset.

**Both mouse buttons held** makes your character run forward (WoW-style "mouse-walk"). You can press the buttons in either order; the moment both are held, forward motion starts. Releasing either button falls back to whatever the remaining button does (LMB → orbit, RMB → drag-turn, neither → idle). RMB drag behavior while both are held continues to rotate the character normally.

Forward movement from mouse buttons and the keyboard move-forward key (W / Up / Numpad 8) coexist cleanly: if you hold both mouse buttons and also press your forward key, releasing either input source does not interrupt the other. You can seamlessly transition between keyboard-driven and mouse-driven forward movement. Strafe and turn keys also work naturally while both mouse buttons are held.

**Config:** Options panel (F10) → **AOReloaded** tab → **Camera**:
- "WoW-style camera (auto-recenter after LMB drag)" — master toggle for the camera system (yaw follow, RMB-align). Lerp speed controlled by the speed slider (default 2; higher = snappier follow).
- "LMB+RMB mouse-run" — separately toggleable. When off, pressing both mouse buttons does nothing extra (stock behavior). When on, holding both buttons runs forward with mouse steering. Enabled by default.

### Numpad Keys in Chat
By default, pressing numpad keys while typing in chat triggers camera or movement actions instead of inserting text. With this fix enabled, numpad digits (0-9), decimal point, and arithmetic operators (+, -, *, /) all type normally in any text field — chat, search boxes, and other text inputs. The fix only activates when a text input has focus; numpad keys work as normal action bindings in gameplay.

**Config:** Options panel (F10) → **AOReloaded** tab → **Input** → "Numpad keys type in chat". Enabled by default.

<!-- 
Template for adding features:

### Feature Name
Brief description of what it does from a player's perspective.

**Config:** How to configure it (options panel, /command, ini file, or "none — always on").

-->

## Options Panel

AOReloaded adds an **AOReloaded** tab to the in-game options panel (F10). Mod settings will appear here as features are added.
