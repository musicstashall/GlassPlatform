// Реестр: полупрозрачная отрисовка темы (DrawThemeBackgroundWithConstantAlpha).

#include "CorePch.h"
#include <commctrl.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Helpers/RegHelper.h>
#include <RootSwcaRegistryQuery.h>
#include <DebugLog.h>
#include <GlassControlKind.h>
#include <GlassMessages.h>
#include <algorithm>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

namespace
{
    struct RedrawSameProcessCtx
    {
        DWORD pid;
    };

    BOOL CALLBACK RedrawTopLevelSameProcessEnum(HWND hwnd, LPARAM lp) noexcept
    {
        const auto* ctx = reinterpret_cast<const RedrawSameProcessCtx*>(lp);
        DWORD         pid = 0;
        const DWORD   wtid = GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid)
            return TRUE;

        constexpr DWORD kRedrawFlags =
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME | RDW_INTERNALPAINT;

        if (wtid == GetCurrentThreadId())
        {
            RedrawWindow(hwnd, nullptr, nullptr, kRedrawFlags);
            return TRUE;
        }

        DWORD_PTR sendResult = 0;
        (void)SendMessageTimeoutW(
            hwnd,
            WndCompat::WM_GLASS_INVALIDATE_TOPLEVEL_VISUAL,
            0,
            0,
            SMTO_ABORTIFHUNG,
            1000,
            &sendResult);
        return TRUE;
    }
} // namespace

namespace GlassCore::ThemePaint
{
    namespace
    {
        std::uint8_t g_drawThemeConstantAlphaActive   = kDefaultDrawThemeConstantAlpha;
        std::uint8_t g_drawThemeConstantAlphaInactive = kDefaultDrawThemeConstantAlpha;

        std::uint8_t g_tabChromaThreshActive   = kDefaultTabChromaThreshold;
        std::uint8_t g_tabChromaThreshInactive = kDefaultTabChromaThreshold;

        constexpr std::wstring_view kRegValueGlassDrawThemeConstantAlpha = L"GlassDrawThemeConstantAlpha";
        constexpr std::wstring_view kRegValueGlassTabChromaThreshold     = L"GlassTabChromaThreshold";

        std::atomic<bool>        g_previewThemeAlphaActiveValid{ false };
        std::atomic<bool>        g_previewThemeAlphaInactiveValid{ false };
        std::atomic<std::uint8_t> g_previewThemeAlphaActive{ kDefaultDrawThemeConstantAlpha };
        std::atomic<std::uint8_t> g_previewThemeAlphaInactive{ kDefaultDrawThemeConstantAlpha };
        std::atomic<bool>         g_previewAppliesToStandardThemedChrome{ false };

        // HWND активного окна (как GetForegroundWindow), только в GlassCore.dll; обновление — Finalize / init.
        std::atomic<std::uintptr_t> g_cachedForegroundHwnd{ 0 };

        // Состояние WM_NCACTIVATE по root — только в GlassCore.dll (не путать с RootNcActiveCache в других PE).
        std::shared_mutex                       g_themeRootNcMutex;
        std::unordered_map<std::uintptr_t, bool> g_themeRootNcActive;

        // Реестр HWND с theme-chrome (Button / Combo / Tab) по chrome-root (WinForms_Window или GA_ROOT).
        std::mutex                                                       g_chromeByRootMutex;
        std::unordered_map<std::uintptr_t, std::unordered_set<std::uintptr_t>> g_chromeHwndByRoot;

        void SoftInvalidateTabThemedChromeClient(HWND hTab) noexcept;
        void SoftInvalidateThemedChromeClientsForGaRootActivationTree(HWND hRoot) noexcept;

        using WndCompatEnsureWinFormsWindowMarkedFn = void(WINAPI*)(HWND);

        [[nodiscard]] WndCompatEnsureWinFormsWindowMarkedFn GetWndCompatEnsureWinFormsWindowMarkedFn() noexcept
        {
            static WndCompatEnsureWinFormsWindowMarkedFn s_pfn =
                []() noexcept -> WndCompatEnsureWinFormsWindowMarkedFn
            {
                const HMODULE hMod = GetModuleHandleW(L"WndCompat.dll");
                if (!hMod)
                    return nullptr;
                return reinterpret_cast<WndCompatEnsureWinFormsWindowMarkedFn>(
                    GetProcAddress(hMod, "WndCompat_EnsureWinFormsWindowMarked"));
            }();
            return s_pfn;
        }

        void DispatchWndCompatEnsureWinFormsWindowMarked(HWND hWnd) noexcept
        {
            if (!hWnd || !IsWindow(hWnd))
                return;
            const WndCompatEnsureWinFormsWindowMarkedFn pfn = GetWndCompatEnsureWinFormsWindowMarkedFn();
            if (!pfn)
                return;
            pfn(hWnd);
        }

        [[nodiscard]] bool IsThemedChromeControlUnderGaRoot(HWND hCtrl, HWND gaRoot) noexcept
        {
            if (!hCtrl || !gaRoot || !IsWindow(hCtrl) || !IsWindow(gaRoot))
                return false;
            if (GetAncestor(hCtrl, GA_ROOT) == gaRoot)
                return true;
            for (HWND w = hCtrl; w; w = GetParent(w))
            {
                if (w == gaRoot)
                    return true;
            }
            return false;
        }

        // pathPass1: bucket для path=pass1; gaRoot — для path=pass2 orphan.
        [[nodiscard]] bool SoftInvalidateThemedChromeClientHwnd(
            HWND hCtrl,
            bool pass1,
            HWND bucket,
            HWND gaRoot) noexcept
        {
            if (!hCtrl || !IsWindow(hCtrl))
                return false;

            using namespace GlassShared;
            const GlassControlKind k = GlassGetWindowKind(hCtrl);
            switch (k)
            {
            case GlassControlKind::Button:
            case GlassControlKind::WinForms_Button:
            case GlassControlKind::ComboBox:
            case GlassControlKind::WinForms_ComboBox:
            case GlassControlKind::UpDown:
            {
                const BOOL invOk = InvalidateRect(hCtrl, nullptr, FALSE);
                if (pass1)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Chrome] SoftInvalidate path=pass1 bucket=0x%p ctrl=0x%p kind=%u InvalidateRect=%d",
                        bucket,
                        hCtrl,
                        static_cast<unsigned>(static_cast<std::uint8_t>(k)),
                        static_cast<int>(invOk));
                }
                else
                {
                    const HWND ctrlGaRoot = GetAncestor(hCtrl, GA_ROOT);
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Chrome] SoftInvalidate path=pass2 orphan ctrl=0x%p bucket=0x%p "
                        L"gaRoot=0x%p ctrlGaRoot=0x%p InvalidateRect=%d",
                        hCtrl,
                        bucket,
                        gaRoot,
                        ctrlGaRoot,
                        static_cast<int>(invOk));
                }
                return true;
            }
            case GlassControlKind::TabControl:
                if (pass1)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Chrome] SoftInvalidate path=pass1 bucket=0x%p ctrl=0x%p kind=TabControl → tab path",
                        bucket,
                        hCtrl);
                }
                else
                {
                    const HWND ctrlGaRoot = GetAncestor(hCtrl, GA_ROOT);
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Chrome] SoftInvalidate path=pass2 orphan ctrl=0x%p bucket=0x%p "
                        L"gaRoot=0x%p ctrlGaRoot=0x%p kind=TabControl → tab path",
                        hCtrl,
                        bucket,
                        gaRoot,
                        ctrlGaRoot);
                }
                SoftInvalidateTabThemedChromeClient(hCtrl);
                return true;
            default:
                if (pass1)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Chrome] SoftInvalidate path=pass1 bucket=0x%p ctrl=0x%p kind=%u skip",
                        bucket,
                        hCtrl,
                        static_cast<unsigned>(static_cast<std::uint8_t>(k)));
                }
                return false;
            }
        }

        [[nodiscard]] HWND ResolveThemedChromeRegistryRoot(HWND hControl, bool syntheticParentCatchUp) noexcept
        {
            if (!hControl)
                return nullptr;
            using namespace GlassShared;

            if (syntheticParentCatchUp)
            {
                for (HWND w = hControl; w; w = GetParent(w))
                {
                    if (GlassGetWindowKind(w) != GlassControlKind::None)
                        continue;
                    DispatchWndCompatEnsureWinFormsWindowMarked(w);
                }
            }

            for (HWND w = hControl; w; w = GetParent(w))
            {
                if (GlassGetWindowKind(w) == GlassControlKind::WinForms_Window)
                    return w;
            }
            HWND ga = GetAncestor(hControl, GA_ROOT);
            return ga ? ga : hControl;
        }

        [[nodiscard]] HWND ResolveThemedChromeRegistryRoot(HWND hControl) noexcept
        {
            return ResolveThemedChromeRegistryRoot(hControl, false);
        }

        [[nodiscard]] bool IsThemedChromeBucketUnderGaRoot(HWND bucketHwnd, HWND gaRoot) noexcept
        {
            if (!bucketHwnd || !gaRoot || !IsWindow(bucketHwnd) || !IsWindow(gaRoot))
                return false;
            if (bucketHwnd == gaRoot)
                return true;
            const HWND bucketGa = GetAncestor(bucketHwnd, GA_ROOT);
            if (bucketGa && bucketGa == gaRoot)
                return true;
            for (HWND w = bucketHwnd; w;)
            {
                const HWND owner = GetWindow(w, GW_OWNER);
                if (!owner)
                    break;
                if (owner == gaRoot)
                    return true;
                if (GetAncestor(owner, GA_ROOT) == gaRoot)
                    return true;
                w = owner;
            }
            return false;
        }

        void LogHwndClassForChromeDiag(HWND hWnd, wchar_t* out, int cch) noexcept
        {
            if (!out || cch <= 0)
                return;
            out[0] = L'\0';
            if (hWnd && IsWindow(hWnd))
                (void)GetClassNameW(hWnd, out, cch);
        }

        [[nodiscard]] bool RootVisuallyActiveForThemedChrome(HWND hWndChrome) noexcept
        {
            if (!hWndChrome || !IsWindow(hWndChrome))
                return true;
            const HWND rootChrome = GetAncestor(hWndChrome, GA_ROOT);
            if (!rootChrome)
                return true;

            {
                std::shared_lock lk(g_themeRootNcMutex);
                const auto it = g_themeRootNcActive.find(reinterpret_cast<std::uintptr_t>(rootChrome));
                if (it != g_themeRootNcActive.end())
                    return it->second;
            }

            const std::uintptr_t cached =
                g_cachedForegroundHwnd.load(std::memory_order_acquire);
            const HWND cachedFg = reinterpret_cast<HWND>(cached);
            // Нет валидного GFW (часто при уходе на рабочий стол / гонке) — не считаем окно активным для DTB.
            if (!cachedFg || !IsWindow(cachedFg))
                return false;
            const HWND rootFg = GetAncestor(cachedFg, GA_ROOT);
            return rootChrome == rootFg;
        }

        [[nodiscard]] bool IsRegisteredThemedChromeKind(GlassShared::GlassControlKind k) noexcept
        {
            using GK = GlassShared::GlassControlKind;
            return k == GK::Button || k == GK::WinForms_Button || k == GK::ComboBox
                || k == GK::WinForms_ComboBox || k == GK::TabControl || k == GK::UpDown;
        }

        void CatchUpThemedChromeInvalidateAfterRegister(HWND hControl, GlassShared::GlassControlKind k) noexcept
        {
            if (g_drawThemeConstantAlphaActive == g_drawThemeConstantAlphaInactive)
                return;
            using GK = GlassShared::GlassControlKind;
            switch (k)
            {
            case GK::Button:
            case GK::WinForms_Button:
            case GK::ComboBox:
            case GK::WinForms_ComboBox:
            case GK::UpDown:
            {
                const BOOL invOk = InvalidateRect(hControl, nullptr, FALSE);
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Chrome] Register catch-up InvalidateRect hCtrl=0x%p kind=%u ok=%d",
                    hControl,
                    static_cast<unsigned>(static_cast<std::uint8_t>(k)),
                    static_cast<int>(invOk));
                break;
            }
            default:
                break;
            }
        }

        [[nodiscard]] bool IsMarkedWinFormsWindowAncestorOf(HWND hMarkedWinFormsWindow, HWND hCtrl) noexcept
        {
            if (!hMarkedWinFormsWindow || !hCtrl || !IsWindow(hMarkedWinFormsWindow) || !IsWindow(hCtrl))
                return false;
            for (HWND w = hCtrl; w; w = GetParent(w))
            {
                if (w == hMarkedWinFormsWindow)
                    return true;
            }
            return false;
        }

        [[nodiscard]] HWND FindThemedChromeClientBucketLocked(HWND hControl) noexcept
        {
            const std::uintptr_t hKey = reinterpret_cast<std::uintptr_t>(hControl);
            for (const auto& entry : g_chromeHwndByRoot)
            {
                if (entry.second.find(hKey) != entry.second.end())
                    return reinterpret_cast<HWND>(entry.first);
            }
            return nullptr;
        }

        void RebindThemedChromeClientBucketLocked(
            HWND hControl,
            HWND oldBucket,
            HWND newBucket) noexcept
        {
            if (!hControl || !oldBucket || !newBucket || oldBucket == newBucket)
                return;

            const std::uintptr_t hKey   = reinterpret_cast<std::uintptr_t>(hControl);
            const std::uintptr_t oldKey = reinterpret_cast<std::uintptr_t>(oldBucket);
            const std::uintptr_t newKey = reinterpret_cast<std::uintptr_t>(newBucket);

            if (const auto oldIt = g_chromeHwndByRoot.find(oldKey); oldIt != g_chromeHwndByRoot.end())
            {
                oldIt->second.erase(hKey);
                if (oldIt->second.empty())
                    g_chromeHwndByRoot.erase(oldIt);
            }
            g_chromeHwndByRoot[newKey].insert(hKey);

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ThemePaint][Chrome] Remigrate hCtrl=0x%p oldBucket=0x%p newBucket=0x%p",
                hControl,
                oldBucket,
                newBucket);
        }

        void RebindThemedChromeClientBucketImpl(HWND hControl) noexcept
        {
            if (!hControl || !IsWindow(hControl))
                return;
            using namespace GlassShared;
            if (!IsRegisteredThemedChromeKind(GlassGetWindowKind(hControl)))
                return;

            const HWND newBucket = ResolveThemedChromeRegistryRoot(hControl, false);
            if (!newBucket)
                return;

            std::lock_guard lock(g_chromeByRootMutex);
            const HWND oldBucket = FindThemedChromeClientBucketLocked(hControl);
            if (!oldBucket || oldBucket == newBucket)
                return;

            RebindThemedChromeClientBucketLocked(hControl, oldBucket, newBucket);
        }

        void RebindThemedChromeClientsAfterWinFormsWindowMarkedImpl(HWND hMarkedWinFormsWindow) noexcept
        {
            if (!hMarkedWinFormsWindow || !IsWindow(hMarkedWinFormsWindow))
                return;
            if (GlassShared::GlassGetWindowKind(hMarkedWinFormsWindow)
                != GlassShared::GlassControlKind::WinForms_Window)
            {
                return;
            }

            std::vector<HWND> controls;
            {
                std::lock_guard lock(g_chromeByRootMutex);
                for (const auto& entry : g_chromeHwndByRoot)
                {
                    for (const std::uintptr_t hKey : entry.second)
                        controls.push_back(reinterpret_cast<HWND>(hKey));
                }
            }

            for (HWND hCtrl : controls)
            {
                if (!IsWindow(hCtrl))
                    continue;
                if (!IsMarkedWinFormsWindowAncestorOf(hMarkedWinFormsWindow, hCtrl))
                    continue;
                RebindThemedChromeClientBucketImpl(hCtrl);
            }
        }

        void RegisterThemedChromeClientImpl(HWND hControl) noexcept
        {
            if (!hControl || !IsWindow(hControl))
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ThemePaint][Chrome] Register reject hCtrl=0x%p IsWindow=%d",
                    hControl,
                    (hControl && IsWindow(hControl)) ? 1 : 0);
                return;
            }
            using namespace GlassShared;
            const GlassControlKind k = GlassGetWindowKind(hControl);
            if (!IsRegisteredThemedChromeKind(k))
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ThemePaint][Chrome] Register reject hCtrl=0x%p kind=%u",
                    hControl,
                    static_cast<unsigned>(static_cast<std::uint8_t>(k)));
                return;
            }
            const HWND bucket = ResolveThemedChromeRegistryRoot(hControl, true);
            if (!bucket)
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ThemePaint][Chrome] Register reject hCtrl=0x%p bucket=null",
                    hControl);
                return;
            }
            const HWND gaRoot = GetAncestor(hControl, GA_ROOT);
            const std::uintptr_t rootKey = reinterpret_cast<std::uintptr_t>(bucket);
            const std::uintptr_t hKey     = reinterpret_cast<std::uintptr_t>(hControl);
            {
                std::lock_guard lock(g_chromeByRootMutex);
                g_chromeHwndByRoot[rootKey].insert(hKey);

                wchar_t cls[128]{};
                LogHwndClassForChromeDiag(hControl, cls, static_cast<int>(std::size(cls)));
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Chrome] Register hCtrl=0x%p kind=%u bucket=0x%p gaRoot=0x%p class=\"%s\"",
                    hControl,
                    static_cast<unsigned>(static_cast<std::uint8_t>(k)),
                    bucket,
                    gaRoot,
                    cls[0] ? cls : L"(null)");
                CatchUpThemedChromeInvalidateAfterRegister(hControl, k);
            }
            RebindThemedChromeClientBucketImpl(hControl);
        }

        void UnregisterThemedChromeClientImpl(HWND hControl) noexcept
        {
            if (!hControl)
                return;
            std::lock_guard lock(g_chromeByRootMutex);
            if (IsWindow(hControl))
            {
                const HWND bucket = ResolveThemedChromeRegistryRoot(hControl);
                if (!bucket)
                    return;
                const auto rIt = g_chromeHwndByRoot.find(reinterpret_cast<std::uintptr_t>(bucket));
                if (rIt == g_chromeHwndByRoot.end())
                    return;
                rIt->second.erase(reinterpret_cast<std::uintptr_t>(hControl));
                if (rIt->second.empty())
                    g_chromeHwndByRoot.erase(rIt);
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Chrome] Unregister hCtrl=0x%p bucket=0x%p",
                    hControl,
                    bucket);
                return;
            }
            for (auto rIt = g_chromeHwndByRoot.begin(); rIt != g_chromeHwndByRoot.end();)
            {
                rIt->second.erase(reinterpret_cast<std::uintptr_t>(hControl));
                if (rIt->second.empty())
                    rIt = g_chromeHwndByRoot.erase(rIt);
                else
                    ++rIt;
            }
        }

        void RemoveThemedChromeClientsForRootImpl(HWND hRoot) noexcept
        {
            if (!hRoot)
                return;
            std::lock_guard lock(g_chromeByRootMutex);
            (void)g_chromeHwndByRoot.erase(reinterpret_cast<std::uintptr_t>(hRoot));
        }

        void ClearThemedChromeRegistryImpl() noexcept
        {
            std::lock_guard lock(g_chromeByRootMutex);
            g_chromeHwndByRoot.clear();
        }

        [[nodiscard]] std::size_t SoftInvalidateThemedChromeClientsForRootImpl(
            HWND hRoot,
            std::unordered_set<std::uintptr_t>& pass1Done) noexcept
        {
            if (!hRoot || !IsWindow(hRoot))
                return 0;
            std::vector<HWND> copy;
            {
                std::lock_guard lock(g_chromeByRootMutex);
                const auto it = g_chromeHwndByRoot.find(reinterpret_cast<std::uintptr_t>(hRoot));
                if (it == g_chromeHwndByRoot.end())
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Chrome] SoftInvalidate bucket=0x%p count=0 (нет записей)",
                        hRoot);
                    return 0;
                }
                copy.reserve(it->second.size());
                for (const std::uintptr_t p : it->second)
                    copy.push_back(reinterpret_cast<HWND>(p));
            }

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ThemePaint][Chrome] SoftInvalidate bucket=0x%p count=%zu",
                hRoot,
                copy.size());

            std::size_t pass1Count = 0;
            for (HWND h : copy)
            {
                if (!IsWindow(h))
                    continue;
                if (SoftInvalidateThemedChromeClientHwnd(h, true, hRoot, nullptr))
                {
                    pass1Done.insert(reinterpret_cast<std::uintptr_t>(h));
                    ++pass1Count;
                }
            }
            return pass1Count;
        }

        void SoftInvalidateThemedChromeClientsForGaRootActivationTree(HWND hRoot) noexcept
        {
            if (!hRoot || !IsWindow(hRoot))
                return;

            HWND gaRoot = hRoot;
            if (const HWND a = GetAncestor(hRoot, GA_ROOT))
                gaRoot = a;

            std::vector<HWND> buckets;
            buckets.push_back(gaRoot);
            if (hRoot != gaRoot)
                buckets.push_back(hRoot);

            {
                std::lock_guard lock(g_chromeByRootMutex);
                for (const auto& entry : g_chromeHwndByRoot)
                {
                    const HWND bucket = reinterpret_cast<HWND>(entry.first);
                    if (bucket == gaRoot || bucket == hRoot)
                        continue;
                    if (IsThemedChromeBucketUnderGaRoot(bucket, gaRoot))
                        buckets.push_back(bucket);
                }
            }

            std::sort(buckets.begin(), buckets.end());
            buckets.erase(std::unique(buckets.begin(), buckets.end()), buckets.end());

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ThemePaint][Chrome] SoftInvalidateTree hRoot=0x%p gaRoot=0x%p buckets=%zu",
                hRoot,
                gaRoot,
                buckets.size());

            std::unordered_set<std::uintptr_t> pass1Done;
            std::size_t pass1Ctrls = 0;
            for (HWND bucket : buckets)
                pass1Ctrls += SoftInvalidateThemedChromeClientsForRootImpl(bucket, pass1Done);

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ThemePaint][Chrome] SoftInvalidateTree pass2 begin gaRoot=0x%p pass1Ctrls=%zu",
                gaRoot,
                pass1Ctrls);

            struct Pass2OrphanEntry
            {
                HWND hCtrl;
                HWND bucketHwnd;
            };

            std::size_t pass2Orphans = 0;
            std::vector<Pass2OrphanEntry> pass2OrphansList;
            {
                std::lock_guard lock(g_chromeByRootMutex);
                for (const auto& entry : g_chromeHwndByRoot)
                {
                    const HWND bucketHwnd = reinterpret_cast<HWND>(entry.first);
                    for (const std::uintptr_t hKey : entry.second)
                    {
                        if (pass1Done.find(hKey) != pass1Done.end())
                            continue;
                        const HWND hCtrl = reinterpret_cast<HWND>(hKey);
                        if (!IsWindow(hCtrl))
                            continue;
                        if (!IsThemedChromeControlUnderGaRoot(hCtrl, gaRoot))
                            continue;
                        pass2OrphansList.push_back({ hCtrl, bucketHwnd });
                    }
                }
            }
            for (const Pass2OrphanEntry& orphan : pass2OrphansList)
            {
                if (SoftInvalidateThemedChromeClientHwnd(
                        orphan.hCtrl, false, orphan.bucketHwnd, gaRoot))
                {
                    ++pass2Orphans;
                }
            }

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ThemePaint][Chrome] SoftInvalidateTree pass2 end gaRoot=0x%p pass1Ctrls=%zu pass2Orphans=%zu",
                gaRoot,
                pass1Ctrls,
                pass2Orphans);
        }

        // Мягкая инвалидация хрома таба: внешний прямоугольник — весь клиент таба; «дыра» тела —
        // GetClientRect + TabCtrl_AdjustRect(FALSE), асимметричный inset (см. ApplyTabBodyChromeSoftInset),
        // регион «клиент минус тело» в координатах клиента hTab — InvalidateRgn(hTab, …).
        constexpr int kTabSoftInvalidateBodyInset = 1;
        // Сужение «дыры» со стороны язычков (px): шов тело/хром + полоса активной вкладки при инвалидации.
        constexpr int kTabHoleSeamInvalidateShrinkPx = 2;

        [[nodiscard]] bool NonEmptyRect(const RECT& r) noexcept
        {
            return r.right > r.left && r.bottom > r.top;
        }

        // MapWindowPoints: при успехе возвращает ненулевое смещение (не число точек); 0 — ошибка.
        // Редкий случай «успех с нулевым смещением» даёт тот же 0 — тогда ClientToScreen/ScreenToClient.
        [[nodiscard]] bool MapClientRectToOtherClient(HWND hFrom, const RECT& rFrom, HWND hTo, RECT* out) noexcept
        {
            if (!out)
                return false;
            RECT r = rFrom;
            if (MapWindowPoints(hFrom, hTo, reinterpret_cast<LPPOINT>(&r), 2) != 0)
            {
                *out = r;
                return true;
            }
            POINT tl{ rFrom.left, rFrom.top };
            POINT br{ rFrom.right, rFrom.bottom };
            if (!ClientToScreen(hFrom, &tl) || !ClientToScreen(hFrom, &br))
                return false;
            if (!ScreenToClient(hTo, &tl) || !ScreenToClient(hTo, &br))
                return false;
            out->left   = tl.x;
            out->top    = tl.y;
            out->right  = br.x;
            out->bottom = br.y;
            return true;
        }

        // Коды отказа AdjustRectOutputPlausible (для диагностики WARN мягкой инвалидации таба).
        enum class TabAdjustPlausibleReject : std::uint8_t
        {
            None = 0,
            BodyEmpty = 1,
            NoIntersection = 2,
            InterAreaGe90Pct = 3,
            HorizTop_InterTopNotBelowChrome = 4,  // inter.top <= client.top + 2
            HorizBottom_InterBottomNotAboveChrome = 5,
        };

        [[nodiscard]] TabAdjustPlausibleReject ClassifyTabAdjustRectNotPlausible(
            const RECT& client,
            const RECT& bodyRaw,
            UINT        style) noexcept
        {
            if (!NonEmptyRect(bodyRaw))
                return TabAdjustPlausibleReject::BodyEmpty;
            RECT inter{};
            if (!IntersectRect(&inter, &client, &bodyRaw))
                return TabAdjustPlausibleReject::NoIntersection;
            const __int64 aClient = static_cast<__int64>(client.right - client.left)
                * static_cast<__int64>(client.bottom - client.top);
            const __int64 aInter = static_cast<__int64>(inter.right - inter.left)
                * static_cast<__int64>(inter.bottom - inter.top);
            if (aClient > 0 && aInter >= (aClient * 9) / 10)
                return TabAdjustPlausibleReject::InterAreaGe90Pct;
            if ((style & TCS_VERTICAL) == 0)
            {
                if ((style & TCS_BOTTOM) == 0)
                {
                    if (inter.top <= client.top + 2)
                        return TabAdjustPlausibleReject::HorizTop_InterTopNotBelowChrome;
                }
                else
                {
                    if (inter.bottom >= client.bottom - 2)
                        return TabAdjustPlausibleReject::HorizBottom_InterBottomNotAboveChrome;
                }
            }
            return TabAdjustPlausibleReject::None;
        }

        // prc после TCM_ADJUSTRECT иногда пригоден, даже если сообщение вернуло FALSE (0 вкладок и т.п.).
        // «Пригодность» тела — ClassifyTabAdjustRectNotPlausible == None.

        // Диагностика WARN: полная цепочка геометрии (временно; см. план glassconfig tab invalidation).
        void LogTabSoftInvalidateWarnPipeline(
            HWND hTab,
            const RECT& client,
            UINT style,
            const RECT& bodyBeforeAdjust,
            const RECT& bodyAfterAdjust,
            BOOL adjOk,
            TabAdjustPlausibleReject plausibleReject) noexcept
        {
            const wchar_t* prName = L"?";
            switch (plausibleReject)
            {
            case TabAdjustPlausibleReject::None:
                prName = L"None";
                break;
            case TabAdjustPlausibleReject::BodyEmpty:
                prName = L"BodyEmpty";
                break;
            case TabAdjustPlausibleReject::NoIntersection:
                prName = L"NoIntersection";
                break;
            case TabAdjustPlausibleReject::InterAreaGe90Pct:
                prName = L"InterAreaGe90Pct";
                break;
            case TabAdjustPlausibleReject::HorizTop_InterTopNotBelowChrome:
                prName = L"HorizTop_InterTopNotBelowChrome";
                break;
            case TabAdjustPlausibleReject::HorizBottom_InterBottomNotAboveChrome:
                prName = L"HorizBottom_InterBottomNotAboveChrome";
                break;
            }

            RECT interForDiag{};
            (void)IntersectRect(&interForDiag, &client, &bodyAfterAdjust);
            const __int64 aC = static_cast<__int64>(client.right - client.left)
                * static_cast<__int64>(client.bottom - client.top);
            const __int64 aI = static_cast<__int64>(interForDiag.right - interForDiag.left)
                * static_cast<__int64>(interForDiag.bottom - interForDiag.top);

            DebugLog(DBG_WARNING,
                L"[GlassCore][ThemePaint][Tab] SoftInvalidate WARN pipeline: hTab=0x%p GWL_STYLE=0x%08X "
                L"client=(%ld,%ld)-(%ld,%ld) adjOk=%d",
                hTab,
                style,
                static_cast<long>(client.left),
                static_cast<long>(client.top),
                static_cast<long>(client.right),
                static_cast<long>(client.bottom),
                static_cast<int>(adjOk));

            DebugLog(DBG_WARNING,
                L"[GlassCore][ThemePaint][Tab] SoftInvalidate WARN: bodyBeforeAdjustRect=(%ld,%ld)-(%ld,%ld) "
                L"bodyAfterAdjustRect=(%ld,%ld)-(%ld,%ld) TabCtrl_AdjustRect(FALSE) written prc",
                static_cast<long>(bodyBeforeAdjust.left),
                static_cast<long>(bodyBeforeAdjust.top),
                static_cast<long>(bodyBeforeAdjust.right),
                static_cast<long>(bodyBeforeAdjust.bottom),
                static_cast<long>(bodyAfterAdjust.left),
                static_cast<long>(bodyAfterAdjust.top),
                static_cast<long>(bodyAfterAdjust.right),
                static_cast<long>(bodyAfterAdjust.bottom));

            DebugLog(DBG_WARNING,
                L"[GlassCore][ThemePaint][Tab] SoftInvalidate WARN: "
                L"AdjustRectOutputPlausible reject=%u (%s) inter(body&client)=(%ld,%ld)-(%ld,%ld) "
                L"aClient=%lld aInter=%lld ratio10x=%lld (отсев 90%% если aInter>=aClient*9/10)",
                static_cast<unsigned>(static_cast<std::uint8_t>(plausibleReject)),
                prName,
                static_cast<long>(interForDiag.left),
                static_cast<long>(interForDiag.top),
                static_cast<long>(interForDiag.right),
                static_cast<long>(interForDiag.bottom),
                static_cast<long long>(aC),
                static_cast<long long>(aI),
                static_cast<long long>(aC > 0 ? (aI * 10) / aC : -1));

            const int n = TabCtrl_GetItemCount(hTab);
            for (int i = 0; i < n; ++i)
            {
                RECT ir{};
                if (!TabCtrl_GetItemRect(hTab, i, &ir))
                {
                    DebugLog(DBG_WARNING,
                        L"[GlassCore][ThemePaint][Tab] SoftInvalidate WARN: TabCtrl_GetItemRect(%d) FAILED", i);
                    continue;
                }
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate WARN: GetItemRect(%d)=(%ld,%ld)-(%ld,%ld)",
                    i,
                    static_cast<long>(ir.left),
                    static_cast<long>(ir.top),
                    static_cast<long>(ir.right),
                    static_cast<long>(ir.bottom));
            }

            LONG maxB     = client.top;
            LONG minT     = client.bottom;
            LONG maxR     = client.left;
            LONG minL     = client.right;
            for (int i = 0; i < n; ++i)
            {
                RECT ir{};
                if (!TabCtrl_GetItemRect(hTab, i, &ir))
                    break;
                if (ir.bottom > maxB)
                    maxB = ir.bottom;
                if (ir.top < minT)
                    minT = ir.top;
                if (ir.right > maxR)
                    maxR = ir.right;
                if (ir.left < minL)
                    minL = ir.left;
            }
            if (style & TCS_VERTICAL)
            {
                const LONG distL = maxR - client.left;
                const LONG distR = client.right - minL;
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate WARN fallback-axis vertical: maxR=%ld minL=%ld "
                    L"distL=%ld distR=%ld clientL/R=(%ld,%ld)",
                    static_cast<long>(maxR),
                    static_cast<long>(minL),
                    static_cast<long>(distL),
                    static_cast<long>(distR),
                    static_cast<long>(client.left),
                    static_cast<long>(client.right));
            }
            else if (style & TCS_BOTTOM)
            {
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate WARN fallback-axis bottom tabs: minT=%ld "
                    L"client.top+2=%ld (fail if minT<=client.top+2)",
                    static_cast<long>(minT),
                    static_cast<long>(client.top + 2));
            }
            else
            {
                const LONG marginUnderTabs = (client.bottom - 2) - maxB;
                DebugLog(DBG_WARNING,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate WARN fallback-axis top tabs: maxB=%ld "
                    L"client.bottom=%ld (client.bottom-2)-maxB=%ld (fail if maxB>=client.bottom-2; need gap>=1px)",
                    static_cast<long>(maxB),
                    static_cast<long>(client.bottom),
                    static_cast<long>(marginUnderTabs));
            }
        }

        [[nodiscard]] bool TabDisplayBodyFallbackFromItemRects(HWND hTab, const RECT& client, UINT style, RECT* outBody) noexcept
        {
            const int n = TabCtrl_GetItemCount(hTab);
            if (n <= 0 || !outBody)
                return false;

            if (style & TCS_VERTICAL)
            {
                LONG maxR = client.left;
                LONG minL = client.right;
                for (int i = 0; i < n; ++i)
                {
                    RECT ir{};
                    if (!TabCtrl_GetItemRect(hTab, i, &ir))
                        return false;
                    if (ir.right > maxR)
                        maxR = ir.right;
                    if (ir.left < minL)
                        minL = ir.left;
                }
                const LONG distL = maxR - client.left;
                const LONG distR = client.right - minL;
                *outBody = client;
                if (distL <= 0 && distR <= 0)
                    return false;
                if (distL < distR)
                {
                    if (maxR >= client.right - 2)
                        return false;
                    outBody->left = maxR;
                }
                else
                {
                    if (minL <= client.left + 2)
                        return false;
                    outBody->right = minL;
                }
                return NonEmptyRect(*outBody);
            }

            if (style & TCS_BOTTOM)
            {
                LONG minT = client.bottom;
                for (int i = 0; i < n; ++i)
                {
                    RECT ir{};
                    if (!TabCtrl_GetItemRect(hTab, i, &ir))
                        return false;
                    if (ir.top < minT)
                        minT = ir.top;
                }
                if (minT <= client.top + 2)
                    return false;
                *outBody = client;
                outBody->bottom = minT;
                return NonEmptyRect(*outBody);
            }

            LONG maxB = client.top;
            for (int i = 0; i < n; ++i)
            {
                RECT ir{};
                if (!TabCtrl_GetItemRect(hTab, i, &ir))
                    return false;
                if (ir.bottom > maxB)
                    maxB = ir.bottom;
            }
            if (maxB >= client.bottom - 2)
                return false;
            *outBody = client;
            outBody->top = maxB;
            return NonEmptyRect(*outBody);
        }

        // Сужение прямоугольника тела для «дыры»: границу у полосы вкладок не сдвигаем (симметричный Inflate
        // «съедает» 1 px хрома). Верхние вкладки — inset L/R/B; TCS_BOTTOM — L/R/T; TCS_VERTICAL — T/R/B.
        void ApplyTabBodyChromeSoftInset(UINT style, RECT* body) noexcept
        {
            if (!body)
                return;
            const int k = kTabSoftInvalidateBodyInset;
            if (style & TCS_VERTICAL)
            {
                body->top += k;
                body->bottom -= k;
                body->right -= k;
                return;
            }
            if (style & TCS_BOTTOM)
            {
                body->left += k;
                body->right -= k;
                body->top += k;
                return;
            }
            body->left += k;
            body->right -= k;
            body->bottom -= k;
        }

        // У границы язычки/тело + активная вкладка: полосы в RGN_DIFF не попадали в регион хрома.
        // Сужаем дыру на kTabHoleSeamInvalidateShrinkPx со стороны полосы вкладок (не вырождая прямоугольник).
        void ShrinkTabBodyHoleTowardBodyForSeamInvalidate(UINT style, RECT* hole) noexcept
        {
            if (!hole || !NonEmptyRect(*hole))
                return;
            const int kShrink = kTabHoleSeamInvalidateShrinkPx;
            if (style & TCS_VERTICAL)
            {
                const int maxDelta = (hole->right - hole->left) - 1;
                const int delta    = (std::min)(kShrink, (std::max)(0, maxDelta));
                hole->left += delta;
                return;
            }
            if (style & TCS_BOTTOM)
            {
                const int maxDelta = (hole->bottom - hole->top) - 1;
                const int delta    = (std::min)(kShrink, (std::max)(0, maxDelta));
                hole->bottom -= delta;
                return;
            }
            const int maxDelta = (hole->bottom - hole->top) - 1;
            const int delta    = (std::min)(kShrink, (std::max)(0, maxDelta));
            hole->top += delta;
        }

        // Деградация: регион язычков в клиенте самого таба (без полной инвалидации родителя).
        void InvalidateTabTongueFallbackInParent(HWND hTab) noexcept
        {
            if (!hTab || !IsWindow(hTab))
                return;

            RECT unionClient{};
            bool haveUnion = false;
            const int n = TabCtrl_GetItemCount(hTab);
            for (int i = 0; i < n; ++i)
            {
                RECT ir{};
                if (!TabCtrl_GetItemRect(hTab, i, &ir))
                    continue;
                if (!haveUnion)
                {
                    unionClient = ir;
                    haveUnion   = NonEmptyRect(unionClient);
                }
                else
                {
                    RECT u{};
                    if (UnionRect(&u, &unionClient, &ir))
                    {
                        unionClient = u;
                        haveUnion   = NonEmptyRect(unionClient);
                    }
                }
            }

            if (haveUnion && NonEmptyRect(unionClient))
            {
                wil::unique_hrgn rgn(CreateRectRgnIndirect(&unionClient));
                if (rgn)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Tab] SoftInvalidate fallback: union GetItemRect hTab=0x%p "
                        L"clipClient=(%ld,%ld)-(%ld,%ld)",
                        hTab,
                        static_cast<long>(unionClient.left),
                        static_cast<long>(unionClient.top),
                        static_cast<long>(unionClient.right),
                        static_cast<long>(unionClient.bottom));
                    (void)InvalidateRgn(hTab, rgn.get(), FALSE);
                    return;
                }
            }

            RECT cr{};
            if (GetClientRect(hTab, &cr) && NonEmptyRect(cr))
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate fallback: InvalidateRect весь клиент hTab=0x%p",
                    hTab);
                (void)InvalidateRect(hTab, nullptr, FALSE);
                return;
            }

            (void)RedrawWindow(hTab, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ERASE);
        }

        void SoftInvalidateTabThemedChromeClient(HWND hTab) noexcept
        {
            RECT client{};
            if (!GetClientRect(hTab, &client))
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate: GetClientRect failed hTab=0x%p — fallback язычков",
                    hTab);
                InvalidateTabTongueFallbackInParent(hTab);
                return;
            }

            // Клиент 0×N / N×0: InvalidateRect(hTab,null) не отмечает пиксели — fallback union язычков / RedrawWindow.
            if (!NonEmptyRect(client))
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate: GetClientRect вырожден hTab=0x%p "
                    L"client=(%ld,%ld)-(%ld,%ld) — fallback язычков (без полного parent)",
                    hTab,
                    static_cast<long>(client.left),
                    static_cast<long>(client.top),
                    static_cast<long>(client.right),
                    static_cast<long>(client.bottom));
                InvalidateTabTongueFallbackInParent(hTab);
                return;
            }

            const UINT style = static_cast<UINT>(GetWindowLongW(hTab, GWL_STYLE));
            const RECT     bodyBeforeAdjust = client;
            RECT           bodyRaw          = client;
            const BOOL     adjOk            = TabCtrl_AdjustRect(hTab, FALSE, &bodyRaw);
            const TabAdjustPlausibleReject plausibleRej = ClassifyTabAdjustRectNotPlausible(client, bodyRaw, style);

            if (!adjOk && plausibleRej != TabAdjustPlausibleReject::None)
            {
                if (!TabDisplayBodyFallbackFromItemRects(hTab, client, style, &bodyRaw))
                {
                    LogTabSoftInvalidateWarnPipeline(
                        hTab, client, style, bodyBeforeAdjust, bodyRaw, adjOk, plausibleRej);
                    DebugLog(DBG_WARNING,
                        L"[GlassCore][ThemePaint][Tab] SoftInvalidate: нет прямоугольника тела "
                        L"(AdjustRect=FALSE, items=%d) — деградация: fallback язычков",
                        TabCtrl_GetItemCount(hTab));
                    InvalidateTabTongueFallbackInParent(hTab);
                    return;
                }
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate: тело по TabCtrl_GetItemRect "
                    L"hTab=0x%p items=%d bodyRaw=(%ld,%ld)-(%ld,%ld)",
                    hTab,
                    TabCtrl_GetItemCount(hTab),
                    static_cast<long>(bodyRaw.left),
                    static_cast<long>(bodyRaw.top),
                    static_cast<long>(bodyRaw.right),
                    static_cast<long>(bodyRaw.bottom));
            }
            else if (!adjOk)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate: TabCtrl_AdjustRect=FALSE но prc пригоден "
                    L"hTab=0x%p bodyRaw=(%ld,%ld)-(%ld,%ld)",
                    hTab,
                    static_cast<long>(bodyRaw.left),
                    static_cast<long>(bodyRaw.top),
                    static_cast<long>(bodyRaw.right),
                    static_cast<long>(bodyRaw.bottom));
            }

            RECT body = bodyRaw;
            ApplyTabBodyChromeSoftInset(style, &body);

            RECT bodyClip{};
            if (!IntersectRect(&bodyClip, &client, &body) || !NonEmptyRect(bodyClip))
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate: bodyClip degenerate hTab=0x%p "
                    L"client=(%ld,%ld)-(%ld,%ld) bodyRaw=(%ld,%ld)-(%ld,%ld) bodyInfl=(%ld,%ld)-(%ld,%ld) "
                    L"— деградация: fallback язычков",
                    hTab,
                    static_cast<long>(client.left),
                    static_cast<long>(client.top),
                    static_cast<long>(client.right),
                    static_cast<long>(client.bottom),
                    static_cast<long>(bodyRaw.left),
                    static_cast<long>(bodyRaw.top),
                    static_cast<long>(bodyRaw.right),
                    static_cast<long>(bodyRaw.bottom),
                    static_cast<long>(body.left),
                    static_cast<long>(body.top),
                    static_cast<long>(body.right),
                    static_cast<long>(body.bottom));
                InvalidateTabTongueFallbackInParent(hTab);
                return;
            }

            const RECT outerTab = client;
            RECT       holeTab  = bodyClip;
            ShrinkTabBodyHoleTowardBodyForSeamInvalidate(style, &holeTab);
            const bool haveHole = NonEmptyRect(holeTab);

            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ThemePaint][Tab] SoftInvalidate hTab=0x%p "
                L"внешнийКлиент=(%ld,%ld)-(%ld,%ld) вычитаемый:%s (%ld,%ld)-(%ld,%ld)",
                hTab,
                static_cast<long>(outerTab.left),
                static_cast<long>(outerTab.top),
                static_cast<long>(outerTab.right),
                static_cast<long>(outerTab.bottom),
                haveHole ? L"да" : L"нет",
                static_cast<long>(holeTab.left),
                static_cast<long>(holeTab.top),
                static_cast<long>(holeTab.right),
                static_cast<long>(holeTab.bottom));

            wil::unique_hrgn rgn(CreateRectRgnIndirect(&outerTab));
            if (!rgn)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Tab] SoftInvalidate: CreateRectRgn outer failed hTab=0x%p — fallback",
                    hTab);
                InvalidateTabTongueFallbackInParent(hTab);
                return;
            }

            if (haveHole)
            {
                wil::unique_hrgn holeRgn(CreateRectRgnIndirect(&holeTab));
                if (!holeRgn)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Tab] SoftInvalidate: CreateRectRgn hole failed hTab=0x%p — fallback",
                        hTab);
                    InvalidateTabTongueFallbackInParent(hTab);
                    return;
                }
                const int cr = CombineRgn(rgn.get(), rgn.get(), holeRgn.get(), RGN_DIFF);
                if (cr == ERROR || cr == NULLREGION)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCore][ThemePaint][Tab] SoftInvalidate: CombineRgn hTab=0x%p cr=%d — fallback",
                        hTab,
                        cr);
                    InvalidateTabTongueFallbackInParent(hTab);
                    return;
                }
            }

            (void)InvalidateRgn(hTab, rgn.get(), FALSE);
        }
    }

    void RefreshCachedForegroundWindow() noexcept
    {
        const HWND fg = GetForegroundWindow();
        g_cachedForegroundHwnd.store(reinterpret_cast<std::uintptr_t>(fg), std::memory_order_release);
    }

    void ClearCachedForegroundWindow() noexcept
    {
        g_cachedForegroundHwnd.store(0, std::memory_order_release);
    }

    void SoftInvalidateThemedChromeClientsForRoot(HWND hRoot) noexcept
    {
        std::unordered_set<std::uintptr_t> pass1Done;
        (void)SoftInvalidateThemedChromeClientsForRootImpl(hRoot, pass1Done);
    }

    void RegisterThemedChromeClient(HWND hControl) noexcept
    {
        RegisterThemedChromeClientImpl(hControl);
    }

    void UnregisterThemedChromeClient(HWND hControl) noexcept
    {
        UnregisterThemedChromeClientImpl(hControl);
    }

    void RebindThemedChromeClientBucket(HWND hControl) noexcept
    {
        RebindThemedChromeClientBucketImpl(hControl);
    }

    void RebindThemedChromeClientsAfterWinFormsWindowMarked(HWND hMarkedWinFormsWindow) noexcept
    {
        RebindThemedChromeClientsAfterWinFormsWindowMarkedImpl(hMarkedWinFormsWindow);
    }

    void RunDeferredThemedChromeSoftInvalidate(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        HWND gaRoot = hRoot;
        if (const HWND a = GetAncestor(hRoot, GA_ROOT))
            gaRoot = a;
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ThemePaint][Chrome] DeferredSoftInvalidate hRoot=0x%p gaRoot=0x%p",
            hRoot,
            gaRoot);
        SoftInvalidateThemedChromeClientsForGaRootActivationTree(hRoot);
    }

    void FinalizeRootActivationVisualUpdate(HWND hRoot, bool ncActivateFromWmNcActivate) noexcept
    {
        HWND gaRoot = hRoot;
        if (hRoot && IsWindow(hRoot))
        {
            if (const HWND a = GetAncestor(hRoot, GA_ROOT))
                gaRoot = a;
            std::unique_lock lk(g_themeRootNcMutex);
            g_themeRootNcActive[reinterpret_cast<std::uintptr_t>(gaRoot)] = ncActivateFromWmNcActivate;
        }

        if (gaRoot && IsWindow(gaRoot))
        {
            GlassCore::Root::ApplyRootBorderOnNcActivateChange(
                gaRoot, ncActivateFromWmNcActivate != FALSE);
        }

        RefreshCachedForegroundWindow();

        // Инвалидация DTB-хрома при смене визуальной активности нужна и когда обе альфы
        // разрешились из общего ключа реестра (прежний ранний выход по resolvedFromRoot гасил
        // перерисовку — стартовый кадр навсегда оставался inactive; заодно покрыт live-превью
        // GlassSettings с разными active/inactive).
        const bool regAlphaDiffers =
            g_drawThemeConstantAlphaActive != g_drawThemeConstantAlphaInactive;
        const bool previewAlphaDiffers =
            g_previewAppliesToStandardThemedChrome.load(std::memory_order_acquire)
            && g_previewThemeAlphaActiveValid.load(std::memory_order_acquire)
            && g_previewThemeAlphaInactiveValid.load(std::memory_order_acquire)
            && (g_previewThemeAlphaActive.load(std::memory_order_acquire)
                != g_previewThemeAlphaInactive.load(std::memory_order_acquire));

        if (regAlphaDiffers || previewAlphaDiffers)
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][ThemePaint][Chrome] Finalize hRoot=0x%p gaRoot=0x%p ncActive=%d "
                L"alphaActive=%u alphaInactive=%u",
                hRoot,
                gaRoot,
                ncActivateFromWmNcActivate ? 1 : 0,
                static_cast<unsigned>(g_drawThemeConstantAlphaActive),
                static_cast<unsigned>(g_drawThemeConstantAlphaInactive));
            if (gaRoot && IsWindow(gaRoot))
            {
                const BOOL posted = PostMessageW(
                    gaRoot,
                    WndCompat::WM_GLASS_DEFER_THEMED_CHROME_INVALIDATE,
                    0,
                    0);
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCore][ThemePaint][Chrome] Finalize defer PostMessage gaRoot=0x%p ok=%d",
                    gaRoot,
                    static_cast<int>(posted));
            }
        }
    }

    void RemoveThemePaintRootNcState(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        std::unique_lock lk(g_themeRootNcMutex);
        (void)g_themeRootNcActive.erase(reinterpret_cast<std::uintptr_t>(hRoot));
        RemoveThemedChromeClientsForRootImpl(hRoot);
    }

    void ClearAllThemePaintRootNcState() noexcept
    {
        std::unique_lock lk(g_themeRootNcMutex);
        g_themeRootNcActive.clear();
        ClearThemedChromeRegistryImpl();
    }

    std::uint8_t GetDrawThemeConstantAlpha() noexcept
    {
        return g_drawThemeConstantAlphaActive;
    }

    std::uint8_t GetDrawThemeConstantAlphaInactive() noexcept
    {
        return g_drawThemeConstantAlphaInactive;
    }

    void ClearThemePaintPreviewAlpha() noexcept
    {
        g_previewThemeAlphaActiveValid.store(false, std::memory_order_release);
        g_previewThemeAlphaInactiveValid.store(false, std::memory_order_release);
        g_previewAppliesToStandardThemedChrome.store(false, std::memory_order_release);
    }

    void SetThemePaintPreviewForGlassSettingsUi(std::uint8_t alpha) noexcept
    {
        g_previewThemeAlphaActive.store(alpha, std::memory_order_release);
        g_previewThemeAlphaInactive.store(alpha, std::memory_order_release);
        g_previewThemeAlphaActiveValid.store(true, std::memory_order_release);
        g_previewThemeAlphaInactiveValid.store(true, std::memory_order_release);
        g_previewAppliesToStandardThemedChrome.store(true, std::memory_order_release);
    }

    void SetThemePaintPreviewPairForGlassSettingsUi(std::uint8_t activeAlpha, std::uint8_t inactiveAlpha) noexcept
    {
        g_previewThemeAlphaActive.store(activeAlpha, std::memory_order_release);
        g_previewThemeAlphaInactive.store(inactiveAlpha, std::memory_order_release);
        g_previewThemeAlphaActiveValid.store(true, std::memory_order_release);
        g_previewThemeAlphaInactiveValid.store(true, std::memory_order_release);
        g_previewAppliesToStandardThemedChrome.store(true, std::memory_order_release);
    }

    std::uint8_t GetEffectiveDrawThemeConstantAlphaForSession(
        GlassShared::GlassControlKind kind,
        HWND hWndChrome) noexcept
    {
        const bool previewChrome = g_previewAppliesToStandardThemedChrome.load(std::memory_order_acquire);
        const bool isPreviewKind =
            kind == GlassShared::GlassControlKind::Button ||
            kind == GlassShared::GlassControlKind::ComboBox ||
            kind == GlassShared::GlassControlKind::WinForms_ComboBox ||
            kind == GlassShared::GlassControlKind::TabControl ||
            kind == GlassShared::GlassControlKind::UpDown;

        const bool rootVisuallyActive = RootVisuallyActiveForThemedChrome(hWndChrome);

        const std::uint8_t regBranch =
            rootVisuallyActive ? g_drawThemeConstantAlphaActive : g_drawThemeConstantAlphaInactive;

        std::uint8_t result = regBranch;
        if (previewChrome && isPreviewKind)
        {
            if (rootVisuallyActive && g_previewThemeAlphaActiveValid.load(std::memory_order_acquire))
                result = g_previewThemeAlphaActive.load(std::memory_order_acquire);
            else if (!rootVisuallyActive && g_previewThemeAlphaInactiveValid.load(std::memory_order_acquire))
                result = g_previewThemeAlphaInactive.load(std::memory_order_acquire);
        }

        return result;
    }

    std::uint8_t GetEffectiveTabChromaThresholdForSession(
        GlassShared::GlassControlKind kind,
        HWND hWndChrome) noexcept
    {
        if (kind != GlassShared::GlassControlKind::TabControl)
            return kDefaultTabChromaThreshold;
        if (!hWndChrome || !IsWindow(hWndChrome))
            return g_tabChromaThreshActive;
        const bool rootVisuallyActive = RootVisuallyActiveForThemedChrome(hWndChrome);
        return rootVisuallyActive ? g_tabChromaThreshActive : g_tabChromaThreshInactive;
    }

    void LoadFromRegistry() noexcept
    {
        const auto activeRead =
            Shared::RootSwcaRegistryQuery::ReadRootGlassDrawThemeConstantAlphaWithSource(true);
        const auto inactiveRead =
            Shared::RootSwcaRegistryQuery::ReadRootGlassDrawThemeConstantAlphaWithSource(false);
        g_drawThemeConstantAlphaActive =
            static_cast<std::uint8_t>(std::min<DWORD>(activeRead.value, 255u));
        g_drawThemeConstantAlphaInactive =
            static_cast<std::uint8_t>(std::min<DWORD>(inactiveRead.value, 255u));

        const DWORD ta = Shared::RegHelper::Get<DWORD>(
            { Shared::RegHelper::kBwmRootActivePath },
            kRegValueGlassTabChromaThreshold,
            static_cast<DWORD>(kDefaultTabChromaThreshold));
        const DWORD taCapped = std::min<DWORD>(ta, static_cast<DWORD>(kMaxTabChromaThreshold));
        const auto tiOpt = Shared::RegHelper::TryGet<DWORD>(
            { Shared::RegHelper::kBwmRootInactivePath },
            kRegValueGlassTabChromaThreshold);
        const DWORD ti
            = tiOpt.has_value() ? std::min<DWORD>(tiOpt.value(), static_cast<DWORD>(kMaxTabChromaThreshold)) : taCapped;
        g_tabChromaThreshActive   = static_cast<std::uint8_t>(taCapped);
        g_tabChromaThreshInactive = static_cast<std::uint8_t>(ti);
    }

    void ReloadThemePaintSettingsFromRegistry() noexcept
    {
        LoadFromRegistry();
        GlassCore::Text::LoadRenderTextSettings();
    }

    void InvalidateTopLevelWindowsInCurrentProcess() noexcept
    {
        const RedrawSameProcessCtx ctx{ GetCurrentProcessId() };
        EnumWindows(RedrawTopLevelSameProcessEnum, reinterpret_cast<LPARAM>(&ctx));
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][ThemePaint] InvalidateTopLevelWindowsInCurrentProcess: alphaActive=%u alphaInactive=%u "
            L"tabChromaTh=%u/%u PID=%lu",
            static_cast<unsigned>(g_drawThemeConstantAlphaActive),
            static_cast<unsigned>(g_drawThemeConstantAlphaInactive),
            static_cast<unsigned>(g_tabChromaThreshActive),
            static_cast<unsigned>(g_tabChromaThreshInactive),
            static_cast<unsigned long>(ctx.pid));
    }

    void OnUserThemePaintSettingsChanged() noexcept
    {
        ReloadThemePaintSettingsFromRegistry();
        InvalidateTopLevelWindowsInCurrentProcess();
    }
} // namespace GlassCore::ThemePaint
