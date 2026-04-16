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
#include <new>

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
    { "AOR_DebugLog", SettingType::Bool, 0,       0,       0,   1  },
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

// ── .ini path helper (standalone, no Log dependency) ───────────────────
//
// Builds the absolute path to AOReloaded.ini next to the exe. Used by
// IsDebugLogEnabled (called before logging is initialized) and by the
// normal settings path resolver.

static bool BuildIniPathNarrow(char* out, int outSize) {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    wchar_t* slash = nullptr;
    for (wchar_t* p = exePath; *p; ++p) {
        if (*p == L'\\' || *p == L'/') slash = p;
    }
    if (!slash) return false;
    *(slash + 1) = L'\0';

    wchar_t widePath[MAX_PATH];
    if (swprintf(widePath, MAX_PATH, L"%sAOReloaded.ini", exePath) < 0)
        return false;

    WideCharToMultiByte(CP_ACP, 0, widePath, -1, out, outSize, nullptr, nullptr);
    return true;
}

// ── Options panel XML injection ────────────────────────────────────────
//
// The AOReloaded tab in Root.xml defines the in-game settings UI. Rather
// than requiring users to manually edit the file, we inject it at runtime
// if it's missing. The XML block is a static string kept here alongside
// the settings table so new settings only need to be added in one place
// (the table for persistence, and the XML for UI).

static const char kAorXmlBlock[] =
    "\n"
    "  <ScrollView h_alignment=\"LEFT\" label=\"AOReloaded\" v_scrollbar_mode=\"auto\""
    " scroll_client=\"aor_scroll\" max_size=\"Point(16000,-1)\">\n"
    "    <ScrollViewChild name=\"aor_scroll\">\n"
    "      <View h_alignment=\"LEFT\" view_layout=\"vertical\""
    " max_size=\"Point(16000,-1)\" layout_borders=\"Rect(0,0,10,0)\">\n"
    "        <TextView value=\"AOReloaded v0.1.0\" layout_borders=\"Rect(0,0,0,5)\" />\n"
    "        <TextView value=\"Client mod framework. Settings are saved to AOReloaded.ini.\""
    " layout_borders=\"Rect(0,0,0,10)\" />\n"
    "\n"
    "        <TextView value=\"Camera\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <OptionCheckBox label=\"WoW-style camera (auto-recenter after LMB drag)\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_CamOn\"/>\n"
    "        <OptionSlider label=\"Camera recenter speed:\""
    " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"AOR_CYawSpd\""
    " value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n"
    "        <OptionCheckBox label=\"LMB+RMB mouse-run (hold both mouse buttons to run forward - requires WoW camera enabled)\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_MouseRun\"/>\n"
    "\n"
    "        <TextView value=\"Debug\" layout_borders=\"Rect(0,10,0,3)\" />\n"
    "        <OptionCheckBox label=\"Enable debug logging (AOReloaded.log, requires restart)\""
    " layout_borders=\"Rect(10,0,0,0)\" opt_type=\"variant\" opt_variable=\"AOR_DebugLog\"/>\n"
    "\n"
    "        <VLayoutSpacer/>\n"
    "      </View>\n"
    "    </ScrollViewChild>\n"
    "  </ScrollView>\n";

// Read the active GUI name from MainPrefs.xml. The value is stored as
// e.g. value="&quot;Default&quot;", so we need to strip the escaped quotes.
// Falls back to "Default" if anything goes wrong.
static bool GetActiveGuiName(char* out, int outSize) {
    char mainPrefsPath[MAX_PATH];
    if (!BuildIniPathNarrow(mainPrefsPath, MAX_PATH)) return false;

    // Replace "AOReloaded.ini" with "cd_image\gui\Default\MainPrefs.xml"
    // by finding the last backslash in the ini path.
    char* lastSlash = nullptr;
    for (char* p = mainPrefsPath; *p; ++p) {
        if (*p == '\\' || *p == '/') lastSlash = p;
    }
    if (!lastSlash) return false;
    *(lastSlash + 1) = '\0';

    char fullPath[MAX_PATH];
    _snprintf_s(fullPath, sizeof(fullPath), _TRUNCATE,
                "%scd_image\\gui\\Default\\MainPrefs.xml", mainPrefsPath);

    // Read the file and search for GUIName value.
    HANDLE hFile = CreateFileA(fullPath, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize > 256 * 1024) {
        CloseHandle(hFile);
        return false;
    }

    auto* buf = new(std::nothrow) char[fileSize + 1];
    if (!buf) { CloseHandle(hFile); return false; }

    DWORD bytesRead = 0;
    ReadFile(hFile, buf, fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);
    buf[bytesRead] = '\0';

    // Find: name="GUIName" value="&quot;SomeName&quot;"
    const char* pos = std::strstr(buf, "\"GUIName\"");
    if (!pos) { delete[] buf; return false; }

    // Find the value= attribute after GUIName.
    const char* valAttr = std::strstr(pos, "value=\"");
    if (!valAttr) { delete[] buf; return false; }
    valAttr += 7;  // skip past value="

    // The value is wrapped in &quot; entities: &quot;Default&quot;
    // Or it might just be a plain string. Handle both.
    const char* nameStart = valAttr;
    if (std::strncmp(nameStart, "&quot;", 6) == 0)
        nameStart += 6;  // skip &quot;

    // Find the end — either &quot; or "
    const char* nameEnd = std::strstr(nameStart, "&quot;");
    if (!nameEnd) nameEnd = std::strchr(nameStart, '"');
    if (!nameEnd || nameEnd == nameStart) { delete[] buf; return false; }

    int nameLen = static_cast<int>(nameEnd - nameStart);
    if (nameLen >= outSize) nameLen = outSize - 1;
    std::memcpy(out, nameStart, nameLen);
    out[nameLen] = '\0';

    delete[] buf;
    return true;
}

static bool BuildRootXmlPath(char* out, int outSize) {
    // Determine the active GUI name from MainPrefs.xml.
    char guiName[128] = "Default";
    GetActiveGuiName(guiName, sizeof(guiName));

    char basePath[MAX_PATH];
    if (!BuildIniPathNarrow(basePath, MAX_PATH)) return false;

    // Strip filename from ini path to get the client directory.
    char* lastSlash = nullptr;
    for (char* p = basePath; *p; ++p) {
        if (*p == '\\' || *p == '/') lastSlash = p;
    }
    if (!lastSlash) return false;
    *(lastSlash + 1) = '\0';

    _snprintf_s(out, outSize, _TRUNCATE,
                "%scd_image\\gui\\%s\\OptionPanel\\Root.xml", basePath, guiName);
    return true;
}

// ── Public API ─────────────────────────────────────────────────────────

bool IsDebugLogEnabled() {
    char iniPath[MAX_PATH];
    if (!BuildIniPathNarrow(iniPath, MAX_PATH)) return false;
    return GetPrivateProfileIntA(kIniSection, "AOR_DebugLog", 0, iniPath) != 0;
}

void PatchOptionsXml() {
    char xmlPath[MAX_PATH];
    if (!BuildRootXmlPath(xmlPath, MAX_PATH)) {
        Log("[settings] could not resolve Root.xml path");
        return;
    }
    Log("[settings] options panel XML: %s", xmlPath);

    // Read the entire file.
    HANDLE hFile = CreateFileA(xmlPath, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log("[settings] Root.xml not found at %s", xmlPath);
        return;
    }

    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE || fileSize > 1024 * 1024) {
        Log("[settings] Root.xml size invalid (%lu)", fileSize);
        CloseHandle(hFile);
        return;
    }

    // +1 for null terminator, + sizeof block for potential injection.
    auto* buf = new(std::nothrow) char[fileSize + sizeof(kAorXmlBlock) + 1];
    if (!buf) {
        CloseHandle(hFile);
        return;
    }

    DWORD bytesRead = 0;
    ReadFile(hFile, buf, fileSize, &bytesRead, nullptr);
    CloseHandle(hFile);
    buf[bytesRead] = '\0';

    // Check if AOReloaded tab already exists.
    if (std::strstr(buf, "label=\"AOReloaded\"")) {
        Log("[settings] Root.xml already has AOReloaded tab");
        delete[] buf;
        return;
    }

    // Find </root> and insert our block before it.
    char* endTag = std::strstr(buf, "</root>");
    if (!endTag) {
        Log("[settings] Root.xml missing </root> — cannot inject");
        delete[] buf;
        return;
    }

    // Build the new file content: [before </root>] + our block + </root>\n
    size_t prefixLen = static_cast<size_t>(endTag - buf);
    size_t blockLen = std::strlen(kAorXmlBlock);
    size_t suffixLen = std::strlen(endTag);  // "</root>" + any trailing whitespace

    auto* newBuf = new(std::nothrow) char[prefixLen + blockLen + suffixLen + 1];
    if (!newBuf) {
        delete[] buf;
        return;
    }

    std::memcpy(newBuf, buf, prefixLen);
    std::memcpy(newBuf + prefixLen, kAorXmlBlock, blockLen);
    std::memcpy(newBuf + prefixLen + blockLen, endTag, suffixLen);
    size_t totalLen = prefixLen + blockLen + suffixLen;
    newBuf[totalLen] = '\0';

    delete[] buf;

    // Write back.
    hFile = CreateFileA(xmlPath, GENERIC_WRITE, 0,
                        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        Log("[settings] failed to open Root.xml for writing: %lu", GetLastError());
        delete[] newBuf;
        return;
    }

    DWORD written = 0;
    WriteFile(hFile, newBuf, static_cast<DWORD>(totalLen), &written, nullptr);
    CloseHandle(hFile);
    delete[] newBuf;

    Log("[settings] injected AOReloaded tab into Root.xml (%u bytes written)", written);
}

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
