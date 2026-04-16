// settings.cpp — AOReloaded persistent settings via .ini file.
//
// Architecture:
//   - A static table of SettingDef entries defines every AOReloaded setting
//     (name, type, default value).
//   - SettingsInit() reads AOReloaded.ini into each entry's `current` field.
//   - SettingsRegisterAll() creates DValues from those values.
//   - SettingsInstallHook() detours SetDValue so that any change to an
//     AOReloaded DValue is immediately persisted to the .ini.
//
// The .ini uses the [AOReloaded] section. Bools are stored as 0/1, ints
// as decimal integers. We use the Win32 Private Profile API which handles
// file locking and atomic writes.

#include "core/settings.h"
#include "core/logging.h"
#include "ao/game_api.h"
#include "ao/types.h"
#include "hooks/hook_engine.h"

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace aor {

// ── Setting definitions ────────────────────────────────────────────────

enum class SettingType : uint8_t { Bool, Int };

struct SettingDef {
    const char* name;        // DValue name (must be <= 15 chars)
    SettingType type;
    int         default_val; // bool: 0/1, int: integer value
    int         current;     // loaded from .ini or set to default
    int         min_val;     // slider min (only meaningful for Int)
    int         max_val;     // slider max (only meaningful for Int)
};

// Master table of all AOReloaded settings. Add new settings here.
static SettingDef g_settings[] = {
    // name           type              default  current  min  max
    { "AOR_CamOn",    SettingType::Bool, 1,       1,       0,   1  },
    { "AOR_CYawSpd",  SettingType::Int,  2,       2,       1,  10  },
    { "AOR_MouseRun", SettingType::Bool, 1,       1,       0,   1  },
};

static constexpr int kSettingCount = sizeof(g_settings) / sizeof(g_settings[0]);
static const char kIniSection[] = "AOReloaded";

// ── DValue tree min/max ────────────────────────────────────────────────
//
// The OptionSlider widget reads min/max from the DValue registry node to
// compute knob position: (value - min) / (max - min). Without min/max,
// the division is zero → crash. AddVariable doesn't set them, so we walk
// the tree and write directly.
//
// Node layout offsets (verified hex dump, see src/ao/CLAUDE.md):
//   +0x10: AOString key
//   +0x50: Variant (type tag + padding)
//   +0x58: value payload
//   +0x61: bool has_min
//   +0x62: bool has_max
//   +0x68: Variant min_value (0x10 bytes)
//   +0x78: Variant max_value (0x10 bytes)
//   +0xA1: char _Isnil (0=real, 1=sentinel)

// RVAs for the global std::map internals in Utils.dll.
static constexpr uint32_t kMyHeadRVA = 0x2e61c;

static bool SetDValueMinMax(const char* name, int minVal, int maxVal) {
    HMODULE utils = GetModuleHandleA("Utils.dll");
    if (!utils) return false;

    auto base = reinterpret_cast<uint8_t*>(utils);

    // Read the tree's sentinel node (head).
    uint8_t* head = nullptr;
    __try {
        head = *reinterpret_cast<uint8_t**>(base + kMyHeadRVA);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[settings] failed to read DValue tree head");
        return false;
    }
    if (!head) return false;

    // Walk the tree starting from head->_Left (the root).
    // In-order traversal looking for our key.
    constexpr int kKeyOffset   = 0x10;
    constexpr int kIsnilOffset = 0xA1;
    constexpr int kHasMinOffset = 0x61;
    constexpr int kHasMaxOffset = 0x62;
    constexpr int kMinValOffset = 0x68;
    constexpr int kMaxValOffset = 0x78;

    uint8_t* cur = nullptr;
    __try {
        // Start at leftmost (smallest key).
        cur = *reinterpret_cast<uint8_t**>(head);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    // Walk up to 500 nodes (safety bound — registry has ~270).
    for (int i = 0; i < 500; ++i) {
        __try {
            if (cur[kIsnilOffset] != 0) break;  // sentinel → done

            auto* key = reinterpret_cast<AOString*>(cur + kKeyOffset);
            const char* keyStr = key->c_str();
            if (keyStr && std::strcmp(keyStr, name) == 0) {
                // Found it. Write min/max.
                *reinterpret_cast<bool*>(cur + kHasMinOffset) = true;
                *reinterpret_cast<bool*>(cur + kHasMaxOffset) = true;
                auto* minVar = reinterpret_cast<AOVariant*>(cur + kMinValOffset);
                auto* maxVar = reinterpret_cast<AOVariant*>(cur + kMaxValOffset);
                *minVar = AOVariant::FromInt(minVal);
                *maxVar = AOVariant::FromInt(maxVal);
                Log("[settings] set min/max for %s: [%d, %d]", name, minVal, maxVal);
                return true;
            }

            // In-order successor: right subtree's leftmost, or walk up.
            uint8_t* right = *reinterpret_cast<uint8_t**>(cur + 0x08);
            if (right[kIsnilOffset] == 0) {
                cur = right;
                uint8_t* left = *reinterpret_cast<uint8_t**>(cur);
                while (left[kIsnilOffset] == 0) {
                    cur = left;
                    left = *reinterpret_cast<uint8_t**>(cur);
                }
            } else {
                uint8_t* par = *reinterpret_cast<uint8_t**>(cur + 0x04);
                while (par[kIsnilOffset] == 0 &&
                       cur == *reinterpret_cast<uint8_t**>(par + 0x08)) {
                    cur = par;
                    par = *reinterpret_cast<uint8_t**>(cur + 0x04);
                }
                cur = par;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[settings] AV while searching for %s", name);
            return false;
        }
    }

    Log("[settings] DValue %s not found in tree", name);
    return false;
}

// ── .ini file path ─────────────────────────────────────────────────────

static wchar_t g_iniPath[MAX_PATH] = {};
static bool    g_iniReady = false;

static void ResolveIniPath() {
    // The exe runs from the client directory. Place .ini alongside it,
    // same as AOReloaded.log.
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        Log("[settings] GetModuleFileName failed");
        return;
    }

    // Strip exe filename, keep trailing backslash.
    wchar_t* slash = nullptr;
    for (wchar_t* p = exePath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') slash = p;
    }
    if (!slash) return;
    *(slash + 1) = L'\0';

    if (swprintf(g_iniPath, MAX_PATH, L"%sAOReloaded.ini", exePath) < 0) {
        Log("[settings] path format failed");
        return;
    }

    g_iniReady = true;
    Log("[settings] ini path: %ls", g_iniPath);
}

// ── .ini read/write ────────────────────────────────────────────────────

static int ReadIniInt(const char* name, int defaultVal) {
    if (!g_iniReady) return defaultVal;
    // GetPrivateProfileStringA so we can use narrow-char section/key names
    // with the wide-char file path via GetPrivateProfileStringW would
    // require wide keys. Instead, use the A variant and convert the path.
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, g_iniPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);
    return GetPrivateProfileIntA(kIniSection, name, defaultVal, narrowPath);
}

static void WriteIniInt(const char* name, int value) {
    if (!g_iniReady) return;
    char narrowPath[MAX_PATH];
    WideCharToMultiByte(CP_ACP, 0, g_iniPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);
    char buf[32];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", value);
    WritePrivateProfileStringA(kIniSection, name, buf, narrowPath);
}

// ── Find a setting by name ─────────────────────────────────────────────

static SettingDef* FindSetting(const char* name) {
    for (int i = 0; i < kSettingCount; ++i) {
        if (std::strcmp(g_settings[i].name, name) == 0)
            return &g_settings[i];
    }
    return nullptr;
}

// ── SetDValue hook ─────────────────────────────────────────────────────
//
// Detours the game's static SetDValue to intercept writes to our settings.
// After calling the original, checks if the name matches any AOReloaded
// setting and persists the new value to the .ini.

static GameAPI::FnSetDValue g_origSetDValue = nullptr;

static void __cdecl SetDValueDetour(const AOString& name, const AOVariant& value) {
    // Always call the original first — the game must see the change.
    g_origSetDValue(name, value);

    // Quick prefix check: all our settings start with "AOR_".
    const char* str = name.c_str();
    if (str[0] != 'A' || str[1] != 'O' || str[2] != 'R' || str[3] != '_')
        return;

    SettingDef* def = FindSetting(str);
    if (!def) return;

    // Extract the value based on the setting's type.
    // The OptionSlider widget sends Double variants (Slider_c uses double
    // internally), so we must handle Double in addition to Int/Float.
    int intVal = def->current;
    switch (def->type) {
    case SettingType::Bool:
        if (value.type == static_cast<uint32_t>(VariantType::Bool))
            intVal = value.as_bool ? 1 : 0;
        else if (value.type == static_cast<uint32_t>(VariantType::Int))
            intVal = value.as_int ? 1 : 0;
        else if (value.type == static_cast<uint32_t>(VariantType::Double))
            intVal = value.as_double != 0.0 ? 1 : 0;
        break;
    case SettingType::Int:
        if (value.type == static_cast<uint32_t>(VariantType::Int))
            intVal = value.as_int;
        else if (value.type == static_cast<uint32_t>(VariantType::Float))
            intVal = static_cast<int>(value.as_float);
        else if (value.type == static_cast<uint32_t>(VariantType::Double))
            intVal = static_cast<int>(value.as_double);
        break;
    }

    if (intVal != def->current) {
        def->current = intVal;
        WriteIniInt(def->name, intVal);
        Log("[settings] persisted %s = %d", def->name, intVal);
    }
}

// ── Public API ─────────────────────────────────────────────────────────

void SettingsInit() {
    ResolveIniPath();

    // Load persisted values from .ini (or use defaults).
    for (int i = 0; i < kSettingCount; ++i) {
        g_settings[i].current = ReadIniInt(
            g_settings[i].name, g_settings[i].default_val);
        Log("[settings] loaded %s = %d (default %d)",
            g_settings[i].name, g_settings[i].current, g_settings[i].default_val);
    }
}

void SettingsRegisterAll() {
    for (int i = 0; i < kSettingCount; ++i) {
        const SettingDef& s = g_settings[i];
        switch (s.type) {
        case SettingType::Bool:
            GameAPI::RegisterBool(s.name, s.current != 0);
            break;
        case SettingType::Int:
            GameAPI::RegisterInt(s.name, s.current);
            // Sliders require min/max in the DValue registry node,
            // otherwise the knob doesn't render and clicking crashes.
            SetDValueMinMax(s.name, s.min_val, s.max_val);
            break;
        }
    }
}

bool SettingsInstallHook() {
    // SetDValue was already resolved by GameAPI::Init(). Hook it.
    void* target = reinterpret_cast<void*>(GameAPI::SetDValue);
    if (!target) {
        Log("[settings] SetDValue not resolved — hook skipped");
        return false;
    }

    auto* bytes = static_cast<uint8_t*>(target);
    Log("[settings] SetDValue at %p, prologue: %02X %02X %02X %02X %02X",
        target, bytes[0], bytes[1], bytes[2], bytes[3], bytes[4]);

    void* trampoline = nullptr;
    if (!InstallHook(target, reinterpret_cast<void*>(&SetDValueDetour), &trampoline)) {
        Log("[settings] SetDValue hook failed — changes won't persist to .ini");
        return false;
    }

    g_origSetDValue = reinterpret_cast<GameAPI::FnSetDValue>(trampoline);

    // Also update the GameAPI function pointer so other code that calls
    // GameAPI::SetDValue goes through the trampoline (not the now-patched
    // address which would infinite-loop).
    GameAPI::SetDValue = g_origSetDValue;

    Log("[settings] SetDValue hook installed — settings will persist to .ini");
    return true;
}

}  // namespace aor
