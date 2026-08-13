#pragma once
#include <unknwn.h>
#include <objidl.h>

struct IXamlDiagnostics;

// IObjectWithSite: точка входа XAML Diagnostics (SetSite получает IXamlDiagnostics).
struct XamlTapSite final : IObjectWithSite
{
    XamlTapSite() noexcept;
    ~XamlTapSite() noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) noexcept override;
    ULONG STDMETHODCALLTYPE AddRef() noexcept override;
    ULONG STDMETHODCALLTYPE Release() noexcept override;

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* pUnkSite) noexcept override;
    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** ppvSite) noexcept override;

    static HRESULT ShutdownVisualTreeWatcher() noexcept;
    // AdviseVisualTreeChange: вызывается из worker-потока (см. XamlTapSite.cpp). WM_GLASS_EXPLORER_XAML_ADVISE в WndProc — legacy/очистка очереди.
    static void CompleteDeferredAdvise() noexcept;
    // Убрать из очереди WM_ADVISE и освободить vts/watcher, если Advise ещё не вызывали.
    static void CancelDeferredAdvisePending() noexcept;
    // После Stop/Unadvise и FreeLibrary UDK: повторная инвалидация окон проводника (острова + Cabinet).
    static void RedrawSurfacesAfterFullStop() noexcept;

    // Канон restore+redraw: WinUI finisher (live OFF = shutdown до Unadvise); Win32 — только stop.
    static void PaintPumpBrief() noexcept;
    static void FinishWinUiHostBarChromeRestore() noexcept;
    static void RedrawExplorerHostBarSurfacesAfterRestore() noexcept;
    static void AfterExplorerChromeApplied(IXamlDiagnostics* diag) noexcept;
    /// ABI: live OFF Win32 — CHROME_REDRAW (NC); полный GDI — на stop.
    static void PostLiveDisableChromeRedraw() noexcept;
    static void RedrawExplorerShellCaptionAfterLiveOff() noexcept;

    // Live NoBackdrop OFF / ON.
    static void RestoreExplorerHostBarChrome(IXamlDiagnostics* diag) noexcept;
    static void ApplyExplorerHostBarChrome(IXamlDiagnostics* diag) noexcept;

    /// Coalesce повторного apply в том же ON-wave.
    static void NoteLiveHostBarApplyCompleted() noexcept;
    [[nodiscard]] static bool ShouldCoalesceLiveHostBarApply() noexcept;

    // Live Root\NoBackdrop=1: делегирует в RestoreExplorerHostBarChrome.
    static void OnRootBackdropDisabled() noexcept;
    // Live ON: FullReplay на UI-очереди острова (DispatcherQueue sync; VTC — fallback).
    static void RequestApplyHostBarChromeOnVtcThread() noexcept;
    static void TryConsumePendingApplyHostBarChrome(IXamlDiagnostics* diag) noexcept;
    static void RequestRestoreHostBarChromeOnVtcThread() noexcept;
    static void TryConsumePendingRestoreHostBarChrome(IXamlDiagnostics* diag) noexcept;
    static void RequestRepatchTabElementsOnVtcThread() noexcept;
    static void TryConsumePendingRepatchTabElements(IXamlDiagnostics* diag) noexcept;
    // Live ON (legacy entry): делегирует в RequestApplyHostBarChromeOnVtcThread.
    static void OnRootBackdropEnabled() noexcept;
    // Обработчик WM_TIMER: переприменить Transparent на tab-элементы.
    static void OnRepatchTimer() noexcept;
    // Одноразовый WM_TIMER после Advise: инвалидация Shell_TrayWnd (без IXamlDiagnostics::HitTest).
    static void OnTrayRedrawAfterAdviseTimer() noexcept;

    // Активная сессия XAML Diagnostics (если есть): +1 Ref; вызывающий обязан Release.
    [[nodiscard]] static IXamlDiagnostics* AddRefXamlDiagnosticsIfAvailable() noexcept;
private:
    void ReleaseSiteBinding() noexcept;

    ULONG m_ref{ 1 };
    IUnknown* m_site{ nullptr };
};
