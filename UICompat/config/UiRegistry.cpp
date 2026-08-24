#include "UiPch.h"
#include "config/UiRegistry.h"

#include <TrayBorderDrawMode.h>
#include <TrayXamlPaint.h>
#include <TrayXamlPolicyMap.h>
#include <DebugLog.h>
#include <Helpers/BorderDisplayHelper.h>
#include <Helpers/ThemeHelper.h>
namespace UICompat::Registry
{
    namespace
    {
        [[nodiscard]] const wchar_t* SubKeyFor(SurfaceKind kind) noexcept
        {
            switch (kind)
            {
            case SurfaceKind::Menu:
                return Shared::TrayXamlPaint::kXamlMenuSubKey;
            case SurfaceKind::Tooltip:
                return Shared::TrayXamlPaint::kXamlTooltipSubKey;
            case SurfaceKind::Popup:
            default:
                return Shared::TrayXamlPaint::kXamlPopupSubKey;
            }
        }

        [[nodiscard]] const wchar_t* SurfaceName(SurfaceKind kind) noexcept
        {
            switch (kind)
            {
            case SurfaceKind::Menu:
                return L"Menu";
            case SurfaceKind::Tooltip:
                return L"Tooltip";
            case SurfaceKind::Popup:
            default:
                return L"Popup";
            }
        }

        void ApplySurfaceSlot(
            Settings& result,
            const Shared::TrayXamlPolicyMap::SurfaceSlot& slot,
            bool darkFromBar) noexcept
        {
            result.xamlEffectType = slot.effectType;
            result.gradientColorPresent =
                (slot.flags & Shared::TrayXamlPolicyMap::kFlagGradientPresent) != 0;
            result.tintArgb = slot.tintArgb;
            result.cornerType =
                Shared::TrayXamlPaint::ClampCornerTypeXaml(slot.cornerType);
            result.cornersPresent =
                (slot.flags & Shared::TrayXamlPolicyMap::kFlagCornersPresent) != 0;
            result.noBorder =
                (slot.flags & Shared::TrayXamlPolicyMap::kFlagNoBorder) != 0;
            result.patchBorder =
                (slot.flags & Shared::TrayXamlPolicyMap::kFlagPatchBorder) != 0;
            result.borderArgb = slot.borderArgb;
            (void)darkFromBar;
        }

        void FillCornerAndBorderFromRegistry(
            Settings& result,
            const wchar_t* bwmSubKey,
            bool darkMode) noexcept
        {
            DWORD corner = Shared::TrayXamlPaint::kDefaultCornerTypeXaml;
            if (Shared::TrayXamlPaint::TryReadCornerTypeFor(bwmSubKey, corner))
            {
                corner = Shared::TrayXamlPaint::ClampCornerTypeXaml(corner);
                result.cornerType = corner;
                result.cornersPresent = corner != 0;
            }
            else
            {
                result.cornerType = Shared::TrayXamlPaint::kDefaultCornerTypeXaml;
                result.cornersPresent = true;
            }

            result.noBorder =
                Shared::TrayXamlPaint::ReadNoBorderColorFor(bwmSubKey, false);
            if (result.noBorder)
            {
                result.patchBorder = false;
                result.borderArgb = 0;
                return;
            }

            DWORD border = 0;
            if (Shared::TrayXamlPaint::TryReadBorderArgbFor(bwmSubKey, darkMode, border))
            {
                result.patchBorder = true;
                result.borderArgb = border;
                return;
            }

            if (Shared::TrayXamlPaint::TryReadCachedSystemStrokeArgb(border))
            {
                result.patchBorder = true;
                result.borderArgb = border;
                return;
            }

            // Pre-capture ChartView — tint бордюра не зависит от CornerType.
            {
                auto ui =
                    Shared::BorderDisplayHelper::QueryForBwmSubKey(bwmSubKey, darkMode);
                if (ui.displayArgb == 0 || ui.colorUseNone)
                {
                    ui = Shared::BorderDisplayHelper::QueryForBwmSubKey(
                        L"Tray\\Xaml", darkMode);
                }
                if (ui.displayArgb != 0 && !ui.colorUseNone)
                {
                    result.patchBorder = true;
                    result.borderArgb = ui.displayArgb;
                    return;
                }
            }

            result.patchBorder = false;
            result.borderArgb = 0;
        }

        Settings ReadSurfaceImpl(SurfaceKind kind) noexcept
        {
            Settings result{};
            const wchar_t* const bwmSubKey = SubKeyFor(kind);
            const char* sourceA = "default";
            const wchar_t* sourceW = L"default";
            int borderSource = 2;
            bool darkForLog = Shared::ThemeHelper::IsTrayXamlDarkMode();

            Shared::TrayXamlPolicyMap::Snapshot mapSnap{};
            if (Shared::TrayXamlPolicyMap::TryRead(mapSnap))
            {
                sourceA = "map";
                sourceW = L"map";
                borderSource = 3;
                darkForLog =
                    (mapSnap.flags & Shared::TrayXamlPolicyMap::kFlagDarkMode) != 0;
                result.paintPathXaml =
                    mapSnap.borderDrawMode
                    == static_cast<DWORD>(Shared::TrayBorderDrawMode::Mode::Xaml);

                const Shared::TrayXamlPolicyMap::SurfaceSlot& slot =
                    kind == SurfaceKind::Menu ? mapSnap.menu
                    : kind == SurfaceKind::Tooltip ? mapSnap.tooltip
                    : mapSnap.popup;

                // AppContainer: prefer map; если HKCU доступен — CornerType из reg
                // (как раньше для bar), иначе map slot (уже с fallback).
                ApplySurfaceSlot(result, slot, darkForLog);
                {
                    DWORD fromReg = 0;
                    if (Shared::TrayXamlPaint::TryReadCornerTypeFor(bwmSubKey, fromReg))
                        result.cornerType =
                            Shared::TrayXamlPaint::ClampCornerTypeXaml(fromReg);
                }
                // Map мог быть опубликован host'ом со старым ChartView (синий).
                // Без явного *BorderColor — выровнять на CachedSystemStroke панели.
                if (!result.noBorder && result.patchBorder)
                {
                    DWORD keyBorder = 0;
                    const bool hasKey = Shared::TrayXamlPaint::TryReadBorderArgbFor(
                        bwmSubKey, darkForLog, keyBorder);
                    DWORD cached = 0;
                    if (!hasKey
                        && Shared::TrayXamlPaint::TryReadCachedSystemStrokeArgb(cached)
                        && result.borderArgb != cached)
                    {
                        result.borderArgb = cached;
                    }
                }
            }
            else
            {
                result.paintPathXaml =
                    Shared::TrayBorderDrawMode::ReadFromRegistry(&borderSource)
                    == Shared::TrayBorderDrawMode::Mode::Xaml;
                result.xamlEffectType =
                    Shared::TrayXamlPaint::ReadEffectTypeFor(bwmSubKey);

                result.gradientColorPresent = false;
                DWORD tint = 0;
                if (Shared::TrayXamlPaint::TryReadGradientArgbFor(
                        bwmSubKey, darkForLog, tint))
                {
                    result.gradientColorPresent = true;
                    result.tintArgb = tint;
                }
                else
                {
                    result.tintArgb = darkForLog ? 0xE42C2C2Cu : 0xE4F3F3F3u;
                }

                FillCornerAndBorderFromRegistry(result, bwmSubKey, darkForLog);

                if (borderSource == 0)
                {
                    sourceA = "hkcu";
                    sourceW = L"hkcu";
                }
                else if (borderSource == 1)
                {
                    sourceA = "users";
                    sourceW = L"users";
                }
            }

            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][Registry] surface=%ls paintXaml=%u effect=%u tint=0x%08X "
                L"corner=%u patchBorder=%u border=0x%08X src=%ls dark=%u",
                SurfaceName(kind),
                result.paintPathXaml ? 1u : 0u,
                result.xamlEffectType,
                result.tintArgb,
                result.cornerType,
                result.patchBorder ? 1u : 0u,
                result.borderArgb,
                sourceW,
                darkForLog ? 1u : 0u);

            (void)sourceA;
            return result;
        }
    }

    Settings ReadFromRegistry() noexcept
    {
        return ReadSurfaceImpl(SurfaceKind::Popup);
    }

    Settings ReadSurface(SurfaceKind kind) noexcept
    {
        return ReadSurfaceImpl(kind);
    }
}
