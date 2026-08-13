#include "TaskbarPch.h"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/XamlGlassIds.hpp"
#include "xaml/TaskbarTapSite.hpp"
#include "xaml/XamlComLifetime.hpp"

#include <DebugLog.h>
#include <GlassMessages.h>
#include <objbase.h>
#include <SystemHelper.h>

#include <atomic>
#include <cstdint>

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

    std::atomic<bool>               g_diagRunning{ false };
    std::atomic<bool>               g_xamlStopRequested{ false };
    std::atomic<bool>               g_tapPathReady{ false };
    std::atomic<bool>               g_ixdeSucceeded{ false };
    std::atomic<bool>               g_installThreadPosted{ false };
    std::atomic<bool>               g_installThreadActive{ false };
    std::atomic<bool>               g_ixdeKickRequested{ false };
    // После успешного IXDE поток ждёт true перед CoUninitialize — выставляется в Stop после ShutdownActiveWatcher.
    std::atomic<bool>               g_ixdeStaMayExit{ true };

    std::atomic<int32_t>            g_ixdeSurfaceInterestOrKick{ 0 };

    std::atomic<std::uintptr_t>     g_windowsUIXamlDllFromHandler{ 0 }; // только наблюдаемое Ldr-событие, без FreeLibrary

    GlassCore_SetXamlHostDllNotifyFn     s_pGlassCore_SetXamlHostDllNotify     = nullptr;
    GlassCore_NotifyExistingXamlHostDllsFn s_pGlassCore_NotifyExistingXamlHostDlls = nullptr;
    bool                                 s_registeredGlassNotify                 = false;

    wchar_t g_tapDllFullPath[MAX_PATH]{};

    DWORD WINAPI IxdeInstallThreadProc(LPVOID) noexcept;
    void ScheduleIxdeThreadIfPossible() noexcept;

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
            L"[TaskbarCompat][XamlDiag] GlassCore/XamlHost: Windows.UI.Xaml.dll @ 0x%p (ModuleHandler)",
            loadedModule);
        ScheduleIxdeThreadIfPossible();
    }

    void RegisterGlassCoreXamlDllNotifyFromTaskbarCompat() noexcept
    {
        if (s_registeredGlassNotify)
            return;

        const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][XamlDiag] GlassCore.dll не загружен — без ModuleHandler UWP-хоста");
            return;
        }

        s_pGlassCore_SetXamlHostDllNotify = reinterpret_cast<GlassCore_SetXamlHostDllNotifyFn>(
            GetProcAddress(hCore, "GlassCore_SetXamlHostDllNotify"));
        s_pGlassCore_NotifyExistingXamlHostDlls = reinterpret_cast<GlassCore_NotifyExistingXamlHostDllsFn>(
            GetProcAddress(hCore, "GlassCore_NotifyExistingXamlHostDlls"));

        if (!s_pGlassCore_SetXamlHostDllNotify)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][XamlDiag] GetProcAddress(SetXamlHostDllNotify) failed");
            return;
        }

        s_pGlassCore_SetXamlHostDllNotify(&OnGlassCoreXamlDllNotify);
        s_registeredGlassNotify = true;

        if (s_pGlassCore_NotifyExistingXamlHostDlls)
            s_pGlassCore_NotifyExistingXamlHostDlls();

        DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][XamlDiag] Зарегистрирован ModuleHandler‑колбэк GlassCore для UWP‑XAML");
    }

    void UnregisterGlassCoreXamlDllNotifyFromTaskbarCompat() noexcept
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
            DebugLog(DBG_ERROR, L"[TaskbarCompat][XamlDiag] CreateThread(IXDE) failed GLE=%u", static_cast<unsigned>(gle));
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
            L"[TaskbarCompat][XamlDiag] IXDE интерес ↑ tag=%ls hwnd=0x%p total=%d",
            tag ? tag : L"",
            hwndLog,
            static_cast<int>(v));
        ScheduleIxdeThreadIfPossible();
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
                CLSID_GlassTaskbarTapSite,
                nullptr);

            if (SUCCEEDED(hr))
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][XamlDiag] InitializeXamlDiagnosticsEx OK endpoint=%s attempt=%d",
                    kEndpointName,
                    attempt + 1);
                return hr;
            }

            DebugLog(
                DBG_NORMAL,
                L"[TaskbarCompat][XamlDiag] InitializeXamlDiagnosticsEx попытка %d hr=0x%08X kick=%d",
                attempt + 1,
                static_cast<unsigned>(hr),
                g_ixdeKickRequested.load(std::memory_order_acquire) ? 1 : 0);

            if (static_cast<HRESULT>(hr) == HRESULT_FROM_WIN32(ERROR_NOT_FOUND))
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][XamlDiag] IXDE runtime gate 0x80070490 — дальнейшие попытки бессмысленны");
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
                L"[TaskbarCompat][XamlDiag] CoInitializeEx hr=0x%08X",
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
                DebugLog(DBG_NORMAL, L"[TaskbarCompat][XamlDiag] IXDE-поток: интерес под треем = 0 — выход без IXDE");
                break;
            }

            if (!g_tapPathReady.load(std::memory_order_acquire))
            {
                DebugLog(DBG_ERROR, L"[TaskbarCompat][XamlDiag] TAP не готов во время IXDE");
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
                    L"[TaskbarCompat][XamlDiag] Windows.UI.Xaml.dll недоступна GLE=%u",
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
                    L"[TaskbarCompat][XamlDiag] GetProcAddress(InitializeXamlDiagnosticsEx) GLE=%u",
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

                DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][XamlDiag] IXDE успешно завершён (после бёрста)");
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
                L"[TaskbarCompat][XamlDiag] IXDE итог: FAILED последний hr=0x%08X",
                static_cast<unsigned>(lastHrAllBursts));
        }

        const bool ixdeSucceeded = g_ixdeSucceeded.load(std::memory_order_acquire);

        // После успешного IXDE STA этого потока должна жить до Stop: иначе CoUninitialize до SetSite TAP
        // даёт гонку с AdviseVisualTreeChange (E_UNEXPECTED).
        if (SUCCEEDED(lastHrAllBursts) && ixdeSucceeded)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][XamlDiag] IXDE STA-поток: ожидание Shutdown TAP и разрешения CoUninitialize");
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

HRESULT TaskbarCompat_PrepareXamlDiagnostics() noexcept
{
    g_xamlStopRequested.store(false, std::memory_order_release);
    g_ixdeSucceeded.store(false, std::memory_order_release);
    g_installThreadPosted.store(false, std::memory_order_release);
    g_diagRunning.store(false, std::memory_order_release);
    g_ixdeKickRequested.store(false, std::memory_order_release);
    g_ixdeStaMayExit.store(false, std::memory_order_release);

    // TAP / IXDE / VTC — только Windows 11; backdrop трея (TaskbarHost) остаётся на Win10 через WndCompat.
    if (Shared::SystemHelper::g_buildNumber < 22000)
    {
        g_tapPathReady.store(false, std::memory_order_release);
        g_ixdeSurfaceInterestOrKick.store(0, std::memory_order_release);
        DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][XamlDiag] Win10: TAP/IXDE отключены (build < 22000)");
        return S_OK;
    }

    if (!GetTapDllFullPath(g_tapDllFullPath, std::size(g_tapDllFullPath)))
    {
        DebugLog(DBG_ERROR, L"[TaskbarCompat][XamlDiag] GetTapDllFullPath failed");
        g_tapPathReady.store(false, std::memory_order_release);
        return E_FAIL;
    }

    DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][XamlDiag] TAP DLL path: %s", g_tapDllFullPath);

    // Новый сеанс: сначала TAP готов для Notify из конкурирующего потока explorer до RegisterGlassCore.
    g_ixdeSurfaceInterestOrKick.store(0, std::memory_order_release);
    g_tapPathReady.store(true, std::memory_order_release);

    RegisterGlassCoreXamlDllNotifyFromTaskbarCompat();

    DebugLog(
        DBG_IMPORTANT,
        L"[TaskbarCompat][XamlDiag] Prepared: IXDE после интереса трея (TrackBar/мост) + ModuleHandler WUX)");
    return S_OK;
}

__declspec(dllexport) BOOL WINAPI TaskbarCompat_NotifyTrayTrackBarHwnd(HWND hwnd) noexcept
{
    if (!g_tapPathReady.load(std::memory_order_acquire))
    {
        DebugLog(DBG_WARNING, L"[TaskbarCompat][XamlDiag] NotifyTrayTrackBar: TAP path not ready — пропуск");
        return FALSE;
    }

    if (g_ixdeSucceeded.load(std::memory_order_acquire))
        return TRUE;

    if (ShouldAbortInstall())
        return FALSE;

    BumpSurfaceInterest(hwnd, L"TrayTrackBar");
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI TaskbarCompat_NotifyContentBridgeWindow(HWND hwnd) noexcept
{
    if (!g_tapPathReady.load(std::memory_order_acquire))
    {
        DebugLog(DBG_WARNING, L"[TaskbarCompat][XamlDiag] NotifyContentBridge: TAP path not ready — пропуск");
        return FALSE;
    }

    if (g_ixdeSucceeded.load(std::memory_order_acquire))
        return TRUE;

    if (ShouldAbortInstall())
        return FALSE;

    BumpSurfaceInterest(hwnd, L"ContentBridge");

    DebugLog(
        DBG_IMPORTANT,
        L"[TaskbarCompat][XamlDiag] NotifyContentBridge: hwnd=0x%p",
        hwnd);

    return TRUE;
}

__declspec(dllexport) void WINAPI TaskbarCompat_ReleaseTrayIxdeSurfaceInterest() noexcept
{
    const int prev = g_ixdeSurfaceInterestOrKick.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 0)
        g_ixdeSurfaceInterestOrKick.fetch_add(1, std::memory_order_relaxed);
}

__declspec(dllexport) void WINAPI TaskbarCompat_ResetIxdeSchedulingAfterFailure() noexcept
{
    g_ixdeKickRequested.store(false, std::memory_order_release);
    g_ixdeSurfaceInterestOrKick.store(0, std::memory_order_release);
}

void TaskbarCompat_StopXamlDiagnostics() noexcept
{
    UnregisterGlassCoreXamlDllNotifyFromTaskbarCompat();

    // DllCanUnloadNow=S_OK до CoFree на STA IXDE/marshal — GlassCompat ещё держит LoadLibrary.
    TaskbarCompatCom::g_shutdownDone.store(true, std::memory_order_release);

    const bool diagWasRunning = g_diagRunning.load(std::memory_order_acquire);
    if (diagWasRunning)
    {
        DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][XamlDiag] Stop: BEGIN");
        TaskbarCompat::TaskbarTapSite::ShutdownActiveWatcher();
        g_diagRunning.store(false, std::memory_order_release);
    }

    // После Shutdown TAP — отпускаем STA IXDE-потока (иначе CoUninitialize ломает Advise/Unadvise).
    g_ixdeStaMayExit.store(true, std::memory_order_release);
    g_xamlStopRequested.store(true, std::memory_order_release);

    g_ixdeSucceeded.store(false, std::memory_order_release);
    g_installThreadPosted.store(false, std::memory_order_release);
    g_tapPathReady.store(false, std::memory_order_release);
    g_ixdeKickRequested.store(false, std::memory_order_release);
    g_ixdeSurfaceInterestOrKick.store(0, std::memory_order_release);

    if (!diagWasRunning)
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][XamlDiag] TAP не был в diagRunning — без ActiveWatcher или IXDE не поднят");
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
        DebugLog(DBG_WARNING, L"[TaskbarCompat][XamlDiag] Stop: поток IXDE-install ещё активен");
    }

    DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][XamlDiag] Stop: END");
}

}
