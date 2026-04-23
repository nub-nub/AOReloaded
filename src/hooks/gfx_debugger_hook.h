#pragma once

// Graphics debugger mode hook — manipulates Debugger_t::m_nDebuggerMode in randy31.dll.
//
// m_nDebuggerMode is a static int member (mangled: ?m_nDebuggerMode@Debugger_t@@2IA)
// that controls renderer debug visualisation features. The value is a
// bitmask of individual debug modes.
//
// Exposed via the AOR_GfxDebug DValue (Int, default 0). Changes are written
// directly to the game's memory at init and on every DValue change.

#include <cstdint>

namespace aor
{

    // Resolve randy31.dll addresses and apply the initial graphics debugger mode value
    // from the DValue. Call after randy31.dll is loaded.
    // Returns true if the address was resolved successfully.
    bool InitDebuggerMode();

    // Write a new value to m_nDebuggerMode.
    void SetDebuggerMode(int mode);

    // Read the current value of m_nDebuggerMode from game memory.
    // Returns -1 if the address hasn't been resolved or the read fails.
    int GetDebuggerMode();

}
