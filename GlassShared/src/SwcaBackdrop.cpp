#include "SharedPch.h"
#include <Helpers/SwcaBackdrop.h>
#include <RootSwcaRegistryQuery.h>
#include <EffectTypeQuery.h>
#include <Helpers/DwmGlassOpacity.h>
#include <Helpers/RegHelper.h>
#include <Helpers/SystemHelper.h>
#include <Helpers/WinApiUtils.h>
#include <RootPolicyQuery.h>


#include <optional>
#include <string_view>

#pragma comment(lib, "dwmapi.lib")

namespace Shared::SwcaBackdrop
{
    namespace
    {
        enum class ACCENT_STATE : DWORD
        {
            ACCENT_DISABLED = 0,
            ACCENT_ENABLE_GRADIENT = 1,
            ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
            ACCENT_ENABLE_BLURBEHIND = 3,
            ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
            ACCENT_ENABLE_HOSTBACKDROP = 5,
        };

        struct ACCENT_POLICY
        {
            DWORD AccentState;
            DWORD AccentFlags;
            DWORD dwGradientColor;
            DWORD dwAnimationId;
        };

        struct WINDOWCOMPOSITIONATTRIBUTEDATA
        {
            DWORD  dwAttribute;
            PVOID  pvData;
            SIZE_T cbData;
        };

        using FnSWCA = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBUTEDATA*);

        [[nodiscard]] FnSWCA ResolveSWCA() noexcept
        {
            static FnSWCA fn = reinterpret_cast<FnSWCA>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
            return fn;
        }

        [[nodiscard]] DWORD GetWindowsBuildNumber() noexcept
        {
            static DWORD build = 0;
            if (build)
                return build;
            HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
            if (!hMod)
                return 0;
            using RtlGetVersionPtr = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
            auto fn = reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(hMod, "RtlGetVersion"));
            if (!fn)
                return 0;
            RTL_OSVERSIONINFOW ver{};
            ver.dwOSVersionInfoSize = sizeof(ver);
            if (fn(&ver) == 0)
                build = ver.dwBuildNumber;
            return build;
        }

        void DwmMakeWindowTransparent(HWND hwnd, BOOL enable) noexcept
        {
            DWM_BLURBEHIND bb{};
            bb.dwFlags = DWM_BB_ENABLE | (enable ? (DWM_BB_BLURREGION | DWM_BB_TRANSITIONONMAXIMIZED) : 0);
            bb.fEnable = enable;
            bb.hRgnBlur = CreateRectRgn(0, 0, -1, -1);
            DwmEnableBlurBehindWindow(hwnd, &bb);
            DeleteObject(bb.hRgnBlur);
        }

        void ApplyBorderEffect(
            HWND hWnd,
            bool /*darkMode*/,
            bool colorUseNone,
            bool colorUseDefault,
            DWORD borderColorArgb,
            DWM_WINDOW_CORNER_PREFERENCE cornerType) noexcept
        {
            if (Shared::SystemHelper::g_buildNumber >= 22000)
            {
                DwmSetWindowAttribute(
                    hWnd,
                    DWMWA_WINDOW_CORNER_PREFERENCE,
                    &cornerType,
                    sizeof(cornerType));
            }

            if (Shared::SystemHelper::g_buildNumber >= 22000)
            {
                if (colorUseNone)
                {
                    COLORREF noColor = DWMWA_COLOR_NONE;
                    DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR, &noColor, sizeof(noColor));
                }
                else if (!colorUseDefault)
                {
                    COLORREF cr = RGB(
                        (borderColorArgb >> 16) & 0xFF,
                        (borderColorArgb >> 8) & 0xFF,
                        borderColorArgb & 0xFF);
                    DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR, &cr, sizeof(cr));
                }
                else
                {
                    COLORREF defColor = DWMWA_COLOR_DEFAULT;
                    DwmSetWindowAttribute(hWnd, DWMWA_BORDER_COLOR, &defColor, sizeof(defColor));
                }
            }
        }

        // Как BWM EffectHelper::ACCENT_FLAG — маска рамки для dropShadow.
        constexpr DWORD kAccentBorderAll =
            (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8);
        // GRADIENT_COLOR и MODERN_ACRYLIC_RECIPE в BWM совпадают по биту (1<<1).
        constexpr DWORD kAccentGradientOrModernRecipe = (1u << 1);
        constexpr DWORD kWcaAccentPolicy = 19u;

        /// ET 6–8 в SWCA-стеке используют DWMSBT_*; поверх root с legacy DWM blur (OpenGlass)
        /// TRANSIENT/Mica не заполняют фон — «дырка». Accent-fallback внутри SWCA-стека.
        [[nodiscard]] bool PopupSystemBackdropBlockedByRootDwmPolicy() noexcept
        {
            return Shared::SystemHelper::g_buildNumber >= 22621
                && !Shared::RootPolicyQuery::UsesSwcaBackdrop();
        }

        void ApplyPopupAccentFallbackForDwmsbtEffect(
            ACCENT_POLICY& accent,
            bool hasTint,
            int effectTypeReg) noexcept
        {
            accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_ENABLE_ACRYLICBLURBEHIND);
            accent.AccentFlags |= kAccentGradientOrModernRecipe;
            if (!hasTint && (effectTypeReg == 7 || effectTypeReg == 8))
                accent.AccentFlags &= ~kAccentGradientOrModernRecipe;

            DebugLog(
                DBG_IMPORTANT,
                L"[GlassShared][SwcaBackdrop] popup ET=%d: root=DWM → accent ModernAcrylic (DWMSBT blocked)",
                effectTypeReg);
        }

        // PopupIn (Animation.cpp): backdrop из SHCreateWorkerWindow с WS_EX_NOREDIRECTIONBITMAP под DComp/thumbnail.
        // Для таких HWND DefWindowProc(WM_NCACTIVATE)+DRAWFRAME даёт тёмный кадр на первых кадрах анимации;
        // обычные framed окна без этого флага — полный паритет с BWM EffectHelper.
        [[nodiscard]] bool ShouldSkipNcRenderingTrigger(HWND hwnd) noexcept
        {
            if (!hwnd || !IsWindow(hwnd))
                return true;
            const LONG_PTR ex = GetWindowLongPtr(hwnd, /* GWLP_EXSTYLE */ static_cast<int>(GWL_EXSTYLE));
            return (ex & static_cast<LONG_PTR>(WS_EX_NOREDIRECTIONBITMAP)) != 0;
        }

        void TriggerWindowNCRendering(HWND hwnd) noexcept
        {
            (void)DefWindowProcW(hwnd, WM_NCACTIVATE, TRUE, 0);
            SetWindowPos(
                hwnd,
                nullptr,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_DRAWFRAME |
                    SWP_NOACTIVATE);
        }
    } // namespace

    void SetImmersiveDarkMode(HWND hwnd, BOOL dark) noexcept
    {
        DWORD build = GetWindowsBuildNumber();
        DWORD attr = (build < 22000) ? 19u : 20u;
        DwmSetWindowAttribute(hwnd, attr, &dark, sizeof(dark));
    }

    void SetWindowBackdrop(HWND hwnd, BOOL dropShadow, DWORD tintAbgr, int effectTypeReg, bool skipNcRendering, bool flyoutBackdrop) noexcept
    {
        const FnSWCA swcaFn = ResolveSWCA();
        const DWORD build = Shared::SystemHelper::g_buildNumber;

        const DWORD bodyAbgr = tintAbgr & 0x00FFFFFFu;
        const bool hasTint =
            (((tintAbgr >> 24) & 0xFF) != 0) || (bodyAbgr != 0);

        ACCENT_POLICY accent{};
        accent.dwGradientColor = tintAbgr;
        accent.dwAnimationId = 0;
        accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_DISABLED);
        accent.AccentFlags = dropShadow ? kAccentBorderAll : 0u;

        DWM_SYSTEMBACKDROP_TYPE backdropType = DWMSBT_NONE;
        BOOL                    micaLegacyAttr = FALSE;
        BOOL                    ncRendering    = FALSE;
        BOOL                    windowTransparent = FALSE;

        switch (effectTypeReg)
        {
        case 0:
            break;
        case 1: // Transparent — как BWM: всегда GRADIENT_COLOR
            accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_ENABLE_TRANSPARENTGRADIENT);
            accent.AccentFlags |= kAccentGradientOrModernRecipe;
            break;
        case 2:
            accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_ENABLE_GRADIENT);
            accent.AccentFlags |= kAccentGradientOrModernRecipe;
            break;
        case 3:
            accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_ENABLE_BLURBEHIND);
            accent.AccentFlags |= kAccentGradientOrModernRecipe;
            break;
        case 4: // AcrylicBlur — BWM: только state, без доп. AccentFlags
            accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_ENABLE_ACRYLICBLURBEHIND);
            break;
        case 5: // ModernAcrylicBlur — BWM: MODERN_ACRYLIC_RECIPE (бит 1<<1)
            accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_ENABLE_ACRYLICBLURBEHIND);
            accent.AccentFlags |= kAccentGradientOrModernRecipe;
            break;
        case 6:
            if (!flyoutBackdrop && PopupSystemBackdropBlockedByRootDwmPolicy())
            {
                ApplyPopupAccentFallbackForDwmsbtEffect(accent, hasTint, effectTypeReg);
            }
            else
            {
                backdropType        = DWMSBT_TRANSIENTWINDOW;
                ncRendering         = TRUE;
                windowTransparent   = TRUE;
            }
            break;
        case 7:
            if (build >= 22000)
            {
                if (!flyoutBackdrop && PopupSystemBackdropBlockedByRootDwmPolicy())
                {
                    ApplyPopupAccentFallbackForDwmsbtEffect(accent, hasTint, effectTypeReg);
                }
                else
                {
                    backdropType      = DWMSBT_MAINWINDOW;
                    micaLegacyAttr    = TRUE;
                    ncRendering       = TRUE;
                    windowTransparent = TRUE;
                }
            }
            else
            {
                accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_ENABLE_ACRYLICBLURBEHIND);
                accent.AccentFlags |= hasTint ? kAccentGradientOrModernRecipe : 0u;
            }
            break;
        case 8:
            if (build >= 22000)
            {
                if (!flyoutBackdrop && PopupSystemBackdropBlockedByRootDwmPolicy())
                {
                    ApplyPopupAccentFallbackForDwmsbtEffect(accent, hasTint, effectTypeReg);
                }
                else
                {
                    backdropType      = DWMSBT_TABBEDWINDOW;
                    ncRendering       = TRUE;
                    windowTransparent = TRUE;
                }
            }
            else
            {
                accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_ENABLE_ACRYLICBLURBEHIND);
                accent.AccentFlags |= hasTint ? kAccentGradientOrModernRecipe : 0u;
            }
            break;
        default:
            accent.AccentState = static_cast<DWORD>(ACCENT_STATE::ACCENT_DISABLED);
            accent.AccentFlags = dropShadow ? kAccentBorderAll : 0u;
            break;
        }

        // Порядок как BlackWindowManager EffectHelper::SetWindowBackdrop:
        // DWMWA legacy Mica (1029) → DWMWA_SYSTEMBACKDROP_TYPE → DwmMakeWindowTransparent → SWCA → NC refresh.
        if (build > 22000 && build < 22621)
        {
            BOOL mica = micaLegacyAttr;
            (void)DwmSetWindowAttribute(hwnd, 1029u, &mica, sizeof(mica));
        }
        if (build >= 22621)
        {
            (void)DwmSetWindowAttribute(
                hwnd,
                DWMWA_SYSTEMBACKDROP_TYPE,
                &backdropType,
                sizeof(backdropType));
        }

        DwmMakeWindowTransparent(hwnd, windowTransparent);

        if (swcaFn)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassShared][SwcaBackdrop] SetWindowBackdrop hwnd=0x%p eff=%d "
                L"dropShadow=%d backdrop=%u AccentState=%u AccentFlags=0x%X "
                L"tintAbgr=0x%08X nc=%d transparent=%d flyout=%d swca=1",
                hwnd,
                effectTypeReg,
                dropShadow ? 1 : 0,
                static_cast<unsigned>(backdropType),
                accent.AccentState,
                accent.AccentFlags,
                tintAbgr,
                ncRendering ? 1 : 0,
                windowTransparent ? 1 : 0,
                flyoutBackdrop ? 1 : 0);

            WINDOWCOMPOSITIONATTRIBUTEDATA data{};
            data.dwAttribute = kWcaAccentPolicy;
            data.pvData      = &accent;
            data.cbData      = sizeof(accent);
            (void)swcaFn(hwnd, &data);
        }
        else if (effectTypeReg == 3)
        {
            DWM_BLURBEHIND bb{};
            bb.dwFlags    = DWM_BB_ENABLE;
            bb.fEnable    = TRUE;
            bb.hRgnBlur   = CreateRectRgn(0, 0, -1, -1);
            DwmEnableBlurBehindWindow(hwnd, &bb);
            DeleteObject(bb.hRgnBlur);
        }

        // FIX: Для меню с анимацией и effectType=6-8 откладываем TriggerWindowNCRendering
        // до после cloaking, чтобы предотвратить мерцание тёмного кадра.
        // skipNcRendering=true передаётся из MenuEvents.cpp когда анимация включена.
        if (ncRendering && !skipNcRendering && !ShouldSkipNcRenderingTrigger(hwnd))
        {
#ifdef GLASS_ENABLE_LOGGING
            {
                DWORD       ownerPid = 0;
                const DWORD ownerTid = GetWindowThreadProcessId(hwnd, &ownerPid);
                const DWORD curTid   = GetCurrentThreadId();
                if (ownerTid != 0 && ownerTid != curTid)
                {
                    DebugLog(DBG_WARNING,
                        L"[GlassShared][SwcaBackdrop] TriggerWindowNCRendering с чужого потока "
                        L"hwnd=0x%p ownerTid=%lu curTid=%lu",
                        hwnd,
                        static_cast<unsigned long>(ownerTid),
                        static_cast<unsigned long>(curTid));
                }
            }
#endif
            DebugLog(DBG_IMPORTANT, L"[GlassShared][SwcaBackdrop] TriggerWindowNCRendering hwnd=0x%p", hwnd);
            TriggerWindowNCRendering(hwnd);
        }
        else if (ncRendering && skipNcRendering)
        {
            DebugLog(DBG_IMPORTANT, L"[GlassShared][SwcaBackdrop] SKIPPED TriggerWindowNCRendering hwnd=0x%p (skipNcRendering=true)", hwnd);
        }
    }

    void ApplyChromeFromBwmPart(HWND hwnd, bool darkMode, std::wstring_view part) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return;

        constexpr DWORD kLightGradient = RootSwcaRegistryQuery::kDefaultLightGradientArgb;
        constexpr DWORD kDarkGradient = RootSwcaRegistryQuery::kDefaultDarkGradientArgb;
        const bool rootVisuallyActive = [&]() noexcept -> bool {
            const HWND fg = GetForegroundWindow();
            if (!fg)
                return true;
            const HWND rWnd = GetAncestor(hwnd, GA_ROOT);
            const HWND rFg = GetAncestor(fg, GA_ROOT);
            return rWnd && rWnd == rFg;
        }();

        RootSwcaRegistryQuery::Snapshot rootSnap{};
        if (part == L"Root")
            RootSwcaRegistryQuery::QuerySnapshot(darkMode, rootVisuallyActive, rootSnap);

        const DWORD effectTypeRaw = (part == L"Root")
            ? rootSnap.effectType
            : EffectTypeQuery::ReadEffectiveEffectType(part);

        DWORD rootBackdropMode = 2u;
        if (part == L"Root")
        {
            if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
                rootBackdropMode = 0u;
            else
                rootBackdropMode = Shared::RootPolicyQuery::ReadRootBackdropMode();
        }

        if (rootBackdropMode > 2u)
            rootBackdropMode = 2u;
        const bool useDwm = (rootBackdropMode == 1u);
        const bool disableBackdrop = (rootBackdropMode == 0u);

        // effectTypeRaw для Root уже engine-нативный (0-8), маппинг не нужен.
        const DWORD swcaEffectType = effectTypeRaw;

        const DWORD enableDropShadow = (part == L"Root")
            ? 0u
            : Shared::RegHelper::Get<DWORD>({ part, L"" }, L"EnableDropShadow", 0, 1);

        DWORD gradientArgb = kLightGradient;
        if (part == L"Root")
        {
            gradientArgb = rootSnap.gradientColorArgb;
        }
        else if (darkMode)
        {
            gradientArgb = Shared::RegHelper::Get<DWORD>(
                { part, L"" },
                L"DarkMode_GradientColor",
                kDarkGradient,
                1);
        }
        else
        {
            gradientArgb = Shared::RegHelper::Get<DWORD>(
                { part, L"" },
                L"LightMode_GradientColor",
                kLightGradient,
                1);
        }

        gradientArgb = Shared::DwmGlassOpacity::MergeGlassOpacityIntoArgb(gradientArgb, hwnd);

        const BYTE a = static_cast<BYTE>((gradientArgb >> 24) & 0xFF);
        const BYTE r = static_cast<BYTE>((gradientArgb >> 16) & 0xFF);
        const BYTE g = static_cast<BYTE>((gradientArgb >> 8) & 0xFF);
        const BYTE b = static_cast<BYTE>(gradientArgb & 0xFF);
        const DWORD accentAbgr =
            (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(b) << 16) | (static_cast<DWORD>(g) << 8) |
            static_cast<DWORD>(r);

        SetImmersiveDarkMode(hwnd, darkMode ? TRUE : FALSE);

        if (disableBackdrop)
        {
            SetWindowBackdrop(hwnd, FALSE, 0, 0);
            (void)Shared::WinApiUtils::EnableBlurBehindWindow(hwnd, false);
            (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hwnd, false);
        }
        else if (useDwm)
        {
            SetWindowBackdrop(hwnd, FALSE, 0, 0);
            (void)Shared::WinApiUtils::EnableBlurBehindWindow(hwnd, true);
        }
        else
        {
            (void)Shared::WinApiUtils::EnableBlurBehindWindow(hwnd, false);
            (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hwnd, false);
            SetWindowBackdrop(
                hwnd,
                enableDropShadow ? TRUE : FALSE,
                accentAbgr,
                static_cast<int>(swcaEffectType));
        }

        const bool colorUseNone =
            Shared::RegHelper::Get<DWORD>({ part, L"" }, L"NoBorderColor", 0, 1) != 0;

        bool colorUseDefault = true;
        DWORD borderColor = 0;
        if (!colorUseNone)
        {
            std::optional<DWORD> explicitColor;
            if (part == L"Root")
                explicitColor = rootSnap.borderColorArgb;
            else if (darkMode)
            {
                explicitColor = Shared::RegHelper::TryGet<DWORD>({ part, L"" }, L"DarkMode_BorderColor", 1);
            }
            else
            {
                explicitColor = Shared::RegHelper::TryGet<DWORD>({ part, L"" }, L"LightMode_BorderColor", 1);
            }

            if (explicitColor.has_value())
            {
                borderColor = explicitColor.value();
                colorUseDefault = false;
            }
        }

        DWM_WINDOW_CORNER_PREFERENCE cornerType = DWMWCP_DONOTROUND;
        if (Shared::SystemHelper::g_buildNumber >= 22000)
        {
            cornerType = static_cast<DWM_WINDOW_CORNER_PREFERENCE>(Shared::RegHelper::Get<DWORD>(
                { part, L"" },
                L"CornerType",
                static_cast<DWORD>(DWMWCP_ROUND),
                1));
        }

        ApplyBorderEffect(
            hwnd,
            darkMode,
            colorUseNone,
            colorUseDefault,
            borderColor,
            cornerType);

        DwmTransitionOwnedWindow(hwnd, DWMTRANSITION_OWNEDWINDOW_REPOSITION);
    }
} // namespace Shared::SwcaBackdrop
