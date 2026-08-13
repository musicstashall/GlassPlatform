#pragma once

#include <Windows.h>

namespace GlassCore::Root
{
    /// Градиент SWCA для root-окон: из подключей Root\Active и Root\Inactive.
    [[nodiscard]] DWORD ReadRootSwcaGradientArgb(bool visuallyActive) noexcept;

    /// Синхронизация OpenGlass-параметров DWM (ColorizationColorOverride/Inactive, GlassOpacity/Inactive).
    void SyncOpenGlassParameters() noexcept;

    /// Текущий InjectionMode == Full (из g_injectionMode в RootEvents.cpp).
    [[nodiscard]] bool IsFullInjectionMode() noexcept;

    void RegisterTrackedRootsBackdropApplier(void (WINAPI* fn)() noexcept) noexcept;

    /// WndCompat::Root_IsAttached — для gate per-root WM_DWMCOLORIZATION (без линка на WndCompat.dll).
    void RegisterRootIsAttachedQuery(bool (WINAPI* fn)(HWND) noexcept) noexcept;

    void OnRootSwcaBackdropActiveApply(HWND hRoot);
    void OnRootSwcaBackdropInactiveApply(HWND hRoot);

    /// Лёгкий SWCA restore после orig(CAPTION_COLOR) в explorer (mode=2, ET 6–8): accent tint + border.
    void ApplyRootSwcaCaptionColorTransition(HWND hRoot, bool active) noexcept;

    /// Пере-применение DWM border при WM_NCACTIVATE (Win11+ root с Glass backdrop).
    void ApplyRootBorderOnNcActivateChange(HWND hRoot, bool isActive) noexcept;

    void ApplyRootBackdropPolicyTransitionForHwnd(HWND hRoot) noexcept;

    /// Сброс кэша ShouldSkipReapplySweep при toggle NoBackdrop (OFF→ON).
    void ResetReapplyFingerprint() noexcept;

    /// После того как Taskbar.dll применила WCA_ACCENT_POLICY TRANSPARENT_GRADIENT (gradient=0) к трею,
    /// повторяем наш SWCA Tray (гонка композита; вызывается из TaskbarHooks, без WndCompat).
    void ReapplyTrayRootSwcaAfterTaskbarAccent(HWND trayHwnd) noexcept;

    /// Одноразовая метка для DebugLog в ApplyRootSwcaBackdrop (NCACTIVATE / REAPPLY / SHOWWINDOW / ATTACH).
    void SetNextRootSwcaApplyReason(const wchar_t* reason) noexcept;

    void ClearRootSwcaApplyCache() noexcept;
    void RemoveRootSwcaApplyCacheEntry(HWND hRoot) noexcept;

    /// CtrlNotifySink, DUIViewWndClassName и т.п. — без SWCA на Root_Attach / WM_SHOWWINDOW.
    [[nodiscard]] bool IsExplorerAuxiliaryRootHwnd(HWND hRoot) noexcept;

    /// Gate для WM_SHOWWINDOW → OnRootSwcaBackdropActiveApply (shell/tray/attached, не auxiliary).
    [[nodiscard]] bool ShouldApplyRootSwcaOnShowWindow(HWND hRoot) noexcept;

    /// OFF/STOP Win11 explorer: ET=7 (Mica) + top-margin = высота шапки.
    void RestoreWin11ExplorerStockFrame(HWND hRoot) noexcept;

    /// Sync CHROME по всем Cabinet/Explore (WndCompat export).
    void TriggerExplorerShellChromeRedrawAll() noexcept;

    /// Sync полный RedrawWindow root+children по всем Cabinet/Explore (WndCompat export).
    void TriggerExplorerShellRootRedrawAll() noexcept;
}
