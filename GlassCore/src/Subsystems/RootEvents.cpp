// GlassCore/src/Subsystems/RootEvents.cpp
//
// Root-эффекты (InjectionMode::Full):
//   OnRootDwmBackdropApply        — DWM backdrop: один раз из WndCompat Attach (Root_Attach / ExplorerRoot::Attach).
//   OnRootSwcaBackdrop* / ApplyRootSwcaBackdrop — SWCA для обычных root: Attach, WM_SHOWWINDOW, WM_NCACTIVATE, WM_DWMCOLORIZATIONCHANGED;
//   Win11 shell-проводник (CabinetWClass/ExploreWClass) + SWCA: IAT DwmSetWindowAttribute блокирует SYSTEMBACKDROP_TYPE;
//   ApplyRootSwcaBackdrop — единая SWCA-ветка (DWMSBT_NONE только для EffectType=1 «По умолчанию»).
//   OnRootDwmColorizationChanged  — WM_DWMCOLORIZATIONCHANGED: OpenGlass sync + переприменение RootBackdropMode
//   (Off → OnRootDetach, DWM → OnRootDwmBackdropApply, SWCA → ApplyRootSwcaBackdrop) без reload GlassCompat.
//   Shell_TrayWnd / Shell_SecondaryTrayWnd — всегда SWCA из Tray\; RootBackdropMode на трей не распространяется.
//   OnRootDetach                  — cleanup (Root_DetachAll).

#include "CorePch.h"
#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")
#include <WinApiUtils.h>
#include <Subsystems/Settings.h>
#include <Subsystems/EffectHelper.h>
#include <Helpers/DwmGlassOpacity.h>
#include <Helpers/RegHelper.h>
#include <OpenGlassDwmPolicy.h>
#include <Subsystems/Settings.h>
#include <SystemHelper.h>
#include <InjectionMode.h>
#include <DebugLog.h>
#include <RootBackdropPolicy.h>
#include <RootPolicyQuery.h>
#include <RootSwcaRegistryQuery.h>
#include <Helpers/ThemeHelper.h>
#include <ProcessBlockList.h>
#include <GlassBackdropMark.h>
#include <GlassMessages.h>
#include <Hooks/ExplorerFrame.h>
#include <Hooks/ScrollBarTheme.h>
#include <Hooks/Tab.h>
#include <Hooks/TaskbarHooks.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <Module/ModuleHandler.h>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace
{
    constexpr wchar_t kDwmKeyPath[] = L"Software\\Microsoft\\Windows\\DWM";

    // BWM-совместимые дефолты ARGB (как DropDown)
    constexpr DWORD kLightGradientDefault = 0x9EDDDDDD;
    constexpr DWORD kDarkGradientDefault  = 0x412B2B2B;

    struct SwcaApplySignature
    {
        bool  active                 = false;
        bool  darkMode               = false;
        DWORD effectType             = 0;
        DWORD gradientArgb           = 0;
        DWORD rootBackdropMode           = 0;
        DWORD borderCorner           = 0;
        DWORD borderColor            = 0;
        bool  borderColorUseDefault  = false;
        bool  borderColorUseNone     = false;
    };

    std::mutex g_swcaApplyCacheMutex;
    std::unordered_map<HWND, SwcaApplySignature> g_swcaApplyCache;

    thread_local const wchar_t* g_nextSwcaApplyReason = L"UNSPECIFIED";

    [[nodiscard]] bool SwcaSignaturesEqual(const SwcaApplySignature& a, const SwcaApplySignature& b) noexcept
    {
        return a.active == b.active
            && a.darkMode == b.darkMode
            && a.effectType == b.effectType
            && a.gradientArgb == b.gradientArgb
            && a.rootBackdropMode == b.rootBackdropMode
            && a.borderCorner == b.borderCorner
            && a.borderColor == b.borderColor
            && a.borderColorUseDefault == b.borderColorUseDefault
            && a.borderColorUseNone == b.borderColorUseNone;
    }

    [[nodiscard]] const wchar_t* ConsumeSwcaApplyReason() noexcept
    {
        const wchar_t* r = g_nextSwcaApplyReason ? g_nextSwcaApplyReason : L"UNSPECIFIED";
        g_nextSwcaApplyReason = L"UNSPECIFIED";
        return r;
    }

    [[nodiscard]] bool TryGetDwmDword(const wchar_t* valueName, DWORD& out) noexcept
    {
        DWORD type = 0, data = 0, size = sizeof(data);
        if (RegGetValueW(HKEY_CURRENT_USER, kDwmKeyPath, valueName,
                         RRF_RT_REG_DWORD, &type, &data, &size) == ERROR_SUCCESS)
        { out = data; return true; }
        if (RegGetValueW(HKEY_LOCAL_MACHINE, kDwmKeyPath, valueName,
                         RRF_RT_REG_DWORD, &type, &data, &size) == ERROR_SUCCESS)
        { out = data; return true; }
        return false;
    }

    void TrySetDwmDword(const wchar_t* valueName, DWORD value) noexcept
    {
        HKEY hk = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kDwmKeyPath, 0, nullptr,
                            0, KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS)
        {
            RegSetValueExW(hk, valueName, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&value), sizeof(value));
            RegCloseKey(hk);
        }
    }

    [[nodiscard]] std::uint32_t AlphaByteFromPercent(std::uint32_t percent0_100) noexcept
    {
        if (percent0_100 > 100) percent0_100 = 100;
        return (percent0_100 * 255u + 50u) / 100u;
    }

    [[nodiscard]] std::uint32_t PercentFromAlphaByte(std::uint32_t alpha0_255) noexcept
    {
        if (alpha0_255 > 255) alpha0_255 = 255;
        return (alpha0_255 * 100u + 127u) / 255u;
    }

    /// Градиент root SWCA: Root\Active | Root\Inactive (Inactive → Root\ parent), см. RootSwcaRegistryQuery.
    [[nodiscard]] DWORD ReadRootSwcaGradientColor(bool active) noexcept
    {
        const bool darkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();
        return Shared::RootSwcaRegistryQuery::ReadRootGradientArgb(darkMode, active);
    }

    /// Синхронизация цвета/альфы градиента с параметрами OpenGlass в DWM-ветке реестра.
    /// Если значения изменились, возвращает true (нужна рассылка WM_DWMCOLORIZATIONCHANGED).
    [[nodiscard]] bool SyncOpenGlassFromRootGradients() noexcept
    {
        // Host OpenGlassDwmPolicy владеет DWM composition keys при active OG pipeline (Vista + Win7).
        if (Shared::OpenGlassDwmPolicy::IsOpenGlassBackdropPipelineActive()
            || Shared::OpenGlassDwmPolicy::IsPolicyColorWriteActive())
            return false;

        const DWORD activeArgb   = ReadRootSwcaGradientColor(true);
        const DWORD inactiveArgb = ReadRootSwcaGradientColor(false);

        // ARGB → BGR (OpenGlass ColorizationColor* = 0x00BBGGRR)
        auto argbToBgr = [](DWORD argb) -> DWORD {
            const BYTE r = static_cast<BYTE>((argb >> 16) & 0xFF);
            const BYTE g = static_cast<BYTE>((argb >> 8) & 0xFF);
            const BYTE b = static_cast<BYTE>(argb & 0xFF);
            return RGB(r, g, b);
        };

        const DWORD activeColor   = argbToBgr(activeArgb);
        const DWORD inactiveColor = argbToBgr(inactiveArgb);
        const DWORD activeAlpha   = PercentFromAlphaByte((activeArgb >> 24) & 0xFF);
        const DWORD inactiveAlpha = PercentFromAlphaByte((inactiveArgb >> 24) & 0xFF);

        bool changed = false;
        DWORD cur = 0;

        const bool hasColorOverride = TryGetDwmDword(L"ColorizationColorOverride", cur);
        const wchar_t* const activeColorKey = hasColorOverride
            ? L"ColorizationColorOverride"
            : L"ColorizationColor";

        if (hasColorOverride)
        {
            if (TryGetDwmDword(activeColorKey, cur) && cur != activeColor)
            {
                TrySetDwmDword(activeColorKey, activeColor);
                changed = true;
            }
        }
        else if (!TryGetDwmDword(activeColorKey, cur) || cur != activeColor)
        {
            TrySetDwmDword(activeColorKey, activeColor);
            changed = true;
        }

        if (TryGetDwmDword(L"ColorizationColorInactive", cur) && cur != inactiveColor)
        {
            TrySetDwmDword(L"ColorizationColorInactive", inactiveColor);
            changed = true;
        }

        if (TryGetDwmDword(L"GlassOpacity", cur) && cur != activeAlpha)
        {
            TrySetDwmDword(L"GlassOpacity", activeAlpha);
            changed = true;
        }

        if (TryGetDwmDword(L"GlassOpacityInactive", cur) && cur != inactiveAlpha)
        {
            TrySetDwmDword(L"GlassOpacityInactive", inactiveAlpha);
            changed = true;
        }

        return changed;
    }
} // namespace

namespace GlassCore::Root
{
    namespace Detail
    {
        std::atomic<Shared::InjectionMode> g_injectionMode{ Shared::InjectionMode::PopupOnly };
        void (WINAPI* g_trackedRootsBackdropApplier)() noexcept = nullptr;
        bool (WINAPI* g_queryRootIsAttached)(HWND) noexcept = nullptr;
        void (WINAPI* g_requestRootAttach)(HWND) noexcept = nullptr;
    }

    void RegisterTrackedRootsBackdropApplier(void (WINAPI* fn)() noexcept) noexcept
    {
        Detail::g_trackedRootsBackdropApplier = fn;
    }

    void RegisterRootIsAttachedQuery(bool (WINAPI* fn)(HWND) noexcept) noexcept
    {
        Detail::g_queryRootIsAttached = fn;
    }

    void RegisterRootAttachRequest(void (WINAPI* fn)(HWND) noexcept) noexcept
    {
        Detail::g_requestRootAttach = fn;
    }

    void RequestRootAttachForHwnd(HWND hWnd) noexcept
    {
        const auto fn = Detail::g_requestRootAttach;
        if (fn && hWnd)
            fn(hWnd);
    }

    bool IsFullInjectionMode() noexcept
    {
        return Detail::g_injectionMode.load(std::memory_order_relaxed) == Shared::InjectionMode::Full;
    }

    /// NoBackdrop + InjectionMode Full (не в GlassShared: static lib — отдельная копия на DLL).
    [[nodiscard]] bool IsRootGlassVisualsAllowed() noexcept
    {
        return IsFullInjectionMode()
            && Shared::RootPolicyQuery::IsRootGlassRenderingEnabled();
    }

    namespace
    {

        [[nodiscard]] bool IsShellTrayWindowClass(HWND hRoot) noexcept
        {
            wchar_t cls[256]{};
            if (GetClassNameW(hRoot, cls, _countof(cls)) <= 0)
                return false;
            return _wcsicmp(cls, L"Shell_TrayWnd") == 0 || _wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0;
        }

        /// Внутренние top-level проводника с Root_Attach, но без root-backdrop (CtrlNotifySink и т.п.).
        [[nodiscard]] bool IsExplorerAuxiliaryRootClass(HWND hRoot) noexcept
        {
            wchar_t cls[256]{};
            if (GetClassNameW(hRoot, cls, _countof(cls)) <= 0)
                return false;
            static constexpr const wchar_t* kAux[] = {
                L"CtrlNotifySink",
                L"NamespaceTreeControl",
                L"DUIViewWndClassName",
                L"Shell Preview Extension Host",
                L"TITLE_BAR_SCAFFOLDING_WINDOW_CLASS",
            };
            for (const wchar_t* aux : kAux)
            {
                if (_wcsicmp(cls, aux) == 0)
                    return true;
            }
            return false;
        }

        [[nodiscard]] bool ShouldApplyColorizationPerRoot(HWND hRoot) noexcept
        {
            if (!hRoot || !IsWindow(hRoot))
                return false;
            if (GetAncestor(hRoot, GA_ROOT) != hRoot)
                return false;
            if (IsShellTrayWindowClass(hRoot))
                return true;
            if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
                return true;
            if (IsExplorerAuxiliaryRootClass(hRoot))
                return false;
            if (Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hRoot))
                return true;
            if (Detail::g_queryRootIsAttached && Detail::g_queryRootIsAttached(hRoot))
                return true;
            return false;
        }

        [[nodiscard]] bool ResolveRootVisuallyActiveForColorization(
            HWND hRoot,
            LPARAM liveRootScope) noexcept
        {
            const LPARAM pivot = WndCompat::GlassLiveRootScopePivot(liveRootScope);
            if (pivot == WndCompat::kGlassLiveRootScopeActive)
                return true;
            if (pivot == WndCompat::kGlassLiveRootScopeInactive)
                return false;

            const HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            const HWND rWnd = GetAncestor(hRoot, GA_ROOT);
            const HWND rFg = GetAncestor(fg, GA_ROOT);
            return rWnd && rWnd == rFg;
        }

        /// Стандартный guard для root callbacks: Full mode + не layered (кроме tray).
        /// hRoot == nullptr — process-wide dispatch (WM_GLASS_DWMCOLORIZATION_DISPATCH), не skip.
        [[nodiscard]] bool ShouldSkipRoot(HWND hRoot) noexcept
        {
            if (!IsFullInjectionMode())
                return true;
            if (!hRoot)
                return false;
            const bool isTray = IsShellTrayWindowClass(hRoot);
            if (!isTray && (GetWindowLongW(hRoot, GWL_EXSTYLE) & WS_EX_LAYERED))
                return true;
            return false;
        }

        /// После SWCA на Shell_*Tray* ApplyEffect может сбросить Fill — перечитать Tray\ и переутвердить TAP.
        void ReloadTaskbarXamlPatchesAfterSwcaApply() noexcept
        {
            const HMODULE hTb = GetModuleHandleW(L"TaskbarCompat.dll");
            if (!hTb)
                return;
            using ReloadFn = void(WINAPI*)() noexcept;
            const auto pfn = reinterpret_cast<ReloadFn>(
                GetProcAddress(hTb, "TaskbarCompat_ReloadSettingsAndReapply"));
            if (pfn)
                pfn();
        }

#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

        /// FileBrows HookManager::SetWindowBlur — только DWM-часть до SWCA (без их SetWindowCompositionAttribute).
        /// Возвращает true, если включили BlurBehind и его нельзя сбрасывать перед ApplyBackdropEffect.
        [[nodiscard]] bool ApplyFileExplorerShellRootBackdropPrepBeforeSwca(
            HWND hRoot,
            const GlassCore::Settings::WindowBackdropEffectContext& regBackdrop,
            unsigned long build) noexcept
        {
            if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
                return false;

            const DWORD et = regBackdrop.effectType;

            if (build >= 22500)
            {
                // FileBrows SetWindowBlur (>=22500): сначала DwmEnableBlurBehindWindow(TRUE), затем SWCA.
                if (et >= 1)
                {
                    (void)Shared::WinApiUtils::EnableBlurBehindWindow(hRoot, true, false, nullptr);
                    return true;
                }
            }
            // Win11 22000–22499: DWMWA_CAPTION_COLOR после SWCA — в ApplyRootSwcaBackdrop (порядок как FileBrows StartAero → caption).

            return false;
        }

        /// Общая логика SWCA применения backdrop для root-окон.
        void ApplyRootSwcaBackdrop(HWND hRoot, bool active) noexcept
        {
            if (!hRoot || !IsWindow(hRoot))
                return;
            if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
                return;

            const wchar_t* const applyReason = ConsumeSwcaApplyReason();

            // ── Панель задач: отдельная ветка реестра \Tray ──────────────────────
            if (IsShellTrayWindowClass(hRoot))
            {
                // Live-переключение BorderDrawMode: SWCA IAT вкл. только на DWM-пути.
                GlassCore::Hooks::TaskbarHooks::SyncHooksForTrayPaintPath();

                const bool registryDark = Shared::ThemeHelper::IsRegistryDarkModeFromColorWindow();
                const bool effectiveDark = Shared::ThemeHelper::ResolveEffectiveDarkMode();
                GlassCore::Settings::WindowBackdropEffectContext trayBackdrop{};
                GlassCore::Settings::QueryTrayBackdropEffectContext(registryDark, trayBackdrop);
                GlassCore::Settings::BorderContext trayBorder{};
                GlassCore::Settings::QueryTrayBorderContext(registryDark, trayBorder);

                // Как у popup (DropDown::ApplyEffect / ветка SWCA root): перед SWCA снимаем blur/extend,
                // затем единый ApplyEffect (dark + backdrop + border + transition). Анимацию трея не подключаем.
                (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hRoot, false);
                (void)Shared::WinApiUtils::EnableBlurBehindWindow(hRoot, false);

                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCore][Root] Tray border color=0x%08X useNone=%u useDefault=%u cornerType=%u hwnd=0x%p",
                    trayBorder.color,
                    trayBorder.colorUseNone ? 1u : 0u,
                    trayBorder.colorUseDefault ? 1u : 0u,
                    static_cast<unsigned>(trayBorder.cornerType),
                    hRoot);

                GlassCore::Settings::ApplyEffect(hRoot, effectiveDark, trayBackdrop, trayBorder);
                (void)DwmFlush();

                const bool foregroundMatchLog = [&]() noexcept -> bool {
                    const HWND fg = GetForegroundWindow();
                    if (!fg)
                        return false;
                    const HWND rWnd = GetAncestor(hRoot, GA_ROOT);
                    const HWND rFg  = GetAncestor(fg, GA_ROOT);
                    return rWnd && rWnd == rFg;
                }();

                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][Root] Tray SWCA %s hwnd=0x%p effectType=%u",
                    foregroundMatchLog ? L"foregroundMatch" : L"noForegroundMatch",
                    hRoot,
                    trayBackdrop.effectType);

                Shared::RootBackdropPolicy::NotifyGlassAppliedRootBackdrop(hRoot);
                ReloadTaskbarXamlPatchesAfterSwcaApply();
                return;
            }

            const bool darkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();

            GlassCore::Settings::WindowBackdropEffectContext backdrop{};
            GlassCore::Settings::QueryRootBackdropEffectContext(darkMode, active, backdrop);
            // gradientColor в Query для Root не задаётся (popups-only); SWCA-тинт — ReadRootSwcaGradientColor(active|inactive).

            GlassCore::Settings::BorderContext border{};
            GlassCore::Settings::QueryRootBorderContext(darkMode, active, border);

            const bool explorerShellRoot =
                Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot);

            SwcaApplySignature sig{};
            sig.active = active;
            sig.darkMode = darkMode;
            sig.effectType = backdrop.effectType;
            sig.rootBackdropMode = backdrop.rootBackdropMode;
            sig.borderCorner = static_cast<DWORD>(border.cornerType);
            sig.borderColor = border.color;
            sig.borderColorUseDefault = border.colorUseDefault;
            sig.borderColorUseNone = border.colorUseNone;

            const DWORD rootGradientArgb = ReadRootSwcaGradientColor(active);
            sig.gradientArgb = rootGradientArgb;

            {
                std::lock_guard lock(g_swcaApplyCacheMutex);
                const auto it = g_swcaApplyCache.find(hRoot);
                if (it != g_swcaApplyCache.end() && SwcaSignaturesEqual(it->second, sig))
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][Root] SWCA skip (cache) reason=%s %s hwnd=0x%p effType=%u gradient=0x%08X",
                        applyReason,
                        active ? L"active" : L"inactive",
                        hRoot,
                        backdrop.effectType,
                        rootGradientArgb);
                    return;
                }
            }

            // Для режима "По умолчанию" (EffectType=1) повторяем политику донора:
            // не сбрасываем системный backdrop/caption на каждом NCPAINT, чтобы не терять
            // нативный полупрозрачный фрейм и его плавный active/inactive переход.
            if (explorerShellRoot
                && Shared::SystemHelper::g_buildNumber >= 22000
                && backdrop.effectType == 1)
            {
                const DWM_SYSTEMBACKDROP_TYPE noneType = DWMSBT_NONE;
                (void)DwmSetWindowAttribute(hRoot, DWMWA_SYSTEMBACKDROP_TYPE, &noneType, sizeof(noneType));
                constexpr DWORD kDwmwaCaptionImmersiveCompat = 1029u;
                DWORD zero = 0;
                (void)DwmSetWindowAttribute(hRoot, kDwmwaCaptionImmersiveCompat, &zero, sizeof(zero));
            }

            (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hRoot, false);

            const bool keepExplorerBlur = explorerShellRoot
                && ApplyFileExplorerShellRootBackdropPrepBeforeSwca(
                    hRoot, backdrop, Shared::SystemHelper::g_buildNumber);

            if (!keepExplorerBlur)
                (void)Shared::WinApiUtils::EnableBlurBehindWindow(hRoot, false);

            // Root SWCA: DWM colorization / COLOR_WINDOW и GlassOpacity (active|inactive). Без MergeGlassOpacityIntoArgb —
            // альфа уже в ReadRootSwcaGradientColor(active).
            backdrop.gradientColor = rootGradientArgb;

            GlassCore::EffectHelper::EnableWindowDarkMode(hRoot, darkMode);
            GlassCore::Settings::WindowBackdropEffectContext mappedBackdrop = backdrop;
            // effectType уже engine-нативный (0-8), маппинг не нужен.
            GlassCore::Settings::ApplyBackdropEffect(hRoot, darkMode, mappedBackdrop);

            // FileBrows SetWindowBlur Win11 22000–22499: после SWCA — DWMWA_CAPTION_COLOR
            // (донор: effType 0/1 с StartAero; у нас UI-типы 1–4, Mica — kRootEngineEffectMica).
            if (explorerShellRoot
                && Shared::SystemHelper::g_buildNumber >= 22000
                && Shared::SystemHelper::g_buildNumber < 22500
                && backdrop.effectType >= 1
                && backdrop.effectType != GlassCore::Settings::kRootEngineEffectMica)
            {
                const COLORREF cr = RGB(
                    static_cast<BYTE>((rootGradientArgb >> 16) & 0xFF),
                    static_cast<BYTE>((rootGradientArgb >> 8) & 0xFF),
                    static_cast<BYTE>(rootGradientArgb & 0xFF));
                (void)DwmSetWindowAttribute(hRoot, DWMWA_CAPTION_COLOR, &cr, sizeof(cr));
            }

            // Win11 «Панель управления» (CabinetWClass без вызова DwmExtendFrameIntoClientArea из explorerframe):
            // IAT хук не сработал → маркер отсутствует → принудительно задаём full-frame margins.
            // Обычный проводник уже получил {-1,-1,-1,-1} через IAT хук в ExplorerFrameHooks.
            if (explorerShellRoot && Shared::SystemHelper::g_buildNumber >= 22000
                && !GlassCore::ExplorerFrameControl::DidExplorerIATExtendApply(hRoot))
            {
                (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hRoot, true);
            }

            GlassCore::Settings::ApplyBorderEffect(hRoot, darkMode, border);
            DwmTransitionOwnedWindow(hRoot, DWMTRANSITION_OWNEDWINDOW_REPOSITION);

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Root] SWCA reason=%s %s hwnd=0x%p gradientARGB=0x%08X "
                L"A=%u R=%u G=%u B=%u effType=%u",
                applyReason,
                active ? L"active" : L"inactive",
                hRoot,
                rootGradientArgb,
                (rootGradientArgb >> 24) & 0xFFu,
                (rootGradientArgb >> 16) & 0xFFu,
                (rootGradientArgb >> 8) & 0xFFu,
                rootGradientArgb & 0xFFu,
                backdrop.effectType);

            {
                std::lock_guard lock(g_swcaApplyCacheMutex);
                g_swcaApplyCache[hRoot] = sig;
            }

            Shared::RootBackdropPolicy::NotifyGlassAppliedRootBackdrop(hRoot);

            if (explorerShellRoot)
                (void)DwmFlush();
        }
    } // namespace

    DWORD ReadRootSwcaGradientArgb(bool visuallyActive) noexcept
    {
        return ReadRootSwcaGradientColor(visuallyActive);
    }

    void SetInjectionMode(DWORD mode) noexcept
    {
        const Shared::InjectionMode previous =
            Detail::g_injectionMode.load(std::memory_order_relaxed);
        const auto next = static_cast<Shared::InjectionMode>(mode);
        Detail::g_injectionMode.store(next, std::memory_order_relaxed);
        // GlassCompat вызывает GlassCore_SetInjectionMode ПОСЛЕ LoadLibrary(GlassCore), т.е. после DllMain →
        // Engine::Initialize → ExplorerFrameControl::Startup. К тому моменту IsFullInjectionMode() ещё был PopupOnly,
        // g_bgHooksEnabled остался false. Пересчитать флаг фоновых IAT-хуков проводника сразу после режима.
        GlassCore::ExplorerFrameControl::SyncExplorerFrameBackdropHookFromRegistry();
        if (IsFullInjectionMode())
        {
            GlassCore::ModuleHandler::ScanFullScopedModules(true);
        }
        else if (previous == Shared::InjectionMode::Full)
        {
            // Live AttachList downgrade: снять Full- и backdrop-scoped IAT (как при NoBackdrop OFF).
            GlassCore::ModuleHandler::ScanFullScopedModules(false);
            GlassCore::ModuleHandler::ScanBackdropScopedModules(false);
        }
    }

    void OnRootDwmBackdropApplyImpl(HWND hRoot, bool visuallyActive) noexcept
    {
        if (ShouldSkipRoot(hRoot))
            return;
        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            return;

        const bool darkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();

        if (IsShellTrayWindowClass(hRoot))
        {
            ApplyRootSwcaBackdrop(hRoot, visuallyActive);
            return;
        }

        GlassCore::Settings::WindowBackdropEffectContext backdrop{};
        GlassCore::Settings::QueryRootBackdropEffectContext(darkMode, visuallyActive, backdrop);

        if (backdrop.rootBackdropMode != GlassCore::Settings::kRootBackdropModeDwm)
            return; // SWCA — обрабатывается в OnRootSwcaBackdropActiveApply

        GlassCore::Settings::BorderContext border{};
        GlassCore::Settings::QueryRootBorderContext(darkMode, visuallyActive, border);

        GlassCore::EffectHelper::EnableWindowDarkMode(hRoot, darkMode);

        const bool win11ExplorerShell =
            Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot)
            && Shared::SystemHelper::g_buildNumber >= 22000;

        if (!win11ExplorerShell)
        {
            // Сбрасываем SWCA и оба DWM-эффекта перед применением нового
            GlassCore::EffectHelper::SetWindowBackdrop(hRoot, FALSE, 0, GlassCore::EffectHelper::EffectType::None);
            (void)Shared::WinApiUtils::EnableBlurBehindWindow(hRoot, false);
            (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hRoot, false);

            const HRESULT hr = Shared::WinApiUtils::EnableBlurBehindWindow(hRoot, true);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Root] DwmBlurBehind hwnd=0x%p hr=0x%08X", hRoot, hr);
        }
        else
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Root] DwmBackdrop Win11 explorer shell: skip blur/SBT reset on attach hwnd=0x%p",
                hRoot);
        }

        // Паритет SWCA-ветки: IAT explorerframe часто не ставится (см. EnableHooksOnModule FAILED) —
        // без ExtendFrame OG-glass на Cabinet/Explore не появляется до WM_NCACTIVATE.
        if (win11ExplorerShell
            && !GlassCore::ExplorerFrameControl::DidExplorerIATExtendApply(hRoot))
        {
            (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hRoot, true);
        }

        GlassCore::Settings::ApplyBorderEffect(hRoot, darkMode, border);
        DwmTransitionOwnedWindow(hRoot, DWMTRANSITION_OWNEDWINDOW_REPOSITION);

        Shared::RootBackdropPolicy::NotifyGlassAppliedRootBackdrop(hRoot);
    }

    void OnRootDwmBackdropApply(HWND hRoot)
    {
        const bool visuallyActive = ResolveRootVisuallyActiveForColorization(
            hRoot, WndCompat::kGlassLiveRootScopeInfer);
        OnRootDwmBackdropApplyImpl(hRoot, visuallyActive);
    }

    void OnRootSwcaBackdropActiveApply(HWND hRoot)
    {
        if (ShouldSkipRoot(hRoot))
            return;
        ApplyRootSwcaBackdrop(hRoot, /*active=*/true);
    }

    void OnRootSwcaBackdropInactiveApply(HWND hRoot)
    {
        if (ShouldSkipRoot(hRoot))
            return;
        ApplyRootSwcaBackdrop(hRoot, /*active=*/false);
    }

    void ApplyRootSwcaCaptionColorTransition(HWND hRoot, bool active) noexcept
    {
        if (ShouldSkipRoot(hRoot))
            return;
        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            return;
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            return;

        const bool darkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();

        GlassCore::Settings::WindowBackdropEffectContext backdrop{};
        GlassCore::Settings::QueryRootBackdropEffectContext(darkMode, active, backdrop);

        if (backdrop.rootBackdropMode != GlassCore::Settings::kRootBackdropModeSwca)
            return;

        GlassCore::Settings::BorderContext border{};
        GlassCore::Settings::QueryRootBorderContext(darkMode, active, border);

        const DWORD rootGradientArgb = ReadRootSwcaGradientColor(active);

        SwcaApplySignature sig{};
        sig.active = active;
        sig.darkMode = darkMode;
        sig.effectType = backdrop.effectType;
        sig.rootBackdropMode = backdrop.rootBackdropMode;
        sig.borderCorner = static_cast<DWORD>(border.cornerType);
        sig.borderColor = border.color;
        sig.borderColorUseDefault = border.colorUseDefault;
        sig.borderColorUseNone = border.colorUseNone;
        sig.gradientArgb = rootGradientArgb;

        {
            std::lock_guard lock(g_swcaApplyCacheMutex);
            const auto it = g_swcaApplyCache.find(hRoot);
            if (it != g_swcaApplyCache.end() && SwcaSignaturesEqual(it->second, sig))
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][Root] SWCA CAPTION_COLOR skip (cache) %s hwnd=0x%p effType=%u gradient=0x%08X",
                    active ? L"active" : L"inactive",
                    hRoot,
                    backdrop.effectType,
                    rootGradientArgb);
                return;
            }
        }

        backdrop.gradientColor = rootGradientArgb;

        GlassCore::EffectHelper::EnableWindowDarkMode(hRoot, darkMode);
        GlassCore::Settings::WindowBackdropEffectContext mappedBackdrop = backdrop;
        GlassCore::Settings::ApplyBackdropEffect(hRoot, darkMode, mappedBackdrop, /*skipNcRendering=*/true);
        GlassCore::Settings::ApplyBorderEffect(hRoot, darkMode, border);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][Root] SWCA CAPTION_COLOR light %s hwnd=0x%p gradientARGB=0x%08X effType=%u",
            active ? L"active" : L"inactive",
            hRoot,
            rootGradientArgb,
            backdrop.effectType);

        {
            std::lock_guard lock(g_swcaApplyCacheMutex);
            g_swcaApplyCache[hRoot] = sig;
        }

        Shared::RootBackdropPolicy::NotifyGlassAppliedRootBackdrop(hRoot);
    }

    void ApplyRootBorderOnNcActivateChange(HWND hRoot, bool isActive) noexcept
    {
        if (ShouldSkipRoot(hRoot))
            return;
        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            return;
        if (IsExplorerAuxiliaryRootHwnd(hRoot))
            return;
        if (IsShellTrayWindowClass(hRoot))
            return;
        if (!Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hRoot))
            return;
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;

        const bool darkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();
        const bool explorerShell =
            Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot);

        // Не-explorer ET 1–5 SWCA: Win11 не вызывает explorer CAPTION_COLOR hook — полный reapply на NC.
        if (!explorerShell)
        {
            GlassCore::Settings::WindowBackdropEffectContext backdrop{};
            GlassCore::Settings::QueryRootBackdropEffectContext(darkMode, isActive, backdrop);

            if (backdrop.rootBackdropMode == GlassCore::Settings::kRootBackdropModeSwca
                && GlassCore::Settings::IsRootSwcaEngineEffectType(backdrop.effectType))
            {
                RemoveRootSwcaApplyCacheEntry(hRoot);
                SetNextRootSwcaApplyReason(L"NCACTIVATE");
                if (isActive)
                    OnRootSwcaBackdropActiveApply(hRoot);
                else
                    OnRootSwcaBackdropInactiveApply(hRoot);
                return;
            }
        }

        GlassCore::Settings::BorderContext border{};
        GlassCore::Settings::QueryRootBorderContext(darkMode, isActive, border);
        GlassCore::Settings::ApplyBorderEffect(hRoot, darkMode, border);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][Root] NC border hwnd=0x%p active=%u color=0x%08X useDefault=%u useNone=%u corner=%u",
            hRoot,
            isActive ? 1u : 0u,
            border.color,
            border.colorUseDefault ? 1u : 0u,
            border.colorUseNone ? 1u : 0u,
            static_cast<unsigned>(border.cornerType));
    }

    void SetNextRootSwcaApplyReason(const wchar_t* reason) noexcept
    {
        g_nextSwcaApplyReason = (reason && reason[0]) ? reason : L"UNSPECIFIED";
    }

    void ClearRootSwcaApplyCache() noexcept
    {
        std::lock_guard lock(g_swcaApplyCacheMutex);
        g_swcaApplyCache.clear();
    }

    void RemoveRootSwcaApplyCacheEntry(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        std::lock_guard lock(g_swcaApplyCacheMutex);
        (void)g_swcaApplyCache.erase(hRoot);
    }

    void ApplyRootBackdropPolicyForHwnd(HWND hRoot, bool isActive) noexcept;
    void ApplyRootLiveCompositionForHwnd(HWND hRoot, bool isActive) noexcept;
    void ReapplyOpenRootBackdrops() noexcept;

    namespace
    {
        std::atomic<uint32_t> s_reappliedColorizationWaveId{ 0 };

        struct ReapplyPolicyFingerprint
        {
            DWORD rootBackdropMode       = 0xFFFFFFFFu;
            DWORD gradientActiveArgb = 0;
            DWORD gradientInactiveArgb = 0;
            bool  glassEnabled       = false;
        };

        ReapplyPolicyFingerprint s_lastReapplyFingerprint{};
        bool s_hasLastReapplyFingerprint = false;

        [[nodiscard]] ReapplyPolicyFingerprint BuildReapplyPolicyFingerprint() noexcept
        {
            ReapplyPolicyFingerprint fp{};
            fp.rootBackdropMode = Shared::RootPolicyQuery::ReadRootBackdropMode();
            fp.gradientActiveArgb = ReadRootSwcaGradientColor(true);
            fp.gradientInactiveArgb = ReadRootSwcaGradientColor(false);
            fp.glassEnabled = IsRootGlassVisualsAllowed();
            return fp;
        }

        [[nodiscard]] bool ReapplyFingerprintsEqual(
            const ReapplyPolicyFingerprint& a,
            const ReapplyPolicyFingerprint& b) noexcept
        {
            return a.rootBackdropMode == b.rootBackdropMode
                && a.gradientActiveArgb == b.gradientActiveArgb
                && a.gradientInactiveArgb == b.gradientInactiveArgb
                && a.glassEnabled == b.glassEnabled;
        }

        // NONE всегда требует sweep (detach); при ENABLE — пропуск, если снимок политики не менялся.
        [[nodiscard]] bool ShouldSkipReapplySweep(const ReapplyPolicyFingerprint& fp) noexcept
        {
            if (!fp.glassEnabled)
                return false;
            if (!s_hasLastReapplyFingerprint)
                return false;
            return ReapplyFingerprintsEqual(fp, s_lastReapplyFingerprint);
        }

        void RememberReapplyFingerprint(const ReapplyPolicyFingerprint& fp) noexcept
        {
            s_lastReapplyFingerprint = fp;
            s_hasLastReapplyFingerprint = true;
        }

        void ResetReapplyFingerprintState() noexcept
        {
            s_hasLastReapplyFingerprint = false;
        }

        // HWND_BROADCAST: у каждого top-level своя очередь — wave=1 может прийти после wave=6.
        // Только монотонно растущий wave (wave > prev). wave==0 (системный DWM) не запускает Reapply —
        // платформа рассылает wave>=1 через NotifyDwmColorizationChanged; coalesce — в WndCompat control window.
        [[nodiscard]] bool TryConsumeReapplyForWave(uint32_t wave) noexcept
        {
            if (wave == 0u)
                return false;

            uint32_t prev = s_reappliedColorizationWaveId.load(std::memory_order_acquire);
            if (wave <= prev)
                return false;

            while (!s_reappliedColorizationWaveId.compare_exchange_weak(
                prev, wave, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                if (wave <= prev)
                    return false;
            }
            return true;
        }
        [[nodiscard]] bool IsExplorerAuxiliaryRootHwndImpl(HWND hRoot) noexcept
        {
            return IsExplorerAuxiliaryRootClass(hRoot);
        }

    } // namespace

#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFEu
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

    namespace
    {
        [[nodiscard]] bool IsWin11FileExplorerShellRoot(HWND hRoot) noexcept
        {
            return Shared::SystemHelper::g_buildNumber >= 22000
                && Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot);
        }

        [[nodiscard]] bool IsExplorerHostBarBridgeClassName(const wchar_t* cls) noexcept
        {
            if (!cls)
                return false;
            return wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0
                || wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0;
        }

        [[nodiscard]] int MeasureExplorerHostBarBridgeHeight(HWND hRoot) noexcept
        {
            if (!hRoot || !IsWindow(hRoot))
                return 0;

            RECT rcRootClient{};
            if (!GetClientRect(hRoot, &rcRootClient))
                return 0;

            POINT rootClientOrigin{ 0, 0 };
            ClientToScreen(hRoot, &rootClientOrigin);
            const int clientH = rcRootClient.bottom - rcRootClient.top;
            if (clientH <= 0)
                return 0;

            constexpr int kHostBarMaxBandPx = 148;
            const int bandPx = (std::min)(kHostBarMaxBandPx, clientH / 3);

            int bestHeight = 0;
            for (HWND child = GetWindow(hRoot, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                wchar_t cls[160]{};
                if (GetClassNameW(child, cls, static_cast<int>(std::size(cls))) <= 0)
                    continue;
                if (!IsExplorerHostBarBridgeClassName(cls))
                    continue;

                RECT rcBridge{};
                if (!GetWindowRect(child, &rcBridge))
                    continue;

                const int offsetY = rcBridge.top - rootClientOrigin.y;
                if (offsetY < 0 || offsetY >= bandPx)
                    continue;

                const int bridgeH = rcBridge.bottom - rcBridge.top;
                if (bridgeH > bestHeight)
                    bestHeight = bridgeH;
            }

            return bestHeight;
        }

        [[nodiscard]] int ResolveExplorerStockCaptionHeight(HWND hRoot) noexcept
        {
            const int captured = GlassCore::ExplorerFrameControl::GetExplorerStockTopMargin(hRoot);
            if (captured > 0)
                return captured;

            const int bridgeH = MeasureExplorerHostBarBridgeHeight(hRoot);
            if (bridgeH > 0)
                return bridgeH;

            return GetSystemMetrics(SM_CYCAPTION)
                + GetSystemMetrics(SM_CYSIZEFRAME)
                + GetSystemMetrics(SM_CXPADDEDBORDER);
        }

        [[nodiscard]] bool ExplorerShellRootIsVisuallyActive(HWND hRoot) noexcept
        {
            const HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            const HWND rWnd = GetAncestor(hRoot, GA_ROOT);
            const HWND rFg = GetAncestor(fg, GA_ROOT);
            return rWnd && rWnd == rFg;
        }
    } // namespace

    void TriggerExplorerShellChromeRedrawAll() noexcept
    {
        using SyncChromeFn = void(WINAPI*)() noexcept;
        if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
        {
            const auto pfnSyncChrome = reinterpret_cast<SyncChromeFn>(
                GetProcAddress(hWndCompat, "WndCompat_SyncExplorerShellChromeRedrawAll"));
            if (pfnSyncChrome)
                pfnSyncChrome();
        }
    }

    void TriggerExplorerShellRootRedrawAll() noexcept
    {
        using SyncRootRedrawFn = void(WINAPI*)() noexcept;
        if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
        {
            const auto pfnSyncRootRedraw = reinterpret_cast<SyncRootRedrawFn>(
                GetProcAddress(hWndCompat, "WndCompat_SyncExplorerShellRootRedrawAll"));
            if (pfnSyncRootRedraw)
                pfnSyncRootRedraw();
        }
    }

    void RestoreWin11ExplorerStockFrame(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        if (!IsWin11FileExplorerShellRoot(hRoot))
            return;

        GlassCore::Settings::DropEffect(
            GlassCore::Settings::kRootRegistryPart,
            hRoot);
        (void)Shared::WinApiUtils::EnableBlurBehindWindow(hRoot, false);
        GlassCore::ExplorerFrameControl::ClearExplorerIATExtendMarker(hRoot);

        const int capH = ResolveExplorerStockCaptionHeight(hRoot);
        const MARGINS stockMargins{ 0, 0, capH, 0 };
        (void)DwmExtendFrameIntoClientArea(hRoot, &stockMargins);

        const DWM_SYSTEMBACKDROP_TYPE micaBackdrop = DWMSBT_MAINWINDOW;
        (void)DwmSetWindowAttribute(
            hRoot,
            DWMWA_SYSTEMBACKDROP_TYPE,
            &micaBackdrop,
            sizeof(micaBackdrop));

        COLORREF defColor = DWMWA_COLOR_DEFAULT;
        (void)DwmSetWindowAttribute(hRoot, DWMWA_CAPTION_COLOR, &defColor, sizeof(defColor));
        (void)DwmSetWindowAttribute(hRoot, DWMWA_TEXT_COLOR, &defColor, sizeof(defColor));

        const bool darkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();
        const bool isActive = ExplorerShellRootIsVisuallyActive(hRoot);

        GlassCore::Settings::BorderContext border{};
        GlassCore::Settings::QueryRootBorderContext(darkMode, isActive, border);
        GlassCore::Settings::ApplyBorderEffect(hRoot, darkMode, border);

        GlassCore::EffectHelper::TriggerWindowNCRendering(hRoot);
        DwmTransitionOwnedWindow(hRoot, DWMTRANSITION_OWNEDWINDOW_REPOSITION);
        GlassCore::ExplorerFrameControl::ClearExplorerStockTopMarginMarker(hRoot);

        DebugLog(
            DBG_IMPORTANT,
            L"[GlassCore][Root] Win11 explorer stock restore: hwnd=0x%p stockTopMargin=%d sbt=%u active=%u",
            hRoot,
            capH,
            static_cast<unsigned>(micaBackdrop),
            isActive ? 1u : 0u);
    }

    void ResetReapplyFingerprint() noexcept
    {
        ResetReapplyFingerprintState();
    }

    bool IsExplorerAuxiliaryRootHwnd(HWND hRoot) noexcept
    {
        return IsExplorerAuxiliaryRootHwndImpl(hRoot);
    }

    bool ShouldApplyRootSwcaOnShowWindow(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return false;
        if (GetAncestor(hRoot, GA_ROOT) != hRoot)
            return false;
        if (IsExplorerAuxiliaryRootHwnd(hRoot))
            return false;
        if (IsShellTrayWindowClass(hRoot))
            return true;
        return Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot);
    }

    void OnRootDwmColorizationChanged(HWND hRoot, WPARAM colorizationWave, LPARAM liveRootScope)
    {
        if (ShouldSkipRoot(hRoot))
            return;

        Shared::RootPolicyQuery::RefreshRootBackdropPolicyCache();

        const uint32_t wave = static_cast<uint32_t>(colorizationWave);

        // Per-root (subclass): лёгкий refresh как ~1408 — без wave guard / fingerprint / Reapply sweep.
        // NONE — только process-wide через dispatch (hRoot == nullptr).
        if (hRoot)
        {
            if (!ShouldApplyColorizationPerRoot(hRoot))
                return;

            if (!IsShellTrayWindowClass(hRoot)
                && !Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
                return;

            [[maybe_unused]] const bool syncedOg = SyncOpenGlassFromRootGradients();

            const bool isActive = ResolveRootVisuallyActiveForColorization(hRoot, liveRootScope);
            const bool compositionOnly =
                WndCompat::GlassLiveRootScopeIsCompositionOnly(liveRootScope);
            const bool repaintOnly =
                WndCompat::GlassLiveRootScopeIsRepaintOnly(liveRootScope);

            RemoveRootSwcaApplyCacheEntry(hRoot);
            SetNextRootSwcaApplyReason(repaintOnly
                ? L"COLORIZATION_REPAINT"
                : (compositionOnly
                    ? L"COLORIZATION_LIVE"
                    : L"COLORIZATION_PER_ROOT"));
            if (repaintOnly)
            {
                GlassCore::TextGlow::SoftInvalidateTextGlowClientsForRoot(hRoot);
            }
            else if (compositionOnly)
            {
                ApplyRootLiveCompositionForHwnd(hRoot, isActive);
            }
            else
            {
                ApplyRootBackdropPolicyForHwnd(hRoot, isActive);
            }

            if (!repaintOnly
                && Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            {
                GlassCore::ExplorerFrameControl::SyncExplorerFrameBackdropHookFromRegistry();
            }

            if (!IsShellTrayWindowClass(hRoot) && !repaintOnly)
            {
                RedrawWindow(hRoot, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
                if (Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
                    (void)DwmFlush();
            }

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Root] COLORIZATION_PER_ROOT: hwnd=0x%p wave=%u scope=0x%lX pivot=%ld "
                L"compositionOnly=%u repaintOnly=%u active=%u RootBackdropMode=%u",
                hRoot,
                wave,
                liveRootScope,
                WndCompat::GlassLiveRootScopePivot(liveRootScope),
                compositionOnly ? 1u : 0u,
                repaintOnly ? 1u : 0u,
                isActive ? 1u : 0u,
                Shared::RootPolicyQuery::ReadRootBackdropMode());
            return;
        }

        // Process-wide: WM_GLASS_DWMCOLORIZATION_DISPATCH на g_controlWindow.
        if (!TryConsumeReapplyForWave(wave))
            return;

        [[maybe_unused]] const bool syncedOg = SyncOpenGlassFromRootGradients();
        const ReapplyPolicyFingerprint fp = BuildReapplyPolicyFingerprint();

        if (ShouldSkipReapplySweep(fp))
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Root] Reapply skipped (policy unchanged) wave=%u RootBackdropMode=%u",
                wave,
                fp.rootBackdropMode);
            return;
        }

        if (!fp.glassEnabled)
        {
            ReapplyOpenRootBackdrops();
            RememberReapplyFingerprint(fp);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Root] NONE: ReapplyOpenRootBackdrops wave=%u RootBackdropMode=%u (process-wide)",
                wave,
                fp.rootBackdropMode);
            return;
        }

        ReapplyOpenRootBackdrops();
        RememberReapplyFingerprint(fp);
        GlassCore::TabControl::OnRootWmThemeChangedForTabChroma(nullptr);
        GlassCore::ExplorerFrameControl::SyncExplorerFrameBackdropHookFromRegistry();
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][Root] ENABLE: ReapplyOpenRootBackdrops wave=%u RootBackdropMode=%u (process-wide)",
            wave,
            fp.rootBackdropMode);
    }

    void OnRootDetach(HWND hRoot)
    {
        // Не требовать Full: live AttachList downgrade вызывает DropEffect после SetInjectionMode(non-Full).

        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
        {
            Shared::RootBackdropPolicy::NotifyGlassClearedRootBackdrop(hRoot);
            return;
        }

        if (IsWin11FileExplorerShellRoot(hRoot)
            && (Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hRoot)
                || GlassCore::ExplorerFrameControl::DidExplorerIATExtendApply(hRoot)))
        {
            RestoreWin11ExplorerStockFrame(hRoot);
            Shared::RootBackdropPolicy::NotifyGlassClearedRootBackdrop(hRoot);
            RemoveRootSwcaApplyCacheEntry(hRoot);
            return;
        }

        if (!Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hRoot))
            return;

        const bool isTray = IsShellTrayWindowClass(hRoot);

        GlassCore::Settings::DropEffect(
            isTray ? GlassCore::Settings::kTrayRegistryPart : GlassCore::Settings::kRootRegistryPart,
            hRoot);

        // Win11 tray: не снимать BlurBehind/ExtendFrame — лишний «глухой» HWND
        // при уже выставленном SWCA Transparent.
        const bool win11Tray =
            isTray && Shared::SystemHelper::g_buildNumber >= 22000;
        if (!win11Tray)
        {
            (void)Shared::WinApiUtils::EnableBlurBehindWindow(hRoot, false);
            (void)Shared::WinApiUtils::ExtendFrameIntoEntireClientArea(hRoot, false);
        }

        if (!isTray)
        {
            constexpr DWORD DWMWA_SYSTEMBACKDROP_TYPE_ATTR = 38;
            DWORD backdropType = 0;
            if (SUCCEEDED(DwmGetWindowAttribute(hRoot, DWMWA_SYSTEMBACKDROP_TYPE_ATTR,
                                                 &backdropType, sizeof(backdropType))))
            {
                DwmSetWindowAttribute(hRoot, DWMWA_SYSTEMBACKDROP_TYPE_ATTR,
                                      &backdropType, sizeof(backdropType));
            }
            if (Shared::SystemHelper::g_buildNumber >= 22000
                && Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            {
                RestoreWin11ExplorerStockFrame(hRoot);
            }
            DebugLog(L"[GlassCore][Root] OnRootDetach: hwnd=0x%p backdropType=%u", hRoot, backdropType);
        }
        else
        {
            DebugLog(L"[GlassCore][Root] OnRootDetach (Tray): hwnd=0x%p", hRoot);
        }

        Shared::RootBackdropPolicy::NotifyGlassClearedRootBackdrop(hRoot);
        RemoveRootSwcaApplyCacheEntry(hRoot);
    }

    void ApplyRootLiveCompositionForHwnd(HWND hRoot, bool isActive) noexcept
    {
        if (ShouldSkipRoot(hRoot))
            return;
        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            return;

        if (IsShellTrayWindowClass(hRoot))
        {
            ApplyRootSwcaBackdrop(hRoot, isActive);
            return;
        }

        const bool darkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();
        GlassCore::Settings::WindowBackdropEffectContext backdrop{};
        GlassCore::Settings::QueryRootBackdropEffectContext(darkMode, isActive, backdrop);

        if (backdrop.rootBackdropMode == GlassCore::Settings::kRootBackdropModeOff)
            return;

        if (backdrop.rootBackdropMode == GlassCore::Settings::kRootBackdropModeDwm)
        {
            GlassCore::Settings::BorderContext border{};
            GlassCore::Settings::QueryRootBorderContext(darkMode, isActive, border);
            GlassCore::Settings::ApplyBorderEffect(hRoot, darkMode, border);
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][Root] LiveComposition DWM border hwnd=0x%p active=%u color=0x%08X "
                L"useDefault=%u useNone=%u corner=%u",
                hRoot,
                isActive ? 1u : 0u,
                border.color,
                border.colorUseDefault ? 1u : 0u,
                border.colorUseNone ? 1u : 0u,
                static_cast<unsigned>(border.cornerType));
            return;
        }

        if (backdrop.rootBackdropMode == GlassCore::Settings::kRootBackdropModeSwca)
            ApplyRootSwcaBackdrop(hRoot, isActive);
    }

    void ApplyRootBackdropPolicyForHwnd(HWND hRoot, bool isActive) noexcept
    {
        if (ShouldSkipRoot(hRoot))
            return;
        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            return;

        if (IsShellTrayWindowClass(hRoot))
        {
            ApplyRootSwcaBackdrop(hRoot, isActive);
            return;
        }

        const bool darkMode = Shared::ThemeHelper::ResolveEffectiveDarkMode();
        GlassCore::Settings::WindowBackdropEffectContext backdrop{};
        GlassCore::Settings::QueryRootBackdropEffectContext(darkMode, isActive, backdrop);

        if (backdrop.rootBackdropMode == GlassCore::Settings::kRootBackdropModeOff)
        {
            if (Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hRoot))
                OnRootDetach(hRoot);
            return;
        }

        if (backdrop.rootBackdropMode == GlassCore::Settings::kRootBackdropModeDwm)
        {
            OnRootDwmBackdropApplyImpl(hRoot, isActive);
            return;
        }

        if (backdrop.rootBackdropMode == GlassCore::Settings::kRootBackdropModeSwca)
            ApplyRootSwcaBackdrop(hRoot, isActive);
    }

    void ApplyRootBackdropPolicyTransitionForHwnd(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hRoot))
            return;

        const bool isTray = IsShellTrayWindowClass(hRoot);
        // Tray игнорирует NoBackdrop, но не InjectionMode; root — Full + NoBackdrop.
        const bool allowed = isTray
            ? IsFullInjectionMode()
            : IsRootGlassVisualsAllowed();
        GlassShared::GlassMarkRootBackdropAllowed(hRoot, allowed);

        const bool isActive = [&]() noexcept -> bool {
            const HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            const HWND rWnd = GetAncestor(hRoot, GA_ROOT);
            const HWND rFg = GetAncestor(fg, GA_ROOT);
            return rWnd && rWnd == rFg;
        }();

        if (!allowed)
        {
            if (Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hRoot))
                OnRootDetach(hRoot);
            if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
            {
                RedrawWindow(hRoot, nullptr, nullptr,
                    RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
            }
            return;
        }

        // Apply-path: Full + layered guard.
        if (ShouldSkipRoot(hRoot))
            return;

        ApplyRootBackdropPolicyForHwnd(hRoot, isActive);
        if (!Shared::RootBackdropPolicy::IsFileExplorerShellRootHwnd(hRoot))
        {
            RedrawWindow(hRoot, nullptr, nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
        }
    }

    void SyncOpenGlassParameters() noexcept
    {
        if (SyncOpenGlassFromRootGradients())
        {
            DebugLog(DBG_IMPORTANT, L"[GlassCore][Root] OpenGlass sync: значения обновлены");
        }
    }

    void ReapplyOpenRootBackdrops() noexcept
    {
        // LogViewer: локальный GlassCore/WndCompat не участвует в platform-wide sweep.
        if (ProcessBlockList::IsCurrentProcessLogViewerHost())
            return;

        SetNextRootSwcaApplyReason(L"REAPPLY");
        ClearRootSwcaApplyCache();
        Shared::RootPolicyQuery::RefreshRootBackdropPolicyCache();
        SyncOpenGlassParameters();

        GlassCore::ModuleHandler::ScanBackdropScopedModules(
            IsRootGlassVisualsAllowed());
        if (!IsRootGlassVisualsAllowed())
            GlassCore::ScrollBarTheme::ShutdownComctlScrollBarIat();
        if (IsRootGlassVisualsAllowed())
            GlassCore::ExplorerFrameControl::SyncExplorerFrameBackdropHookFromRegistry();

        if (Detail::g_trackedRootsBackdropApplier)
            Detail::g_trackedRootsBackdropApplier();
        else
        {
            using TrackedApplierFn = void(WINAPI*)() noexcept;
            using MarshalFn = void(WINAPI*)(HWND) noexcept;

            TrackedApplierFn pfnApplier = nullptr;
            MarshalFn pfnMarshal = nullptr;
            if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
            {
                pfnApplier = reinterpret_cast<TrackedApplierFn>(
                    GetProcAddress(hWndCompat, "WndCompat_ApplyBackdropPolicyToTrackedRoots"));
                pfnMarshal = reinterpret_cast<MarshalFn>(
                    GetProcAddress(hWndCompat, "WndCompat_MarshalApplyRootBackdropReapplyForHwnd"));
            }

            if (pfnApplier)
            {
                pfnApplier();
            }
            else
            {
                const auto hwnds = Shared::RootBackdropPolicy::SnapshotGlassAppliedRootHwnds();
                for (HWND hRoot : hwnds)
                {
                    if (!hRoot || !IsWindow(hRoot))
                        continue;
                    if (pfnMarshal)
                        pfnMarshal(hRoot);
                    else
                        ApplyRootBackdropPolicyTransitionForHwnd(hRoot);
                }
            }
        }

        if (!IsRootGlassVisualsAllowed())
            GlassCore::ExplorerFrameControl::SyncExplorerFrameBackdropHookFromRegistry();

        if (!IsRootGlassVisualsAllowed())
        {
            TriggerExplorerShellChromeRedrawAll();
            TriggerExplorerShellRootRedrawAll();

            using PostRedrawFn = void(WINAPI*)() noexcept;
            if (const HMODULE hEc = GetModuleHandleW(L"ExplorerCompat.dll"))
            {
                const auto pfnPostRedraw = reinterpret_cast<PostRedrawFn>(
                    GetProcAddress(hEc, "ExplorerCompat_PostLiveDisableChromeRedraw"));
                if (pfnPostRedraw)
                    pfnPostRedraw();
            }
        }

        const auto appliedRoots = Shared::RootBackdropPolicy::SnapshotGlassAppliedRootHwnds();
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][Root] ReapplyOpenRootBackdrops: sweep done RootBackdropMode=%u glassAppliedRoots=%zu",
            Shared::RootPolicyQuery::ReadRootBackdropMode(),
            appliedRoots.size());
    }

    void ReapplyTrayRootSwcaAfterTaskbarAccent(HWND hTrayRoot) noexcept
    {
        if (!hTrayRoot || !IsWindow(hTrayRoot))
            return;
        if (!IsFullInjectionMode())
            return;
        if (ShouldSkipRoot(hTrayRoot))
            return;
        if (!IsShellTrayWindowClass(hTrayRoot))
            return;
        if (Shared::RootBackdropPolicy::IsHwndBackdropExempt(hTrayRoot))
            return;
        if (!Shared::RootBackdropPolicy::DidGlassApplyRootBackdrop(hTrayRoot))
            return;

        const bool foregroundMatch = [&]() noexcept -> bool {
            const HWND fg = GetForegroundWindow();
            if (!fg)
                return false;
            const HWND rWnd = GetAncestor(hTrayRoot, GA_ROOT);
            const HWND rFg  = GetAncestor(fg, GA_ROOT);
            return rWnd && rWnd == rFg;
        }();

        ApplyRootSwcaBackdrop(hTrayRoot, foregroundMatch);
    }
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_ReapplyOpenRootBackdrops() noexcept
{
    GlassCore::Root::ReapplyOpenRootBackdrops();
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_ResetReapplyFingerprint() noexcept
{
    GlassCore::Root::ResetReapplyFingerprint();
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_SetNextRootSwcaApplyReason(const wchar_t* reason) noexcept
{
    GlassCore::Root::SetNextRootSwcaApplyReason(reason);
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_SyncExplorerFrameBackdropHookFromRegistry() noexcept
{
    GlassCore::ExplorerFrameControl::SyncExplorerFrameBackdropHookFromRegistry();
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_BeginExplorerShellChromeRestore() noexcept
{
    GlassCore::ExplorerFrameControl::BeginExplorerShellChromeRestore();
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_SetInjectionMode(DWORD mode) noexcept
{
    GlassCore::Root::SetInjectionMode(mode);
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_RegisterTrackedRootsBackdropApplier(
    void (WINAPI* fn)() noexcept) noexcept
{
    GlassCore::Root::RegisterTrackedRootsBackdropApplier(fn);
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_RegisterRootIsAttachedQuery(
    bool (WINAPI* fn)(HWND) noexcept) noexcept
{
    GlassCore::Root::RegisterRootIsAttachedQuery(fn);
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_RegisterRootAttachRequest(
    void (WINAPI* fn)(HWND) noexcept) noexcept
{
    GlassCore::Root::RegisterRootAttachRequest(fn);
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassCore_IsExplorerAuxiliaryRootHwnd(HWND hRoot) noexcept
{
    return GlassCore::Root::IsExplorerAuxiliaryRootHwnd(hRoot) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassCore_ShouldApplyRootSwcaOnShowWindow(HWND hRoot) noexcept
{
    return GlassCore::Root::ShouldApplyRootSwcaOnShowWindow(hRoot) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_RemoveRootSwcaApplyCacheEntry(HWND hRoot) noexcept
{
    GlassCore::Root::RemoveRootSwcaApplyCacheEntry(hRoot);
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_ApplyRootBackdropPolicyTransitionForHwnd(
    HWND hRoot) noexcept
{
    GlassCore::Root::ApplyRootBackdropPolicyTransitionForHwnd(hRoot);
}
