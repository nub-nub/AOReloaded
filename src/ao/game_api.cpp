#include "ao/game_api.h"
#include "core/logging.h"

#include <windows.h>

namespace aor {

GameAPI::FnSetDValue         GameAPI::SetDValue         = nullptr;
GameAPI::FnGetDValue         GameAPI::GetDValue         = nullptr;
GameAPI::FnDoesVariableExist GameAPI::DoesVariableExist = nullptr;
GameAPI::FnAddVariable       GameAPI::AddVariable       = nullptr;

namespace {

template <typename T>
bool Resolve(HMODULE mod, const char* mangled, T*& out, const char* friendly) {
    out = reinterpret_cast<T*>(GetProcAddress(mod, mangled));
    if (!out) {
        Log("[api] FAILED to resolve %s", friendly);
        return false;
    }
    Log("[api] resolved %s -> %p", friendly, reinterpret_cast<void*>(out));
    return true;
}

}  // namespace

bool GameAPI::Init() {
    HMODULE utils = GetModuleHandleA("Utils.dll");
    if (!utils) {
        Log("[api] Utils.dll not loaded");
        return false;
    }
    Log("[api] Utils.dll at %p", utils);

    bool ok = true;
    ok &= Resolve(utils,
        "?SetDValue@DistributedValue_c@@SAXABVString@@ABVVariant@@@Z",
        SetDValue, "SetDValue");
    ok &= Resolve(utils,
        "?GetDValue@DistributedValue_c@@SA?AVVariant@@ABVString@@_N@Z",
        GetDValue, "GetDValue");
    ok &= Resolve(utils,
        "?DoesVariableExist@DistributedValue_c@@SA_NABVString@@@Z",
        DoesVariableExist, "DoesVariableExist");
    ok &= Resolve(utils,
        "?AddVariable@DistributedValue_c@@SAXABVString@@ABVVariant@@_N2@Z",
        AddVariable, "AddVariable");

    return ok;
}

// ── Convenience wrappers ──────────────────────────────────────────────

bool GameAPI::SetInt(const char* name, int value) {
    if (!SetDValue) return false;
    AOString key = AOString::FromShort(name);
    AOVariant val = AOVariant::FromInt(value);
    SetDValue(key, val);
    return true;
}

bool GameAPI::SetFloat(const char* name, float value) {
    if (!SetDValue) return false;
    AOString key = AOString::FromShort(name);
    AOVariant val = AOVariant::FromFloat(value);
    SetDValue(key, val);
    return true;
}

bool GameAPI::SetBool(const char* name, bool value) {
    if (!SetDValue) return false;
    AOString key = AOString::FromShort(name);
    AOVariant val = AOVariant::FromBool(value);
    SetDValue(key, val);
    return true;
}

bool GameAPI::GetVariant(const char* name, AOVariant& out) {
    if (!GetDValue) return false;
    AOString key = AOString::FromShort(name);
    GetDValue(&out, key, false);
    return true;
}

bool GameAPI::Exists(const char* name) {
    if (!DoesVariableExist) return false;
    AOString key = AOString::FromShort(name);
    return DoesVariableExist(key);
}

bool GameAPI::RegisterInt(const char* name, int default_val) {
    if (!AddVariable) return false;
    AOString key = AOString::FromShort(name);
    AOVariant val = AOVariant::FromInt(default_val);
    AddVariable(key, val, false, false);
    Log("[api] registered DValue \"%s\" = %d (Int)", name, default_val);
    return true;
}

bool GameAPI::RegisterFloat(const char* name, float default_val) {
    if (!AddVariable) return false;
    AOString key = AOString::FromShort(name);
    AOVariant val = AOVariant::FromFloat(default_val);
    AddVariable(key, val, false, false);
    Log("[api] registered DValue \"%s\" = %.2f (Float)", name,
        static_cast<double>(default_val));
    return true;
}

bool GameAPI::RegisterBool(const char* name, bool default_val) {
    if (!AddVariable) return false;
    AOString key = AOString::FromShort(name);
    AOVariant val = AOVariant::FromBool(default_val);
    AddVariable(key, val, false, false);
    Log("[api] registered DValue \"%s\" = %s (Bool)", name,
        default_val ? "true" : "false");
    return true;
}

}  // namespace aor
