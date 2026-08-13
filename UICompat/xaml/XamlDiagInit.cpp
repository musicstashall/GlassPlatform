#include "UiPch.h"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/XamlGlassIds.hpp"
#include "xaml/UiTapSite.hpp"
#include "xaml/XamlComLifetime.hpp"

#include <DebugLog.h>
#include <GlassMessages.h>
#include <RootBackdropPolicy.h>
#include <objbase.h>
#include <SystemHelper.h>
#include <dwmapi.h>
#include <atomic>
#include <cstdint>

#pragma comment(lib, "dwmapi.lib")

namespace
{
    using InitializeXamlDiagnosticsExFn = HRESULT(WINAPI*)(
        LPCWSTR endPointName,
        DWORD pid,
        LPCWSTR wszDllXamlDiagnostics,
        LPCWSTR wszTAPDllName,
        CLSID tapClsid,
        LPCWSTR wszInitializationData);

    using GlassCore_SetXamlHostDllNotifyFn = void(WINAPI*)(void(WINAPI*)(HMODULE, const wchar_t*));
    using GlassCore_NotifyExistingXamlHostDllsFn         = void(WINAPI*)();

    // Как TranslucentTB ExplorerTAP/tapsite.cpp: Live Visual Tree endpoint.
    constexpr const wchar_t* kEndpointName = L"VisualDiagConnection1";

    // Паритет tapsite.cpp: до 60 неудач с паузой 500 ms (~30 с).
    constexpr int kIxdeDonorAttempts  = 60;
    constexpr int kIxdeDonorSleepMs   = 500;

    constexpr int kKickWaitAfterFailMs = 3000;

    // Windhawk Start Menu Styler: перебор endpoint при ERROR_NOT_FOUND (Start/Search).
    constexpr int kShellSurfaceEndpointMax = 64;

    std::atomic<bool>               g_diagRunning{ false };
    std::atomic<bool>               g_xamlStopRequested{ false };
    std::atomic<bool>               g_tapPathReady{ false };
    std::atomic<bool>               g_ixdeSucceeded{ false };
    std::atomic<bool>               g_installThreadPosted{ false };
    std::atomic<bool>               g_installThreadActive{ false };
    std::atomic<bool>               g_ixdeKickRequested{ false };
    // После успешного IXDE поток ждёт true перед CoUninitialize — выставляется в Stop после ShutdownActiveWatcher.
    std::atomic<bool>               g_ixdeStaMayExit{ true };
    // Start/Search: IXDE на UI-потоке CoreWindow (не IxdeInstallThreadProc).
    std::atomic<bool>               g_shellSurfaceIxdePath{ false };
    std::atomic<DWORD>              g_shellSurfaceThreadId{ 0 };
    std::atomic<bool>               g_searchDeferPosted{ false };
    // SEH: toast/1×1 отложил IXDE — повторный поиск высокой NC (cy≥600).
    std::atomic<bool>               g_sehTallIxdeRetryArmed{ false };
    // ShellHost: ControlCenterWindow cx/cy=0 при AttachExisting — retry до размера.
    std::atomic<bool>               g_shellHostSizedIxdeRetryArmed{ false };

    std::atomic<int32_t>            g_ixdeSurfaceInterestOrKick{ 0 };

    std::atomic<std::uintptr_t>     g_windowsUIXamlDllFromHandler{ 0 }; // только наблюдаемое Ldr-событие, без FreeLibrary

    GlassCore_SetXamlHostDllNotifyFn     s_pGlassCore_SetXamlHostDllNotify     = nullptr;
    GlassCore_NotifyExistingXamlHostDllsFn s_pGlassCore_NotifyExistingXamlHostDlls = nullptr;
    bool                                 s_registeredGlassNotify                 = false;

    wchar_t g_tapDllFullPath[MAX_PATH]{};

    DWORD WINAPI IxdeInstallThreadProc(LPVOID) noexcept;
    void ScheduleIxdeThreadIfPossible() noexcept;
    void InitShellSurfaceIxdeOnUiThread(HWND hwnd) noexcept;

    using WndCompatOnTaskbarIxdeOutcomeFn = void(WINAPI*)(BOOL succeeded);

    WndCompatOnTaskbarIxdeOutcomeFn ResolveWndCompatIxdeOutcome() noexcept
    {
        const HMODULE wnd = GetModuleHandleW(L"WndCompat.dll");
        if (!wnd)
            return nullptr;
        return reinterpret_cast<WndCompatOnTaskbarIxdeOutcomeFn>(
            GetProcAddress(wnd, "WndCompat_OnTaskbarIxdeInstallOutcome"));
    }

    void NotifyWndCompatIxdeOutcome(BOOL succeeded) noexcept
    {
        auto* pfn = ResolveWndCompatIxdeOutcome();
        if (pfn)
            pfn(succeeded);

        // NC CoreWindow: первый Notify может прийти до IXDE; после IXDE нужны re-notify и reapply.
        if (succeeded)
        {
            if (const HMODULE wnd = GetModuleHandleW(L"WndCompat.dll"))
            {
                using FlushFn = void(WINAPI*)();
                using ReadyFn = void(WINAPI*)() noexcept;
                if (const auto pFlush = reinterpret_cast<FlushFn>(
                        GetProcAddress(wnd, "WndCompat_FlushPendingCoreWindows")))
                {
                    pFlush();
                }
                if (const auto pReady = reinterpret_cast<ReadyFn>(
                        GetProcAddress(wnd, "WndCompat_OnCoreWindowIxdeReady")))
                {
                    pReady();
                }
            }
            UICompat::UiTapSite::ReloadSettingsAndReapply();
        }
    }

    [[nodiscard]] bool ShouldAbortInstall() noexcept
    {
        return g_xamlStopRequested.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool GetTapDllFullPath(wchar_t* outPath, size_t outPathSize) noexcept
    {
        HMODULE hModule = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetTapDllFullPath),
                &hModule))
        {
            return false;
        }

        if (GetModuleFileNameW(hModule, outPath, static_cast<DWORD>(outPathSize)) == 0)
            return false;

        return true;
    }

    void InterruptibleSleepMs(int totalMs) noexcept
    {
        int left = totalMs;
        while (left > 0 && !ShouldAbortInstall())
        {
            const int chunk = left > 50 ? 50 : left;
            Sleep(static_cast<DWORD>(chunk));
            left -= chunk;
        }
    }

    void WINAPI OnGlassCoreXamlDllNotify(HMODULE loadedModule, const wchar_t* baseName) noexcept
    {
        if (ShouldAbortInstall() || !loadedModule || !baseName)
            return;
        if (_wcsicmp(baseName, L"Windows.UI.Xaml.dll") != 0)
            return;

        g_windowsUIXamlDllFromHandler.store(
            reinterpret_cast<std::uintptr_t>(loadedModule),
            std::memory_order_release);
        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][XamlDiag] GlassCore/XamlHost: Windows.UI.Xaml.dll @ 0x%p (ModuleHandler)",
            loadedModule);
        ScheduleIxdeThreadIfPossible();
    }

    void RegisterGlassCoreXamlDllNotifyFromUICompat() noexcept
    {
        if (s_registeredGlassNotify)
            return;

        const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
        {
            DebugLog(DBG_WARNING, L"[UICompat][XamlDiag] GlassCore.dll не загружен — без ModuleHandler UWP-хоста");
            return;
        }

        s_pGlassCore_SetXamlHostDllNotify = reinterpret_cast<GlassCore_SetXamlHostDllNotifyFn>(
            GetProcAddress(hCore, "GlassCore_SetXamlHostDllNotify"));
        s_pGlassCore_NotifyExistingXamlHostDlls = reinterpret_cast<GlassCore_NotifyExistingXamlHostDllsFn>(
            GetProcAddress(hCore, "GlassCore_NotifyExistingXamlHostDlls"));

        if (!s_pGlassCore_SetXamlHostDllNotify)
        {
            DebugLog(DBG_WARNING, L"[UICompat][XamlDiag] GetProcAddress(SetXamlHostDllNotify) failed");
            return;
        }

        s_pGlassCore_SetXamlHostDllNotify(&OnGlassCoreXamlDllNotify);
        s_registeredGlassNotify = true;

        if (s_pGlassCore_NotifyExistingXamlHostDlls)
            s_pGlassCore_NotifyExistingXamlHostDlls();

        DebugLog(DBG_IMPORTANT, L"[UICompat][XamlDiag] Зарегистрирован ModuleHandler‑колбэк GlassCore для UWP‑XAML");
    }

    void UnregisterGlassCoreXamlDllNotifyFromUICompat() noexcept
    {
        if (!s_registeredGlassNotify || !s_pGlassCore_SetXamlHostDllNotify)
            return;

        s_pGlassCore_SetXamlHostDllNotify(nullptr);
        s_pGlassCore_SetXamlHostDllNotify     = nullptr;
        s_pGlassCore_NotifyExistingXamlHostDlls = nullptr;
        s_registeredGlassNotify               = false;
        g_windowsUIXamlDllFromHandler.store(0, std::memory_order_release);
    }

    void ScheduleIxdeThreadIfPossible() noexcept
    {
        if (!g_tapPathReady.load(std::memory_order_acquire))
            return;

        if (ShouldAbortInstall() || g_ixdeSucceeded.load(std::memory_order_acquire))
            return;

        // Start/Search/NC(SEH|ShellHost): IXDE только на UI-потоке окна — не IxdeInstallThreadProc.
        if (g_shellSurfaceIxdePath.load(std::memory_order_acquire)
            || Shared::RootBackdropPolicy::IsStartMenuHostProcess()
            || Shared::RootBackdropPolicy::IsSearchHostProcess()
            || Shared::RootBackdropPolicy::IsShellNotificationHostProcess())
            return;

        if (g_ixdeSurfaceInterestOrKick.load(std::memory_order_acquire) <= 0)
            return;

        g_ixdeKickRequested.store(true, std::memory_order_release);

        bool postedExpected = false;
        if (!g_installThreadPosted.compare_exchange_strong(
                postedExpected,
                true,
                std::memory_order_acq_rel))
            return;

        const HANDLE th = CreateThread(nullptr, 0, IxdeInstallThreadProc, nullptr, 0, nullptr);
        if (!th)
        {
            const DWORD gle = GetLastError();
            DebugLog(DBG_ERROR, L"[UICompat][XamlDiag] CreateThread(IXDE) failed GLE=%u", static_cast<unsigned>(gle));
            g_installThreadPosted.store(false, std::memory_order_release);
            return;
        }
        CloseHandle(th);
    }

    void BumpSurfaceInterest(HWND hwndLog, const wchar_t* tag) noexcept
    {
        const int v = g_ixdeSurfaceInterestOrKick.fetch_add(1, std::memory_order_acq_rel) + 1;
        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][XamlDiag] IXDE интерес ↑ tag=%ls hwnd=0x%p total=%d",
            tag ? tag : L"",
            hwndLog,
            static_cast<int>(v));
        ScheduleIxdeThreadIfPossible();
    }

    // ── Start/Search: IXDE на UI-потоке CoreWindow (Windhawk) ─────────────────

    // Приватные сообщения marshal: WM_NULL нельзя — хост на стопе рассылает WM_NULL
    // (PostWmNullToTrackedHwnds + broadcast) и это запускало init во время выгрузки.
    [[nodiscard]] UINT RunFromWindowThreadMsg() noexcept
    {
        static const UINT msg = RegisterWindowMessageW(L"GlassUICompat_RunFromWindowThread");
        return msg;
    }

    [[nodiscard]] UINT ShellSurfaceDeferMsg() noexcept
    {
        static const UINT msg = RegisterWindowMessageW(L"GlassUICompat_ShellSurfaceDeferInit");
        return msg;
    }

    struct RunFromWindowThreadParam
    {
        void (*proc)(PVOID) noexcept{};
        PVOID procParam{};
    };

    LRESULT CALLBACK CallWndProcRunOnUi(int code, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (code == HC_ACTION)
        {
            const auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
            if (cwp && cwp->message == RunFromWindowThreadMsg())
            {
                auto* p = reinterpret_cast<RunFromWindowThreadParam*>(cwp->lParam);
                if (p && p->proc)
                    p->proc(p->procParam);
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Windhawk RunFromWindowThread: приватное сообщение + параметр в cwp->lParam.
    // Callback выполняется синхронно внутри SendMessageTimeout → стек жив, UAF нет.
    [[nodiscard]] bool RunFromWindowThreadImpl(
        HWND hwnd,
        void (*fn)(PVOID) noexcept,
        PVOID param) noexcept
    {
        if (!hwnd || !IsWindow(hwnd) || !fn)
            return false;

        const DWORD tid = GetWindowThreadProcessId(hwnd, nullptr);
        if (!tid)
            return false;

        if (tid == GetCurrentThreadId())
        {
            fn(param);
            return true;
        }

        HMODULE hMod{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&RunFromWindowThreadImpl),
                &hMod))
        {
            hMod = GetModuleHandleW(nullptr);
        }

        const HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, CallWndProcRunOnUi, hMod, tid);
        if (!hook)
            return false;

        RunFromWindowThreadParam p{ fn, param };
        DWORD_PTR sendResult = 0;
        // Таймаут обязателен: SendMessage без таймаута вешал GlassGuiThread под g_cwMutex.
        const LRESULT sent = SendMessageTimeoutW(
            hwnd,
            RunFromWindowThreadMsg(),
            0,
            reinterpret_cast<LPARAM>(&p),
            SMTO_ABORTIFHUNG,
            5000,
            &sendResult);
        // Снимаем хук до выхода: после этого callback не может обратиться к стеку.
        UnhookWindowsHookEx(hook);
        return sent != 0;
    }

    // Обёртка со старой сигнатурой для внутренних вызовов.
    [[nodiscard]] bool RunFromWindowThread(
        HWND hwnd,
        void (*fn)(PVOID) noexcept,
        PVOID param) noexcept
    {
        return RunFromWindowThreadImpl(hwnd, fn, param);
    }

    std::atomic<HHOOK> g_searchDeferHook{ nullptr };

    LRESULT CALLBACK GetMsgProcSearchDefer(int code, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (code == HC_ACTION && wParam == PM_REMOVE)
        {
            auto* msg = reinterpret_cast<MSG*>(lParam);
            if (msg && msg->message == ShellSurfaceDeferMsg())
            {
                const HWND hwnd = reinterpret_cast<HWND>(msg->lParam);
                msg->message = WM_NULL; // не отдавать приватное сообщение приложению
                if (const HHOOK hook = g_searchDeferHook.exchange(nullptr, std::memory_order_acq_rel))
                    UnhookWindowsHookEx(hook);
                InitShellSurfaceIxdeOnUiThread(hwnd);
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Вызывается уже на UI-потоке CoreWindow: для Search синхронный init слишком ранний.
    void PostSearchDeferInit(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return;
        if (g_searchDeferPosted.exchange(true, std::memory_order_acq_rel))
        {
            // CREATE (cloaked) уже поставил defer/IXDE; SHOW/повторный notify
            // обязан CatchUp+Reapply — иначе AppBorder приходит только с VTC
            // спустя секунды после первого показа (регрессия ~3 с).
            InitShellSurfaceIxdeOnUiThread(hwnd);
            return;
        }

        HMODULE hMod{};
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&PostSearchDeferInit),
                &hMod))
        {
            hMod = GetModuleHandleW(nullptr);
        }

        const HHOOK hook = SetWindowsHookExW(
            WH_GETMESSAGE, GetMsgProcSearchDefer, hMod, GetCurrentThreadId());
        if (!hook)
        {
            g_searchDeferPosted.store(false, std::memory_order_release);
            DebugLog(DBG_WARNING, L"[UICompat][XamlDiag] Search defer: SetWindowsHookEx failed");
            return;
        }
        g_searchDeferHook.store(hook, std::memory_order_release);

        if (PostMessageW(hwnd, ShellSurfaceDeferMsg(), 0, reinterpret_cast<LPARAM>(hwnd)) == 0)
        {
            if (const HHOOK h = g_searchDeferHook.exchange(nullptr, std::memory_order_acq_rel))
                UnhookWindowsHookEx(h);
            g_searchDeferPosted.store(false, std::memory_order_release);
            DebugLog(DBG_WARNING, L"[UICompat][XamlDiag] Search defer: PostMessage failed");
        }
    }

    void UnhookSearchDeferIfAny() noexcept
    {
        if (const HHOOK h = g_searchDeferHook.exchange(nullptr, std::memory_order_acq_rel))
            UnhookWindowsHookEx(h);
    }

    [[nodiscard]] HRESULT RunShellSurfaceIxdeBurst(const InitializeXamlDiagnosticsExFn ixde, DWORD pid) noexcept
    {
        // Сначала VisualDiagConnection1 (как NC); при ERROR_NOT_FOUND — перебор.
        HRESULT hr = ixde(
            kEndpointName,
            pid,
            nullptr,
            g_tapDllFullPath,
            CLSID_GlassUICompatTapSite,
            nullptr);
        if (SUCCEEDED(hr))
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][XamlDiag] ShellSurface IXDE OK endpoint=%s",
                kEndpointName);
            return hr;
        }
        if (static_cast<HRESULT>(hr) != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][XamlDiag] ShellSurface IXDE hr=0x%08X (не NOT_FOUND — без перебора)",
                static_cast<unsigned>(hr));
            return hr;
        }

        for (int i = 2; i <= kShellSurfaceEndpointMax && !ShouldAbortInstall(); ++i)
        {
            wchar_t name[64]{};
            swprintf_s(name, L"VisualDiagConnection%d", i);
            hr = ixde(
                name,
                pid,
                nullptr,
                g_tapDllFullPath,
                CLSID_GlassUICompatTapSite,
                nullptr);
            if (SUCCEEDED(hr))
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[UICompat][XamlDiag] ShellSurface IXDE OK endpoint=%s",
                    name);
                return hr;
            }
            if (static_cast<HRESULT>(hr) != HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            {
                DebugLog(
                    DBG_WARNING,
                    L"[UICompat][XamlDiag] ShellSurface IXDE %s hr=0x%08X",
                    name,
                    static_cast<unsigned>(hr));
                return hr;
            }
        }
        return hr;
    }

    // ShellHost QS (лог H12/H14): ControlCenter SHOW при cx=cy=0; sibling CoreWindow 1×1
    // раньше крал CAS IXDE → Advise на пустом дереве. IXDE только с sized ControlCenterWindow.
    [[nodiscard]] bool ShouldDeferShellHostIxde(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;
        if (!Shared::RootBackdropPolicy::IsShellHostProcess())
            return false;

        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) <= 0)
            return true;

        // CoreWindow в ShellHost — не источник IXDE (только reapply после CC).
        if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0)
            return true;

        if (_wcsicmp(cls, L"ControlCenterWindow") != 0)
            return true;

        RECT rc{};
        if (!GetClientRect(hwnd, &rc))
            return true;
        const int cx = rc.right - rc.left;
        const int cy = rc.bottom - rc.top;
        return cx <= 0 || cy <= 0;
    }

    // SEH: IXDE только с высокой панели NC (~360×1032).
    // 1×1 (лог H12 pid=38092) и toast ~396×171 — пустое/чужое дерево → нет
    // NotificationCenterGrid. Retry через LOCATIONCHANGE/SHOW когда cy≥600.
    [[nodiscard]] bool ShouldDeferSehToastSizedIxde(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd))
            return false;
        if (!Shared::RootBackdropPolicy::IsShellExperienceHostProcess())
            return false;

        wchar_t cls[64]{};
        if (GetClassNameW(hwnd, cls, 64) <= 0)
            return false;
        if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0)
            return false;

        RECT rc{};
        if (!GetClientRect(hwnd, &rc))
            return false;
        const int cy = rc.bottom - rc.top;
        return cy < 600;
    }

    // Sync CatchUp+Reapply только для NC-высокой панели (не toast / не 1×1 sibling).
    [[nodiscard]] bool ShouldSehSyncReapplyOnHwnd(HWND hwnd) noexcept
    {
        if (!Shared::RootBackdropPolicy::IsShellExperienceHostProcess())
            return true;
        if (!hwnd || !IsWindow(hwnd))
            return false;
        RECT rc{};
        if (!GetClientRect(hwnd, &rc))
            return false;
        const int cy = rc.bottom - rc.top;
        return cy >= 600;
    }

    [[nodiscard]] HWND FindTallSehCoreWindowHwnd() noexcept
    {
        if (!Shared::RootBackdropPolicy::IsShellExperienceHostProcess())
            return nullptr;

        struct EnumCtx
        {
            DWORD pid;
            HWND best;
            int bestCy;
        } ctx{ GetCurrentProcessId(), nullptr, 0 };

        EnumWindows(
            [](HWND hwnd, LPARAM lp) noexcept -> BOOL {
                auto* c = reinterpret_cast<EnumCtx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != c->pid)
                    return TRUE;
                wchar_t cls[64]{};
                if (GetClassNameW(hwnd, cls, 64) <= 0)
                    return TRUE;
                if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0)
                    return TRUE;
                RECT rc{};
                if (!GetClientRect(hwnd, &rc))
                    return TRUE;
                const int cy = rc.bottom - rc.top;
                if (cy < 600)
                    return TRUE;
                if (cy > c->bestCy)
                {
                    c->bestCy = cy;
                    c->best = hwnd;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    }

    void ScheduleSehTallIxdeRetry() noexcept
    {
        if (!Shared::RootBackdropPolicy::IsShellExperienceHostProcess())
            return;
        if (g_ixdeSucceeded.load(std::memory_order_acquire))
            return;
        bool expected = false;
        if (!g_sehTallIxdeRetryArmed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;

        HANDLE th = CreateThread(
            nullptr,
            0,
            [](LPVOID) -> DWORD {
                constexpr DWORD kDelaysMs[] = { 100u, 400u, 1200u, 3000u };
                for (const DWORD delayMs : kDelaysMs)
                {
                    Sleep(delayMs);
                    if (ShouldAbortInstall()
                        || g_ixdeSucceeded.load(std::memory_order_acquire))
                        break;
                    const HWND tall = FindTallSehCoreWindowHwnd();
                    if (!tall)
                        continue;
                    (void)RunFromWindowThread(
                        tall,
                        [](PVOID p) noexcept {
                            InitShellSurfaceIxdeOnUiThread(static_cast<HWND>(p));
                        },
                        tall);
                    if (g_ixdeSucceeded.load(std::memory_order_acquire))
                        break;
                }
                return 0;
            },
            nullptr,
            0,
            nullptr);
        if (th)
            CloseHandle(th);
        else
            g_sehTallIxdeRetryArmed.store(false, std::memory_order_release);
    }

    [[nodiscard]] HWND FindSizedControlCenterWindowHwnd() noexcept
    {
        if (!Shared::RootBackdropPolicy::IsShellHostProcess())
            return nullptr;

        struct EnumCtx
        {
            DWORD pid;
            HWND best;
            int bestArea;
        } ctx{ GetCurrentProcessId(), nullptr, 0 };

        EnumWindows(
            [](HWND hwnd, LPARAM lp) noexcept -> BOOL {
                auto* c = reinterpret_cast<EnumCtx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != c->pid)
                    return TRUE;
                wchar_t cls[64]{};
                if (GetClassNameW(hwnd, cls, 64) <= 0)
                    return TRUE;
                if (_wcsicmp(cls, L"ControlCenterWindow") != 0)
                    return TRUE;
                RECT rc{};
                if (!GetClientRect(hwnd, &rc))
                    return TRUE;
                const int cx = rc.right - rc.left;
                const int cy = rc.bottom - rc.top;
                if (cx <= 1 || cy <= 1)
                    return TRUE;
                const int area = cx * cy;
                if (area > c->bestArea)
                {
                    c->bestArea = area;
                    c->best = hwnd;
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        return ctx.best;
    }

    void ScheduleShellHostSizedIxdeRetry() noexcept
    {
        if (!Shared::RootBackdropPolicy::IsShellHostProcess())
            return;
        if (g_ixdeSucceeded.load(std::memory_order_acquire))
            return;
        bool expected = false;
        if (!g_shellHostSizedIxdeRetryArmed.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;

        HANDLE th = CreateThread(
            nullptr,
            0,
            [](LPVOID) -> DWORD {
                constexpr DWORD kDelaysMs[] = { 100u, 400u, 1200u, 3000u, 8000u };
                for (const DWORD delayMs : kDelaysMs)
                {
                    Sleep(delayMs);
                    if (ShouldAbortInstall()
                        || g_ixdeSucceeded.load(std::memory_order_acquire))
                        break;
                    const HWND sized = FindSizedControlCenterWindowHwnd();
                    if (!sized)
                        continue;
                    (void)RunFromWindowThread(
                        sized,
                        [](PVOID p) noexcept {
                            InitShellSurfaceIxdeOnUiThread(static_cast<HWND>(p));
                        },
                        sized);
                    if (g_ixdeSucceeded.load(std::memory_order_acquire))
                        break;
                }
                return 0;
            },
            nullptr,
            0,
            nullptr);
        if (th)
            CloseHandle(th);
        else
            g_shellHostSizedIxdeRetryArmed.store(false, std::memory_order_release);
    }

    void ShellSurfaceCatchUpOnUiThread(PVOID param) noexcept
    {
        if (ShouldAbortInstall())
            return;
        const HWND hwnd = static_cast<HWND>(param);
        // Toast/1×1 sibling после IXDE: не adopt + не reapply (лог: fromTid toast→NC).
        if (!ShouldSehSyncReapplyOnHwnd(hwnd))
        {
            return;
        }
        const DWORD tid = GetCurrentThreadId();
        (void)UICompat::TryAdoptShellSurfaceThreadIdForNcVtc(tid);
        // Sync на UI hwnd: CatchUp + Reapply (не Post на marshal toast tid).
        UICompat::UiTapSite::EnsureMarshalWindowOnUiThread();
        UICompat::UiTapSite::CatchUpNcOnCurrentThread();
        UICompat::UiTapSite::ReapplyOnCurrentThread();
    }

    void InitShellSurfaceIxdeOnUiThread(HWND hwnd) noexcept
    {
        if (ShouldAbortInstall() || !g_tapPathReady.load(std::memory_order_acquire))
            return;

        if (g_ixdeSucceeded.load(std::memory_order_acquire))
        {
            // CREATE/SHOW при живом IXDE — donor: CatchUp + sync reapply на этом UI.
            ShellSurfaceCatchUpOnUiThread(hwnd);
            return;
        }

        BOOL cloakedAttr = FALSE;
        const HRESULT hrCloakAttr = DwmGetWindowAttribute(
            hwnd, DWMWA_CLOAKED, &cloakedAttr, sizeof(cloakedAttr));
        const unsigned cloakedU =
            (SUCCEEDED(hrCloakAttr) && cloakedAttr != FALSE) ? 1u : 0u;
        const unsigned visibleU = IsWindowVisible(hwnd) ? 1u : 0u;
        RECT rcClient{};
        (void)GetClientRect(hwnd, &rcClient);
        const int cx = rcClient.right - rcClient.left;
        const int cy = rcClient.bottom - rcClient.top;

        if (ShouldDeferShellHostIxde(hwnd) || ShouldDeferSehToastSizedIxde(hwnd))
        {
            wchar_t deferCls[64]{};
            GetClassNameW(hwnd, deferCls, 64);
            const bool sehToast = ShouldDeferSehToastSizedIxde(hwnd);
            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][XamlDiag] Defer %s IXDE hwnd=0x%p cls=%s "
                L"cloaked=%u visible=%u cx=%d cy=%d",
                sehToast ? L"SEH-toast" : L"ShellHost",
                hwnd,
                deferCls,
                cloakedU,
                visibleU,
                cx,
                cy);
            if (sehToast)
            {
                // Высокая NC уже есть (cloaked) — IXDE на её UI-потоке, не на toast.
                if (const HWND tall = FindTallSehCoreWindowHwnd())
                {
                    if (tall != hwnd)
                    {
                        (void)RunFromWindowThread(
                            tall,
                            [](PVOID p) noexcept {
                                InitShellSurfaceIxdeOnUiThread(static_cast<HWND>(p));
                            },
                            tall);
                        if (g_ixdeSucceeded.load(std::memory_order_acquire))
                            return;
                    }
                }
                ScheduleSehTallIxdeRetry();
            }
            else if (Shared::RootBackdropPolicy::IsShellHostProcess())
            {
                // Лог pid=8272: defer cx/cy=0 → IXDE только при открытии QS (~+15с).
                ScheduleShellHostSizedIxdeRetry();
            }
            // Не CAS g_shellSurfaceThreadId — иначе блокируем поздний Init на UI-потоке.
            return;
        }

        DWORD expected = 0;
        const DWORD tid = GetCurrentThreadId();
        if (!g_shellSurfaceThreadId.compare_exchange_strong(
                expected, tid, std::memory_order_acq_rel))
        {
            // Уже занят другим потоком — только reapply если IXDE готов.
            if (g_ixdeSucceeded.load(std::memory_order_acquire))
                UICompat::UiTapSite::PostReapplyIfActive();
            return;
        }

        g_shellSurfaceIxdePath.store(true, std::memory_order_release);
        g_ixdeSurfaceInterestOrKick.fetch_add(1, std::memory_order_acq_rel);

        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][XamlDiag] ShellSurface IXDE на UI tid=%u hwnd=0x%p",
            static_cast<unsigned>(tid),
            hwnd);

        HMODULE wuxHandle = LoadLibraryExW(
            L"Windows.UI.Xaml.dll",
            nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!wuxHandle)
        {
            DebugLog(
                DBG_ERROR,
                L"[UICompat][XamlDiag] ShellSurface: Windows.UI.Xaml.dll GLE=%u",
                static_cast<unsigned>(GetLastError()));
            g_shellSurfaceThreadId.store(0, std::memory_order_release);
            return;
        }

        const auto ixde = reinterpret_cast<InitializeXamlDiagnosticsExFn>(
            GetProcAddress(wuxHandle, "InitializeXamlDiagnosticsEx"));
        if (!ixde)
        {
            DebugLog(DBG_ERROR, L"[UICompat][XamlDiag] ShellSurface: GetProcAddress(IXDE) failed");
            FreeLibrary(wuxHandle);
            g_shellSurfaceThreadId.store(0, std::memory_order_release);
            return;
        }

        const HRESULT hr = RunShellSurfaceIxdeBurst(ixde, GetCurrentProcessId());
        FreeLibrary(wuxHandle);

        if (SUCCEEDED(hr))
        {
            g_ixdeSucceeded.store(true, std::memory_order_release);
            g_diagRunning.store(true, std::memory_order_release);
            // STA IXDE-потока нет — g_ixdeStaMayExit не блокируем.
            g_ixdeStaMayExit.store(true, std::memory_order_release);
            NotifyWndCompatIxdeOutcome(TRUE);
            // Sync на UI IXDE (обычно NC): не Post на поздний toast marshal.
            UICompat::UiTapSite::EnsureMarshalWindowOnUiThread();
            UICompat::UiTapSite::CatchUpNcOnCurrentThread();
            UICompat::UiTapSite::ReapplyOnCurrentThread();
            DebugLog(DBG_IMPORTANT, L"[UICompat][XamlDiag] ShellSurface IXDE успешно");
        }
        else
        {
            NotifyWndCompatIxdeOutcome(FALSE);
            g_shellSurfaceThreadId.store(0, std::memory_order_release);
            g_shellSurfaceIxdePath.store(false, std::memory_order_release);
            DebugLog(
                DBG_ERROR,
                L"[UICompat][XamlDiag] ShellSurface IXDE FAILED hr=0x%08X",
                static_cast<unsigned>(hr));
        }
    }

    void ShellSurfaceNotifyOnUiThread(PVOID param) noexcept
    {
        const HWND hwnd = static_cast<HWND>(param);
        if (Shared::RootBackdropPolicy::IsSearchHostProcess())
        {
            // Windhawk: синхронно на CREATE — слишком рано для Search.
            PostSearchDeferInit(hwnd);
            return;
        }
        InitShellSurfaceIxdeOnUiThread(hwnd);
    }

    HRESULT RunIxdeDonorStyleBurst(const InitializeXamlDiagnosticsExFn ixde, DWORD pid) noexcept
    {
        HRESULT hr = E_FAIL;

        // Первый вызов немедленно; между неудачами — Sleep(500) как TranslucentTB.
        int attempt = 0;
        while (attempt < kIxdeDonorAttempts && !ShouldAbortInstall())
        {
            if (g_ixdeSucceeded.load(std::memory_order_acquire))
                return S_OK;

            // InitializeXamlDiagnosticsEx на том же STA, что и IxdeInstallThreadProc (без временного thread).
            hr = ixde(
                kEndpointName,
                pid,
                nullptr,
                g_tapDllFullPath,
                CLSID_GlassUICompatTapSite,
                nullptr);

            if (SUCCEEDED(hr))
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[UICompat][XamlDiag] InitializeXamlDiagnosticsEx OK endpoint=%s attempt=%d",
                    kEndpointName,
                    attempt + 1);
                return hr;
            }

            DebugLog(
                DBG_NORMAL,
                L"[UICompat][XamlDiag] InitializeXamlDiagnosticsEx попытка %d hr=0x%08X kick=%d",
                attempt + 1,
                static_cast<unsigned>(hr),
                g_ixdeKickRequested.load(std::memory_order_acquire) ? 1 : 0);

            if (static_cast<HRESULT>(hr) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            {
                DebugLog(
                    DBG_WARNING,
                    L"[UICompat][XamlDiag] IXDE runtime gate 0x80070490 — дальнейшие попытки бессмысленны");
                return hr;
            }

            ++attempt;
            if (attempt >= kIxdeDonorAttempts)
                break;

            if (ShouldAbortInstall())
                break;

            // Новый триггер (вторая панель задач и т.п.) — не ждём полный интервал между попытками.
            if (g_ixdeKickRequested.exchange(false, std::memory_order_acq_rel))
                continue;

            InterruptibleSleepMs(kIxdeDonorSleepMs);

            // После пробуждения возможен повторный kick — снимаем, чтобы следующий Sleep не блокировал.
            (void)g_ixdeKickRequested.exchange(false, std::memory_order_acq_rel);
        }

        return hr;
    }

    DWORD WINAPI IxdeInstallThreadProc(LPVOID) noexcept
    {
        g_installThreadActive.store(true, std::memory_order_release);

        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool coDidInit = (hrCo == S_OK);
        if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
        {
            DebugLog(
                DBG_ERROR,
                L"[UICompat][XamlDiag] CoInitializeEx hr=0x%08X",
                static_cast<unsigned>(hrCo));
            g_installThreadPosted.store(false, std::memory_order_release);
            g_installThreadActive.store(false, std::memory_order_release);
            return 1;
        }

        HRESULT lastHrAllBursts = E_FAIL;

        // Один установленный поток может выполнять несколько «бёрстов» при повторном kick после ошибки —
        // без второго параллельного CreateThread.
        while (!ShouldAbortInstall())
        {
            if (g_ixdeSucceeded.load(std::memory_order_acquire))
            {
                lastHrAllBursts = S_OK;
                break;
            }

            if (g_ixdeSurfaceInterestOrKick.load(std::memory_order_acquire) <= 0)
            {
                DebugLog(DBG_NORMAL, L"[UICompat][XamlDiag] IXDE-поток: интерес под треем = 0 — выход без IXDE");
                break;
            }

            if (!g_tapPathReady.load(std::memory_order_acquire))
            {
                DebugLog(DBG_ERROR, L"[UICompat][XamlDiag] TAP не готов во время IXDE");
                break;
            }

            bool    ownLoadUxaml = false;
            HMODULE wuxHandle =
                reinterpret_cast<HMODULE>(g_windowsUIXamlDllFromHandler.load(std::memory_order_acquire));
            if (!wuxHandle || reinterpret_cast<ULONG_PTR>(wuxHandle) < 0x10000)
            {
                wuxHandle = LoadLibraryExW(
                    L"Windows.UI.Xaml.dll",
                    nullptr,
                    LOAD_LIBRARY_SEARCH_SYSTEM32);
                ownLoadUxaml = true;
            }

            if (!wuxHandle)
            {
                const DWORD gle = GetLastError();
                DebugLog(
                    DBG_ERROR,
                    L"[UICompat][XamlDiag] Windows.UI.Xaml.dll недоступна GLE=%u",
                    static_cast<unsigned>(gle));
                lastHrAllBursts = HRESULT_FROM_WIN32(gle);

                InterruptibleSleepMs(kKickWaitAfterFailMs);

                // Ожидание второго триггера — либо выход если интерес упал ниже минимальной и нет нового kick.
                if (g_ixdeSurfaceInterestOrKick.load(std::memory_order_acquire) <= 0 &&
                    !g_ixdeKickRequested.load(std::memory_order_acquire))
                {
                    break;
                }

                continue;
            }

            const auto ixde = reinterpret_cast<InitializeXamlDiagnosticsExFn>(
                GetProcAddress(wuxHandle, "InitializeXamlDiagnosticsEx"));
            if (!ixde)
            {
                const DWORD gle = GetLastError();
                DebugLog(
                    DBG_ERROR,
                    L"[UICompat][XamlDiag] GetProcAddress(InitializeXamlDiagnosticsEx) GLE=%u",
                    static_cast<unsigned>(gle));
                if (ownLoadUxaml && wuxHandle)
                    FreeLibrary(wuxHandle);

                lastHrAllBursts = HRESULT_FROM_WIN32(gle);

                InterruptibleSleepMs(kKickWaitAfterFailMs);

                continue;
            }

            const DWORD pid             = GetCurrentProcessId();
            const HRESULT burstHr       = RunIxdeDonorStyleBurst(ixde, pid);

            if (ownLoadUxaml && wuxHandle)
            {
                FreeLibrary(wuxHandle);
                wuxHandle = nullptr;
            }

            if (SUCCEEDED(burstHr))
            {
                lastHrAllBursts = burstHr;
                g_ixdeSucceeded.store(true, std::memory_order_release);
                g_diagRunning.store(true, std::memory_order_release);
                NotifyWndCompatIxdeOutcome(TRUE);

                DebugLog(DBG_IMPORTANT, L"[UICompat][XamlDiag] IXDE успешно завершён (после бёрста)");
                // Первый QS мог пройти bump_only до TAP: reapply после готовности IXDE.
                UICompat::UiTapSite::PostReapplyIfActive();
                break;
            }

            NotifyWndCompatIxdeOutcome(FALSE);
            lastHrAllBursts = burstHr;

            // Дать WinUi возможность переоткрыть NOTIFY после очистки мостов.
            InterruptibleSleepMs(kKickWaitAfterFailMs);

            if (!g_ixdeKickRequested.exchange(false, std::memory_order_acq_rel) &&
                g_ixdeSurfaceInterestOrKick.load(std::memory_order_acquire) <= 0)
            {
                break;
            }
        }

        if (!SUCCEEDED(lastHrAllBursts))
        {
            DebugLog(
                DBG_ERROR,
                L"[UICompat][XamlDiag] IXDE итог: FAILED последний hr=0x%08X",
                static_cast<unsigned>(lastHrAllBursts));
        }

        const bool ixdeSucceeded = g_ixdeSucceeded.load(std::memory_order_acquire);

        // После успешного IXDE STA этого потока должна жить до Stop: иначе CoUninitialize до SetSite TAP
        // даёт гонку с AdviseVisualTreeChange (E_UNEXPECTED).
        if (SUCCEEDED(lastHrAllBursts) && ixdeSucceeded)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][XamlDiag] IXDE STA-поток: ожидание Shutdown TAP и разрешения CoUninitialize");
            while (!g_ixdeStaMayExit.load(std::memory_order_acquire))
                InterruptibleSleepMs(50);

            CoFreeUnusedLibraries();
            for (int i = 0; i < 3; ++i)
            {
                Sleep(10);
                CoFreeUnusedLibraries();
            }
        }

        if (coDidInit)
            CoUninitialize();

        g_installThreadPosted.store(false, std::memory_order_release);

        // Если уже пришёл новый kick/интерес — поднимем следующий установленный раунд.
        if (!ShouldAbortInstall() && !ixdeSucceeded &&
            (g_ixdeKickRequested.exchange(false, std::memory_order_acq_rel) ||
             g_ixdeSurfaceInterestOrKick.load(std::memory_order_acquire) > 0))
        {
            ScheduleIxdeThreadIfPossible();
        }

        g_installThreadActive.store(false, std::memory_order_release);

        return SUCCEEDED(lastHrAllBursts) ? 0 : 1;
    }
}

extern "C" {

HRESULT UICompat_PrepareXamlDiagnostics() noexcept
{
    g_xamlStopRequested.store(false, std::memory_order_release);
    g_ixdeSucceeded.store(false, std::memory_order_release);
    g_installThreadPosted.store(false, std::memory_order_release);
    g_diagRunning.store(false, std::memory_order_release);
    g_ixdeKickRequested.store(false, std::memory_order_release);
    g_ixdeStaMayExit.store(false, std::memory_order_release);
    g_shellSurfaceIxdePath.store(false, std::memory_order_release);
    g_shellSurfaceThreadId.store(0, std::memory_order_release);
    g_searchDeferPosted.store(false, std::memory_order_release);

    // TAP / IXDE / VTC — только Windows 11; backdrop трея (TaskbarHost) остаётся на Win10 через WndCompat.
    if (Shared::SystemHelper::g_buildNumber < 22000)
    {
        g_tapPathReady.store(false, std::memory_order_release);
        g_ixdeSurfaceInterestOrKick.store(0, std::memory_order_release);
        DebugLog(DBG_IMPORTANT, L"[UICompat][XamlDiag] Win10: TAP/IXDE отключены (build < 22000)");
        return S_OK;
    }

    if (!GetTapDllFullPath(g_tapDllFullPath, std::size(g_tapDllFullPath)))
    {
        DebugLog(DBG_ERROR, L"[UICompat][XamlDiag] GetTapDllFullPath failed");
        g_tapPathReady.store(false, std::memory_order_release);
        return E_FAIL;
    }

    DebugLog(DBG_IMPORTANT, L"[UICompat][XamlDiag] TAP DLL path: %s", g_tapDllFullPath);

    // Новый сеанс: сначала TAP готов для Notify из конкурирующего потока explorer до RegisterGlassCore.
    g_ixdeSurfaceInterestOrKick.store(0, std::memory_order_release);
    g_tapPathReady.store(true, std::memory_order_release);

    RegisterGlassCoreXamlDllNotifyFromUICompat();

    DebugLog(
        DBG_IMPORTANT,
        L"[UICompat][XamlDiag] Prepared: IXDE после интереса CoreWindow + ModuleHandler WUX");
    return S_OK;
}

__declspec(dllexport) BOOL WINAPI UICompat_NotifyShellSurfaceWindow(HWND hwnd) noexcept
{
    if (!g_tapPathReady.load(std::memory_order_acquire))
    {
        DebugLog(DBG_WARNING, L"[UICompat][XamlDiag] NotifyShellSurface: TAP path not ready");
        return FALSE;
    }
    if (ShouldAbortInstall())
        return FALSE;
    if (!hwnd || !IsWindow(hwnd))
        return FALSE;

    if (g_ixdeSucceeded.load(std::memory_order_acquire))
    {
        // Donor-like: CatchUp+Reapply sync на UI-потоке hwnd (без Post на чужой marshal).
        const bool catchupOk = RunFromWindowThread(
            hwnd, ShellSurfaceCatchUpOnUiThread, hwnd);
        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][XamlDiag] NotifyShellSurface: hwnd=0x%p (reapply_sync=%u)",
            hwnd,
            catchupOk ? 1u : 0u);
        return TRUE;
    }

    const bool ok = RunFromWindowThread(hwnd, ShellSurfaceNotifyOnUiThread, hwnd);
    DebugLog(
        DBG_IMPORTANT,
        L"[UICompat][XamlDiag] NotifyShellSurface: hwnd=0x%p marshal=%u",
        hwnd,
        ok ? 1u : 0u);
    return ok ? TRUE : FALSE;
}

__declspec(dllexport) BOOL WINAPI UICompat_NotifyCoreWindow(HWND hwnd) noexcept
{
    // Start/Search/NC(SEH|ShellHost): IXDE на UI-потоке окна (Window::Current валиден
    // для CatchUp). STA IxdeInstallThreadProc оставляем только как legacy fallback
    // для неизвестных хостов — у ShellNotificationHost он ломал первый SHOW QS
    // (catch-up на чужом потоке → registered=0, патч только со 2-го раза).
    if (Shared::RootBackdropPolicy::IsStartMenuHostProcess()
        || Shared::RootBackdropPolicy::IsSearchHostProcess()
        || Shared::RootBackdropPolicy::IsShellNotificationHostProcess())
    {
        return UICompat_NotifyShellSurfaceWindow(hwnd);
    }

    if (!g_tapPathReady.load(std::memory_order_acquire))
    {
        DebugLog(DBG_WARNING, L"[UICompat][XamlDiag] NotifyCoreWindow: TAP path not ready — пропуск");
        return FALSE;
    }

    if (ShouldAbortInstall())
        return FALSE;

    if (g_ixdeSucceeded.load(std::memory_order_acquire))
    {
        // IXDE уже есть — reapply на SHOW (компоузитор/панели могли появиться позже).
        UICompat::UiTapSite::PostReapplyIfActive();
        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][XamlDiag] NotifyCoreWindow: hwnd=0x%p (reapply)",
            hwnd);
        return TRUE;
    }

    BumpSurfaceInterest(hwnd, L"CoreWindow");

    DebugLog(
        DBG_IMPORTANT,
        L"[UICompat][XamlDiag] NotifyCoreWindow: hwnd=0x%p",
        hwnd);

    return TRUE;
}

__declspec(dllexport) void WINAPI UICompat_ReleaseIxdeSurfaceInterest() noexcept
{
    const int prev = g_ixdeSurfaceInterestOrKick.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 0)
        g_ixdeSurfaceInterestOrKick.fetch_add(1, std::memory_order_relaxed);
}

__declspec(dllexport) void WINAPI UICompat_ResetIxdeSchedulingAfterFailure() noexcept
{
    g_ixdeKickRequested.store(false, std::memory_order_release);
    g_ixdeSurfaceInterestOrKick.store(0, std::memory_order_release);
}

void UICompat_StopXamlDiagnostics() noexcept
{
    UnregisterGlassCoreXamlDllNotifyFromUICompat();
    // Хук WH_GETMESSAGE Search мог остаться при закрытии окна до init — снять до FreeLibrary.
    UnhookSearchDeferIfAny();

    // DllCanUnloadNow=S_OK до CoFree на STA IXDE/marshal — GlassCompat ещё держит LoadLibrary.
    UICompatCom::g_shutdownDone.store(true, std::memory_order_release);

    const bool diagWasRunning = g_diagRunning.load(std::memory_order_acquire);
    if (diagWasRunning)
    {
        DebugLog(DBG_IMPORTANT, L"[UICompat][XamlDiag] Stop: BEGIN");
    }

    // Для ShellExperienceHost на Stop `diagWasRunning` может быть false,
    // хотя активный watcher ещё существует.
    // Поэтому ShutdownActiveWatcher должен вызываться независимо от diagWasRunning:
    // если нечего останавливать — он вернётся быстро сам.
    UICompat::UiTapSite::ShutdownActiveWatcher();

    if (diagWasRunning)
        g_diagRunning.store(false, std::memory_order_release);

    // После Shutdown TAP — отпускаем STA IXDE-потока (иначе CoUninitialize ломает Advise/Unadvise).
    g_ixdeStaMayExit.store(true, std::memory_order_release);
    g_xamlStopRequested.store(true, std::memory_order_release);

    g_ixdeSucceeded.store(false, std::memory_order_release);
    g_installThreadPosted.store(false, std::memory_order_release);
    g_tapPathReady.store(false, std::memory_order_release);
    g_ixdeKickRequested.store(false, std::memory_order_release);
    g_ixdeSurfaceInterestOrKick.store(0, std::memory_order_release);
    g_shellSurfaceIxdePath.store(false, std::memory_order_release);
    g_shellSurfaceThreadId.store(0, std::memory_order_release);
    g_searchDeferPosted.store(false, std::memory_order_release);

    if (!diagWasRunning)
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][XamlDiag] TAP не был в diagRunning — без ActiveWatcher или IXDE не поднят");
    }

    constexpr int kThreadDrainMs = 8000;
    for (int w = 0; w < kThreadDrainMs; w += 50)
    {
        if (g_installThreadActive.load(std::memory_order_acquire))
        {
            Sleep(50);
            continue;
        }
        break;
    }
    if (g_installThreadActive.load(std::memory_order_acquire))
    {
        DebugLog(DBG_WARNING, L"[UICompat][XamlDiag] Stop: поток IXDE-install ещё активен");
    }

    DebugLog(DBG_IMPORTANT, L"[UICompat][XamlDiag] Stop: END");
}

}

namespace UICompat
{
    DWORD GetShellSurfaceThreadId() noexcept
    {
        return g_shellSurfaceThreadId.load(std::memory_order_acquire);
    }

    bool TryAdoptShellSurfaceThreadIdForNcVtc(DWORD tid) noexcept
    {
        if (!tid)
            return false;
        if (!g_ixdeSucceeded.load(std::memory_order_acquire))
            return false;
        if (!Shared::RootBackdropPolicy::IsShellNotificationHostProcess())
            return false;

        DWORD cur = g_shellSurfaceThreadId.load(std::memory_order_acquire);
        if (cur == tid)
            return true;
        if (cur == 0)
        {
            return g_shellSurfaceThreadId.compare_exchange_strong(
                cur, tid, std::memory_order_acq_rel);
        }

        // Сменить tid первого CoreWindow (IXDE) на tid потока с NotificationCenterGrid.
        if (g_shellSurfaceThreadId.compare_exchange_strong(
                cur, tid, std::memory_order_acq_rel))
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][XamlDiag] Adopt shellSurfaceTid %u → %u (NC VTC)",
                static_cast<unsigned>(cur),
                static_cast<unsigned>(tid));
            return true;
        }
        return g_shellSurfaceThreadId.load(std::memory_order_acquire) == tid;
    }

    bool RunFromWindowThread(HWND hwnd, void (*fn)(void*) noexcept, void* param) noexcept
    {
        return RunFromWindowThreadImpl(
            hwnd, reinterpret_cast<void (*)(PVOID) noexcept>(fn), param);
    }
}
