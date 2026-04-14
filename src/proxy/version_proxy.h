#pragma once

// Lazily loads the real System32\version.dll and resolves all exports.
// Called automatically on first forwarded API call.
void EnsureRealVersionDllLoaded();
