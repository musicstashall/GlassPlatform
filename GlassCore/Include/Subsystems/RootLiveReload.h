#pragma once

#include <Windows.h>
#include <GlassMessages.h>

namespace GlassCore::RootLiveReload
{
    /// scope: 0 = все g_rootWindows, 1 = только SnapshotGlassAppliedRootHwnds.
    void PostLiveRootBackdropReapply(DWORD scope) noexcept;

    /// Перечитать DrawTheme alpha из реестра и soft-invalidate chrome по g_rootWindows.
    void InvalidateThemedChromeOnTrackedRoots() noexcept;

    /// Soft-invalidate TextGlow-клиентов (Moderate/Global) по g_rootWindows.
    void InvalidateTextGlowClientsOnTrackedRoots() noexcept;

    /// Soft-invalidate Edit/RichEdit (+ WinForms) по g_rootWindows.
    void InvalidateTextInkClientsOnTrackedRoots() noexcept;

    /// Live Root composition: WM_DWMCOLORIZATIONCHANGED (wParam=0, lParam=scope) по applied roots.
    void PostLiveRootColorizationToAppliedRoots(LPARAM liveRootScope = WndCompat::kGlassLiveRootScopeInfer) noexcept;
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_PostLiveRootBackdropReapply(DWORD scope) noexcept;
extern "C" __declspec(dllexport) void WINAPI GlassCore_InvalidateThemedChromeOnTrackedRoots() noexcept;
extern "C" __declspec(dllexport) void WINAPI GlassCore_InvalidateTextGlowClientsOnTrackedRoots() noexcept;
extern "C" __declspec(dllexport) void WINAPI GlassCore_InvalidateTextInkClientsOnTrackedRoots() noexcept;
extern "C" __declspec(dllexport) void WINAPI GlassCore_PostLiveRootColorizationToAppliedRoots(LPARAM liveRootScope) noexcept;
