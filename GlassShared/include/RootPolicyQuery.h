#pragma once
#include <windows.h>

namespace Shared::RootPolicyQuery
{
    /// HKCU\...\Root\NoBackdrop: отсутствует или не 1 → backdrop разрешён; 1 → подавлен.
    [[nodiscard]] bool UsesSwcaBackdrop() noexcept;

    /// RootBackdropMode: 0=off (NoBackdrop=1), 1=DWM (OG pipeline), 2=SWCA.
    /// Чтение: NoBackdrop → snapshot mapping (seed) → кэш → OG-fallback.
    [[nodiscard]] DWORD ReadRootBackdropMode() noexcept;

    [[nodiscard]] bool IsRootGlassRenderingEnabled() noexcept;

    [[nodiscard]] DWORD ReadNoBackdropFromRegistry() noexcept;

    /// Writer: только хост / PlatformPresenceMonitor.
    void SetRootBackdropMode(DWORD mode) noexcept;

    void RefreshRootBackdropPolicyCache() noexcept;
}
