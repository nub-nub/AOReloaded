#pragma once

// Minimal 5-byte inline hook engine for x86 Windows.
//
// Overwrites the first 5 bytes of a target function with `jmp rel32` to a
// detour, and builds a trampoline that executes the saved original bytes
// then jumps back to target+5.
//
// Supports two common MSVC x86 prologues:
//   1. Hot-patch:  mov edi,edi; push ebp; mov ebp,esp  (8B FF 55 8B EC)
//   2. Standard:   push ebp; mov ebp,esp; sub esp,NN   (55 8B EC 83 EC xx)
//                  or push ebp; mov ebp,esp; push reg   (55 8B EC 5x)
//
// For prologue type 2, the first 5 bytes may include part of a longer
// instruction (sub esp,imm8 is 3 bytes, so 55 8B EC 83 EC = 5 bytes exactly).
// We validate known-safe patterns before hooking.
//
// Ported from ao-crashfix, extended for AOReloaded.

#include <windows.h>
#include <cstdint>

namespace aor {

// Install a 5-byte inline hook at `target`, redirecting to `detour`.
// On success, `*trampoline_out` receives a pointer that calls the original.
// Returns true on success. The trampoline is leaked (never freed).
bool InstallHook(void* target, void* detour, void** trampoline_out);

// Resolve a function address from a loaded DLL by RVA.
// Returns (module_base + rva), or nullptr if module not loaded.
void* ResolveRVA(const char* module_name, uint32_t rva);

}  // namespace aor
