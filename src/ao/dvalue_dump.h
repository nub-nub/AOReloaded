#pragma once

namespace aor {

// Walk the game's global DistributedValue registry (an MSVC std::map
// inside Utils.dll) and log every registered key, its type, value, and
// category. Must be called after Utils.dll is loaded and the game has
// populated the registry (ideally after zone-in).
void DumpAllDValues();

}  // namespace aor
