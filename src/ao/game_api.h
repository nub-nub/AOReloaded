#pragma once

#include "ao/types.h"

// Runtime-resolved function pointers into AO's DLLs.
//
// Resolved by MSVC mangled name via GetProcAddress at runtime.
// Populated by GameAPI::Init() after the client's DLLs are loaded.

namespace aor {

struct GameAPI {
    // Resolve all function pointers. Returns false if critical resolves fail.
    static bool Init();

    // ── DistributedValue_c (from Utils.dll) ────────────────────────────
    //
    // Static SetDValue: __cdecl, args passed on stack.
    //   void SetDValue(const String& name, const Variant& value)
    //
    // Static GetDValue: __cdecl, returns Variant by value.
    //   On MSVC x86, return-by-value structs > 8 bytes use a hidden
    //   pointer as the first parameter (caller allocates, callee fills).
    //   Variant is exactly 16 bytes, so this applies.
    //   Actual calling convention: void(Variant* retval, const String& name, bool use_default)
    //
    // DoesVariableExist: __cdecl, returns bool.
    //   bool DoesVariableExist(const String& name)

    using FnSetDValue         = void(__cdecl*)(const AOString&, const AOVariant&);
    using FnGetDValue         = void(__cdecl*)(AOVariant* ret, const AOString&, bool);
    using FnDoesVariableExist = bool(__cdecl*)(const AOString&);

    // AddVariable: register a new DValue in the global registry.
    //   void AddVariable(const String& name, const Variant& default, bool persist, bool notify)
    using FnAddVariable = void(__cdecl*)(const AOString&, const AOVariant&, bool, bool);

    static FnSetDValue         SetDValue;
    static FnGetDValue         GetDValue;
    static FnDoesVariableExist DoesVariableExist;
    static FnAddVariable       AddVariable;

    // ── Convenience wrappers ──────────────────────────────────────────
    // Only safe for key names < 16 chars (SSO path).

    static bool SetInt(const char* name, int value);
    static bool SetFloat(const char* name, float value);
    static bool SetBool(const char* name, bool value);
    static bool GetVariant(const char* name, AOVariant& out);
    static bool Exists(const char* name);
    static bool RegisterInt(const char* name, int default_val);
    static bool RegisterFloat(const char* name, float default_val);
    static bool RegisterBool(const char* name, bool default_val);
};

}  // namespace aor
