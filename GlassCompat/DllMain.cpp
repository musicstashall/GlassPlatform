// DllMain.cpp (минимальный)
#include "CompatPch.h"
#include "GlassCompat.h"
#include <CrashGuard.h>
#include <Windows.h>
#include <GlassUlTrace.h>

#if defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>
#endif

// #region agent log
// GLASS-ULTRACE (R5): сколько прожил модуль в процессе. Нужно для short-lived процессов
// (taskhostw живёт десятки мс): по alive_ms видно, успел ли наш watcher вообще начать работу.
static ULONGLONG g_ultraceAttachTick = 0;
// #endregion

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        // CrashGuard не ставим здесь: INCONTEXT грузит DLL и в BlockList/Forbidden
        // (taskhostw и др.), где нужна только выгрузка path=2/3. UEF/VEH — в
        // long-lived хосте (/start) и в GlassCompat::Initialize для Normal.
#if defined(_DEBUG)
        // Debug CRT по умолчанию показывает модальный abort()/assert-диалог.
        // При reboot это блокирует выключение в conhost/msedgewebview2.
        // Сообщение остаётся в OutputDebugString; WER не трогаем.
        (void)_set_abort_behavior(0, _WRITE_ABORT_MSG);
        (void)_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
        (void)_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
        (void)_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif
        g_ultraceAttachTick = GetTickCount64();
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H82",
            "DllMain.cpp:DllMain",
            "dll_attach",
            "\"base\":\"%p\",\"reason\":\"attach\",\"rc\":%u",
            static_cast<void*>(hModule),
            GlassUlTrace::QueryLoadCount(hModule));
        // #endregion
        break;
    case DLL_PROCESS_DETACH:
        // Не Stop() лог-клиента здесь: join воркера под loader lock запрещён.
        // GUI / MinimalUnloadWatcher / short-lived Main стопают клиент сами.
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H82",
            "DllMain.cpp:DllMain",
            "dll_detach",
            "\"base\":\"%p\",\"reason\":\"detach\",\"process_exit\":%d,\"alive_ms\":%llu",
            static_cast<void*>(hModule),
            reserved ? 1 : 0,
            g_ultraceAttachTick ? (GetTickCount64() - g_ultraceAttachTick) : 0ull);
        // #endregion
        if (GlassCompat::IsProcessCrashGuardInstalled())
            GlassCompat::UninstallProcessCrashGuard();
        break;
    }
    return TRUE;
}
