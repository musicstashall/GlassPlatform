#include "SharedPch.h"
#include <OpenGlassDwmPolicy.h>
#include <OpenGlassHostState.h>
#include <Helpers/DwmGlassOpacity.h>
#include <Helpers/RegHelper.h>
#include <Helpers/ThemeHelper.h>
#include <PartRegistryQuery.h>
#include <RootSwcaRegistryQuery.h>

#include <TlHelp32.h>
#include <wil/registry.h>
#include <atomic>
#include <optional>
#include <string_view>

#ifndef WM_DWMCOLORIZATIONCOLORCHANGED
#define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#endif

namespace Shared::OpenGlassDwmPolicy
{
    namespace
    {
        constexpr wchar_t kDwmKey[] = L"Software\\Microsoft\\Windows\\DWM";

        constexpr wchar_t kGlassColorOverride[] = L"ColorizationColorOverride";
        constexpr wchar_t kGlassColorPrimary[] = L"ColorizationColor";
        constexpr wchar_t kColorBalanceOverride[] = L"ColorizationColorBalanceOverride";
        constexpr wchar_t kColorBalancePrimary[] = L"ColorizationColorBalance";
        constexpr wchar_t kBlurBalanceOverride[] = L"ColorizationBlurBalanceOverride";
        constexpr wchar_t kBlurBalancePrimary[] = L"ColorizationBlurBalance";
        constexpr wchar_t kAfterglowOverride[] = L"ColorizationAfterglowOverride";
        constexpr wchar_t kAfterglowPrimary[] = L"ColorizationAfterglow";
        constexpr wchar_t kAfterglowBalanceOverride[] = L"ColorizationAfterglowBalanceOverride";
        constexpr wchar_t kAfterglowBalancePrimary[] = L"ColorizationAfterglowBalance";
        constexpr wchar_t kGlassOpacity[] = L"GlassOpacity";
        constexpr wchar_t kGlassType[] = L"GlassType";
        constexpr wchar_t kAccentColor[] = L"AccentColor";

        constexpr std::wstring_view kDisablePolicyValue = L"OpenGlassDwmPolicyDisable";

        constexpr std::wstring_view kDwmModuleHints[] = {
            L"OpenGlass.dll",
        };

        // Platform defaults (паритет DwmColorRegistry / OpenGlassGUI).
        constexpr DWORD kPlatformColorBalancePercent = 10u;
        constexpr DWORD kPlatformBlurBalancePercent = 50u;
        constexpr DWORD kPlatformAfterglowBalancePercent = 10u;

        // GS gate-off (Win7GlassColorPolicy).
        constexpr DWORD kGateOffColorBalancePercent = 35u;
        constexpr DWORD kGateOffBlurBalancePercent = 50u;
        constexpr DWORD kGateOffAfterglowBalancePercent = 20u;

        // uxtheme/logon junk (DWM1→DWM2).
        constexpr DWORD kLogonColorBalancePercent = 89u;
        constexpr DWORD kLogonBlurBalancePercent = 1u;
        constexpr DWORD kLogonAfterglowBalancePercent = 10u;

        std::atomic<int> g_policyColorWriteDepth{0};
        std::optional<bool> g_lastKnownDarkMode{};

        constexpr std::wstring_view kCanonicalPopupRegSub = L"Menu";

        // Сброс при смене визуального стиля (OpenGlass registry reference; без GlassOpacity-пары).
        constexpr std::wstring_view kResetOnStyleChange[] = {
            L"ColorizationOpacityInactive",
            L"ColorizationOpacityInactiveMaximized",
            L"ColorizationGlassReflectionOpacityInactive",
            L"ColorizationGlassReflectionOpacityInactiveMaximized",
            L"ColorizationColorCaptionInactive",
            L"ColorizationColorCaptionInactiveMaximized",
        };

        [[nodiscard]] bool TryGetRegDword(HKEY root, const wchar_t* valueName, DWORD& out) noexcept
        {
            DWORD type = 0;
            DWORD data = 0;
            DWORD size = sizeof(data);
            const LSTATUS ls = RegGetValueW(
                root,
                kDwmKey,
                valueName,
                RRF_RT_REG_DWORD,
                &type,
                &data,
                &size);
            if (ls != ERROR_SUCCESS || type != REG_DWORD)
                return false;
            out = data;
            return true;
        }

        [[nodiscard]] bool GetDwmDwordUserThenMachine(const wchar_t* valueName, DWORD& out) noexcept
        {
            if (TryGetRegDword(HKEY_CURRENT_USER, valueName, out))
                return true;
            return TryGetRegDword(HKEY_LOCAL_MACHINE, valueName, out);
        }

        [[nodiscard]] bool DwmValueExistsUser(const wchar_t* valueName) noexcept
        {
            DWORD ignored = 0;
            return TryGetRegDword(HKEY_CURRENT_USER, valueName, ignored);
        }

        [[nodiscard]] bool OpenOrCreateDwmKeyForWrite(HKEY& outKey) noexcept
        {
            outKey = nullptr;
            return RegCreateKeyExW(
                       HKEY_CURRENT_USER,
                       kDwmKey,
                       0,
                       nullptr,
                       0,
                       KEY_SET_VALUE,
                       nullptr,
                       &outKey,
                       nullptr) == ERROR_SUCCESS;
        }

        [[nodiscard]] DWORD Rgb24FromDwmDword(DWORD dw) noexcept
        {
            return dw & 0x00FFFFFFu;
        }

        /// COLORREF DWORD (R,G,B в младших байтах) → DWM BGR (паритет DwmCaptionColorHelper).
        [[nodiscard]] DWORD ColorRefDwordToDwmBgr(DWORD colorRef) noexcept
        {
            const BYTE r = static_cast<BYTE>(colorRef & 0xFF);
            const BYTE g = static_cast<BYTE>((colorRef >> 8) & 0xFF);
            const BYTE b = static_cast<BYTE>((colorRef >> 16) & 0xFF);
            return static_cast<DWORD>(b)
                | (static_cast<DWORD>(g) << 8)
                | (static_cast<DWORD>(r) << 16);
        }

        [[nodiscard]] DWORD GradientArgbToBgrRgb(DWORD argb) noexcept
        {
            const BYTE r = static_cast<BYTE>((argb >> 16) & 0xFF);
            const BYTE g = static_cast<BYTE>((argb >> 8) & 0xFF);
            const BYTE b = static_cast<BYTE>(argb & 0xFF);
            return static_cast<DWORD>(b | (static_cast<DWORD>(g) << 8) | (static_cast<DWORD>(r) << 16));
        }

        [[nodiscard]] DWORD GradientArgbToOpacityPercent(DWORD argb) noexcept
        {
            const DWORD alpha = (argb >> 24) & 0xFFu;
            return (alpha * 100u + 127u) / 255u;
        }

        [[nodiscard]] DWORD ComposePlatformColorDword(DWORD gradientArgb) noexcept
        {
            const DWORD alpha = (gradientArgb >> 24) & 0xFFu;
            const DWORD bgr = GradientArgbToBgrRgb(gradientArgb);
            return (alpha << 24) | bgr;
        }

        /// Выбор LightMode_/DarkMode_ Menu — только COLOR_WINDOW (паритет ThemeRegistry / SWCA).
        [[nodiscard]] DWORD ResolvePlatformGradientArgb() noexcept
        {
            const bool darkMode = ThemeHelper::IsRegistryDarkModeFromColorWindow();
            return PartRegistryQuery::ReadUiGradientArgb(kCanonicalPopupRegSub, darkMode);
        }

        [[nodiscard]] bool HasActiveOverrideKey() noexcept
        {
            return DwmValueExistsUser(kGlassColorOverride)
                || DwmValueExistsUser(kColorBalanceOverride)
                || DwmValueExistsUser(kBlurBalanceOverride);
        }

        [[nodiscard]] bool HasAfterglowOverrideKey() noexcept
        {
            return DwmValueExistsUser(kAfterglowOverride)
                || DwmValueExistsUser(kAfterglowBalanceOverride);
        }

        [[nodiscard]] bool Rgb24Equal(DWORD a, DWORD b) noexcept
        {
            return (Rgb24FromDwmDword(a) & 0x00FFFFFFu) == (Rgb24FromDwmDword(b) & 0x00FFFFFFu);
        }

        [[nodiscard]] DWORD ResolvePlatformGlassGradientBgr() noexcept
        {
            return GradientArgbToBgrRgb(ResolvePlatformGradientArgb());
        }

        /// Другая ветка Menu gradient из реестра (не только OG defaults).
        [[nodiscard]] DWORD ResolveOtherBranchPlatformGlassGradientBgr() noexcept
        {
            const bool darkMode = ThemeHelper::IsRegistryDarkModeFromColorWindow();
            const DWORD argb =
                PartRegistryQuery::ReadUiGradientArgb(kCanonicalPopupRegSub, !darkMode);
            return GradientArgbToBgrRgb(argb);
        }

        [[nodiscard]] bool IsWin7GlassType() noexcept
        {
            DWORD glassType = 0;
            if (!GetDwmDwordUserThenMachine(kGlassType, glassType))
                return false;
            return glassType == 1u;
        }

        [[nodiscard]] bool TryReadOverrideDword(const wchar_t* overrideName, DWORD& out) noexcept
        {
            return GetDwmDwordUserThenMachine(overrideName, out);
        }

        [[nodiscard]] bool TryReadEffectiveOverrideOrPrimary(
            const wchar_t* primaryName,
            const wchar_t* overrideName,
            DWORD& out,
            bool& outFromOverride) noexcept
        {
            outFromOverride = false;
            if (TryGetRegDword(HKEY_CURRENT_USER, overrideName, out))
            {
                outFromOverride = true;
                return true;
            }
            if (TryGetRegDword(HKEY_LOCAL_MACHINE, overrideName, out))
            {
                outFromOverride = true;
                return true;
            }
            if (TryGetRegDword(HKEY_CURRENT_USER, primaryName, out))
                return true;
            return TryGetRegDword(HKEY_LOCAL_MACHINE, primaryName, out);
        }

        [[nodiscard]] bool TryReadSystemAccentBgr(DWORD& bgrOut) noexcept
        {
            DWORD accent = 0;
            if (GetDwmDwordUserThenMachine(kAccentColor, accent))
            {
                bgrOut = ColorRefDwordToDwmBgr(accent);
                return true;
            }
            DWORD color = 0;
            bool fromOverride = false;
            if (TryReadEffectiveOverrideOrPrimary(
                    kGlassColorPrimary, kGlassColorOverride, color, fromOverride))
            {
                bgrOut = Rgb24FromDwmDword(color);
                return true;
            }
            return false;
        }

        [[nodiscard]] DWORD PreserveAlphaCombine(DWORD previousDword, DWORD bgrRgb) noexcept
        {
            DWORD a = (previousDword >> 24) & 0xFFu;
            const DWORD rgb = bgrRgb & 0x00FFFFFFu;
            if (a == 0 && (previousDword == 0xFFFFFFFFu || previousDword == 0))
                return 0xFF000000u | rgb;
            if (a == 0)
                a = 0xFFu;
            return (a << 24) | rgb;
        }

        [[nodiscard]] bool MatchesAnyPlatformGradientBgr(DWORD bgrRgb) noexcept
        {
            const DWORD bgr = bgrRgb & 0x00FFFFFFu;
            return bgr == (ResolvePlatformGlassGradientBgr() & 0x00FFFFFFu)
                || bgr == (ResolveOtherBranchPlatformGlassGradientBgr() & 0x00FFFFFFu)
                || bgr == (GradientArgbToBgrRgb(RootSwcaRegistryQuery::kDefaultLightGradientArgb) & 0x00FFFFFFu)
                || bgr == (GradientArgbToBgrRgb(RootSwcaRegistryQuery::kDefaultDarkGradientArgb) & 0x00FFFFFFu);
        }

        [[nodiscard]] bool IsKnownLogonBalance(DWORD value, DWORD logonValue) noexcept
        {
            return value == logonValue;
        }

        [[nodiscard]] bool IsWin7ActiveGateOffPreset() noexcept
        {
            return IsWin7GlassType() && !HasActiveOverrideKey();
        }

        [[nodiscard]] bool IsWin7AfterglowGateOffPreset() noexcept
        {
            return IsWin7GlassType() && !HasAfterglowOverrideKey();
        }

        [[nodiscard]] bool IsVistaActiveGateOffPreset() noexcept
        {
            return !IsWin7GlassType() && !DwmValueExistsUser(kGlassColorOverride);
        }

        [[nodiscard]] bool ShouldWritePlatformColorOverride(DWORD platformColorDword) noexcept
        {
            if (!DwmValueExistsUser(kGlassColorOverride))
                return false;

            DWORD current = 0;
            bool fromOverride = false;
            if (!TryReadEffectiveOverrideOrPrimary(
                    kGlassColorPrimary, kGlassColorOverride, current, fromOverride))
                return true;

            const DWORD curBgr = Rgb24FromDwmDword(current);
            if ((curBgr & 0x00FFFFFFu) == (Rgb24FromDwmDword(platformColorDword) & 0x00FFFFFFu))
                return false;

            if (MatchesAnyPlatformGradientBgr(curBgr))
                return true;

            DWORD accentBgr = 0;
            if (TryReadSystemAccentBgr(accentBgr) && (curBgr & 0x00FFFFFFu) == (accentBgr & 0x00FFFFFFu))
                return true;

            return false;
        }

        [[nodiscard]] bool ShouldWritePlatformBalanceOverride(
            const wchar_t* primaryName,
            const wchar_t* overrideName,
            DWORD platformValue,
            DWORD gateOffValue,
            DWORD logonValue) noexcept
        {
            if (!DwmValueExistsUser(overrideName))
                return false;

            DWORD current = 0;
            bool fromOverride = false;
            if (!TryReadEffectiveOverrideOrPrimary(primaryName, overrideName, current, fromOverride))
                return true;

            if (current == platformValue)
                return false;

            if (current == gateOffValue)
                return false;

            if (IsKnownLogonBalance(current, logonValue))
                return true;

            return false;
        }

        [[nodiscard]] bool ShouldWritePlatformGlassOpacity(DWORD platformOpacity) noexcept
        {
            if (!DwmValueExistsUser(kGlassOpacity))
                return false;

            DWORD current = 0;
            if (!GetDwmDwordUserThenMachine(kGlassOpacity, current))
                return true;

            if (current == platformOpacity)
                return false;

            const DWORD lightOpacity =
                GradientArgbToOpacityPercent(RootSwcaRegistryQuery::kDefaultLightGradientArgb);
            const DWORD darkOpacity =
                GradientArgbToOpacityPercent(RootSwcaRegistryQuery::kDefaultDarkGradientArgb);
            if (current == lightOpacity || current == darkOpacity)
                return true;

            return false;
        }

        [[nodiscard]] bool TryDeleteGlassColorOverrideOnThemeBranchFlip(HKEY hKey) noexcept
        {
            if (!DwmValueExistsUser(kGlassColorOverride))
                return false;

            return RegDeleteValueW(hKey, kGlassColorOverride) == ERROR_SUCCESS;
        }

        [[nodiscard]] bool TryDeleteInactiveGlassOnThemeBranchFlip(HKEY hKey) noexcept
        {
            bool changed = false;
            if (DwmValueExistsUser(L"ColorizationColorInactive")
                && RegDeleteValueW(hKey, L"ColorizationColorInactive") == ERROR_SUCCESS)
            {
                changed = true;
            }

            if (DwmValueExistsUser(L"GlassOpacityInactive")
                && RegDeleteValueW(hKey, L"GlassOpacityInactive") == ERROR_SUCCESS)
            {
                changed = true;
            }

            return changed;
        }

        [[nodiscard]] bool WriteDwmDword(HKEY hKey, const wchar_t* valueName, DWORD value) noexcept
        {
            return RegSetValueExW(
                       hKey,
                       valueName,
                       0,
                       REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value),
                       sizeof(value)) == ERROR_SUCCESS;
        }

        [[nodiscard]] bool TrySyncPlatformColorToPrimary(HKEY hKey) noexcept
        {
            if (DwmValueExistsUser(kGlassColorOverride))
                return false;

            const DWORD platformColor = ComposePlatformColorDword(ResolvePlatformGradientArgb());

            DWORD current = 0;
            if (GetDwmDwordUserThenMachine(kGlassColorPrimary, current) && Rgb24Equal(current, platformColor))
                return false;

            DWORD writeColor = platformColor;
            if (GetDwmDwordUserThenMachine(kGlassColorPrimary, current))
                writeColor = PreserveAlphaCombine(current, Rgb24FromDwmDword(platformColor));

            return WriteDwmDword(hKey, kGlassColorPrimary, writeColor);
        }

        [[nodiscard]] bool TryApplyVistaDefaults(HKEY hKey) noexcept
        {
            const DWORD gradientArgb = ResolvePlatformGradientArgb();
            const DWORD platformColor = ComposePlatformColorDword(gradientArgb);
            const DWORD platformOpacity = GradientArgbToOpacityPercent(gradientArgb);

            bool changed = false;

            if (ShouldWritePlatformColorOverride(platformColor))
            {
                DWORD writeColor = platformColor;
                DWORD existing = 0;
                if (TryReadOverrideDword(kGlassColorOverride, existing))
                    writeColor = PreserveAlphaCombine(existing, Rgb24FromDwmDword(platformColor));
                if (WriteDwmDword(hKey, kGlassColorOverride, writeColor))
                    changed = true;
            }

            if (ShouldWritePlatformGlassOpacity(platformOpacity))
            {
                if (WriteDwmDword(hKey, kGlassOpacity, platformOpacity))
                    changed = true;
            }

            return changed;
        }

        [[nodiscard]] bool TryApplyWin7Defaults(HKEY hKey) noexcept
        {
            const DWORD gradientArgb = ResolvePlatformGradientArgb();
            const DWORD platformColor = ComposePlatformColorDword(gradientArgb);

            DWORD accentBgr = 0;
            (void)TryReadSystemAccentBgr(accentBgr);

            bool changed = false;

            if (ShouldWritePlatformColorOverride(platformColor))
            {
                DWORD writeColor = platformColor;
                DWORD existing = 0;
                if (TryReadOverrideDword(kGlassColorOverride, existing))
                    writeColor = PreserveAlphaCombine(existing, Rgb24FromDwmDword(platformColor));
                if (WriteDwmDword(hKey, kGlassColorOverride, writeColor))
                    changed = true;
            }

            if (ShouldWritePlatformBalanceOverride(
                    kColorBalancePrimary,
                    kColorBalanceOverride,
                    kPlatformColorBalancePercent,
                    kGateOffColorBalancePercent,
                    kLogonColorBalancePercent))
            {
                if (WriteDwmDword(hKey, kColorBalanceOverride, kPlatformColorBalancePercent))
                    changed = true;
            }

            if (ShouldWritePlatformBalanceOverride(
                    kBlurBalancePrimary,
                    kBlurBalanceOverride,
                    kPlatformBlurBalancePercent,
                    kGateOffBlurBalancePercent,
                    kLogonBlurBalancePercent))
            {
                if (WriteDwmDword(hKey, kBlurBalanceOverride, kPlatformBlurBalancePercent))
                    changed = true;
            }

            if (accentBgr != 0 && DwmValueExistsUser(kAfterglowOverride))
            {
                DWORD writeAfterglow = 0;
                DWORD existing = 0;
                if (TryReadOverrideDword(kAfterglowOverride, existing))
                    writeAfterglow = PreserveAlphaCombine(existing, accentBgr);
                else
                    writeAfterglow = PreserveAlphaCombine(0, accentBgr);

                DWORD current = 0;
                bool fromOverride = false;
                const bool hasCurrent = TryReadEffectiveOverrideOrPrimary(
                    kAfterglowPrimary, kAfterglowOverride, current, fromOverride);
                if (!hasCurrent || (Rgb24FromDwmDword(current) & 0x00FFFFFFu) != (accentBgr & 0x00FFFFFFu))
                {
                    if (WriteDwmDword(hKey, kAfterglowOverride, writeAfterglow))
                        changed = true;
                }
            }

            if (ShouldWritePlatformBalanceOverride(
                    kAfterglowBalancePrimary,
                    kAfterglowBalanceOverride,
                    kPlatformAfterglowBalancePercent,
                    kGateOffAfterglowBalancePercent,
                    kLogonAfterglowBalancePercent))
            {
                if (WriteDwmDword(hKey, kAfterglowBalanceOverride, kPlatformAfterglowBalancePercent))
                    changed = true;
            }

            return changed;
        }

        [[nodiscard]] DWORD FindDwmPid() noexcept
        {
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap == INVALID_HANDLE_VALUE)
                return 0;

            PROCESSENTRY32W pe{};
            pe.dwSize = sizeof(pe);
            if (!Process32FirstW(snap, &pe))
            {
                CloseHandle(snap);
                return 0;
            }
            do
            {
                if (_wcsicmp(pe.szExeFile, L"dwm.exe") == 0)
                {
                    const DWORD pid = pe.th32ProcessID;
                    CloseHandle(snap);
                    return pid;
                }
            } while (Process32NextW(snap, &pe));

            CloseHandle(snap);
            return 0;
        }

        [[nodiscard]] bool DwmProcessHasKnownGlassModule(DWORD pid) noexcept
        {
            if (pid == 0)
                return false;

            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            if (snap == INVALID_HANDLE_VALUE)
                return false;

            MODULEENTRY32W me{};
            me.dwSize = sizeof(me);
            bool found = false;
            if (Module32FirstW(snap, &me))
            {
                do
                {
                    for (std::wstring_view hint : kDwmModuleHints)
                    {
                        if (_wcsicmp(me.szModule, hint.data()) == 0)
                        {
                            found = true;
                            break;
                        }
                    }
                } while (!found && Module32NextW(snap, &me));
            }
            CloseHandle(snap);
            return found;
        }

        struct PolicyColorWriteScope
        {
            PolicyColorWriteScope() noexcept
            {
                g_policyColorWriteDepth.fetch_add(1, std::memory_order_acq_rel);
            }

            ~PolicyColorWriteScope() noexcept
            {
                g_policyColorWriteDepth.fetch_sub(1, std::memory_order_acq_rel);
            }

            PolicyColorWriteScope(const PolicyColorWriteScope&) = delete;
            PolicyColorWriteScope& operator=(const PolicyColorWriteScope&) = delete;
        };

        /// forceThemeBranchFlip: visual-style apply (жёсткий Phase1); иначе только dark↔light по COLOR_WINDOW.
        [[nodiscard]] bool ApplyEnvironmentColorPolicy(HKEY hKey, bool forceThemeBranchFlip) noexcept
        {
            const bool darkMode = ThemeHelper::IsRegistryDarkModeFromColorWindow();
            bool changed = false;
            const bool flip = forceThemeBranchFlip
                || (g_lastKnownDarkMode.has_value() && g_lastKnownDarkMode.value() != darkMode);
            if (flip)
            {
                // Фаза 1: удалить ColorizationColorOverride (+ Vista inactive) до Primary sync.
                changed |= TryDeleteGlassColorOverrideOnThemeBranchFlip(hKey);
                changed |= TryDeleteInactiveGlassOnThemeBranchFlip(hKey);
            }
            g_lastKnownDarkMode = darkMode;

            // Фаза 2: ColorizationColor ← Menu gradient, только если Override absent после фазы 1.
            changed |= TrySyncPlatformColorToPrimary(hKey);

            // Фаза 3: обновление существующих Override / afterglow←accent (gate ON).
            changed |= IsWin7GlassType() ? TryApplyWin7Defaults(hKey) : TryApplyVistaDefaults(hKey);
            return changed;
        }

        [[nodiscard]] bool TryApplyPlatformCompositionDefaultsCore(bool forceThemeBranchFlip) noexcept
        {
            if (!ShouldApplyDwmCompositionPolicy())
                return false;

            HKEY hKey = nullptr;
            if (!OpenOrCreateDwmKeyForWrite(hKey))
                return false;

            const auto closeKey = wil::scope_exit([&] { RegCloseKey(hKey); });

            PolicyColorWriteScope policyScope{};
            DwmGlassOpacity::BeginDwmColorizationNotifyBatch();
            const auto endBatch = wil::scope_exit([] { DwmGlassOpacity::EndDwmColorizationNotifyBatch(); });

            return ApplyEnvironmentColorPolicy(hKey, forceThemeBranchFlip);
        }
    } // namespace

    CapabilityTier QueryOpenGlassCapabilityTier() noexcept
    {
        RTL_OSVERSIONINFOW rovi{};
        rovi.dwOSVersionInfoSize = sizeof(rovi);
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        static RtlGetVersionFn pRtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (!pRtlGetVersion || pRtlGetVersion(&rovi) != 0)
            return CapabilityTier::Unknown;
        if (rovi.dwBuildNumber >= kBuildLegacyDwmGlassCutoff)
            return CapabilityTier::EffectsWithoutLegacyBlur;
        return CapabilityTier::BlurSupported;
    }

    bool IsOpenGlassNotificationWindowPresent() noexcept
    {
        return FindWindowW(L"Dwm", nullptr) != nullptr;
    }

    bool IsDwmGlassInjectedPipelinePresent() noexcept
    {
        const DWORD pid = FindDwmPid();
        return DwmProcessHasKnownGlassModule(pid);
    }

    bool IsOpenGlassBackdropPipelineActive() noexcept
    {
        const DWORD pid = FindDwmPid();
        if (DwmProcessHasKnownGlassModule(pid))
            return true;

        if (!IsOpenGlassNotificationWindowPresent())
            return false;

        return OpenGlassHostState::QueryOrchestrationUiState()
            == OpenGlassHostState::HostUiState::Running;
    }

    bool ShouldApplyDwmCompositionPolicy() noexcept
    {
        if (IsHostDwmPolicyDisabled())
            return false;

        // Живой pipeline (OpenGlass.dll / окно Dwm + orch Running) или зарегистрированы служба/задача.
        if (IsOpenGlassBackdropPipelineActive())
            return true;

        return OpenGlassHostState::OrchestrationInstalled();
    }

    bool TryReadEffectiveColorizationColors(DWORD& activeOut, DWORD& inactiveOut) noexcept
    {
        DWORD overrideVal = 0;
        const bool hasOverride = GetDwmDwordUserThenMachine(kGlassColorOverride, overrideVal);
        DWORD colorVal = 0;
        const bool hasColor = GetDwmDwordUserThenMachine(kGlassColorPrimary, colorVal);

        DWORD active = 0xFFFFFFFFu;
        if (hasOverride)
            active = overrideVal;
        else if (hasColor)
            active = colorVal;

        DWORD inactiveExplicit = 0;
        if (GetDwmDwordUserThenMachine(L"ColorizationColorInactive", inactiveExplicit))
        {
            inactiveOut = inactiveExplicit;
        }
        else
        {
            inactiveOut = active;
        }
        activeOut = active;
        return true;
    }

    bool IsPolicyColorWriteActive() noexcept
    {
        return g_policyColorWriteDepth.load(std::memory_order_acquire) > 0;
    }

    bool TryApplyPlatformCompositionDefaults() noexcept
    {
        return TryApplyPlatformCompositionDefaultsCore(/*forceThemeBranchFlip=*/false);
    }

    bool TrySyncColorizationWithWindowBackground() noexcept
    {
        return TryApplyPlatformCompositionDefaults();
    }

    void ResetInactiveDwmValuesOnVisualStyleChange() noexcept
    {
        if (!ShouldApplyDwmCompositionPolicy())
            return;

        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kDwmKey, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
            return;

        const auto closeKey = wil::scope_exit([&] { RegCloseKey(hKey); });

        for (std::wstring_view name : kResetOnStyleChange)
        {
            (void)RegDeleteValueW(hKey, name.data());
        }
    }

    void ApplyDwmPolicyOnVisualStyleChange() noexcept
    {
        if (!ShouldApplyDwmCompositionPolicy())
            return;

        ResetInactiveDwmValuesOnVisualStyleChange();
        // Жёсткий Phase1 на любой visual-style apply (не только cold g_lastKnownDarkMode).
        (void)TryApplyPlatformCompositionDefaultsCore(/*forceThemeBranchFlip=*/true);
        DwmGlassOpacity::NotifyDwmColorizationChanged();
    }

    bool IsHostDwmPolicyDisabled() noexcept
    {
        DWORD type = 0;
        DWORD data = 0;
        DWORD size = sizeof(data);
        std::wstring key(Shared::RegHelper::g_regPath);
        const LSTATUS ls = RegGetValueW(
            HKEY_CURRENT_USER,
            key.c_str(),
            kDisablePolicyValue.data(),
            RRF_RT_REG_DWORD,
            &type,
            &data,
            &size);
        return ls == ERROR_SUCCESS && type == REG_DWORD && data == 1;
    }
}
