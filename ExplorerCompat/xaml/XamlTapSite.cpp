#include "ExplorerPch.h"
#include "xaml/XamlTapSite.hpp"
#include "xaml/XamlVisualTreeWatcher.hpp"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/Patcher/XamlHostBarPatcherState.h"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/XamlShutdownMarshal.hpp"
#include "xaml/XamlDiagTiming.hpp"
#include "xaml/XamlDiagInit.hpp"

#include <GlassMessages.h>
#include <RootPolicyQuery.h>
#include <xamlOM.h>
#include <DebugLog.h>
#include <atomic>
#include <mutex>
#include <mutex>
#include <winreg.h>

static std::atomic<ULONGLONG> g_diagTickBeginAdvise{};
static std::atomic<ULONGLONG> g_diagTickAdviseDone{};
static std::atomic<DWORD> g_diagTidBegin{};
static std::atomic<bool> g_diagFirstVtcLogged{};

void GlassExplorerXamlDiag_BeginAdviseSequence(const DWORD setSiteOrCallerTid) noexcept
{
    g_diagTickBeginAdvise.store(GetTickCount64(), std::memory_order_relaxed);
    g_diagTidBegin.store(setSiteOrCallerTid, std::memory_order_relaxed);
    g_diagFirstVtcLogged.store(false, std::memory_order_relaxed);
    // Счётчик сэмплера VTC — до Advise, чтобы синхронные Add во время вызова не «съедали» нумерацию до сброса.
    XamlVisualTreeWatcher::ResetVtcAddSampleCounter();
}

void GlassExplorerXamlDiag_OnAdviseCompleted(const DWORD threadThatCalledAdvise) noexcept
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG t0 = g_diagTickBeginAdvise.load(std::memory_order_relaxed);
    g_diagTickAdviseDone.store(now, std::memory_order_relaxed);
    const ULONGLONG dt = t0 != 0 ? (now - t0) : 0;
    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlDiagTicks] Advise OK: Δ begin→Advise=%llu ms, adviseTid=%u beginTid=%u",
        dt,
        static_cast<unsigned>(threadThatCalledAdvise),
        static_cast<unsigned>(g_diagTidBegin.load(std::memory_order_relaxed)));
}

void GlassExplorerXamlDiag_OnVisualTreeCallback(const DWORD vtcTid) noexcept
{
    g_vtcCallbackTid.store(vtcTid, std::memory_order_release);

    bool expected = false;
    if (!g_diagFirstVtcLogged.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_relaxed))
        return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG tAdvise = g_diagTickAdviseDone.load(std::memory_order_relaxed);
    const ULONGLONG t0 = g_diagTickBeginAdvise.load(std::memory_order_relaxed);
    const ULONGLONG dAdvise = tAdvise != 0 ? (now - tAdvise) : 0;
    const ULONGLONG d0 = t0 != 0 ? (now - t0) : 0;
    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlDiagTicks] первый OnVTC: Δ Advise→VTC=%llu ms, Δ begin→VTC=%llu ms vtcTid=%u",
        dAdvise,
        d0,
        static_cast<unsigned>(vtcTid));
}

// CabinetWClass / ExploreWClass: CHROME_REDRAW должен отработать до GlassCore_Shutdown / WndCompat
// (ExecuteShutdown), иначе SetRootEventProvider({}) обнуляет SWCA-колбэки, а PostMessage доставляется
// позже — шапка остаётся «дырявой». SendMessageTimeoutW: провайдеры ещё живы (см. GlassCompat порядок).
// Не RDW_ERASE на корне — только то, что делает TryHandleChromeRedraw (RDW_INVALIDATE|RDW_FRAME).
static BOOL CALLBACK EnumSyncChromeRedraw(HWND top, LPARAM lParam) noexcept
{
    auto* sent = reinterpret_cast<unsigned*>(lParam);
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
        return TRUE;
    wchar_t cls[96]{};
    if (GetClassNameW(top, cls, static_cast<int>(std::size(cls))) <= 0)
        return TRUE;
    if (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0)
        return TRUE;

    constexpr UINT kTimeoutMs = 5000;
    DWORD_PTR smResult = 0;
    if (SendMessageTimeoutW(
            top,
            ExplorerCompatMsg::WM_GLASS_EXPLORER_CHROME_REDRAW,
            0,
            0,
            SMTO_ABORTIFHUNG,
            kTimeoutMs,
            &smResult) != 0)
    {
        ++*sent;
    }
    else
    {
        DebugLog(
            DBG_WARNING,
            L"[ExplorerCompat][XamlTree] SendMessageTimeout CHROME_REDRAW failed hwnd=0x%p GLE=%u",
            top,
            static_cast<unsigned>(GetLastError()));
    }
    return TRUE;
}

static void RedrawExplorerTopLevelShellWindowsImpl() noexcept
{
    const DWORD selfPid = GetCurrentProcessId();
    unsigned sent = 0;
    EnumWindows(EnumSyncChromeRedraw, reinterpret_cast<LPARAM>(&sent));
    DebugLog(DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] Sync CHROME_REDRAW (SendMessageTimeout) для %u окон Cabinet/Explore, pid=%u",
        sent,
        static_cast<unsigned>(selfPid));
}

namespace
{
    IVisualTreeService3* g_visualTreeService = nullptr;
    XamlVisualTreeWatcher* g_treeWatcher = nullptr;
    // Один проход RedrawAllAfterXamlShutdown после Unadvise; ExplorerCompat_Stop не дублирует.
    bool g_shutdownRedrawIssued{};
    std::atomic<bool> g_shutdownVtwActive{ false };
    std::atomic<XamlTapSite*> g_liveTapSite{ nullptr };
    // IXamlDiagnostics для OnRepatchTimer (хранится отдельно от g_visualTreeService).
    IXamlDiagnostics* g_xamlDiag = nullptr;
    std::mutex g_xamlDiagMx;
    // Между SetSite и AdviseVisualTreeChange на worker-потоке (или WM_ADVISE для совместимости).
    IVisualTreeService3* g_pendingAdviseVts = nullptr;
    XamlVisualTreeWatcher* g_pendingAdviseWatcher = nullptr;

    std::mutex g_adviseMx;
    HANDLE g_hAdviseWorker{};

    DWORD WINAPI AdviseWorkerThreadProc(LPVOID) noexcept
    {
        // Как FileBrowsWindowPatch (VisualTreeWatcher): Advise с отдельного потока — не ждём pump
        // очереди потока SetSite (PostMessage к HWND_MESSAGE там давал многосекундные «дыры»).
        const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // S_FALSE = COM уже инициализирован на потоке — не вызывать CoUninitialize (дисбаланс).
        const bool shouldCoUninit = (hrCo == S_OK);
        if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE)
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][TAPSite] Advise worker CoInitializeEx hr=0x%08X",
                static_cast<unsigned>(hrCo));
        }

        XamlTapSite::CompleteDeferredAdvise();

        if (shouldCoUninit)
            CoUninitialize();
        return 0;
    }

    // После put_Fill/Background Win11 часто оставляет устаревший композитный кадр — белая «простыня».
    void RedrawShellTaskbarAfterXamlRestore() noexcept
    {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tray)
            return;
        RedrawWindow(
            tray,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        DebugLog(DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RedrawWindow(Shell_TrayWnd) после Advise (таймер инвалидации композита)");
    }

    void ScheduleTrayPostAdviseTimers(HWND hMsg) noexcept
    {
        if (!hMsg)
            return;
        KillTimer(hMsg, XamlShutdownMarshal::kTrayRedrawAfterAdviseTimerId);
        if (SetTimer(hMsg, XamlShutdownMarshal::kTrayRedrawAfterAdviseTimerId, 1500, nullptr) == 0)
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][TAPSite] SetTimer(TrayRedrawAfterAdvise) failed GLE=%u",
                static_cast<unsigned>(GetLastError()));
        }
    }

    // По мотивам FileBrowsWindowPatch: острова проводника — WinUI; инвалидация нужна после Unadvise,
    // иначе композиция остаётся с «застывшим» кадром после Restore SetProperty.
    [[nodiscard]] bool IsWinUiIslandOrBridgeClass(LPCWSTR cls) noexcept
    {
        if (!cls)
            return false;
        return wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0
            || wcscmp(cls, L"Microsoft.UI.Content.DesktopWindowHost") == 0
            || wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0
            || wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0;
    }

    void RedrawWinUiHostInSubtree(HWND h) noexcept
    {
        if (!h || !IsWindow(h))
            return;
        wchar_t cls[160]{};
        if (GetClassNameW(h, cls, static_cast<int>(std::size(cls))) > 0 && IsWinUiIslandOrBridgeClass(cls))
        {
            // После RestoreHostBarPropertyPatches touch (Get+Set) гонит render pass; тяжёлый
            // RDW_ALLCHILDREN|UPDATENOW на мосту давал мерцание без закрытия дыр с OpenGlass.
            InvalidateRect(h, nullptr, FALSE);
        }
        EnumChildWindows(
            h,
            [](HWND ch, LPARAM) -> BOOL {
                RedrawWinUiHostInSubtree(ch);
                return TRUE;
            },
            0);
    }

    void RedrawExplorerWinUiXamlHosts() noexcept
    {
        const DWORD selfPid = GetCurrentProcessId();
        EnumWindows(
            [](HWND top, LPARAM) -> BOOL {
                DWORD pid = 0;
                if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                    return TRUE;
                RedrawWinUiHostInSubtree(top);
                return TRUE;
            },
            0);
        DebugLog(DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] Redraw WinUI hosts (острова/мосты), pid=%u",
            static_cast<unsigned>(selfPid));
    }

    // Один раз после Advise: дети Shell_TrayWnd (мосты / Xaml / первые уровни) — диагностика «остров закрывает SWCA».
    void LogShellTrayBridgeHwndsOnce() noexcept
    {
        static std::atomic<bool> s_done{};
        if (s_done.exchange(true))
            return;

        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!tray)
        {
            DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlTree][TrayHwnd] Shell_TrayWnd не найден");
            return;
        }

        DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlTree][TrayHwnd] Shell_TrayWnd=0x%p", tray);

        unsigned logged = 0;
        EnumChildWindows(
            tray,
            [](HWND ch, LPARAM lp) -> BOOL {
                auto* pLogged = reinterpret_cast<unsigned*>(lp);
                if (*pLogged >= 48u)
                    return FALSE;
                wchar_t cls[200]{};
                if (GetClassNameW(ch, cls, static_cast<int>(std::size(cls))) <= 0)
                    return TRUE;
                DWORD tid = 0;
                (void)GetWindowThreadProcessId(ch, &tid);
                const bool interest = IsWinUiIslandOrBridgeClass(cls) || wcsstr(cls, L"Bridge") != nullptr
                    || wcsstr(cls, L"Xaml") != nullptr;
                // Первые 10 любых + все «интересные» — без полного шума десятков WorkerW.
                if (interest || *pLogged < 10u)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlTree][TrayHwnd] L1 hwnd=0x%p cls=%s tid=%u interest=%d",
                        ch,
                        cls,
                        static_cast<unsigned>(tid),
                        interest ? 1 : 0);
                    ++*pLogged;
                }
                EnumChildWindows(
                    ch,
                    [](HWND gch, LPARAM lp2) -> BOOL {
                        auto* p2 = reinterpret_cast<unsigned*>(lp2);
                        if (*p2 >= 48u)
                            return FALSE;
                        wchar_t gcls[200]{};
                        if (GetClassNameW(gch, gcls, static_cast<int>(std::size(gcls))) <= 0)
                            return TRUE;
                        DWORD gtid = 0;
                        (void)GetWindowThreadProcessId(gch, &gtid);
                        const bool gInterest = IsWinUiIslandOrBridgeClass(gcls) || wcsstr(gcls, L"Bridge") != nullptr
                            || wcsstr(gcls, L"Xaml") != nullptr;
                        if (gInterest)
                        {
                            DebugLog(DBG_IMPORTANT,
                                L"[ExplorerCompat][XamlTree][TrayHwnd] L2 hwnd=0x%p cls=%s tid=%u",
                                gch,
                                gcls,
                                static_cast<unsigned>(gtid));
                            ++*p2;
                        }
                        return TRUE;
                    },
                    lp);
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&logged));
    }

    [[nodiscard]] bool IsExplorerIslandPrepClass(const wchar_t* cls) noexcept
    {
        if (!cls)
            return false;
        return wcscmp(cls, L"SHELLDLL_DefView") == 0
            || wcscmp(cls, L"FileExplorer_FolderViewIsland") == 0;
    }

    // Как XamlVisualTreeWatcher::IsIslandPrimeClass: DefView, FolderViewIsland, DesktopChildSiteBridge,
    // XamlExplorerHostIslandWindow. Шапка и клиентский остров — разные HWND, оба часто DesktopChildSiteBridge.
    [[nodiscard]] bool IsGdiPrimeTargetClass(const wchar_t* cls) noexcept
    {
        if (!cls)
            return false;
        return IsExplorerIslandPrepClass(cls)
            || wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0
            || wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0;
    }

    // WinUI-мосты шапки (верхняя полоса Cabinet): FillRect(COLOR_WINDOW) заливает системные кнопки.
    // Класс DesktopChildSiteBridge общий и для клиентского острова — отличаем по геометрии.
    [[nodiscard]] bool IsExplorerHostBarBridgeHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;

        wchar_t cls[160]{};
        if (GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) <= 0)
            return false;
        if (wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") != 0
            && wcscmp(cls, L"XamlExplorerHostIslandWindow") != 0)
            return false;

        const HWND hRoot = GetAncestor(hWnd, GA_ROOT);
        if (!hRoot || !IsWindow(hRoot))
            return true;

        wchar_t rootCls[96]{};
        if (GetClassNameW(hRoot, rootCls, static_cast<int>(std::size(rootCls))) <= 0)
            return true;
        if (wcscmp(rootCls, L"CabinetWClass") != 0 && wcscmp(rootCls, L"ExploreWClass") != 0)
            return true;

        RECT rcRootClient{};
        if (!GetClientRect(hRoot, &rcRootClient))
            return true;

        POINT bridgeOrigin{ 0, 0 };
        ClientToScreen(hWnd, &bridgeOrigin);
        POINT rootClientOrigin{ 0, 0 };
        ClientToScreen(hRoot, &rootClientOrigin);

        const int offsetY = bridgeOrigin.y - rootClientOrigin.y;
        const int clientH = rcRootClient.bottom - rcRootClient.top;
        if (clientH <= 0)
            return true;

        constexpr int kHostBarMaxBandPx = 148;
        const int bandPx = (std::min)(kHostBarMaxBandPx, clientH / 3);
        return offsetY >= 0 && offsetY < bandPx;
    }

    void UndoGdiPrimeBlackUnderHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;
        wchar_t cls[160]{};
        if (GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) <= 0)
            return;
        if (!IsGdiPrimeTargetClass(cls))
            return;

        if (IsExplorerHostBarBridgeHwnd(hWnd))
        {
            // Сброс чёрного GDI-prime без FillRect — иначе заливает caption buttons.
            InvalidateRect(hWnd, nullptr, FALSE);
            return;
        }

        RECT rc{};
        if (!GetClientRect(hWnd, &rc))
            return;
        if (rc.right <= rc.left || rc.bottom <= rc.top)
            return;

        HDC hdc = GetDC(hWnd);
        if (!hdc)
            return;

        HBRUSH br = GetSysColorBrush(COLOR_WINDOW);
        if (!br)
            br = reinterpret_cast<HBRUSH>(static_cast<ULONG_PTR>(COLOR_WINDOW) + 1);
        FillRect(hdc, &rc, br);
        ReleaseDC(hWnd, hdc);
    }

    void UndoExplorerIslandGdiPrimeAllProcess() noexcept
    {
        EnumWindows(
            [](HWND top, LPARAM) -> BOOL {
                DWORD pid = 0;
                if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                    return TRUE;

                UndoGdiPrimeBlackUnderHwnd(top);
                EnumChildWindows(
                    top,
                    [](HWND child, LPARAM) -> BOOL {
                        UndoGdiPrimeBlackUnderHwnd(child);
                        return TRUE;
                    },
                    0);
                return TRUE;
            },
            0);
        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] Undo GDI prime (COLOR_WINDOW; мост шапки — InvalidateRect), pid=%u",
            static_cast<unsigned>(GetCurrentProcessId()));
    }

    void RedrawExplorerTopLevelShellWindows() noexcept
    {
        RedrawExplorerTopLevelShellWindowsImpl();
    }

    // Size-toggle трюк: кратковременно уменьшить окно на 1px и вернуть обратно.
    // Форсирует полный WinUI layout pass + re-render всех XAML-островов.
    void SizeToggleExplorerWindows() noexcept
    {
        EnumWindows(
            [](HWND top, LPARAM) -> BOOL {
                DWORD pid = 0;
                if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                    return TRUE;
                wchar_t cls[96]{};
                if (GetClassNameW(top, cls, static_cast<int>(std::size(cls))) <= 0)
                    return TRUE;
                if (wcscmp(cls, L"CabinetWClass") != 0 && wcscmp(cls, L"ExploreWClass") != 0)
                    return TRUE;

                RECT rc{};
                if (!GetWindowRect(top, &rc))
                    return TRUE;
                const int w = rc.right - rc.left;
                const int h = rc.bottom - rc.top;
                if (w <= 1 || h <= 1)
                    return TRUE;

                // Уменьшить на 1px → полный re-layout WinUI островов
                SetWindowPos(top, nullptr, 0, 0, w - 1, h,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                // Вернуть обратно
                SetWindowPos(top, nullptr, 0, 0, w, h,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                return TRUE;
            },
            0);
        DebugLog(DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] SizeToggle Cabinet/Explore (WinUI re-layout), pid=%u",
            static_cast<unsigned>(GetCurrentProcessId()));
    }

    void RedrawExplorerHostBarSurfacesAfterApplyImpl() noexcept
    {
        RedrawExplorerWinUiXamlHosts();
        RedrawExplorerTopLevelShellWindows();
        SizeToggleExplorerWindows();
    }

    void RedrawExplorerHostBarSurfacesAfterRestoreImpl() noexcept
    {
        UndoExplorerIslandGdiPrimeAllProcess();
        RedrawExplorerHostBarSurfacesAfterApplyImpl();
    }

    void RedrawAllAfterXamlShutdown() noexcept
    {
        RedrawExplorerHostBarSurfacesAfterRestoreImpl();
        RedrawShellTaskbarAfterXamlRestore();
    }

    enum class ExplorerWin32SurfaceScope
    {
        HostBarOnly,
        HostBarAndTray,
    };

    void RestoreExplorerWin32Surfaces(ExplorerWin32SurfaceScope scope) noexcept
    {
        switch (scope)
        {
        case ExplorerWin32SurfaceScope::HostBarOnly:
            RedrawExplorerHostBarSurfacesAfterRestoreImpl();
            break;
        case ExplorerWin32SurfaceScope::HostBarAndTray:
            RedrawAllAfterXamlShutdown();
            break;
        default:
            break;
        }
    }

    void PrimeIslandBackgroundUnderHwnd(HWND hWnd) noexcept
    {
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;
        if (!hWnd || !IsWindow(hWnd))
            return;

        wchar_t cls[128]{};
        if (GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) <= 0)
            return;
        if (!IsGdiPrimeTargetClass(cls))
            return;

        RECT rc{};
        if (!GetClientRect(hWnd, &rc))
            return;
        if (rc.right <= rc.left || rc.bottom <= rc.top)
            return;

        HDC hdc = GetDC(hWnd);
        if (!hdc)
            return;

        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        ReleaseDC(hWnd, hdc);

        if (wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0)
        {
            InvalidateRect(hWnd, nullptr, FALSE);
            UpdateWindow(hWnd);
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] PrimeIslandBackground: hwnd=0x%p class=%s rc={%ld,%ld,%ld,%ld}",
            hWnd,
            cls,
            rc.left,
            rc.top,
            rc.right,
            rc.bottom);
    }

    void PrimeExplorerIslandBackgrounds() noexcept
    {
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return;
        EnumWindows(
            [](HWND top, LPARAM) -> BOOL {
                DWORD pid = 0;
                if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                    return TRUE;

                PrimeIslandBackgroundUnderHwnd(top);
                EnumChildWindows(
                    top,
                    [](HWND child, LPARAM) -> BOOL {
                        PrimeIslandBackgroundUnderHwnd(child);
                        return TRUE;
                    },
                    0);
                return TRUE;
            },
            0);
    }

    HANDLE g_hApplyHostBarChromeDone{};
    HANDLE g_hRestoreHostBarChromeDone{};
    std::once_flag g_applyHostBarChromeEventOnce;
    std::once_flag g_restoreHostBarChromeEventOnce;

    void EnsureApplyHostBarChromeDoneEvent() noexcept
    {
        std::call_once(g_applyHostBarChromeEventOnce, []() noexcept {
            g_hApplyHostBarChromeDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        });
    }

    void EnsureRestoreHostBarChromeDoneEvent() noexcept
    {
        std::call_once(g_restoreHostBarChromeEventOnce, []() noexcept {
            g_hRestoreHostBarChromeDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        });
    }

    void SignalApplyHostBarChromeDone() noexcept
    {
        if (g_hApplyHostBarChromeDone)
            SetEvent(g_hApplyHostBarChromeDone);
    }

    void SignalRestoreHostBarChromeDone() noexcept
    {
        if (g_hRestoreHostBarChromeDone)
            SetEvent(g_hRestoreHostBarChromeDone);
    }

    void NudgeExplorerWinUiForVtcWake() noexcept
    {
        RedrawExplorerWinUiXamlHosts();
    }

    void RedrawExplorerShellCaptionAfterLiveOffImpl() noexcept
    {
        // Live OFF Win32: только CHROME_REDRAW (NC-путь в TryHandleChromeRedraw). Без GDI FillRect/SizeToggle.
        RedrawExplorerTopLevelShellWindowsImpl();
    }
}

void XamlTapSite::RedrawSurfacesAfterFullStop() noexcept
{
    if (g_shutdownRedrawIssued)
        return;
    RestoreExplorerWin32Surfaces(ExplorerWin32SurfaceScope::HostBarAndTray);
}

static std::atomic<ULONGLONG> g_lastLiveHostBarApplySuccessTick{};

void XamlTapSite::PaintPumpBrief() noexcept
{
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_PAINT);
    MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_PAINT);
}

void XamlTapSite::FinishWinUiHostBarChromeRestore() noexcept
{
    // WinUI finisher (live OFF = shutdown до Unadvise): только paint pump.
    PaintPumpBrief();
}

void XamlTapSite::RedrawExplorerHostBarSurfacesAfterRestore() noexcept
{
    RestoreExplorerWin32Surfaces(ExplorerWin32SurfaceScope::HostBarOnly);
}

void XamlTapSite::AfterExplorerChromeApplied(IXamlDiagnostics* diag) noexcept
{
    if (!diag)
        return;

    PaintPumpBrief();
    XamlVisualTreeWatcher::PrimeExplorerIslandsAttachPulse();
    RedrawExplorerHostBarSurfacesAfterApplyImpl();
}

void XamlTapSite::PostLiveDisableChromeRedraw() noexcept
{
    XamlShutdownMarshal::RunPostLiveDisableChromeRedrawOnXamlThread();
}

void XamlTapSite::RedrawExplorerShellCaptionAfterLiveOff() noexcept
{
    RedrawExplorerShellCaptionAfterLiveOffImpl();
}

void XamlTapSite::NoteLiveHostBarApplyCompleted() noexcept
{
    g_lastLiveHostBarApplySuccessTick.store(GetTickCount64(), std::memory_order_release);
}

bool XamlTapSite::ShouldCoalesceLiveHostBarApply() noexcept
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG prev = g_lastLiveHostBarApplySuccessTick.load(std::memory_order_acquire);
    constexpr ULONGLONG kCoalesceMs = 4000;
    return prev != 0 && now >= prev && (now - prev) < kCoalesceMs;
}

void XamlTapSite::RestoreExplorerHostBarChrome(IXamlDiagnostics* diag) noexcept
{
    if (!diag)
        return;

    g_replayRestoreAfterOff.store(true, std::memory_order_release);

    XamlHostBarPatcher::RestoreExplorerChrome(
        diag, XamlHostBarPatcher::ExplorerChromeRestoreScope::HostBarOnly);
    FinishWinUiHostBarChromeRestore();

    DebugLog(DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] RestoreExplorerHostBarChrome: HostBarOnly (Win32 на stop)");
}

void XamlTapSite::ApplyExplorerHostBarChrome(IXamlDiagnostics* diag) noexcept
{
    if (!diag)
        return;

    g_disableHostBarPatching.store(false, std::memory_order_release);
    const int patched = XamlHostBarPatcher::ApplyExplorerChrome(
        diag, XamlHostBarPatcher::ExplorerChromeApplyScope::FullReplay);
    AfterExplorerChromeApplied(diag);

    DebugLog(DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] ApplyExplorerHostBarChrome: FullReplay patched=%d tid=%u",
        patched,
        static_cast<unsigned>(GetCurrentThreadId()));
    if (patched > 0)
        NoteLiveHostBarApplyCompleted();
}

void XamlTapSite::TryConsumePendingApplyHostBarChrome(IXamlDiagnostics* diag) noexcept
{
    if (!diag)
        return;
    if (!g_pendingApplyHostBarChrome.exchange(false, std::memory_order_acq_rel))
        return;

    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] TryConsumePendingApplyHostBarChrome: vtcTid=%u",
        static_cast<unsigned>(GetCurrentThreadId()));
    ApplyExplorerHostBarChrome(diag);
    SignalApplyHostBarChromeDone();
}

void XamlTapSite::RequestRepatchTabElementsOnVtcThread() noexcept
{
    if (!XamlHostBarPatcher::IsExplorerXamlPatchingAllowed())
        return;

    const DWORD vtcTid = g_vtcCallbackTid.load(std::memory_order_acquire);
    const DWORD curTid = GetCurrentThreadId();
    if (vtcTid != 0 && curTid == vtcTid)
    {
        if (IXamlDiagnostics* diag = AddRefXamlDiagnosticsIfAvailable())
        {
            XamlHostBarPatcher::RepatchTabElements(diag);
            diag->Release();
        }
        return;
    }

    const bool wasPending = g_pendingRepatchTabElements.exchange(true, std::memory_order_acq_rel);
    if (!wasPending)
        NudgeExplorerWinUiForVtcWake();
}

void XamlTapSite::TryConsumePendingRepatchTabElements(IXamlDiagnostics* diag) noexcept
{
    if (!diag)
        return;
    if (!g_pendingRepatchTabElements.exchange(false, std::memory_order_acq_rel))
        return;

    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] TryConsumePendingRepatchTabElements: vtcTid=%u",
        static_cast<unsigned>(GetCurrentThreadId()));
    XamlHostBarPatcher::RepatchTabElements(diag);
}

void XamlTapSite::RequestRestoreHostBarChromeOnVtcThread() noexcept
{
    const DWORD vtcTid = g_vtcCallbackTid.load(std::memory_order_acquire);
    const DWORD curTid = GetCurrentThreadId();
    const bool shutdownRestore = GlassExplorerCom::g_shutdownDone.load(std::memory_order_acquire);

    // Live: на VTC можно SetProperty напрямую. Shutdown: даже curTid==vtcTid часто popup
    // (HWND_MESSAGE на том же tid) — сначала island UI-queue.
    if (!shutdownRestore && vtcTid != 0 && curTid == vtcTid)
    {
        if (IXamlDiagnostics* diag = AddRefXamlDiagnosticsIfAvailable())
        {
            RestoreExplorerHostBarChrome(diag);
            diag->Release();
        }
        SignalRestoreHostBarChromeDone();
        return;
    }

    EnsureRestoreHostBarChromeDoneEvent();

    auto tryUiQueueTwice = [](IXamlDiagnostics* diag) noexcept -> bool
    {
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            bool restored = false;
            if (XamlHostBarPatcher::TryRestoreExplorerHostBarChromeOnUiQueueSync(diag, &restored) && restored)
                return true;
        }
        return false;
    };

    if (IXamlDiagnostics* diag = AddRefXamlDiagnosticsIfAvailable())
    {
        if (tryUiQueueTwice(diag))
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] RequestRestoreHostBarChromeOnVtcThread: UI-queue sync curTid=%u shutdown=%d",
                static_cast<unsigned>(curTid),
                shutdownRestore ? 1 : 0);
            diag->Release();
            SignalRestoreHostBarChromeDone();
            return;
        }
        diag->Release();
    }

    // STOP / ShellExt: контекстное меню портит last-VTC (popup). Ждать VTC — потерять restore
    // перед Unadvise (подтверждено 2026-07-14). Без VTC-fallback; last-ditch на текущем потоке.
    if (shutdownRestore)
    {
        if (IXamlDiagnostics* diag = AddRefXamlDiagnosticsIfAvailable())
        {
            const bool previousUiQueueRestore = g_hostBarRestoreOnIslandUiQueue;
            g_hostBarRestoreOnIslandUiQueue = true;
            RestoreExplorerHostBarChrome(diag);
            g_hostBarRestoreOnIslandUiQueue = previousUiQueueRestore;
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] RequestRestoreHostBarChromeOnVtcThread: shutdown last-ditch "
                L"curTid=%u poisonedVtc=%u",
                static_cast<unsigned>(curTid),
                static_cast<unsigned>(vtcTid));
            diag->Release();
        }
        else
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] RequestRestoreHostBarChromeOnVtcThread: shutdown — no diag "
                L"(poisoned vtcTid=%u curTid=%u)",
                static_cast<unsigned>(vtcTid),
                static_cast<unsigned>(curTid));
        }
        SignalRestoreHostBarChromeDone();
        return;
    }

    const bool wasPending = g_pendingRestoreHostBarChrome.exchange(true, std::memory_order_acq_rel);
    if (wasPending)
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RequestRestoreHostBarChromeOnVtcThread: already pending — wait");
        constexpr DWORD kWaitMs = 2000;
        (void)WaitForSingleObject(g_hRestoreHostBarChromeDone, kWaitMs);
        return;
    }

    if (vtcTid == 0)
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RequestRestoreHostBarChromeOnVtcThread: pending until first VTC "
            L"(curTid=%u)",
            static_cast<unsigned>(curTid));
        return;
    }

    ResetEvent(g_hRestoreHostBarChromeDone);
    NudgeExplorerWinUiForVtcWake();

    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] RequestRestoreHostBarChromeOnVtcThread: VTC fallback wait vtcTid=%u curTid=%u",
        static_cast<unsigned>(vtcTid),
        static_cast<unsigned>(curTid));

    constexpr DWORD kWaitMs = 2000;
    const DWORD wr = WaitForSingleObject(g_hRestoreHostBarChromeDone, kWaitMs);
    if (wr == WAIT_TIMEOUT && g_pendingRestoreHostBarChrome.load(std::memory_order_acquire))
    {
        if (IXamlDiagnostics* diag = AddRefXamlDiagnosticsIfAvailable())
        {
            bool restored = false;
            if (XamlHostBarPatcher::TryRestoreExplorerHostBarChromeOnUiQueueSync(diag, &restored) && restored)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] RequestRestoreHostBarChromeOnVtcThread: UI-queue retry curTid=%u",
                    static_cast<unsigned>(curTid));
                g_pendingRestoreHostBarChrome.store(false, std::memory_order_release);
                diag->Release();
                SignalRestoreHostBarChromeDone();
                return;
            }
            diag->Release();
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RequestRestoreHostBarChromeOnVtcThread: WARN VTC timeout %ums",
            kWaitMs);
        g_pendingRestoreHostBarChrome.store(false, std::memory_order_release);
    }
}

void XamlTapSite::TryConsumePendingRestoreHostBarChrome(IXamlDiagnostics* diag) noexcept
{
    if (!diag)
        return;
    if (!g_pendingRestoreHostBarChrome.exchange(false, std::memory_order_acq_rel))
        return;

    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] TryConsumePendingRestoreHostBarChrome: vtcTid=%u",
        static_cast<unsigned>(GetCurrentThreadId()));
    RestoreExplorerHostBarChrome(diag);
    SignalRestoreHostBarChromeDone();
}

void XamlTapSite::RequestApplyHostBarChromeOnVtcThread() noexcept
{
    const DWORD vtcTid = g_vtcCallbackTid.load(std::memory_order_acquire);
    const DWORD curTid = GetCurrentThreadId();

    if (vtcTid != 0 && curTid == vtcTid)
    {
        if (IXamlDiagnostics* diag = AddRefXamlDiagnosticsIfAvailable())
        {
            ApplyExplorerHostBarChrome(diag);
            diag->Release();
        }
        SignalApplyHostBarChromeDone();
        return;
    }

    EnsureApplyHostBarChromeDoneEvent();

    if (IXamlDiagnostics* diag = AddRefXamlDiagnosticsIfAvailable())
    {
        int patched = 0;
        if (XamlHostBarPatcher::TryApplyExplorerHostBarChromeOnUiQueueSync(diag, &patched) && patched > 0)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] RequestApplyHostBarChromeOnVtcThread: UI-queue sync patched=%d "
                L"curTid=%u",
                patched,
                static_cast<unsigned>(curTid));
            NoteLiveHostBarApplyCompleted();
            AfterExplorerChromeApplied(diag);
            diag->Release();
            SignalApplyHostBarChromeDone();
            return;
        }
        diag->Release();
    }

    const bool wasPending = g_pendingApplyHostBarChrome.exchange(true, std::memory_order_acq_rel);
    if (wasPending)
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RequestApplyHostBarChromeOnVtcThread: already pending — wait");
        constexpr DWORD kWaitMs = 2000;
        (void)WaitForSingleObject(g_hApplyHostBarChromeDone, kWaitMs);
        return;
    }

    if (vtcTid == 0)
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RequestApplyHostBarChromeOnVtcThread: pending until first VTC "
            L"(curTid=%u)",
            static_cast<unsigned>(curTid));
        return;
    }

    ResetEvent(g_hApplyHostBarChromeDone);
    NudgeExplorerWinUiForVtcWake();

    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] RequestApplyHostBarChromeOnVtcThread: VTC fallback wait vtcTid=%u curTid=%u",
        static_cast<unsigned>(vtcTid),
        static_cast<unsigned>(curTid));

    constexpr DWORD kWaitMs = 2000;
    const DWORD wr = WaitForSingleObject(g_hApplyHostBarChromeDone, kWaitMs);
    if (wr == WAIT_TIMEOUT && g_pendingApplyHostBarChrome.load(std::memory_order_acquire))
    {
        if (IXamlDiagnostics* diag = AddRefXamlDiagnosticsIfAvailable())
        {
            int patched = 0;
            if (XamlHostBarPatcher::TryApplyExplorerHostBarChromeOnUiQueueSync(diag, &patched) && patched > 0)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] RequestApplyHostBarChromeOnVtcThread: UI-queue retry patched=%d",
                    patched);
                g_pendingApplyHostBarChrome.store(false, std::memory_order_release);
                NoteLiveHostBarApplyCompleted();
                AfterExplorerChromeApplied(diag);
                diag->Release();
                SignalApplyHostBarChromeDone();
                return;
            }
            diag->Release();
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RequestApplyHostBarChromeOnVtcThread: WARN VTC timeout %ums",
            kWaitMs);
        g_pendingApplyHostBarChrome.store(false, std::memory_order_release);
    }
}

void XamlTapSite::OnRootBackdropDisabled() noexcept
{
    RequestRestoreHostBarChromeOnVtcThread();
}

void XamlTapSite::OnRootBackdropEnabled() noexcept
{
    RequestApplyHostBarChromeOnVtcThread();
}

IXamlDiagnostics* XamlTapSite::AddRefXamlDiagnosticsIfAvailable() noexcept
{
    std::lock_guard lock(g_xamlDiagMx);
    if (!g_xamlDiag)
        return nullptr;
    g_xamlDiag->AddRef();
    return g_xamlDiag;
}

XamlTapSite::XamlTapSite() noexcept
{
    GlassExplorerCom::g_objectCount.fetch_add(1, std::memory_order_relaxed);
}

XamlTapSite::~XamlTapSite() noexcept
{
    ReleaseSiteBinding();
}

void XamlTapSite::ReleaseSiteBinding() noexcept
{
    if (!m_site)
        return;
    m_site->Release();
    m_site = nullptr;
}

HRESULT STDMETHODCALLTYPE XamlTapSite::QueryInterface(REFIID riid, void** ppvObject) noexcept
{
    if (!ppvObject)
        return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown || riid == IID_IObjectWithSite)
    {
        *ppvObject = static_cast<IObjectWithSite*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE XamlTapSite::AddRef() noexcept
{
    return ++m_ref;
}

ULONG STDMETHODCALLTYPE XamlTapSite::Release() noexcept
{
    const ULONG c = --m_ref;
    if (c == 0)
    {
        XamlTapSite* expected = this;
        (void)g_liveTapSite.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        GlassExplorerCom::g_objectCount.fetch_sub(1, std::memory_order_relaxed);
        delete this;
    }
    return c;
}

void XamlTapSite::CancelDeferredAdvisePending() noexcept
{
    const HWND h = XamlShutdownMarshal::GetMessageWindowHwnd();
    if (h)
    {
        KillTimer(h, XamlShutdownMarshal::kTrayRedrawAfterAdviseTimerId);
        KillTimer(h, XamlShutdownMarshal::kRepatchTimerId);
        MSG msg{};
        while (PeekMessageW(
            &msg,
            h,
            ExplorerCompatMsg::WM_GLASS_EXPLORER_XAML_ADVISE,
            ExplorerCompatMsg::WM_GLASS_EXPLORER_XAML_ADVISE,
            PM_REMOVE))
        {
        }
    }

    HANDLE hWorker = nullptr;
    {
        std::lock_guard lock(g_adviseMx);
        if (g_pendingAdviseVts && g_pendingAdviseWatcher)
        {
            g_pendingAdviseWatcher->Release();
            g_pendingAdviseVts->Release();
            g_pendingAdviseVts = nullptr;
            g_pendingAdviseWatcher = nullptr;
        }
        hWorker = g_hAdviseWorker;
        g_hAdviseWorker = nullptr;
    }

    if (hWorker)
    {
        constexpr DWORD kWaitMs = 10'000;
        const DWORD wr = WaitForSingleObject(hWorker, kWaitMs);
        if (wr != WAIT_OBJECT_0)
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][TAPSite] CancelDeferred: ожидание advise worker wr=%u GLE=%u",
                static_cast<unsigned>(wr),
                static_cast<unsigned>(GetLastError()));
        }
        CloseHandle(hWorker);
    }
}

void XamlTapSite::CompleteDeferredAdvise() noexcept
{
    IVisualTreeService3* vts = nullptr;
    XamlVisualTreeWatcher* watcher = nullptr;
    {
        std::lock_guard lock(g_adviseMx);
        vts = g_pendingAdviseVts;
        watcher = g_pendingAdviseWatcher;
        g_pendingAdviseVts = nullptr;
        g_pendingAdviseWatcher = nullptr;
    }

    if (!vts || !watcher)
        return;

    // Повторный GDI-prime на потоке worker сразу перед Advise — порядок не зависит от потока SetSite (WinUI).
    PrimeExplorerIslandBackgrounds();

    const HRESULT hr = vts->AdviseVisualTreeChange(reinterpret_cast<IVisualTreeServiceCallback*>(watcher));
    if (FAILED(hr))
    {
        DebugLog(
            DBG_WARNING,
            L"[ExplorerCompat][TAPSite] отложенный AdviseVisualTreeChange hr=0x%08X",
            static_cast<unsigned>(hr));
        watcher->Release();
        vts->Release();
        return;
    }

    g_visualTreeService = vts;
    g_treeWatcher = watcher;
    DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][TAPSite] отложенный Advise OK, VisualTreeWatcher active");
    GlassExplorerXamlDiag_OnAdviseCompleted(GetCurrentThreadId());

    LogShellTrayBridgeHwndsOnce();

    const HWND hMsg = XamlShutdownMarshal::GetMessageWindowHwnd();
    ScheduleTrayPostAdviseTimers(hMsg);
}

HRESULT XamlTapSite::ShutdownVisualTreeWatcher() noexcept
{
    bool expectedVtw = false;
    if (!g_shutdownVtwActive.compare_exchange_strong(expectedVtw, true, std::memory_order_acq_rel))
    {
        return S_OK;
    }

    struct ShutdownVtwScope final
    {
        ~ShutdownVtwScope() noexcept
        {
            g_shutdownVtwActive.store(false, std::memory_order_release);
        }
    } vtwScope;

    CancelDeferredAdvisePending();

    const HWND hMsgProbe = XamlShutdownMarshal::GetMessageWindowHwnd();
    if (hMsgProbe)
    {
        KillTimer(hMsgProbe, XamlShutdownMarshal::kTrayRedrawAfterAdviseTimerId);
        KillTimer(hMsgProbe, XamlShutdownMarshal::kRepatchTimerId);
    }

    if (g_visualTreeService && g_treeWatcher)
    {
        // ShellExt-меню травит g_vtcCallbackTid (popup). HWND_MESSAGE живёт на Advise-потоке —
        // родном VTC host bar. Сброс VTC: не defer на popup и не UI-queue с vtcMatch popup.
        (void)g_vtcCallbackTid.exchange(0, std::memory_order_acq_rel);

        // Сначала восстановить свойства через IXamlDiagnostics (пока Advise ещё активен).
        g_treeWatcher->RestoreAllFills();
        FinishWinUiHostBarChromeRestore();
        g_visualTreeService->UnadviseVisualTreeChange(
            reinterpret_cast<IVisualTreeServiceCallback*>(g_treeWatcher));
        g_treeWatcher->Release();
        g_treeWatcher = nullptr;
        g_visualTreeService->Release();
        g_visualTreeService = nullptr;
        {
            std::lock_guard lock(g_xamlDiagMx);
            if (g_xamlDiag)
            {
                g_xamlDiag->Release();
                g_xamlDiag = nullptr;
            }
        }
        // Перерисовка после Unadvise — иначе WinUI не подхватывает дефолтный вид островов.
        RestoreExplorerWin32Surfaces(ExplorerWin32SurfaceScope::HostBarAndTray);
        g_shutdownRedrawIssued = true;
        // Короткий pump вместо второго полного прохода (убирает мерцание шапки при Stop).
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 50, QS_PAINT);
    }

    if (XamlTapSite* live = g_liveTapSite.load(std::memory_order_acquire))
        live->ReleaseSiteBinding();

    return S_OK;
}

void XamlTapSite::OnRepatchTimer() noexcept
{
    if (!g_treeWatcher)
        return;

    int cyclesLeft = 0;
    {
        std::lock_guard lock(g_xamlMutex);
        if (g_repatchCyclesRemaining <= 0)
        {
            HWND hMsg = XamlShutdownMarshal::GetMessageWindowHwnd();
            if (hMsg)
                KillTimer(hMsg, XamlShutdownMarshal::kRepatchTimerId);
            return;
        }
        --g_repatchCyclesRemaining;
        cyclesLeft = g_repatchCyclesRemaining;
    }

    RequestRepatchTabElementsOnVtcThread();

    if (cyclesLeft <= 0)
    {
        HWND hMsg = XamlShutdownMarshal::GetMessageWindowHwnd();
        if (hMsg)
            KillTimer(hMsg, XamlShutdownMarshal::kRepatchTimerId);
    }
}

void XamlTapSite::OnTrayRedrawAfterAdviseTimer() noexcept
{
    // Сохраняется как универсальный invalidation Shell_TrayWnd после Advise; зона 1 этим
    // модулем больше не патчится, но redraw остаётся безвредным no-op для трея TaskbarCompat.
    RedrawShellTaskbarAfterXamlRestore();
}

HRESULT STDMETHODCALLTYPE XamlTapSite::SetSite(IUnknown* pUnkSite) noexcept
{
    // Как TranslucentTB / FileBrowsWindowPatch: одна активная сессия VisualTreeWatcher.
    // Раньше здесь всегда вызывали Shutdown — вторая InitializeXamlDiagnosticsEx (WinUI)
    // снимала Advises первой (UWP), из‑за чего пропадала цепочка для островов проводника.
    if (!pUnkSite)
    {
        ShutdownVisualTreeWatcher();
        if (m_site)
        {
            m_site->Release();
            m_site = nullptr;
        }
        DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][TAPSite] SetSite(nullptr)");
        return S_OK;
    }

    DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][TAPSite] SetSite: вход от WinUI (IXDE → TAP)");
    g_shutdownRedrawIssued = false;
    // Критично: до старта/перестройки острова подготавливаем чёрный GDI-фон вручную.
    // Это не зависит от WM_PAINT/rcPaint и работает даже когда paint-rect пуст.
    PrimeExplorerIslandBackgrounds();

    if (g_treeWatcher != nullptr || g_visualTreeService != nullptr || g_pendingAdviseVts != nullptr)
    {
        DebugLog(DBG_WARNING,
            L"[ExplorerCompat][TAPSite] SetSite: вторая сессия XAML Diagnostics отклонена "
            L"(уже есть активный VisualTreeWatcher)");
        return E_UNEXPECTED;
    }

    if (m_site)
    {
        m_site->Release();
        m_site = nullptr;
    }

    m_site = pUnkSite;
    m_site->AddRef();
    g_liveTapSite.store(this, std::memory_order_release);

    IXamlDiagnostics* xamlDiag = nullptr;
    HRESULT hr = pUnkSite->QueryInterface(IID_IXamlDiagnostics, reinterpret_cast<void**>(&xamlDiag));
    if (FAILED(hr))
    {
        DebugLog(DBG_WARNING, L"[ExplorerCompat][TAPSite] QI IXamlDiagnostics hr=0x%08X", static_cast<unsigned>(hr));
        m_site->Release();
        m_site = nullptr;
        return hr;
    }

    IVisualTreeService3* vts = nullptr;
    hr = pUnkSite->QueryInterface(IID_IVisualTreeService3, reinterpret_cast<void**>(&vts));
    if (FAILED(hr))
    {
        DebugLog(DBG_WARNING, L"[ExplorerCompat][TAPSite] QI IVisualTreeService3 hr=0x%08X", static_cast<unsigned>(hr));
        xamlDiag->Release();
        m_site->Release();
        m_site = nullptr;
        return hr;
    }

    auto* watcher = new (std::nothrow) XamlVisualTreeWatcher(xamlDiag);
    // Сохраняем IXamlDiagnostics для OnRepatchTimer (AddRef — своя ссылка).
    {
        std::lock_guard lock(g_xamlDiagMx);
        g_xamlDiag = xamlDiag; // уже имеет +1 от QI выше
    }
    // НЕ вызываем Release — g_xamlDiag теперь владеет ссылкой из QI.
    // watcher получает свою ссылку через конструктор (AddRef внутри).
    if (!watcher)
    {
        {
            std::lock_guard lock(g_xamlDiagMx);
            if (g_xamlDiag)
            {
                g_xamlDiag->Release();
                g_xamlDiag = nullptr;
            }
        }
        vts->Release();
        m_site->Release();
        m_site = nullptr;
        return E_OUTOFMEMORY;
    }

    // HWND_MESSAGE для WM_SHUTDOWN/Unadvise с другого потока. Advise всегда на отдельном worker
    // (раньше синхронный путь при SetSite != поток HWND_MESSAGE давал VTC на «чужом» STA — ломалась прозрачность шапки).
    XamlShutdownMarshal::EnsureMessageWindow();
    const HWND hMsg = XamlShutdownMarshal::GetMessageWindowHwnd();
    DWORD msgTid = 0;
    if (hMsg)
        msgTid = GetWindowThreadProcessId(hMsg, nullptr);

    const DWORD setSiteTid = GetCurrentThreadId();
    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][TAPSite] SetSite: Advise=worker (по умолчанию), setSiteTid=%u msgTid=%u",
        static_cast<unsigned>(setSiteTid),
        static_cast<unsigned>(msgTid));

    GlassExplorerXamlDiag_BeginAdviseSequence(setSiteTid);
    {
        std::lock_guard lock(g_adviseMx);
        g_pendingAdviseVts = vts;
        g_pendingAdviseWatcher = watcher;
    }

    HANDLE hThread = CreateThread(nullptr, 0, AdviseWorkerThreadProc, nullptr, 0, nullptr);
    if (!hThread)
    {
        DebugLog(
            DBG_WARNING,
            L"[ExplorerCompat][TAPSite] CreateThread(Advise) failed GLE=%u — синхронный Advise (fallback)",
            static_cast<unsigned>(GetLastError()));
        {
            std::lock_guard lock(g_adviseMx);
            g_pendingAdviseVts = nullptr;
            g_pendingAdviseWatcher = nullptr;
        }

        hr = vts->AdviseVisualTreeChange(reinterpret_cast<IVisualTreeServiceCallback*>(watcher));
        if (FAILED(hr))
        {
            DebugLog(DBG_WARNING, L"[ExplorerCompat][TAPSite] AdviseVisualTreeChange hr=0x%08X", static_cast<unsigned>(hr));
            watcher->Release();
            vts->Release();
            {
                std::lock_guard lock(g_xamlDiagMx);
                if (g_xamlDiag)
                {
                    g_xamlDiag->Release();
                    g_xamlDiag = nullptr;
                }
            }
            m_site->Release();
            m_site = nullptr;
            return hr;
        }

        g_visualTreeService = vts;
        g_treeWatcher = watcher;
        GlassExplorerXamlDiag_OnAdviseCompleted(GetCurrentThreadId());
        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][TAPSite] SetSite OK, VisualTreeWatcher active (синхронный Advise fallback, tid=%u)",
            static_cast<unsigned>(GetCurrentThreadId()));

        LogShellTrayBridgeHwndsOnce();
        const HWND hMsgSync = XamlShutdownMarshal::GetMessageWindowHwnd();
        ScheduleTrayPostAdviseTimers(hMsgSync);
        return S_OK;
    }

    {
        std::lock_guard lock(g_adviseMx);
        g_hAdviseWorker = hThread;
    }
    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][TAPSite] Advise на worker-потоке, SetSite возвращает управление (setSiteTid=%u)",
        static_cast<unsigned>(setSiteTid));
    return S_OK;
}

HRESULT STDMETHODCALLTYPE XamlTapSite::GetSite(REFIID riid, void** ppvSite) noexcept
{
    if (!ppvSite)
        return E_POINTER;
    *ppvSite = nullptr;
    if (!m_site)
        return E_FAIL;
    return m_site->QueryInterface(riid, ppvSite);
}
