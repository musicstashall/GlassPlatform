#pragma once
#include <windows.h>
#include <uxtheme.h>
#include <cstdint>
#include <string_view>

namespace Shared::ThemeHelper
{
    // Универсальный сэмпл цвета пикселя из битмапа визуального стиля (GetThemeBitmap) по центру.
    enum class ThemePixelSampleStrategy : std::uint8_t
    {
        Center = 0,
    };

    struct ThemeImageSampleRequest
    {
        PCWSTR          pszlThemeClass;      // e.g. L"Tab"
        int             iPartId   = 0;
        int             iStateId  = 0;
        const int*      pBitmapPropIds = nullptr; // TMT_DIBDATA, TMT_HBITMAP, …
        int             bitmapPropIdCount = 0;
        ULONG           getThemeBitmapFlags = 1;   // GBF_DIRECT
        ThemePixelSampleStrategy strategy = ThemePixelSampleStrategy::Center;
        bool            useColorFallback = false;   // GetThemeColor если битмапы не дались
        int             colorFallbackPropId = 3802; // TMT_FILLCOLOR (vssym32)
    };

    // Ядро: OpenThemeData → GetThemeBitmap(цепочка) → пиксель по стратегии; опционально GetThemeColor.
    [[nodiscard]] bool TrySampleThemeImagePixelColor(HWND themeAnchorWnd, const ThemeImageSampleRequest& request, COLORREF& outRgb) noexcept;

    // tis1Based: TIS_NORMAL(1), TIS_DISABLED(4), TIS_FOCUSED(5) — DIB+медиана как у offscreen-маски.
    [[nodiscard]] bool TryGetTabChromaKeyFromThemeForState(HWND themeAnchorWnd, int tis1Based, COLORREF& outRgb) noexcept;

    // Сценарий: заливка TIS_NORMAL; то же, что ForState(…, TIS_NORMAL, …).
    [[nodiscard]] bool TryGetTabInactiveChromaKeyFromTheme(HWND themeAnchorWnd, COLORREF& outRgb) noexcept;

    // Получить имя класса темы (например, "Menu", "DarkMode::Menu") — uxtheme ordinal 74
    HRESULT GetThemeClass(HTHEME hTheme, LPWSTR className, int cch);

    // Получить цвет из темы по части/состоянию/свойству
    HRESULT GetThemeColorFromClass(HTHEME hTheme, int iPartId, int iStateId, int iPropId, COLORREF& color);

    // Проверить, совпадает ли color с TMT_FILLCOLOR темы classname (ExplorerNavPane, ProperTree и т.д.)
    bool IsThemeColor(COLORREF color, const wchar_t* classname) noexcept;

    // Неофициальные экспорты uxtheme.dll
    [[nodiscard]] bool ShouldAppsUseDarkMode() noexcept;       // ordinal 132
    [[nodiscard]] bool ShouldSystemUseDarkMode() noexcept;     // ordinal 138
    [[nodiscard]] bool IsDarkModeAllowedForApp() noexcept;     // ordinal 139

    // uxtheme ordinal #65: SetSystemVisualStyle — смена системного визуального стиля (.msstyles, схемы цвета/размера).
    // flags: 0 — стиль без шрифтов sysmetrics; 65 — со шрифтами темы (как BWM ThemeUI).
    [[nodiscard]] HRESULT SetSystemVisualStyle(
        LPCWSTR pszFilename, LPCWSTR pszColor, LPCWSTR pszSize, int flags) noexcept;

    [[nodiscard]] bool IsHighContrastActive() noexcept;

    // Корректное определение тёмной темы для приложения.
    // Использует ShouldAppsUseDarkMode() && IsDarkModeAllowedForApp()
    // (uxtheme ordinals 132 + 139).
    [[nodiscard]] bool IsSystemDarkMode() noexcept;

    /// Визуальный детект тёмной темы для логики Glass (Root SWCA, flyout, tooltip).
    /// Win11 24H2: COLOR_WINDOW может оставаться белым при AppsUseLightTheme=0 — тогда fallback на
    /// IsSystemDarkMode() и реестр Personalize. Не использовать для имён BWM-ключей LightMode_/DarkMode_.
    [[nodiscard]] bool ResolveEffectiveDarkMode() noexcept;

    /// BWM: выбор LightMode_* vs DarkMode_* в реестре по COLOR_WINDOW (паритет RootEvents / DropDownHooks).
    /// Не путать с ResolveEffectiveDarkMode() / IsSystemDarkMode() — для имён ключей реестра не используются.
    [[nodiscard]] inline bool IsRegistryDarkModeFromColorWindow() noexcept
    {
        return GetSysColor(COLOR_WINDOW) < 0xFFFFFF;
    }

    /// LightMode_*/DarkMode_* для Tray\Xaml* и смежных (TaskbarCompat / UICompat / policy map).
    /// uxtheme #132 (ShouldAppsUseDarkMode), fallback Personalize\\AppsUseLightTheme==0.
    /// Не #139: IsDarkModeAllowedForApp в host/AppContainer часто false при тёмной теме —
    /// иначе в TrayXamlPolicy уходит LightMode_ градиент (NC/QS/Пуск/Поиск).
    /// Не COLOR_WINDOW.
    [[nodiscard]] bool IsTrayXamlDarkMode() noexcept;

    /// Реестр: HKCU\Software\StanislavKa\BlackWindowManager, Win32DarkModeKickDisable=1 — отключить пинок uxtheme.
    [[nodiscard]] bool IsWin32DarkModeKickEnabled() noexcept;

    /// Process-level пinок uxtheme (ordinals 135/104/136): SetPreferredAppMode + refresh. Идемпотентно.
    void TryEnableProcessDarkMode() noexcept;

    /// Per-window пinок uxtheme (ordinal 133): AllowDarkModeForWindow.
    void EnableWindowDarkModeUxTheme(HWND hwnd, bool dark) noexcept;

    /// Immersive color set (BWM ThemeHelper::GetThemeColorizationColor) — ARGB 0xAARRGGBB.
    [[nodiscard]] DWORD GetThemeColorizationColor(std::wstring_view type) noexcept;
}
