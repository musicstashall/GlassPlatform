// DllMain.cpp (минимальный)
#include "CompatPch.h"
#include "GlassCompat.h"
#include <CrashGuard.h>
#include <Windows.h>

#if defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>
#endif

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
        break;
    case DLL_PROCESS_DETACH:
        // Не Stop() лог-клиента здесь: join воркера под loader lock запрещён.
        // GUI / MinimalUnloadWatcher / short-lived Main стопают клиент сами.
        if (GlassCompat::IsProcessCrashGuardInstalled())
            GlassCompat::UninstallProcessCrashGuard();
        break;
    }
    return TRUE;
}
