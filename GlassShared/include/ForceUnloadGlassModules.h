#pragma once

#include <Windows.h>

namespace Shared::ForceUnloadGlassModules
{
    enum class ForceUnloadResult : int
    {
        Failed = 0,
        Ok = 1,
        /// GS x64 / иной native x64: цель WOW64 — CreateRemoteThread(FreeLibrary) здесь невозможен.
        NeedsX86Agent = 2,
        BlockedHost = 3,
        /// В целевом PID идёт shutdown (фаза 1..6): слепой FreeLibrary снял бы
        /// образ под выполняющимся потоком (дамп SEH 17.11.2026). Повторить,
        /// когда shutdown завершится (фаза CompatDone) или не начнётся.
        ShutdownInProgress = 4,
    };

    /// Unlocker-стиль: CreateRemoteThread(FreeLibrary) для Glass*.dll в целевом PID (same-bitness).
    /// ExplorerCompat / TaskbarCompat: IXDE LoadLibrary-pin — не Remote FreeLibrary.
    [[nodiscard]] ForceUnloadResult ForceUnload(DWORD pid) noexcept;

    [[nodiscard]] bool IsWow64ProcessPid(DWORD pid) noexcept;
}
