#include "SharedPch.h"
#include <RootBackdropPolicy.h>

#include <array>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace Shared::RootBackdropPolicy
{
    namespace
    {
        constexpr wchar_t kBackdropAppliedProp[] = L"GlassPlatform.RootBackdropApplied";

        std::mutex              g_appliedHwndsMutex;
        std::unordered_set<HWND> g_appliedHwnds;

        [[nodiscard]] bool EqualsClassImpl(const wchar_t* a, const wchar_t* b) noexcept
        {
            if (!a || !b)
                return false;
            return _wcsicmp(a, b) == 0;
        }

        [[nodiscard]] bool EqualsClass(const std::wstring& cls, const wchar_t* lit) noexcept
        {
            return EqualsClassImpl(cls.c_str(), lit);
        }
    }

    std::wstring NormalizeWindowClass(HWND hWnd) noexcept
    {
        WCHAR raw[MAX_PATH]{};
        if (!GetClassNameW(hWnd, raw, MAX_PATH))
            return L"";

        std::wstring cls = raw;

        if (cls.rfind(L"WindowsForms10.", 0) == 0)
        {
            constexpr size_t prefixLen = 15;
            const size_t pos = cls.find(L'.', prefixLen);
            if (pos != std::wstring::npos)
                return L"." + cls.substr(prefixLen, pos - prefixLen);
        }

        if (cls.rfind(L"HwndWrapper", 0) == 0)
            return L"WPF_HwndWrapper";

        if (cls == L"ApplicationFrameWindow")
            return L"WinUI_Frame";

        return cls;
    }

    bool IsNormalizedClassBackdropExempt(const std::wstring& cls) noexcept
    {
        if (cls.empty())
            return false;

        if (cls.rfind(L"WPF_HwndWrapper", 0) == 0)
            return true;

        // Внимание: классы проводника (CabinetWClass / ExploreWClass) сюда НЕ включать.
        // IsNormalizedClassBackdropExempt → IsHwndBackdropExempt → GlassAttachGate::ShouldAttachGlassToHwnd:
        // при exempt для корня весь поддерево проводника перестаёт получать стекло и субклассы.
        // Подавление только root SWCA (без ломания патчей) — отдельная логика, не этот список.
        static const std::array<const wchar_t*, 43> kHardList{
            L"IME",
            L"DcppUserAdapterWindowClass",
            L"SystemUserAdapterWindowClass",
            L"Shell Preview Extension Host Foreground Msg",
            L"URL Moniker Notification Window",
            L"Chrome_WidgetWin_0",
            L"Chrome_WidgetWin_1",
            L"Chrome_MessageWindow",
            L"CLIPBRDWNDCLASS",
            L"OleMainThreadWndClass",
            L"Base_PowerMessageWindow",
            L"CicMarshalWndClass",
            L"MITMessageOnlyWindowClass",
            L"MSCTFIME UI",
            L"WorkerW",
            L"Progman",
            L"SCENIC_DROPSHADOW_WINDOW_CLASS",
            L"SysShadow",
            L"Xaml_WindowedPopupClass",
            L"XamlExplorerHostIslandWindow",
            // CoreWindow / ControlCenterWindow: SWCA/Root exempt. NC attach — только через
            // Classify carve-out (ShellNotificationCore) ДО IsHardBlockedClass. Не удалять.
            L"Windows.UI.Core.CoreWindow",
            L"ControlCenterWindow",
            L"Windows.UI.Input.InputSite.WindowClass",
            L"WinUI_urlmonMessageWindow",
            L"crashpad_SessionEndWatcher",
            L"ApplicationFrameInputSinkWindow",
            L"ANIMATION_TIMER_HWND",
            L"Message",
            L"WndCompat.ControlWindow",
            L"BlackWindowManagerWorkerW",
            L"RainmeterMeterWindow",
            L"RainmeterSystem",
            L"DummyRainWClass",
            L"GDI+ Hook Window Class",
            L"Event Viewer Snapin Synch",
            L"UIAInvokeHelperWndClass",
            L"XCPTimerClass",
            L"Shell_InputSwitchTopLevelWindow",
            L"Shell_InputSwitchDismissOverlay",
            // Notification Center / Action Center dismiss layers (Win11) —
            // не Root.OtherTopLevel, без SWCA/DWM (аналог Shell_InputSwitch*).
            L"Shell_LightDismissMessageWindow",
            L"Shell_LightDismissOverlay",
            L"TopLevelWindowForOverflowXamlIsland",
            L"WinUIDesktopWin32WindowClass",
        };

        for (const wchar_t* entry : kHardList)
        {
            if (EqualsClass(cls, entry))
                return true;
        }

        return false;
    }

    bool IsHwndBackdropExempt(HWND hWnd) noexcept
    {
        if (!hWnd)
            return false;
        return IsNormalizedClassBackdropExempt(NormalizeWindowClass(hWnd));
    }

    namespace
    {
        [[nodiscard]] const wchar_t* CurrentProcessBaseName() noexcept
        {
            static thread_local wchar_t s_base[MAX_PATH]{};
            static thread_local bool s_ready = false;
            if (!s_ready)
            {
                wchar_t path[MAX_PATH]{};
                const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
                if (n == 0 || n >= MAX_PATH)
                {
                    s_base[0] = L'\0';
                }
                else
                {
                    const wchar_t* base = path;
                    for (const wchar_t* p = path; *p; ++p)
                    {
                        if (*p == L'\\' || *p == L'/')
                            base = p + 1;
                    }
                    wcsncpy_s(s_base, base, _TRUNCATE);
                }
                s_ready = true;
            }
            return s_base;
        }
    }

    bool IsShellNotificationHostProcess() noexcept
    {
        const wchar_t* base = CurrentProcessBaseName();
        return _wcsicmp(base, L"ShellExperienceHost.exe") == 0
            || _wcsicmp(base, L"ShellHost.exe") == 0;
    }

    bool IsShellHostProcess() noexcept
    {
        return _wcsicmp(CurrentProcessBaseName(), L"ShellHost.exe") == 0;
    }

    bool IsShellExperienceHostProcess() noexcept
    {
        return _wcsicmp(CurrentProcessBaseName(), L"ShellExperienceHost.exe") == 0;
    }

    bool IsStartMenuHostProcess() noexcept
    {
        return _wcsicmp(CurrentProcessBaseName(), L"StartMenuExperienceHost.exe") == 0;
    }

    bool IsSearchHostProcess() noexcept
    {
        const wchar_t* base = CurrentProcessBaseName();
        return _wcsicmp(base, L"SearchHost.exe") == 0
            || _wcsicmp(base, L"SearchApp.exe") == 0;
    }

    bool IsCoreWindowHostProcess() noexcept
    {
        return IsShellNotificationHostProcess()
            || IsStartMenuHostProcess()
            || IsSearchHostProcess();
    }

    bool IsFileExplorerShellRootHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        if (GetAncestor(hWnd, GA_ROOT) != hWnd)
            return false;
        wchar_t cls[96]{};
        if (GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) <= 0)
            return false;
        return _wcsicmp(cls, L"CabinetWClass") == 0 || _wcsicmp(cls, L"ExploreWClass") == 0;
    }

    namespace
    {
        [[nodiscard]] static bool WalkForShellTabClass(HWND hwnd, const wchar_t* want, int depthLeft) noexcept
        {
            if (!hwnd || depthLeft < 0)
                return false;
            wchar_t buf[96]{};
            if (GetClassNameW(hwnd, buf, static_cast<int>(std::size(buf))) > 0
                && _wcsicmp(buf, want) == 0)
                return true;
            for (HWND ch = GetWindow(hwnd, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT))
            {
                if (WalkForShellTabClass(ch, want, depthLeft - 1))
                    return true;
            }
            return false;
        }
    } // namespace

    bool ExplorerShellRootHasShellTabDescendant(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return false;
        for (HWND ch = GetWindow(hRoot, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT))
        {
            if (WalkForShellTabClass(ch, L"ShellTabWindowClass", 14))
                return true;
        }
        return false;
    }

    void NotifyGlassAppliedRootBackdrop(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;
        (void)SetPropW(hWnd, kBackdropAppliedProp, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(1)));
        {
            std::lock_guard<std::mutex> lock(g_appliedHwndsMutex);
            g_appliedHwnds.insert(hWnd);
        }
    }

    void NotifyGlassClearedRootBackdrop(HWND hWnd) noexcept
    {
        if (!hWnd)
            return;
        (void)RemovePropW(hWnd, kBackdropAppliedProp);
        {
            std::lock_guard<std::mutex> lock(g_appliedHwndsMutex);
            g_appliedHwnds.erase(hWnd);
        }
    }

    bool DidGlassApplyRootBackdrop(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        return GetPropW(hWnd, kBackdropAppliedProp) != nullptr;
    }

    std::vector<HWND> SnapshotGlassAppliedRootHwnds() noexcept
    {
        std::lock_guard<std::mutex> lock(g_appliedHwndsMutex);
        return std::vector<HWND>(g_appliedHwnds.begin(), g_appliedHwnds.end());
    }
}
