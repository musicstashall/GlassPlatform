#pragma once

#include <GlassControlKind.h>
#include <Windows.h>
#include <cstdint>

namespace GlassCore::ThemePaint
{
    // Альфа DrawThemeBackground → offscreen + GdiAlphaBlend (Tab, Combo, push-Button).
    // Реестр: HKCU\Software\StanislavKa\BlackWindowManager
    //   Root\Active\GlassDrawThemeConstantAlpha и Root\Inactive\GlassDrawThemeConstantAlpha
    //   (REG_DWORD 0…255). При отсутствии ключа — kDefaultDrawThemeConstantAlpha (255);
    //   fallthrough: Inactive/Active subkey → Root\ (RootSwcaRegistryQuery).
    // Ветка Active/Inactive: снимок WM_NCACTIVATE по root в этой DLL + кэш GetForegroundWindow (Finalize при NCACTIVATE,
    // старт движка). На пути GetEffective: сначала карта root→ncActive, иначе сравнение GA_ROOT с кэшем GFW.
    constexpr std::uint8_t kDefaultDrawThemeConstantAlpha = 255;

    // Порог хрома для SysTab: DrawTheme + RGB ± threshold на канал (по умолчанию 5).
    // Реестр GlassTabChromaThreshold, 0…kMax; ветки Active/Inactive как у GlassDrawThemeConstantAlpha.
    constexpr std::uint8_t kDefaultTabChromaThreshold     = 5u;
    constexpr std::uint8_t kMaxTabChromaThreshold         = 64u;

    [[nodiscard]] std::uint8_t GetDrawThemeConstantAlpha() noexcept;
    [[nodiscard]] std::uint8_t GetDrawThemeConstantAlphaInactive() noexcept;

    void ClearThemePaintPreviewAlpha() noexcept;

    // GlassSettings: превью для Button / ComboBox / TabControl — одно значение на обе ветки (совместимость).
    void SetThemePaintPreviewForGlassSettingsUi(std::uint8_t alpha) noexcept;

    // GlassSettings: отдельные черновики Active / Inactive (пара в RAM процесса настроек).
    void SetThemePaintPreviewPairForGlassSettingsUi(std::uint8_t activeAlpha, std::uint8_t inactiveAlpha) noexcept;

    // Эффективная альфа: превью GlassSettings для chrome-видов при необходимости; иначе из реестра по ветке root (active/inactive).
    [[nodiscard]] std::uint8_t GetEffectiveDrawThemeConstantAlphaForSession(
        GlassShared::GlassControlKind kind,
        HWND hWndChrome) noexcept;

    // Порог хрома для TabControl (GDI, не превью); иначе kDefault.
    [[nodiscard]] std::uint8_t GetEffectiveTabChromaThresholdForSession(
        GlassShared::GlassControlKind kind,
        HWND hWndChrome) noexcept;

    // Снимок GetForegroundWindow() в кэш GlassCore.dll (для ThemePaint; не путать с RootNcActiveCache в других модулях).
    void RefreshCachedForegroundWindow() noexcept;
    void ClearCachedForegroundWindow() noexcept;

    // Точечная инвалидация зарегистрированных chrome-HWND (Button, Combo, Tab) для chrome-root bucket — без EnumChild, только карта Register.
    // Ключ bucket: ближайший WinForms_Window-предок или GA_ROOT (см. RegisterThemedChromeClient).
    void SoftInvalidateThemedChromeClientsForRoot(HWND hRoot) noexcept;

    // WndCompat: при attach / detach кнопки, комбо, SysTab (после GlassMarkWindow / до GlassUnmarkWindow).
    void RegisterThemedChromeClient(HWND hControl) noexcept;
    void UnregisterThemedChromeClient(HWND hControl) noexcept;

    // Перенос hCtrl в bucket по актуальному Resolve (без catch-up).
    void RebindThemedChromeClientBucket(HWND hControl) noexcept;

    // После GlassMarkWindow(WinForms_Window) на hMarked: rebind всех зарегистрированных потомков.
    void RebindThemedChromeClientsAfterWinFormsWindowMarked(HWND hMarkedWinFormsWindow) noexcept;

    // После Def*Proc на WM_NCACTIVATE: записать ncActive для root, обновить кэш GFW, точечно инвалидировать chrome.
    void FinalizeRootActivationVisualUpdate(HWND hRoot, bool ncActivateFromWmNcActivate) noexcept;

    // Обработчик WM_GLASS_DEFER_THEMED_CHROME_INVALIDATE на gaRoot (после return из WM_NCACTIVATE).
    void RunDeferredThemedChromeSoftInvalidate(HWND hRoot) noexcept;

    void RemoveThemePaintRootNcState(HWND hRoot) noexcept;
    void ClearAllThemePaintRootNcState() noexcept;

    void LoadFromRegistry() noexcept;

    void ReloadThemePaintSettingsFromRegistry() noexcept;

    void InvalidateTopLevelWindowsInCurrentProcess() noexcept;

    void OnUserThemePaintSettingsChanged() noexcept;
} // namespace GlassCore::ThemePaint
