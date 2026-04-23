#include "hooks/gfx_debugger_hook.h"
#include "core/settings.h"
#include "core/logging.h"
#include "ao/game_api.h"
#include "ao/types.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace aor
{

    namespace
    {

        // m_nDebuggerMode: static int member of Debugger_t
        // Mangled: ?m_nDebuggerMode@Debugger_t@@2IA
        // randy31.dll data section
        constexpr uint32_t kDebuggerModeRVA = 0xb7500;

        // Debugger_t::Get() — static singleton accessor
        // Mangled: ?Get@Debugger_t@@SAPAV1@XZ
        // randy31.dll code section
        constexpr uint32_t kDebuggerGetRVA = 0x2cabf;

        using FnDebuggerGet = void *(__cdecl *)();

        static uint32_t *g_debuggerModePtr = nullptr;
        static FnDebuggerGet g_debuggerGet = nullptr;

        static void OnSettingChanged(const char *name, int newValue)
        {
            if (std::strcmp(name, "AOR_GfxDebug") == 0)
            {
                SetDebuggerMode(newValue);
            }
        }

    }

    bool InitDebuggerMode()
    {
        HMODULE mod = GetModuleHandleA("randy31.dll");
        if (!mod)
        {
            Log("[gfx_debugger] randy31.dll not loaded");
            return false;
        }

        auto base = reinterpret_cast<uint8_t *>(mod);

        g_debuggerModePtr = reinterpret_cast<uint32_t *>(base + kDebuggerModeRVA);
        g_debuggerGet = reinterpret_cast<FnDebuggerGet>(base + kDebuggerGetRVA);

        Log("[gfx_debugger] m_nDebuggerMode at %p, Debugger_t::Get at %p",
            g_debuggerModePtr, reinterpret_cast<void *>(g_debuggerGet));

        int currentMode = GetDebuggerMode();
        if (currentMode >= 0)
        {
            Log("[gfx_debugger] current m_nDebuggerMode = 0x%08X (%d)", currentMode, currentMode);
        }

        int dval = 0;
        AOVariant v{};
        if (GameAPI::GetVariant("AOR_GfxDebug", v) &&
            v.type == static_cast<uint32_t>(VariantType::Int))
        {
            dval = v.as_int;
        }

        if (dval != currentMode)
        {
            SetDebuggerMode(dval);
        }

        RegisterSettingCallback(OnSettingChanged);
        Log("[gfx_debugger] Graphics debugger mode hook ready");
        return true;
    }

    void SetDebuggerMode(int mode)
    {
        if (!g_debuggerModePtr)
            return;

        __try
        {
            *g_debuggerModePtr = static_cast<uint32_t>(mode);
            Log("[gfx_debugger] set m_nDebuggerMode = 0x%08X (%d)", mode, mode);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[gfx_debugger] AV writing m_nDebuggerMode at %p", g_debuggerModePtr);
        }
    }

    int GetDebuggerMode()
    {
        if (!g_debuggerModePtr)
            return -1;

        int value = -1;
        __try
        {
            value = static_cast<int>(*g_debuggerModePtr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("[gfx_debugger] AV reading m_nDebuggerMode at %p", g_debuggerModePtr);
        }
        return value;
    }

}
