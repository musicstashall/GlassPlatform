#pragma once

// Настройки XAML-пути панели задач: HKCU\...\GlassPlatform\Tray\Xaml

#include <Windows.h>
#include <Helpers/RealUserReg.h>
#include <NcSystemCornerRadiusMap.h>

namespace Shared::TrayXamlPaint
{
    // Относительный путь под GlassPlatform.
    inline constexpr const wchar_t* kXamlSubKey = L"Tray\\Xaml";
    // Смежные окна (per-surface): fallback на Tray\Xaml при отсутствии value.
    inline constexpr const wchar_t* kXamlPopupSubKey = L"Tray\\Xaml\\Popup";
    inline constexpr const wchar_t* kXamlPreviewSubKey = L"Tray\\Xaml\\Preview";
    inline constexpr const wchar_t* kXamlMenuSubKey = L"Tray\\Xaml\\Menu";
    inline constexpr const wchar_t* kXamlTooltipSubKey = L"Tray\\Xaml\\Tooltip";

    inline constexpr const wchar_t* kRegPath =
        L"Software\\StanislavKa\\GlassPlatform\\Tray\\Xaml";
    inline constexpr const wchar_t* kRegPathPopup =
        L"Software\\StanislavKa\\GlassPlatform\\Tray\\Xaml\\Popup";
    inline constexpr const wchar_t* kRegPathPreview =
        L"Software\\StanislavKa\\GlassPlatform\\Tray\\Xaml\\Preview";
    inline constexpr const wchar_t* kRegPathMenu =
        L"Software\\StanislavKa\\GlassPlatform\\Tray\\Xaml\\Menu";
    inline constexpr const wchar_t* kRegPathTooltip =
        L"Software\\StanislavKa\\GlassPlatform\\Tray\\Xaml\\Tooltip";

    inline constexpr const wchar_t* kEffectTypeValue = L"EffectType";
    inline constexpr const wchar_t* kSeparateZonesValue = L"SeparateZones";
    inline constexpr const wchar_t* kCornerTypeValue = L"CornerType";
    // 1 = layout 0–4 (MaxRound=4); 2 = 0–5 intro; 3 = remap stray 4→MaxRound.
    inline constexpr const wchar_t* kCornerTypeSchemaValue = L"CornerTypeSchema";
    // Системный CornerRadius CoreWindow NC (centi-DIP = R*100), пишет UICompat.
    inline constexpr const wchar_t* kNcSystemCornerRadiusValue = L"NcSystemCornerRadius";
    inline constexpr const wchar_t* kLightGradient = L"LightMode_GradientColor";
    inline constexpr const wchar_t* kDarkGradient = L"DarkMode_GradientColor";
    inline constexpr const wchar_t* kLightBorder = L"LightMode_BorderColor";
    inline constexpr const wchar_t* kDarkBorder = L"DarkMode_BorderColor";
    inline constexpr const wchar_t* kNoBorderColor = L"NoBorderColor";

    // Surface → абсолютный путь HKCU под Software\...\GlassPlatform.
    [[nodiscard]] inline const wchar_t* RegPathForSubKey(const wchar_t* bwmSubKey) noexcept
    {
        if (!bwmSubKey || !*bwmSubKey || wcscmp(bwmSubKey, kXamlSubKey) == 0)
            return kRegPath;
        if (wcscmp(bwmSubKey, kXamlPopupSubKey) == 0)
            return kRegPathPopup;
        if (wcscmp(bwmSubKey, kXamlPreviewSubKey) == 0)
            return kRegPathPreview;
        if (wcscmp(bwmSubKey, kXamlMenuSubKey) == 0)
            return kRegPathMenu;
        if (wcscmp(bwmSubKey, kXamlTooltipSubKey) == 0)
            return kRegPathTooltip;
        return kRegPath;
    }

    // CornerType XAML: 0 Default, 1 Rect, 2 Round(8), 3 SmallRound(4),
    // 4 Increased (фиксированно 16), 5 MaxRound (R = h/2).
    inline constexpr DWORD kCornerTypeIncreasedXaml = 4;
    inline constexpr DWORD kCornerTypeMaxRoundXaml = 5;
    inline constexpr DWORD kCornerTypeMaxXaml = 5;
    inline constexpr DWORD kCornerTypeSchemaCurrent = 3;
    // Increased: между Large(8) и MaxRound(h/2).
    inline constexpr double kIncreasedCornerRadiusDip = 16.0;
    // Системный CoreWindow NC chrome (H70) — справочно / map, не для Increased.
    inline constexpr DWORD kDefaultNcSystemCornerRadiusCentidip = 800; // 8.0 DIP
    inline constexpr double kNcSystemCornerRadiusDip = 8.0;

    // Индексы EffectType для XAML-пути (не путать с Tray\EffectType SWCA 0–8).
    // 5 = System (оригинальный бэкдроп острова). Legacy Mica/MicaAlt (5/6) → System.
    enum class EffectType : DWORD
    {
        Disable     = 0,
        Transparent = 1,
        Solid       = 2,
        Blur        = 3,
        Acrylic     = 4,
        System      = 5,
    };

    inline constexpr const wchar_t* kCachedSystemFill = L"CachedSystemFillArgb";
    inline constexpr const wchar_t* kCachedSystemStroke = L"CachedSystemStrokeArgb";

    [[nodiscard]] inline bool ReadDwordValueAt(
        const wchar_t* regPath, const wchar_t* name, DWORD& out) noexcept
    {
        // AppContainer: fallback на HKEY_USERS\<SID>\... (виртуальный HKCU пуст).
        return Shared::RealUserReg::GetDword(regPath, name, out, nullptr) == ERROR_SUCCESS;
    }

    [[nodiscard]] inline bool ReadDwordValue(const wchar_t* name, DWORD& out) noexcept
    {
        return ReadDwordValueAt(kRegPath, name, out);
    }

    /// Sample BackgroundStroke до патча (пишет TaskbarCompat). Дефолт бордюра без *BorderColor.
    [[nodiscard]] inline bool TryReadCachedSystemStrokeArgb(DWORD& outArgb) noexcept
    {
        DWORD v = 0;
        if (!ReadDwordValue(kCachedSystemStroke, v) || v == 0)
            return false;
        outArgb = v;
        return true;
    }

    // Surface, затем Tray\Xaml. false = нет ни в surface, ни в bar.
    [[nodiscard]] inline bool ReadDwordValueFor(
        const wchar_t* bwmSubKey, const wchar_t* name, DWORD& out) noexcept
    {
        const wchar_t* const surfacePath = RegPathForSubKey(bwmSubKey);
        if (surfacePath != kRegPath && ReadDwordValueAt(surfacePath, name, out))
            return true;
        return ReadDwordValueAt(kRegPath, name, out);
    }

    // Только surface-ключ (без fallback) — для gate/наличия.
    [[nodiscard]] inline bool TryReadDwordValueLocal(
        const wchar_t* bwmSubKey, const wchar_t* name, DWORD& out) noexcept
    {
        return ReadDwordValueAt(RegPathForSubKey(bwmSubKey), name, out);
    }

    inline void WriteDwordValue(const wchar_t* name, DWORD value) noexcept
    {
        RegSetKeyValueW(
            HKEY_CURRENT_USER, kRegPath, name, REG_DWORD, &value, sizeof(value));
    }

    /// Theme/reload: сбросить sample stroke (иначе stale theme → adjacent/GS Default).
    inline void DeleteCachedSystemStrokeArgb() noexcept
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegPath, kCachedSystemStroke);
    }

    [[nodiscard]] inline DWORD NormalizeEffectType(
        DWORD v,
        DWORD defaultValue = static_cast<DWORD>(EffectType::Acrylic)) noexcept
    {
        // Legacy MicaAlt=6 → System (5). Явный 5 остаётся System.
        if (v == 6u)
            return static_cast<DWORD>(EffectType::System);
        if (v > static_cast<DWORD>(EffectType::System))
            return defaultValue;
        return v;
    }

    // Нет ключа EffectType = Acrylic (продуктовый дефолт XAML-пути), не Disable/System.
    // Disable/System — только явное значение при включённом gate.
    [[nodiscard]] inline DWORD ReadEffectType(
        DWORD defaultValue = static_cast<DWORD>(EffectType::Acrylic)) noexcept
    {
        DWORD v = defaultValue;
        if (!ReadDwordValue(kEffectTypeValue, v))
            return defaultValue;
        return NormalizeEffectType(v, defaultValue);
    }

    // Surface EffectType → fallback Tray\Xaml → default Acrylic.
    [[nodiscard]] inline DWORD ReadEffectTypeFor(
        const wchar_t* bwmSubKey,
        DWORD defaultValue = static_cast<DWORD>(EffectType::Acrylic)) noexcept
    {
        DWORD v = defaultValue;
        if (!ReadDwordValueFor(bwmSubKey, kEffectTypeValue, v))
            return defaultValue;
        return NormalizeEffectType(v, defaultValue);
    }

    // Нет ключа CornerType = крупное скругление (2).
    inline constexpr DWORD kDefaultCornerTypeXaml = 2;

    [[nodiscard]] inline DWORD ClampCornerTypeXaml(DWORD corner) noexcept
    {
        return corner > kCornerTypeMaxXaml ? kCornerTypeMaxXaml : corner;
    }

    // Schema 0/1: 4 = старый MaxRound → 5. Schema ≥2: 4 = Increased (не трогать).
    // H74: schema=2 + remap 4→5 ломал NC при выборе Increased.
    inline void MigrateCornerTypeSchemaIfNeeded() noexcept
    {
        DWORD schema = 0;
        (void)ReadDwordValue(kCornerTypeSchemaValue, schema);
        if (schema >= kCornerTypeSchemaCurrent)
            return;

        if (schema < 2u)
        {
            DWORD corner = 0;
            if (ReadDwordValue(kCornerTypeValue, corner) && corner == 4u)
                WriteDwordValue(kCornerTypeValue, kCornerTypeMaxRoundXaml);
        }

        WriteDwordValue(kCornerTypeSchemaValue, kCornerTypeSchemaCurrent);
    }

    // Только schema отсутствует / 0 / 1: raw 4 = старый MaxRound.
    [[nodiscard]] inline DWORD ResolveCornerTypeXaml(DWORD raw) noexcept
    {
        MigrateCornerTypeSchemaIfNeeded();
        DWORD schema = 0;
        const bool hasSchema = ReadDwordValue(kCornerTypeSchemaValue, schema);
        if ((!hasSchema || schema < 2u) && raw == 4u)
            return kCornerTypeMaxRoundXaml;
        return ClampCornerTypeXaml(raw);
    }

    [[nodiscard]] inline bool ReadSeparateZones(bool defaultValue = false) noexcept
    {
        DWORD v = defaultValue ? 1u : 0u;
        if (!ReadDwordValue(kSeparateZonesValue, v))
            return defaultValue;
        return v != 0;
    }

    [[nodiscard]] inline bool TryReadCornerType(DWORD& out) noexcept
    {
        MigrateCornerTypeSchemaIfNeeded();
        DWORD raw = 0;
        if (!ReadDwordValue(kCornerTypeValue, raw))
            return false;
        out = ResolveCornerTypeXaml(raw);
        return true;
    }

    // Surface CornerType → fallback Tray\Xaml.
    [[nodiscard]] inline bool TryReadCornerTypeFor(
        const wchar_t* bwmSubKey, DWORD& out) noexcept
    {
        MigrateCornerTypeSchemaIfNeeded();

        DWORD raw = 0;
        const wchar_t* const surfacePath = RegPathForSubKey(bwmSubKey);
        if (surfacePath != kRegPath
            && ReadDwordValueAt(surfacePath, kCornerTypeValue, raw))
        {
            out = ResolveCornerTypeXaml(raw);
            return true;
        }
        return TryReadCornerType(out);
    }

    // Публикация системного R CoreWindow (UICompat CaptureChrome).
    // Map — для AppContainer; HKCU — best-effort (explorer / GS).
    // Значение зафиксировано: Win11 NC chrome = 8.0 DIP (лог H70).
    inline void PublishNcSystemCornerRadiusDip(double radiusDip) noexcept
    {
        // Принимаем только разумный захват; иначе — зафиксированные 8.0.
        double r = radiusDip;
        if (r < 1.0 || r > 64.0)
            r = kNcSystemCornerRadiusDip;
        const DWORD centi =
            static_cast<DWORD>(r * 100.0 + 0.5);
        (void)NcSystemCornerRadiusMap::PublishCentiDip(centi);
        WriteDwordValue(kNcSystemCornerRadiusValue, centi);
    }

    [[nodiscard]] inline double ReadNcSystemCornerRadiusDip() noexcept
    {
        DWORD centi = 0;
        if (NcSystemCornerRadiusMap::TryReadCentiDip(centi) && centi > 0u)
        {
            (void)NcSystemCornerRadiusMap::PublishCentiDip(centi);
            return static_cast<double>(centi) / 100.0;
        }
        centi = kDefaultNcSystemCornerRadiusCentidip;
        if (ReadDwordValue(kNcSystemCornerRadiusValue, centi) && centi > 0u)
            return static_cast<double>(centi) / 100.0;
        return kNcSystemCornerRadiusDip;
    }

    // Единый радиус XAML CornerType для Taskbar / UICompat.
    [[nodiscard]] inline double XamlCornerRadiusDip(
        DWORD cornerType,
        double panelHeightDip) noexcept
    {
        switch (cornerType)
        {
        case 1:
            return 0.0;
        case 2:
            return 8.0;
        case 3:
            return 4.0;
        case kCornerTypeIncreasedXaml:
            return kIncreasedCornerRadiusDip; // 16
        case kCornerTypeMaxRoundXaml:
            return panelHeightDip > 1.0 ? panelHeightDip * 0.5 : 8.0;
        case 0:
        default:
            return 8.0;
        }
    }

    // Высота Shell_*TrayWnd в DIP (клиент) — база для MaxRound popup/трея.
    [[nodiscard]] inline double QueryTaskbarHeightDip() noexcept
    {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tray)
            tray = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
        if (!tray)
            return 48.0;
        RECT rc{};
        if (!GetClientRect(tray, &rc))
            return 48.0;
        const int h = rc.bottom - rc.top;
        return h > 1 ? static_cast<double>(h) : 48.0;
    }

    // Popup (ToolTip / MenuFlyout): радиус всегда от высоты трея.
    // MaxRound: R = trayH/2; если высота popup < 2R — Increased, иначе штатный R трея.
    [[nodiscard]] inline double XamlPopupCornerRadiusDip(
        DWORD cornerType,
        double trayHeightDip,
        double popupHeightDip) noexcept
    {
        if (cornerType == kCornerTypeMaxRoundXaml)
        {
            const double rTray = XamlCornerRadiusDip(kCornerTypeMaxRoundXaml, trayHeightDip);
            if (popupHeightDip + 0.5 < rTray * 2.0)
                return kIncreasedCornerRadiusDip;
            return rTray;
        }
        return XamlCornerRadiusDip(cornerType, trayHeightDip);
    }

    [[nodiscard]] inline bool TryReadGradientArgb(bool darkMode, DWORD& outArgb) noexcept
    {
        return ReadDwordValue(darkMode ? kDarkGradient : kLightGradient, outArgb);
    }

    [[nodiscard]] inline bool TryReadGradientArgbFor(
        const wchar_t* bwmSubKey, bool darkMode, DWORD& outArgb) noexcept
    {
        return ReadDwordValueFor(
            bwmSubKey, darkMode ? kDarkGradient : kLightGradient, outArgb);
    }

    [[nodiscard]] inline bool TryReadBorderArgb(bool darkMode, DWORD& outArgb) noexcept
    {
        return ReadDwordValue(darkMode ? kDarkBorder : kLightBorder, outArgb);
    }

    [[nodiscard]] inline bool TryReadBorderArgbFor(
        const wchar_t* bwmSubKey, bool darkMode, DWORD& outArgb) noexcept
    {
        return ReadDwordValueFor(
            bwmSubKey, darkMode ? kDarkBorder : kLightBorder, outArgb);
    }

    [[nodiscard]] inline bool ReadNoBorderColor(bool defaultValue = false) noexcept
    {
        DWORD v = defaultValue ? 1u : 0u;
        if (!ReadDwordValue(kNoBorderColor, v))
            return defaultValue;
        return v != 0;
    }

    // Surface NoBorder → fallback Tray\Xaml.
    [[nodiscard]] inline bool ReadNoBorderColorFor(
        const wchar_t* bwmSubKey, bool defaultValue = false) noexcept
    {
        DWORD v = 0;
        if (TryReadDwordValueLocal(bwmSubKey, kNoBorderColor, v))
            return v != 0;
        if (bwmSubKey && wcscmp(bwmSubKey, kXamlSubKey) != 0)
            return ReadNoBorderColor(defaultValue);
        return defaultValue;
    }
}
