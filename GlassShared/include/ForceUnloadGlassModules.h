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
    };

    /// Unlocker-стиль: CreateRemoteThread(FreeLibrary) для Glass*.dll в целевом PID (same-bitness).
    /// ExplorerCompat / TaskbarCompat: IXDE LoadLibrary-pin — не Remote FreeLibrary.
    [[nodiscard]] ForceUnloadResult ForceUnload(DWORD pid) noexcept;

    [[nodiscard]] bool IsWow64ProcessPid(DWORD pid) noexcept;
}
