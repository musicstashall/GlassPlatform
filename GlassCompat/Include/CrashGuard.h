#pragma once
#include <Windows.h>

namespace GlassCompat
{
    // Все процессы, куда загружен GlassCompat: UEF → опциональный минидамп (как BWM для инжектируемых).
    // Диалог «остановить платформу» — только при серии падений explorer.exe (см. CrashGuard.cpp).
    //
    // HKCU\Software\StanislavKa\BlackWindowManager\Crashes — основной ключ; при первой загрузке создаётся ключ и
    // при отсутствии — EnableMiniDump=1, плюс каталог %LOCALAPPDATA%\GlassPlatform\Crashes.
    // Чтение DWORD (EnableMiniDump, флаги VEH и т.д.): только …\Crashes и Glass\CrashGuard — не корень BWM
    // (коллизии имён). DumpFolder (REG_SZ) по-прежнему: Crashes → корень BWM → legacy.
    //   EnableMiniDump (DWORD, по умолчанию 1), MiniDumpWithFullMemory,
    //   DumpFolder (REG_EXPAND_SZ по умолчанию %LOCALAPPDATA%\GlassPlatform\Crashes — пишется при первой загрузке);
    //   иначе по очереди: %LOCALAPPDATA%\GlassPlatform\Crashes, %TEMP%\GlassPlatformCrashes.
    //   Имя файла: {exe}.{pid}.dmp (как WER LocalDumps / %LOCALAPPDATA%\CrashDumps).
    //   EnableVectoredFastFailGuard — VEH на 0xC0000409 / 0xC0000374;
    //   EnableVehStowedExceptions — доп. VEH для не-explorer или дубль условий в explorer;
    //   DisableExplorerVehFatalDumps = 1 — отключить встроенный VEH (AV/fast-fail/heap/stowed) в explorer.exe;
    //   Не-explorer VEH: first-chance AV → дамп только если ExceptionAddress в Glass*/WndCompat
    //   (иначе AutoIt/JIT PAGE_NOACCESS заливал MiniDump и подвешивал UI).
    //   GlassCrashSeriesCount / GlassCrashSeriesLastTime — серия падений explorer (диалог остановки платформы).
    [[nodiscard]] bool IsProcessCrashGuardInstalled() noexcept;

    void InstallProcessCrashGuard() noexcept;
    // Повторно вызвать SetUnhandledExceptionFilter(наш фильтр) после загрузки GlassCore/WndCompat —
    // иначе чужой рантайм мог перезаписать UEF, заданный в DllMain.
    void ReassertProcessCrashGuard() noexcept;
    void UninstallProcessCrashGuard() noexcept;
}
