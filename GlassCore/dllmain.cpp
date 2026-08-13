#include "CorePch.h"
#include "Core\Engine.h"
#include <windows.h>

namespace
{
    // UI-хосты вызывают GlassCore_HostUiEnsureEngine сразу после LoadLibrary.
    // Тяжёлый Initialize в DllMain под loader lock в WinUI-процессе (особенно x86)
    // даёт process-killing AV до возврата из LoadLibrary — SEH в DllMain не спасает.
    // Inject/mmc/HWiNFO: по-прежнему init в DllMain (H64).
    [[nodiscard]] bool IsUiHostProcess() noexcept
    {
        wchar_t path[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return false;

        const wchar_t* base = path;
        for (const wchar_t* p = path; *p; ++p)
        {
            if (*p == L'\\' || *p == L'/')
                base = p + 1;
        }

        return ::_wcsicmp(base, L"GlassSettings.exe") == 0
            || ::_wcsicmp(base, L"LogViewer.exe") == 0;
    }
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        // H64: init в DllMain под SEH (ранние хуки для mmc). HWiNFO AV → abort, LoadLibrary OK.
        // Не retry после g_initFailed. g_initialized только в конце успешного Initialize.
        DisableThreadLibraryCalls(hModule);
        if (!IsUiHostProcess())
            (void)GlassCore::Engine::InitializeSealed(hModule);
        break;
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
