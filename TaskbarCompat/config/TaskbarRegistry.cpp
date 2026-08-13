#include "TaskbarPch.h"
#include "config/TaskbarRegistry.h"

#include <TrayBorderDrawMode.h>
#include <TrayXamlPaint.h>
#include <DebugLog.h>
#include <Helpers/BorderDisplayHelper.h>
#include <Helpers/ThemeHelper.h>
#include <atomic>

namespace TaskbarCompat::Registry
{
    namespace
    {
        constexpr const wchar_t* kSubKey = L"Software\\StanislavKa\\BlackWindowManager\\Tray";
        constexpr const wchar_t* kValueCornerType = L"CornerType";

        // Последние системные SolidColorBrush с XAML-острова (только память — не реестр).
        std::atomic<DWORD> g_systemFillArgb{ 0 };
        std::atomic<DWORD> g_systemStrokeArgb{ 0 };

        void PurgeStaleCachedSystemSamples() noexcept
        {
            static std::atomic<bool> done{ false };
            if (done.exchange(true, std::memory_order_relaxed))
                return;
            // Fill: только in-memory (тема). Stroke публикуем в CachedSystemStroke для GS Default.
            RegDeleteKeyValueW(
                HKEY_CURRENT_USER,
                Shared::TrayXamlPaint::kRegPath,
                Shared::TrayXamlPaint::kCachedSystemFill);
        }

        [[nodiscard]] bool ReadDword(
            HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD& out) noexcept
        {
            DWORD cb = sizeof(out);
            const LSTATUS ls = RegGetValueW(
                root, subKey, valueName, RRF_RT_REG_DWORD, nullptr, &out, &cb);
            return ls == ERROR_SUCCESS;
        }

        [[nodiscard]] const wchar_t* SubKeyFor(SurfaceKind kind) noexcept
        {
            switch (kind)
            {
            case SurfaceKind::Popup:
                return Shared::TrayXamlPaint::kXamlPopupSubKey;
            case SurfaceKind::Preview:
                return Shared::TrayXamlPaint::kXamlPreviewSubKey;
            case SurfaceKind::Menu:
                return Shared::TrayXamlPaint::kXamlMenuSubKey;
            case SurfaceKind::Tooltip:
                return Shared::TrayXamlPaint::kXamlTooltipSubKey;
            case SurfaceKind::Bar:
            default:
                return Shared::TrayXamlPaint::kXamlSubKey;
            }
        }

        [[nodiscard]] const wchar_t* SurfaceName(SurfaceKind kind) noexcept
        {
            switch (kind)
            {
            case SurfaceKind::Popup:
                return L"Popup";
            case SurfaceKind::Preview:
                return L"Preview";
            case SurfaceKind::Menu:
                return L"Menu";
            case SurfaceKind::Tooltip:
                return L"Tooltip";
            case SurfaceKind::Bar:
            default:
                return L"Bar";
            }
        }

        [[nodiscard]] DWORD ReadTintArgbFor(
            const wchar_t* bwmSubKey,
            bool darkMode,
            bool paintPathXaml,
            bool& gradientPresent) noexcept
        {
            gradientPresent = false;
            if (paintPathXaml)
            {
                DWORD argb = 0;
                if (Shared::TrayXamlPaint::TryReadGradientArgbFor(bwmSubKey, darkMode, argb))
                {
                    gradientPresent = true;
                    return argb;
                }

                const DWORD sys = g_systemFillArgb.load(std::memory_order_relaxed);
                if (sys != 0)
                {
                    // Opaque system fill (часто A=0xFF) нельзя кормить в XamlBlurBrush Flood:
                    // SOURCE_OVER с A=1 полностью гасит blur → чёрные сегменты (лог tint=0xFF202020).
                    const BYTE a = static_cast<BYTE>((sys >> 24) & 0xFF);
                    if (a <= 0xE4)
                    {
                        DebugLog(
                            DBG_IMPORTANT,
                            L"[TaskbarCompat][Registry] XAML tint default from system BackgroundFill=0x%08X",
                            sys);
                        return sys;
                    }
                    const DWORD clamped = (sys & 0x00FFFFFFu) | (0xE4u << 24);
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[TaskbarCompat][Registry] XAML tint from system fill clamped 0x%08X → 0x%08X",
                        sys,
                        clamped);
                    return clamped;
                }

                // Sample для UI/кисти до capture; fill при отсутствии ключа = system brush.
                return darkMode ? 0xE42C2C2Cu : 0xE4F3F3F3u;
            }

            const wchar_t* const name =
                darkMode ? L"DarkMode_GradientColor" : L"LightMode_GradientColor";
            DWORD argb = darkMode ? 0x412B2B2Bu : 0x9EDDDDDDu;
            if (ReadDword(HKEY_CURRENT_USER, kSubKey, name, argb))
                gradientPresent = true;
            return argb;
        }

        void FillBorderStrokeSettings(
            Settings& result,
            const wchar_t* bwmSubKey,
            bool darkMode) noexcept
        {
            DWORD cachedStroke = 0;
            (void)Shared::TrayXamlPaint::ReadDwordValue(
                Shared::TrayXamlPaint::kCachedSystemStroke, cachedStroke);
            const DWORD memStroke = g_systemStrokeArgb.load(std::memory_order_relaxed);

            if (!result.paintPathXaml)
            {
                // DWM рисует бордюр — XAML stroke скрываем.
                result.patchBorderStroke = true;
                result.borderColorArgb = 0;
                return;
            }

            if (Shared::TrayXamlPaint::ReadNoBorderColorFor(bwmSubKey, false))
            {
                result.patchBorderStroke = true;
                result.borderColorArgb = 0;
                DebugLog(DBG_IMPORTANT,
                    L"[TaskbarCompat][Registry] NoBorderColor=1 — border off (surface=%ls)",
                    bwmSubKey);
                return;
            }

            DWORD argb = 0;
            if (Shared::TrayXamlPaint::TryReadBorderArgbFor(bwmSubKey, darkMode, argb))
            {
                result.patchBorderStroke = true;
                result.borderColorArgb = argb;
                return;
            }

            // XAML path: без *BorderColor — системный stroke (после capture).
            // Legacy Tray\BorderColor НЕ использовать (ломает «по умолчанию» vs факт).
            const DWORD sysStroke = memStroke;
            if (sysStroke != 0)
            {
                result.patchBorderStroke = true;
                result.borderColorArgb = sysStroke;
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][Registry] XAML border default from system BackgroundStroke=0x%08X",
                    sysStroke);
                return;
            }

            // После Invalidate theme-reload cache удалён; иначе stale sample > ChartView.
            if (cachedStroke != 0)
            {
                result.patchBorderStroke = true;
                result.borderColorArgb = cachedStroke;
                g_systemStrokeArgb.store(cachedStroke, std::memory_order_relaxed);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][Registry] XAML border default from CachedSystemStroke=0x%08X",
                    cachedStroke);
                return;
            }

            // Pre-capture: ChartView (паритет GS). Tint бордюра не зависит от углов.
            {
                auto ui = Shared::BorderDisplayHelper::QueryForBwmSubKey(bwmSubKey, darkMode);
                if (ui.displayArgb == 0 || ui.colorUseNone)
                {
                    ui = Shared::BorderDisplayHelper::QueryForBwmSubKey(L"Tray\\Xaml", darkMode);
                }
                if (ui.displayArgb != 0 && !ui.colorUseNone)
                {
                    result.patchBorderStroke = true;
                    result.borderColorArgb = ui.displayArgb;
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[TaskbarCompat][Registry] XAML border pre-capture ChartView=0x%08X "
                        L"(cornerType=%u theme=%ls)",
                        ui.displayArgb,
                        result.cornerType,
                        darkMode ? L"dark" : L"light");
                    return;
                }
            }

            DebugLog(DBG_IMPORTANT,
                L"[TaskbarCompat][Registry] XAML border не задан (theme=%ls) — stroke не патчится",
                darkMode ? L"dark" : L"light");
        }

        Settings ReadSurfaceImpl(SurfaceKind kind) noexcept
        {
            Settings result{};
            const wchar_t* const bwmSubKey = SubKeyFor(kind);
            const bool darkMode = Shared::ThemeHelper::IsTrayXamlDarkMode();
            result.paintPathXaml =
                Shared::TrayBorderDrawMode::ReadFromRegistry()
                == Shared::TrayBorderDrawMode::Mode::Xaml;
            result.tintArgb = ReadTintArgbFor(
                bwmSubKey, darkMode, result.paintPathXaml, result.gradientColorPresent);
            result.xamlEffectType = Shared::TrayXamlPaint::ReadEffectTypeFor(bwmSubKey);

            DWORD corner = Shared::TrayXamlPaint::kDefaultCornerTypeXaml;
            if (result.paintPathXaml)
            {
                if (Shared::TrayXamlPaint::TryReadCornerTypeFor(bwmSubKey, corner))
                {
                    result.cornerType =
                        corner > Shared::TrayXamlPaint::kCornerTypeMaxXaml
                            ? Shared::TrayXamlPaint::kCornerTypeMaxXaml
                            : corner;
                    result.cornersPresent = result.cornerType != 0;
                }
                else
                {
                    result.cornersPresent = true;
                    result.cornerType = Shared::TrayXamlPaint::kDefaultCornerTypeXaml;
                }
            }
            else if (kind == SurfaceKind::Bar
                && ReadDword(HKEY_CURRENT_USER, kSubKey, kValueCornerType, corner))
            {
                result.cornersPresent = true;
                result.cornerType = corner > 3 ? 3 : corner;
            }
            else if (kind == SurfaceKind::Bar)
            {
                // Нет Tray\CornerType → крупное скругление (2 / DWMWCP_ROUND).
                result.cornersPresent = true;
                result.cornerType = 2;
            }
            else
            {
                // Adjacent на DWM-пути: углы с bar-дефолтом XAML (restore path).
                result.cornersPresent = true;
                result.cornerType = Shared::TrayXamlPaint::kDefaultCornerTypeXaml;
            }

            result.separateZones =
                kind == SurfaceKind::Bar
                && result.paintPathXaml
                && result.cornersPresent
                && Shared::TrayXamlPaint::ReadSeparateZones();

            FillBorderStrokeSettings(result, bwmSubKey, darkMode);

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][Registry] surface=%ls paintXaml=%u effect=%u tint=0x%08X "
                L"corners=%u cornerType=%u zones=%u patchStroke=%u border=0x%08X",
                SurfaceName(kind),
                result.paintPathXaml ? 1u : 0u,
                result.xamlEffectType,
                result.tintArgb,
                result.cornersPresent ? 1u : 0u,
                result.cornerType,
                result.separateZones ? 1u : 0u,
                result.patchBorderStroke ? 1u : 0u,
                result.borderColorArgb);

            return result;
        }
    }

    void NoteSystemFillArgb(DWORD argb) noexcept
    {
        // Только in-memory: тема может смениться — не пишем Cached* в реестр.
        PurgeStaleCachedSystemSamples();

        const BYTE a = static_cast<BYTE>((argb >> 24) & 0xFF);
        if (a < 8)
            return;
        // Opaque system fill (часто A=0xFF) — clamp как в ReadTintArgb.
        const DWORD cached =
            a > 0xE4 ? ((argb & 0x00FFFFFFu) | (0xE4u << 24)) : argb;
        const BYTE r = static_cast<BYTE>((cached >> 16) & 0xFF);
        const BYTE g = static_cast<BYTE>((cached >> 8) & 0xFF);
        const BYTE b = static_cast<BYTE>(cached & 0xFF);
        const unsigned lum = (static_cast<unsigned>(r) * 30u
            + static_cast<unsigned>(g) * 59u
            + static_cast<unsigned>(b) * 11u) / 100u;
        if (Shared::ThemeHelper::IsTrayXamlDarkMode() && lum >= 220u)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][Registry] ignore bright system fill 0x%08X in dark theme",
                argb);
            return;
        }
        g_systemFillArgb.store(cached, std::memory_order_relaxed);
    }

    void NoteSystemStrokeArgb(DWORD argb) noexcept
    {
        PurgeStaleCachedSystemSamples();

        const BYTE a = static_cast<BYTE>((argb >> 24) & 0xFF);
        if (a < 8)
            return;
        g_systemStrokeArgb.store(argb, std::memory_order_relaxed);
        // GS «По умолчанию» для Tray\Xaml* читает sample до патча.
        Shared::TrayXamlPaint::WriteDwordValue(
            Shared::TrayXamlPaint::kCachedSystemStroke, argb);
    }

    void InvalidateSystemBrushSamples() noexcept
    {
        g_systemFillArgb.store(0, std::memory_order_relaxed);
        g_systemStrokeArgb.store(0, std::memory_order_relaxed);
        // HKCU sample тоже: иначе ReadSurface сразу поднимает stale stroke прошлой темы.
        Shared::TrayXamlPaint::DeleteCachedSystemStrokeArgb();
        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][Registry] InvalidateSystemBrushSamples (theme/reload, cache cleared)");
    }

    Settings ReadFromRegistry() noexcept
    {
        return ReadSurfaceImpl(SurfaceKind::Bar);
    }

    Settings ReadSurface(SurfaceKind kind) noexcept
    {
        return ReadSurfaceImpl(kind);
    }
}
