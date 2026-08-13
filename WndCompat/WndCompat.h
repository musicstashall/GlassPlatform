#pragma once
#include <windows.h>
#include <InjectionMode.h>
#include <cstdint>

namespace WndCompat
{
    // Инициализация подсистемы (внутри GUI‑потока GlassCompat)
    void Initialize(Shared::InjectionMode mode);

    // Завершение (снятие хуков, остановка UiDispatcher)
    void Shutdown();
    /// Host UI завершает процесс: без каскада *_DetachAll, короткое ожидание.
    void ShutdownHostExit();

    // Служебные статусы
    bool IsShuttingDown();
    bool IsShutdownComplete();
    bool IsInitialized();
    HWND GetControlWindow();
    void EnsureControlWindow();
    /// HWND_BROADCAST WM_DWMCOLORIZATIONCHANGED → очередь control window (один Reapply на coalesced wave).
    void PostDwmColorizationDispatch(WPARAM colorizationWave) noexcept;
    // Ожидание завершения (опционально)
    void WaitForShutdownComplete(DWORD timeoutMs = 2000);

    // После WM_NCACTIVATE на root: записать ncActive в GlassCore, кэш GFW, точечная инвалидация DTB-chrome.
    void FinalizeGlassCoreRootActivationVisualUpdate(HWND hRoot, BOOL ncActivateFromWmNcActivate) noexcept;
    void RemoveGlassCoreThemePaintRootNcState(HWND hRoot) noexcept;

    void RemoveTextGlowClientsForRootWithGlassCore(HWND hRoot) noexcept;

    void RemoveTextInkClientsForRootWithGlassCore(HWND hRoot) noexcept;

    // ThemePaint: карта chrome-HWND per root (GlassCore), вызывать в паре с GlassMark / GlassUnmark.
    void RegisterThemedChromeClientWithGlassCore(HWND hControl) noexcept;
    void UnregisterThemedChromeClientWithGlassCore(HWND hControl) noexcept;
    void RebindThemedChromeClientsAfterWinFormsWindowMarkedWithGlassCore(HWND hMarkedWinFormsWindow) noexcept;
    void ScheduleThemedChromeRemigrateAfterWinFormsWindowMarked(HWND hMarkedWinFormsWindow) noexcept;
    void RunDeferredThemedChromeSoftInvalidateWithGlassCore(HWND hRoot) noexcept;

    // TextGlow: реестр glow-клиентов (GlassCore TextGlowClientRegistry).
    void TryRegisterTextGlowClientWithGlassCore(HWND hControl) noexcept;
    void RegisterTextGlowPopupClientWithGlassCore(HWND hControl, std::uint8_t category) noexcept;
    void UnregisterTextGlowClientWithGlassCore(HWND hControl) noexcept;
    void TryRegisterMenuBarTextGlowOwnerWithGlassCore(HWND hOwner) noexcept;
    void RunDeferredTextGlowSoftInvalidateWithGlassCore(HWND hRoot) noexcept;
    void RunDeferredTextInkSoftInvalidateWithGlassCore(HWND hRoot) noexcept;
}
