#pragma once

// LargeAddressAware PE header patching.
//
// Patches AnarchyOnline.exe on disk to set IMAGE_FILE_LARGE_ADDRESS_AWARE
// in the COFF Characteristics field. This allows the 32-bit process to use
// up to 4 GB of virtual address space on 64-bit Windows (instead of 2 GB).
//
// The flag is checked by the Windows loader at process creation, so the
// patch takes effect on the *next* launch after being applied. Because
// the running exe is locked for writes by the OS, we use a rename-copy-
// patch strategy: rename the locked exe to .bak, copy .bak to a new file
// at the original path, patch the new (unlocked) file, and clean up .bak
// on the next launch.
//
// Call from OnProcessAttach — requires no game DLLs.

namespace aor {

// Patch AnarchyOnline.exe to set the LAA flag if it isn't already set.
// Safe to call from DLL_PROCESS_ATTACH. Logs results via aor::Log().
void PatchLargeAddressAware();

}  // namespace aor
