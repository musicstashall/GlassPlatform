#include "UiPch.h"
#include "xaml/UiTapSite.hpp"
#include "xaml/UiVisualTreeWatcher.hpp"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "config/UiRegistry.h"

#include <GlassMessages.h>
#include <RootBackdropPolicy.h>
#include <xamlOM.h>
#include <DebugLog.h>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace UICompat
{
    namespace
    {
        std::mutex g_sessionMx;
        UiVisualTreeWatcher* g_activeWatcher{ nullptr };
        IVisualTreeService3* g_activeVts{ nullptr };
        std::atomic<bool> g_setSiteBusy{ false };

        Registry::Settings g_pendingSettings{};
        std::atomic<bool> g_pendingSettingsValid{ false };

        HWND s_marshalHwnd{};

        [[nodiscard]] bool IsTapShutdownRequested() noexcept
        {
            return UICompatCom::g_shutdownDone.load(std::memory_order_acquire);
        }

        struct ShutdownPayload
        {
            UiVisualTreeWatcher* watcher{};
            IVisualTreeService3* vts{};
        };

        void RunShutdownSession(UiVisualTreeWatcher* watcher, IVisualTreeService3* vts) noexcept
        {
            if (watcher)
                watcher->RestoreAllFills();
            if (vts && watcher)
                vts->UnadviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(watcher));
            if (vts)
                vts->Release();
            if (watcher)
                watcher->Release();
        }

        void ReloadSettingsAndReapplyOnMarshalThread() noexcept
        {
            if (IsTapShutdownRequested())
                return;
            // Theme/visual style: Recapture (ClearValue → new originals), не Reapply на stale.
            const Registry::Settings settings = Registry::ReadFromRegistry();
            UiTapSite::SetPendingSettings(settings);

            UiVisualTreeWatcher* watcher = nullptr;
            {
                std::lock_guard lock(g_sessionMx);
                watcher = g_activeWatcher;
                if (watcher)
                    watcher->AddRef();
            }
            if (!watcher)
                return;

            // Не Recapture/ClearValue: ломает Binding NC/QS и originals → Stop noOrig,
            // Search Invisible при ClearValue-Recapture. Theme-recapture — TaskbarCompat.
            watcher->UpdateSettings(settings);
            if (Shared::RootBackdropPolicy::IsStartMenuHostProcess()
                || Shared::RootBackdropPolicy::IsSearchHostProcess())
            {
                if (watcher->ShellSurfacePanelCount() == 0)
                    (void)watcher->CatchUpExistingShellSurfacePanels();
            }
            else if (watcher->NcPanelCount() == 0)
            {
                (void)watcher->CatchUpExistingNcPanels();
            }
            const unsigned panels =
                (Shared::RootBackdropPolicy::IsStartMenuHostProcess()
                 || Shared::RootBackdropPolicy::IsSearchHostProcess())
                    ? watcher->ShellSurfacePanelCount()
                    : watcher->NcPanelCount();
            watcher->Release();
            DebugLog(DBG_IMPORTANT, L"[UICompat][TapSite] ReloadSettingsAndReapply OK");

            // Первый show до/без VTC Add: panels=0. Один повторный Flush (не цикл).
            if (panels == 0)
            {
                static std::atomic<bool> s_emptyFlushOnce{ false };
                bool expected = false;
                if (s_emptyFlushOnce.compare_exchange_strong(expected, true))
                {
                    if (const HMODULE wnd = GetModuleHandleW(L"WndCompat.dll"))
                    {
                        using FlushFn = void(WINAPI*)();
                        if (const auto pFlush = reinterpret_cast<FlushFn>(
                                GetProcAddress(wnd, "WndCompat_FlushPendingCoreWindows")))
                        {
                            pFlush();
                        }
                    }
                }
            }
        }

        void RefreshBrushesAfterThemeChangeOnMarshalThread() noexcept
        {
            if (IsTapShutdownRequested())
                return;
            const Registry::Settings settings = Registry::ReadFromRegistry();
            UiTapSite::SetPendingSettings(settings);

            UiVisualTreeWatcher* watcher = nullptr;
            {
                std::lock_guard lock(g_sessionMx);
                watcher = g_activeWatcher;
                if (watcher)
                    watcher->AddRef();
            }
            if (!watcher)
                return;

            watcher->RefreshBrushesAfterThemeChange();
            watcher->Release();
            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][TapSite] RefreshBrushesAfterThemeChange OK");
        }

        void UnregisterMarshalClass() noexcept;

        LRESULT CALLBACK UiXamlMarshalWndProc(HWND hwnd, UINT msg, WPARAM, LPARAM lParam) noexcept
        {
            if (msg == UICompatMsg::WM_GLASS_UI_XAML_SHUTDOWN)
            {
                auto* payload = reinterpret_cast<ShutdownPayload*>(lParam);
                if (payload)
                {
                    RunShutdownSession(payload->watcher, payload->vts);
                    delete payload;
                    CoFreeUnusedLibraries();
                }
                DestroyWindow(hwnd);
                if (s_marshalHwnd == hwnd)
                    s_marshalHwnd = nullptr;
                UnregisterMarshalClass();
                return 0;
            }
            if (msg == UICompatMsg::WM_GLASS_UI_REAPPLY_SETTINGS)
            {
                if (!IsTapShutdownRequested())
                    ReloadSettingsAndReapplyOnMarshalThread();
                return 0;
            }
            if (msg == UICompatMsg::WM_GLASS_UI_THEME_BRUSH_REFRESH)
            {
                if (!IsTapShutdownRequested())
                    RefreshBrushesAfterThemeChangeOnMarshalThread();
                return 0;
            }
            if (msg == UICompatMsg::WM_GLASS_UI_CATCHUP_NC)
            {
                if (IsTapShutdownRequested())
                    return 0;
                UiVisualTreeWatcher* watcher = nullptr;
                {
                    std::lock_guard lock(g_sessionMx);
                    watcher = g_activeWatcher;
                    if (watcher)
                        watcher->AddRef();
                }
                if (watcher)
                {
                    if (Shared::RootBackdropPolicy::IsStartMenuHostProcess()
                        || Shared::RootBackdropPolicy::IsSearchHostProcess())
                    {
                        watcher->FlushPendingShellPrepares();
                        (void)watcher->CatchUpExistingShellSurfacePanels();
                        watcher->FlushPendingShellPrepares();
                    }
                    else
                        (void)watcher->CatchUpExistingNcPanels();
                    watcher->Release();
                }
                return 0;
            }
            if (msg == UICompatMsg::WM_GLASS_UI_SHELL_PREPARE)
            {
                if (IsTapShutdownRequested())
                    return 0;
                UiVisualTreeWatcher* watcher = nullptr;
                {
                    std::lock_guard lock(g_sessionMx);
                    watcher = g_activeWatcher;
                    if (watcher)
                        watcher->AddRef();
                }
                if (watcher)
                {
                    watcher->FlushPendingShellPrepares();
                    watcher->Release();
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, 0, lParam);
        }

        [[nodiscard]] HMODULE MarshalModuleHandle() noexcept
        {
            HMODULE hMod{};
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&MarshalModuleHandle),
                    &hMod))
            {
                hMod = GetModuleHandleW(nullptr);
            }
            return hMod;
        }

        // Класс живёт в процессе оболочки дольше UICompat. После FreeLibrary его
        // lpfnWndProc указывает в выгруженный модуль: следующая загрузка (другой билд)
        // получает ERROR_CLASS_ALREADY_EXISTS и падает внутри CreateWindowExW.
        void UnregisterMarshalClass() noexcept
        {
            const HMODULE hMod = MarshalModuleHandle();
            (void)UnregisterClassW(UICompatMsg::kUiXamlShutdownWndClass, hMod);
        }

        void EnsureMarshalWindow() noexcept
        {
            // Один marshal на процесс: первый HWND (обычно NC/IXDE tid) — путь Shutdown/Restore.
            // Toast на другом CoreWindow tid раньше «крал» marshal (recreate) → Stop без
            // restore_slot в SEH (лог H31: только ShellHost RestoreAll).
            if (s_marshalHwnd && IsWindow(s_marshalHwnd))
                return;

            const HMODULE hMod = MarshalModuleHandle();

            WNDCLASSEXW existing{};
            existing.cbSize = sizeof(existing);
            const bool classExists =
                GetClassInfoExW(hMod, UICompatMsg::kUiXamlShutdownWndClass, &existing) != 0;
            const bool procStale =
                classExists && existing.lpfnWndProc != UiXamlMarshalWndProc;

            if (procStale)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[UICompat][TapSite] marshal-класс от прошлой загрузки — re-register");
                UnregisterMarshalClass();

                WNDCLASSEXW recheck{};
                recheck.cbSize = sizeof(recheck);
                if (GetClassInfoExW(hMod, UICompatMsg::kUiXamlShutdownWndClass, &recheck)
                    && recheck.lpfnWndProc != UiXamlMarshalWndProc)
                {
                    // CreateWindowExW отправит WM_NCCREATE в выгруженный код → падение
                    // процесса оболочки. Лучше остаться без marshal-окна.
                    DebugLog(
                        DBG_ERROR,
                        L"[UICompat][TapSite] stale marshal-класс не снят — окно не создаём");
                    s_marshalHwnd = nullptr;
                    return;
                }
            }

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = UiXamlMarshalWndProc;
            wc.hInstance = hMod;
            wc.lpszClassName = UICompatMsg::kUiXamlShutdownWndClass;
            (void)RegisterClassExW(&wc);

            s_marshalHwnd = CreateWindowExW(
                0,
                UICompatMsg::kUiXamlShutdownWndClass,
                L"",
                0,
                0,
                0,
                0,
                0,
                HWND_MESSAGE,
                nullptr,
                hMod,
                nullptr);
        }
    }

    UiTapSite::UiTapSite() noexcept
    {
        UICompatCom::g_objectCount.fetch_add(1, std::memory_order_relaxed);
    }

    HRESULT STDMETHODCALLTYPE UiTapSite::QueryInterface(REFIID riid, void** ppvObject) noexcept
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

    ULONG STDMETHODCALLTYPE UiTapSite::AddRef() noexcept
    {
        return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE UiTapSite::Release() noexcept
    {
        const ULONG c = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0)
        {
            UICompatCom::g_objectCount.fetch_sub(1, std::memory_order_relaxed);
            delete this;
        }
        return c;
    }

    HRESULT STDMETHODCALLTYPE UiTapSite::SetSite(IUnknown* pUnkSite) noexcept
    {
        __try
        {
            return SetSiteImpl(pUnkSite);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_setSiteBusy.store(false, std::memory_order_release);
            UICompat_ResetIxdeSchedulingAfterFailure();
            return E_FAIL;
        }
    }

    HRESULT UiTapSite::SetSiteImpl(IUnknown* pUnkSite) noexcept
    {
        if (!pUnkSite)
        {
            // IXDE снимает site без UICompat_Shutdown — иначе g_activeWatcher/panels
            // остаются с патчем, а ShutdownActiveWatcher на stop видит nullptr
            // SetSite(nullptr) раньше оставлял пустой watcher без Restore.
            // ShutdownActiveWatcher снимает TAP и восстанавливает chrome до empty.
            ShutdownActiveWatcher();
            if (m_site)
            {
                m_site->Release();
                m_site = nullptr;
            }
            return S_OK;
        }

        bool expectedBusy = false;
        if (!g_setSiteBusy.compare_exchange_strong(
                expectedBusy, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return E_UNEXPECTED;
        }

        struct BusyGuard
        {
            ~BusyGuard() noexcept
            {
                g_setSiteBusy.store(false, std::memory_order_release);
            }
        } busyGuard;

        {
            std::lock_guard lock(g_sessionMx);
            if (g_activeWatcher)
                return E_UNEXPECTED;
        }

        if (m_site)
        {
            m_site->Release();
            m_site = nullptr;
        }
        m_site = pUnkSite;
        m_site->AddRef();

        IXamlDiagnostics* diag = nullptr;
        HRESULT hr = m_site->QueryInterface(IID_IXamlDiagnostics, reinterpret_cast<void**>(&diag));
        if (FAILED(hr) || !diag)
            return hr;

        Registry::Settings settings{};
        if (g_pendingSettingsValid.load(std::memory_order_acquire))
            settings = g_pendingSettings;

        auto* watcher = new (std::nothrow) UiVisualTreeWatcher(diag, settings);
        if (!watcher)
        {
            diag->Release();
            return E_OUTOFMEMORY;
        }

        IVisualTreeService3* vts3 = nullptr;
        hr = diag->QueryInterface(IID_IVisualTreeService3, reinterpret_cast<void**>(&vts3));
        diag->Release();
        if (FAILED(hr) || !vts3)
        {
            watcher->Release();
            return hr;
        }

        // Marshal HWND — только с UI/VTC (RegisterPanel). SetSite в SEH может быть
        // на другом STA → RPC_E_WRONG_THREAD при RestoreAll.

        const bool shellSurface =
            Shared::RootBackdropPolicy::IsStartMenuHostProcess()
            || Shared::RootBackdropPolicy::IsSearchHostProcess();

        // Marshal HWND — пока мы на UI-потоке (IXDE → SetSite).
        EnsureMarshalWindow();

        if (shellSurface)
        {
            // Синхронный Advise на UI-потоке зависает в Пуске.
            // Async worker без Wait: RegisterBorder теперь store-only → Advise должен
            // вернуть S_OK (раньше E_FAIL из-за Name()/Apply на стеке VTC).
            struct AdviseJob
            {
                IVisualTreeService3* vts{};
                UiVisualTreeWatcher* watcher{};
            };
            auto* job = new (std::nothrow) AdviseJob{ vts3, watcher };
            if (!job)
            {
                UICompat_ResetIxdeSchedulingAfterFailure();
                vts3->Release();
                watcher->Release();
                return E_OUTOFMEMORY;
            }
            watcher->AddRef();
            vts3->AddRef();
            HANDLE th = CreateThread(
                nullptr,
                0,
                [](LPVOID p) -> DWORD
                {
                    auto* j = static_cast<AdviseJob*>(p);
                    const HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    const bool coInit = (hrCo == S_OK || hrCo == S_FALSE);
                    const HRESULT hr = j->vts->AdviseVisualTreeChange(j->watcher);

                    if (SUCCEEDED(hr) && !IsTapShutdownRequested())
                    {
                        {
                            std::lock_guard lock(g_sessionMx);
                            g_activeWatcher = j->watcher;
                            g_activeVts = j->vts;
                        }
                        DebugLog(DBG_IMPORTANT, L"[UICompat][TapSite] AdviseVisualTreeChange OK (async)");
                        const HWND hwnd = s_marshalHwnd;
                        if (hwnd && IsWindow(hwnd))
                        {
                            (void)PostMessageW(
                                hwnd, UICompatMsg::WM_GLASS_UI_SHELL_PREPARE, 0, 0);
                            (void)PostMessageW(
                                hwnd, UICompatMsg::WM_GLASS_UI_CATCHUP_NC, 0, 0);
                            j->watcher->AddRef();
                            HANDLE delayed = CreateThread(
                                nullptr,
                                0,
                                [](LPVOID wp) -> DWORD
                                {
                                    auto* w = static_cast<UiVisualTreeWatcher*>(wp);
                                    Sleep(500);
                                    const HWND h = s_marshalHwnd;
                                    if (h && IsWindow(h) && !IsTapShutdownRequested())
                                    {
                                        PostMessageW(h, UICompatMsg::WM_GLASS_UI_SHELL_PREPARE, 0, 0);
                                        PostMessageW(h, UICompatMsg::WM_GLASS_UI_CATCHUP_NC, 0, 0);
                                    }
                                    w->Release();
                                    return 0;
                                },
                                j->watcher,
                                0,
                                nullptr);
                            if (delayed)
                                CloseHandle(delayed);
                            else
                                j->watcher->Release();
                        }
                    }
                    else
                    {
                        if (SUCCEEDED(hr))
                        {
                            (void)j->vts->UnadviseVisualTreeChange(
                                static_cast<IVisualTreeServiceCallback*>(j->watcher));
                        }
                        else
                        {
                            // VTC мог успеть при store-only — Unadvise на всякий случай.
                            (void)j->vts->UnadviseVisualTreeChange(
                                static_cast<IVisualTreeServiceCallback*>(j->watcher));
                            UICompat_ResetIxdeSchedulingAfterFailure();
                        }
                        j->vts->Release();
                        j->watcher->Release();
                    }

                    j->vts->Release();
                    j->watcher->Release();
                    if (coInit)
                        CoUninitialize();
                    delete j;
                    return 0;
                },
                job,
                0,
                nullptr);
            if (!th)
            {
                watcher->Release();
                vts3->Release();
                delete job;
                UICompat_ResetIxdeSchedulingAfterFailure();
                vts3->Release();
                watcher->Release();
                return HRESULT_FROM_WIN32(GetLastError());
            }
            CloseHandle(th);
            return S_OK;
        }

        HRESULT hrAdvise = E_UNEXPECTED;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            hrAdvise = vts3->AdviseVisualTreeChange(watcher);
            if (SUCCEEDED(hrAdvise) || hrAdvise != E_UNEXPECTED)
                break;
            Sleep(20u + static_cast<DWORD>(attempt) * 25u);
        }

        if (FAILED(hrAdvise))
        {
            (void)vts3->UnadviseVisualTreeChange(
                static_cast<IVisualTreeServiceCallback*>(watcher));
            UICompat_ResetIxdeSchedulingAfterFailure();
            vts3->Release();
            watcher->Release();
            return hrAdvise;
        }

        {
            std::lock_guard lock(g_sessionMx);
            g_activeWatcher = watcher;
            g_activeVts = vts3;
        }

        DebugLog(DBG_IMPORTANT, L"[UICompat][TapSite] AdviseVisualTreeChange OK");
        // CatchUp в try: sync toast/VTC раньше мог кинуть и срезать delayed post
        // (лог SEH pid=52020: нет delayed_catchup / нет NC panel_reg).
        try
        {
            (void)watcher->CatchUpExistingNcPanels();
        }
        catch (...)
        {
        }
        if (s_marshalHwnd && IsWindow(s_marshalHwnd))
        {
            (void)PostMessageW(
                s_marshalHwnd, UICompatMsg::WM_GLASS_UI_CATCHUP_NC, 0, 0);
        }
        // SEH: cloaked NC — CatchUp сразу часто empty (лог: нет panel_reg).
        // Короткий повтор 50ms (было 500 — заметный late custom после uncloak).
        if (Shared::RootBackdropPolicy::IsShellExperienceHostProcess()
            && s_marshalHwnd && IsWindow(s_marshalHwnd))
        {
            watcher->AddRef();
            HANDLE delayed = CreateThread(
                nullptr,
                0,
                [](LPVOID wp) -> DWORD
                {
                    auto* w = static_cast<UiVisualTreeWatcher*>(wp);
                    Sleep(50);
                    const HWND h = s_marshalHwnd;
                    if (h && IsWindow(h) && !IsTapShutdownRequested())
                    {
                        (void)PostMessageW(
                            h, UICompatMsg::WM_GLASS_UI_CATCHUP_NC, 0, 0);
                        (void)PostMessageW(
                            h, UICompatMsg::WM_GLASS_UI_REAPPLY_SETTINGS, 0, 0);
                    }
                    w->Release();
                    return 0;
                },
                watcher,
                0,
                nullptr);
            if (delayed)
                CloseHandle(delayed);
            else
                watcher->Release();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE UiTapSite::GetSite(REFIID riid, void** ppvSite) noexcept
    {
        if (!ppvSite)
            return E_POINTER;
        *ppvSite = nullptr;
        if (!m_site)
            return E_FAIL;
        return m_site->QueryInterface(riid, ppvSite);
    }

    void UiTapSite::ShutdownActiveWatcher() noexcept
    {
        // Как TaskbarTapSite::ShutdownActiveWatcher: marshal 15s+30s, иначе fallback
        // RunShutdownSession (не бросать payload при timeout — residual Advise в SEH).
        UiVisualTreeWatcher* watcher = nullptr;
        IVisualTreeService3* vts = nullptr;
        {
            std::lock_guard lock(g_sessionMx);
            watcher = g_activeWatcher;
            vts = g_activeVts;
            g_activeWatcher = nullptr;
            g_activeVts = nullptr;
        }

        if (!watcher && !vts)
        {
            DebugLog(DBG_NORMAL, L"[UICompat][TapSite] Shutdown: nothing to stop");
            return;
        }

        DebugLog(DBG_IMPORTANT, L"[UICompat][TapSite] ShutdownActiveWatcher: BEGIN");

        const HWND hMsg = (s_marshalHwnd && IsWindow(s_marshalHwnd)) ? s_marshalHwnd : nullptr;
        DWORD wndTid = 0;
        if (hMsg)
            wndTid = GetWindowThreadProcessId(hMsg, nullptr);

        const bool needMarshal = hMsg && wndTid != 0 && GetCurrentThreadId() != wndTid;
        if (needMarshal)
        {
            auto* payload = new (std::nothrow) ShutdownPayload{ watcher, vts };
            if (!payload)
            {
                DebugLog(DBG_ERROR, L"[UICompat][TapSite] Shutdown: нет памяти — прямой вызов");
                RunShutdownSession(watcher, vts);
                CoFreeUnusedLibraries();
                DebugLog(DBG_IMPORTANT, L"[UICompat][TapSite] ShutdownActiveWatcher: END");
                return;
            }

            DWORD_PTR result = 0;
            if (SendMessageTimeoutW(
                    hMsg,
                    UICompatMsg::WM_GLASS_UI_XAML_SHUTDOWN,
                    0,
                    reinterpret_cast<LPARAM>(payload),
                    SMTO_ABORTIFHUNG,
                    15'000,
                    &result) != 0)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[UICompat][TapSite] Shutdown marshalled на tid=%u",
                    static_cast<unsigned>(wndTid));
                DebugLog(DBG_IMPORTANT, L"[UICompat][TapSite] ShutdownActiveWatcher: END");
                return;
            }

            DebugLog(
                DBG_WARNING,
                L"[UICompat][TapSite] SendMessageTimeout marshal GLE=%u — повтор 30s",
                static_cast<unsigned>(GetLastError()));
            if (SendMessageTimeoutW(
                    hMsg,
                    UICompatMsg::WM_GLASS_UI_XAML_SHUTDOWN,
                    0,
                    reinterpret_cast<LPARAM>(payload),
                    SMTO_ABORTIFHUNG,
                    30'000,
                    &result) != 0)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[UICompat][TapSite] Shutdown marshalled (повтор) tid=%u",
                    static_cast<unsigned>(wndTid));
                DebugLog(DBG_IMPORTANT, L"[UICompat][TapSite] ShutdownActiveWatcher: END");
                return;
            }

            DebugLog(
                DBG_CRITICAL,
                L"[UICompat][TapSite] marshal не удался — Restore/Unadvise с чужого потока");
            delete payload;
        }

        RunShutdownSession(watcher, vts);
        CoFreeUnusedLibraries();

        if (!needMarshal && hMsg && wndTid != 0 && GetCurrentThreadId() == wndTid)
        {
            DestroyWindow(hMsg);
            if (s_marshalHwnd == hMsg)
                s_marshalHwnd = nullptr;
            UnregisterMarshalClass();
        }

        DebugLog(DBG_IMPORTANT, L"[UICompat][TapSite] ShutdownActiveWatcher: END");
    }

    void UiTapSite::ReloadSettingsAndReapply() noexcept
    {
        // Как TaskbarTapSite: НЕ создавать marshal HWND здесь.
        // Иначе IXDE-success на STA IXDE создаёт окно раньше SetSite → Restore/Unadvise
        // идут не на UI-потоке XAML (nc_restore_all fail=N).
        if (IsTapShutdownRequested())
            return;
        if (s_marshalHwnd && IsWindow(s_marshalHwnd))
        {
            const DWORD marshalTid = GetWindowThreadProcessId(s_marshalHwnd, nullptr);
            if (marshalTid != 0 && marshalTid != GetCurrentThreadId())
            {
                DWORD_PTR sendResult = 0;
                if (SendMessageTimeoutW(
                        s_marshalHwnd,
                        UICompatMsg::WM_GLASS_UI_REAPPLY_SETTINGS,
                        0,
                        0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK,
                        3000,
                        &sendResult) == 0)
                {
                    DebugLog(
                        DBG_WARNING,
                        L"[UICompat][TapSite] SendMessageTimeout(REAPPLY) GLE=%u — inline",
                        static_cast<unsigned>(GetLastError()));
                    ReloadSettingsAndReapplyOnMarshalThread();
                }
                return;
            }
        }
        ReloadSettingsAndReapplyOnMarshalThread();
    }

    void UiTapSite::RefreshBrushesAfterThemeChange() noexcept
    {
        if (IsTapShutdownRequested())
            return;
        if (s_marshalHwnd && IsWindow(s_marshalHwnd))
        {
            const DWORD marshalTid = GetWindowThreadProcessId(s_marshalHwnd, nullptr);
            if (marshalTid != 0 && marshalTid != GetCurrentThreadId())
            {
                DWORD_PTR sendResult = 0;
                if (SendMessageTimeoutW(
                        s_marshalHwnd,
                        UICompatMsg::WM_GLASS_UI_THEME_BRUSH_REFRESH,
                        0,
                        0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK,
                        3000,
                        &sendResult) == 0)
                {
                    DebugLog(
                        DBG_WARNING,
                        L"[UICompat][TapSite] SendMessageTimeout(THEME_BRUSH) GLE=%u — inline",
                        static_cast<unsigned>(GetLastError()));
                    RefreshBrushesAfterThemeChangeOnMarshalThread();
                }
                return;
            }
        }
        RefreshBrushesAfterThemeChangeOnMarshalThread();
    }

    void UiTapSite::SetPendingSettings(Registry::Settings settings) noexcept
    {
        g_pendingSettings = settings;
        g_pendingSettingsValid.store(true, std::memory_order_release);
    }

    void UiTapSite::EnsureMarshalWindowOnUiThread() noexcept
    {
        EnsureMarshalWindow();
    }

    void UiTapSite::PostReapplyIfActive() noexcept
    {
        if (IsTapShutdownRequested())
            return;
        if (!s_marshalHwnd || !IsWindow(s_marshalHwnd))
            return;
        if (PostMessageW(
                s_marshalHwnd,
                UICompatMsg::WM_GLASS_UI_REAPPLY_SETTINGS,
                0,
                0) == 0)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][TapSite] PostMessage(REAPPLY) failed GLE=%u",
                static_cast<unsigned>(GetLastError()));
        }
    }

    void UiTapSite::PostShellPrepareIfActive() noexcept
    {
        if (IsTapShutdownRequested())
            return;
        if (!s_marshalHwnd || !IsWindow(s_marshalHwnd))
            return;
        (void)PostMessageW(
            s_marshalHwnd, UICompatMsg::WM_GLASS_UI_SHELL_PREPARE, 0, 0);
    }

    void UiTapSite::CatchUpNcOnCurrentThread() noexcept
    {
        if (IsTapShutdownRequested())
            return;
        UiVisualTreeWatcher* watcher = nullptr;
        {
            std::lock_guard lock(g_sessionMx);
            watcher = g_activeWatcher;
            if (watcher)
                watcher->AddRef();
        }
        if (!watcher)
            return;
        unsigned registered = 0;
        unsigned total = 0;
        if (Shared::RootBackdropPolicy::IsStartMenuHostProcess()
            || Shared::RootBackdropPolicy::IsSearchHostProcess())
        {
            watcher->FlushPendingShellPrepares();
            registered = watcher->CatchUpExistingShellSurfacePanels();
            watcher->FlushPendingShellPrepares();
            total = watcher->ShellSurfacePanelCount();
        }
        else
        {
            registered = watcher->CatchUpExistingNcPanels();
            total = watcher->NcPanelCount();
        }
        watcher->Release();
    }

    void UiTapSite::ReapplyOnCurrentThread() noexcept
    {
        if (IsTapShutdownRequested())
            return;
        // На UI-потоке surface hwnd — sync Apply (HasThreadAccess), без Post на marshal
        // toast tid (лог: marshalTid≠NC → кастом NC только со 2-го SHOW).
        EnsureMarshalWindow();
        ReloadSettingsAndReapplyOnMarshalThread();
    }

}
