#pragma once
#include <windows.h>
#include <dwmapi.h>
#include <string_view>
#include <optional>
#include <vector>
#include <Providers/MenuContext.h>

namespace GlassCore::Settings
{
    /// Ветка реестра (…\GlassPlatform\Root\) для backdrop/border **root**-окон.
    /// Подключи Active и Inactive хранят параметры для активного и неактивного окна.
    inline constexpr std::wstring_view kRootRegistryPart{L"Root"};
    inline constexpr std::wstring_view kTrayRegistryPart{L"Tray"};

    // Структуры контекста
    struct WindowBackdropEffectContext
    {
        /// SWCA эффект (внутренний enum движка).
        DWORD effectType;
        DWORD enableDropShadow;
        /// ARGB; для Root SWCA фактический тинт задаётся из подключей Root\Active и Root\Inactive.
        DWORD gradientColor;
        /// RootBackdropMode (память/snapshot): 0 = off, 1 = DWM, 2 = SWCA.
        DWORD rootBackdropMode;
    };

    inline constexpr DWORD kRootBackdropModeOff = 0;
    inline constexpr DWORD kRootBackdropModeDwm = 1;
    inline constexpr DWORD kRootBackdropModeSwca = 2;

    /// Engine-значение EffectType для Mica (DWMSBT_MAINWINDOW).
    inline constexpr DWORD kRootEngineEffectMica = 7;

    /// ET 1–5 — SWCA-бэкdrop (SetWindowCompositionAttribute); ET 6–8 — DWM-бэкdrop.
    [[nodiscard]] inline constexpr bool IsRootSwcaEngineEffectType(DWORD effectType) noexcept
    {
        return effectType >= 1u && effectType <= 5u;
    }

    struct BorderContext
    {
        bool colorUseNone;
        bool colorUseDefault;
        DWORD color;               // ARGB
        DWM_WINDOW_CORNER_PREFERENCE cornerType;
    };

    struct FlyoutAnimationContext
    {
        bool enable;
        DWORD fadeOutTime;          // мс

        bool immediateInterupting;
        DWORD startRatio;            // 0-100 (%)
        DWORD popInTime;             // мс
        DWORD fadeInTime;            // мс
        DWORD popInStyle;            // 0-6
    };

    struct TooltipRenderingContext
    {
        COLORREF color;
        DWORD marginsType;
        MARGINS margins;
    };

    // Применение эффектов к окну
    void ApplyEffect(HWND hWnd, bool darkMode, const WindowBackdropEffectContext& backdropContext, const BorderContext& border, bool skipNcRendering = false);
    /// Единая политика root: читает Root из реестра, выбирает SWCA или DwmBlur.
    /// НЕ загружает/выгружает OpenGlass — управление OG отдельно в LogViewer UI.
    void ApplyRootWindowGlassEffect(HWND hWnd, bool darkMode);
    /// Текущая политика Root — ветка SWCA (`ApplyBackdropEffect`), а не `DwmEnableBlurBehindWindow`.
    [[nodiscard]] bool RootPolicyUsesSwcaBackdrop(bool darkMode);
    void ApplyBorderEffect(HWND hWnd, bool darkMode, const BorderContext& border);
    void ApplyBackdropEffect(HWND hWnd, bool darkMode, const WindowBackdropEffectContext& backdropContext, bool skipNcRendering = false, bool flyoutBackdrop = false);
    /// SWCA-prep для popup (DropDown/ComboLBox): полный сброс DWM/SWCA перед ApplyEffect (как OnRootDwmBackdropApply + SWCA root).
    void PreparePopupSwcaBackdropHwnd(HWND hWnd) noexcept;
    /// Popup всегда по SWCA-политике (ET 0–8 из реестра части): prep + ApplyBackdropEffect + ApplyBorderEffect.
    void ApplyPopupSwcaEffect(HWND hWnd, bool darkMode,
        const WindowBackdropEffectContext& backdropContext,
        const BorderContext& border,
        bool skipNcRendering = false);
    void DropEffect(std::wstring_view part, HWND hWnd);

    // Чтение настроек из реестра
    void QueryBackdropEffectContext(std::wstring_view part, bool darkMode, WindowBackdropEffectContext& context);
    void QueryBorderContext(std::wstring_view part, bool darkMode, BorderContext& context);
    /// Удобные обёртки: всегда `kRootRegistryPart`.
    /// visuallyActive — per-state ключи Root\Active / Root\Inactive через RootSwcaRegistryQuery (без animation/dropShadow).
    void QueryRootBackdropEffectContext(bool darkMode, bool visuallyActive, WindowBackdropEffectContext& context);
    void QueryRootBorderContext(bool darkMode, bool visuallyActive, BorderContext& context);
    void QueryTrayBackdropEffectContext(bool darkMode, WindowBackdropEffectContext& context);
    void QueryTrayBorderContext(bool darkMode, BorderContext& context);
    void QueryMenuCustomRenderingContext(bool darkMode, WndCompat::Menu::MenuCustomRendering& context);
    /// Удаление фона иконок для произвольной ветки (presence ColorTreatAsTransparent = enable).
    /// keyTree — подключи относительно корня BWM, например { L"Root", L"Control", L"Ribbon" }.
    void QueryIconBackgroundColorRemovalContext(std::vector<std::wstring_view> keyTree, WndCompat::Menu::IconBackgroundColorRemoval& context);
    void QueryMenuIconBackgroundColorRemovalContext(WndCompat::Menu::IconBackgroundColorRemoval& context);
    void QueryFlyoutAnimationContext(std::wstring_view part, FlyoutAnimationContext& context);
    void QueryTooltipRenderingContext(TooltipRenderingContext& context, bool darkMode);

    // Реестр Tooltip / DisabledList / высококонтраст (упрощённый порт BWM Api::IsPartDisabled для части Tooltip)
    [[nodiscard]] bool IsTooltipCustomizationDisabled() noexcept;

    // Специальные проверки
    bool IsComboBoxAnimation();
}