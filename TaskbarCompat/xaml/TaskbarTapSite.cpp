#include "TaskbarPch.h"
#include "xaml/TaskbarTapSite.hpp"
#include "xaml/TaskbarVisualTreeWatcher.hpp"
#include "xaml/TaskbarBackgroundRegistry.hpp"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "config/TaskbarRegistry.h"

#include <GlassMessages.h>
#include <xamlOM.h>
#include <DebugLog.h>
#include <OpenGlassDwmPolicy.h>
#include <OpenGlassHostState.h>
#include <atomic>
#include <mutex>

namespace TaskbarCompat
{
    constexpr UINT_PTR kTrayRedrawDeferredTimerId            = 0x5482;
    constexpr UINT_PTR kTrayBridgeRescanTimerDeferredId      = 0x5483;
    constexpr UINT_PTR kTrayBridgeRescanTimerDelayedSecondId = 0x5484;
    constexpr UINT_PTR kTrayPatchReassertTimerId               = 0x5486;

    namespace
    {
        // Активная сессия (один TapSite на процесс).
        std::mutex g_sessionMx;
        TaskbarVisualTreeWatcher* g_activeWatcher{ nullptr };
        IVisualTreeService3*      g_activeVts{ nullptr };
        // Защита от реентерабельного SetSite (Sleep/Advise + marshal HWND).
        std::atomic<bool> g_setSiteBusy{ false };

        // Настройки реестра, опубликованные TaskbarCompat_Initialize.
        Registry::Settings g_pendingSettings{};
        std::atomic<bool>  g_pendingSettingsValid{ false };

        HWND s_marshalHwnd{};
        constexpr unsigned kPatchReassertRoundCount       = 12;
        unsigned           s_patchReassertRoundsRemaining = 0;

        [[nodiscard]] bool IsTapShutdownRequested() noexcept
        {
            return TaskbarCompatCom::g_shutdownDone.load(std::memory_order_acquire);
        }

        void SafeOutputDebugStringW(const wchar_t* msg) noexcept
        {
            if (!msg)
                return;
            __try
            {
                OutputDebugStringW(msg);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        void CancelMarshalTimers(HWND hwnd) noexcept
        {
            if (!hwnd || !IsWindow(hwnd))
                return;
            KillTimer(hwnd, kTrayRedrawDeferredTimerId);
            KillTimer(hwnd, kTrayBridgeRescanTimerDeferredId);
            KillTimer(hwnd, kTrayBridgeRescanTimerDelayedSecondId);
            KillTimer(hwnd, kTrayPatchReassertTimerId);
            s_patchReassertRoundsRemaining = 0;
        }

        void CancelMarshalTimersOnSta() noexcept
        {
            HWND hwnd = s_marshalHwnd;
            if (!hwnd || !IsWindow(hwnd))
                return;

            const DWORD marshalTid = GetWindowThreadProcessId(hwnd, nullptr);
            if (marshalTid != 0 && marshalTid == GetCurrentThreadId())
            {
                CancelMarshalTimers(hwnd);
                return;
            }

            (void)SendMessageW(hwnd, TaskbarCompatMsg::WM_GLASS_TASKBAR_CANCEL_TIMERS, 0, 0);
        }

        void ReloadSettingsAndReapplyOnMarshalThread() noexcept
        {
            if (IsTapShutdownRequested())
                return;
            // Theme/visual style: сброс mem+HKCU sample → Recapture (не Reapply на stale original*).
            Registry::InvalidateSystemBrushSamples();
            const Registry::Settings settings = Registry::ReadFromRegistry();
            TaskbarTapSite::SetPendingSettings(settings);

            TaskbarVisualTreeWatcher* watcher = nullptr;
            {
                std::lock_guard lock(g_sessionMx);
                watcher = g_activeWatcher;
                if (watcher)
                    watcher->AddRef();
            }
            if (!watcher)
                return;

            watcher->RecaptureAfterThemeChange(settings);
            watcher->Release();
            RedrawShellTraysAfterTaskbarXamlPatch();

            TaskbarTapSite::StartOgPatchReassertIfNeeded();

            DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][TapSite] ReloadSettingsAndReapply OK (STA, recapture)");
        }

        [[nodiscard]] bool IsOpenGlassAffectingTrayBorders() noexcept
        {
            return Shared::OpenGlassHostState::IsPipelineAffectingBorders()
                || Shared::OpenGlassDwmPolicy::IsDwmGlassInjectedPipelinePresent();
        }

        void StartOgPatchReassertTimerOnMarshalHwnd() noexcept
        {
            if (IsTapShutdownRequested())
                return;
            if (!s_marshalHwnd || !IsWindow(s_marshalHwnd))
                return;

            const Registry::Settings reassertSettings = Registry::ReadFromRegistry();
            if (!reassertSettings.patchBorderStroke || !IsOpenGlassAffectingTrayBorders())
            {
                s_patchReassertRoundsRemaining = 0;
                return;
            }

            s_patchReassertRoundsRemaining = kPatchReassertRoundCount;
            if (SetTimer(s_marshalHwnd, kTrayPatchReassertTimerId, 4000, nullptr) == 0)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][TapSite] SetTimer(PatchReassert) failed GLE=%u",
                    static_cast<unsigned>(GetLastError()));
                s_patchReassertRoundsRemaining = 0;
            }
            else
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][TapSite] Reassert scheduled (OG active)");
            }
        }

        void RetryActiveWatcherPendingRectangles() noexcept
        {
            if (IsTapShutdownRequested())
                return;
            TaskbarVisualTreeWatcher* watcher = nullptr;
            {
                std::lock_guard lock(g_sessionMx);
                watcher = g_activeWatcher;
                if (watcher)
                    watcher->AddRef();
            }
            if (!watcher)
                return;
            (void)watcher->RetryPendingRectanglePatches();
            watcher->Release();
        }

        struct ShutdownPayload
        {
            TaskbarVisualTreeWatcher* watcher{};
            IVisualTreeService3*      vts{};
        };

        struct VtcRectPayload
        {
            TaskbarVisualTreeWatcher* watcher{};
            InstanceHandle            parent{};
            InstanceHandle            element{};
            bool                      isBackgroundFill{};
        };

        [[nodiscard]] bool PostVtcRectangleAdd(
            TaskbarVisualTreeWatcher* watcher,
            std::uint64_t             parentHandle,
            std::uint64_t             elementHandle,
            bool                      isBackgroundFill) noexcept
        {
            if (IsTapShutdownRequested())
                return false;
            if (!watcher || parentHandle == 0 || elementHandle == 0)
                return false;
            if (!s_marshalHwnd || !IsWindow(s_marshalHwnd))
                return false;
            const DWORD marshalTid = GetWindowThreadProcessId(s_marshalHwnd, nullptr);
            if (marshalTid == 0)
                return false;
            if (GetCurrentThreadId() == marshalTid)
                return false;

            watcher->AddRef();
            auto* payload = new (std::nothrow) VtcRectPayload{
                watcher,
                static_cast<InstanceHandle>(parentHandle),
                static_cast<InstanceHandle>(elementHandle),
                isBackgroundFill,
            };
            if (!payload)
            {
                watcher->Release();
                return false;
            }

            if (PostMessageW(
                    s_marshalHwnd,
                    TaskbarCompatMsg::WM_GLASS_TASKBAR_VTC_APPLY_RECT,
                    0,
                    reinterpret_cast<LPARAM>(payload)) == 0)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][TapSite] PostMessage(WM_GLASS_TASKBAR_VTC_APPLY_RECT) failed GLE=%u",
                    static_cast<unsigned>(GetLastError()));
                watcher->Release();
                delete payload;
                return false;
            }

            DebugLog(
                DBG_NORMAL,
                L"[TaskbarCompat][TapSite] VTC Rectangle → PostMessage STA tid=%u (VTC tid=%u)",
                static_cast<unsigned>(marshalTid),
                static_cast<unsigned>(GetCurrentThreadId()));
            return true;
        }

        void RunShutdownSession(TaskbarVisualTreeWatcher* watcher, IVisualTreeService3* vts) noexcept
        {
            if (watcher)
            {
                watcher->RestoreAllFills();
            }
            if (vts && watcher)
            {
                vts->UnadviseVisualTreeChange(static_cast<IVisualTreeServiceCallback*>(watcher));
            }
            if (vts)
                vts->Release();
            if (watcher)
                watcher->Release();
        }

        [[nodiscard]] HMODULE MarshalModuleHandle() noexcept
        {
            HMODULE hMod{};
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(&MarshalModuleHandle),
                    &hMod))
            {
                hMod = GetModuleHandleW(nullptr);
            }
            return hMod;
        }

        // Класс живёт в explorer дольше TaskbarCompat. После FreeLibrary lpfnWndProc
        // указывает в выгруженный модуль → CreateWindowExW падает (см. UiTapSite).
        void UnregisterMarshalClass() noexcept
        {
            const HMODULE hMod = MarshalModuleHandle();
            (void)UnregisterClassW(TaskbarCompatMsg::kTaskbarXamlShutdownWndClass, hMod);
        }

        LRESULT CALLBACK TaskbarXamlMarshalWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept;

        void DestroyMarshalWindowOnOwnerThread(HWND hwnd) noexcept
        {
            if (!hwnd || !IsWindow(hwnd))
                return;
            if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId())
                return;
            DestroyWindow(hwnd);
            if (s_marshalHwnd == hwnd)
                s_marshalHwnd = nullptr;
            UnregisterMarshalClass();
        }

        void InvokeWndCompatRescanTaskbarDesktopBridges() noexcept
        {
            const HMODULE hw = GetModuleHandleW(L"WndCompat.dll");
            if (!hw)
                return;
            auto* pfn = reinterpret_cast<void(WINAPI*)()>(
                GetProcAddress(hw, "WndCompat_RescanTaskbarDesktopWindowContentBridges"));
            if (pfn)
                pfn();
        }

        void InvokeActiveWatcherReapplyPatchedBrushes() noexcept
        {
            if (IsTapShutdownRequested())
                return;
            TaskbarVisualTreeWatcher* watcher = nullptr;
            {
                std::lock_guard lock(g_sessionMx);
                watcher = g_activeWatcher;
                if (watcher)
                    watcher->AddRef();
            }
            if (!watcher)
                return;
            watcher->ReapplyPatchedBrushes();
            watcher->Release();
        }

        LRESULT CALLBACK TaskbarXamlMarshalWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
        {
            if (msg == TaskbarCompatMsg::WM_GLASS_TASKBAR_CANCEL_TIMERS)
            {
                CancelMarshalTimers(hwnd);
                return 0;
            }
            if (msg == TaskbarCompatMsg::WM_GLASS_TASKBAR_XAML_SHUTDOWN)
            {
                CancelMarshalTimers(hwnd);
                auto* payload = reinterpret_cast<ShutdownPayload*>(lParam);
                if (payload)
                {
                    RunShutdownSession(payload->watcher, payload->vts);
                    delete payload;
                    CoFreeUnusedLibraries();
                }
                else
                {
                    DebugLog(DBG_WARNING,
                        L"[TaskbarCompat][TapSite] WM_GLASS_TASKBAR_XAML_SHUTDOWN: пустой lParam");
                }
                DestroyMarshalWindowOnOwnerThread(hwnd);
                return 0;
            }
            if (msg == TaskbarCompatMsg::WM_GLASS_TASKBAR_REAPPLY_SETTINGS)
            {
                if (IsTapShutdownRequested())
                    return 0;
                ReloadSettingsAndReapplyOnMarshalThread();
                return 0;
            }
            if (msg == TaskbarCompatMsg::WM_GLASS_TASKBAR_START_OG_REASSERT)
            {
                StartOgPatchReassertTimerOnMarshalHwnd();
                return 0;
            }
            if (msg == TaskbarCompatMsg::WM_GLASS_TASKBAR_VTC_APPLY_RECT)
            {
                auto* p = reinterpret_cast<VtcRectPayload*>(lParam);
                if (!p)
                    return 0;

                TaskbarVisualTreeWatcher* const w = p->watcher;
                const InstanceHandle        par = p->parent;
                const InstanceHandle        elem = p->element;
                const bool                  isFill = p->isBackgroundFill;
                delete p;

                if (!w)
                    return 0;

                bool doApply = false;
                {
                    std::lock_guard lock(g_sessionMx);
                    if (g_activeWatcher == w)
                        doApply = true;
                }
                if (doApply)
                    (void)w->ApplyRectangleAddFromHandles(par, elem, isFill);
                else
                {
                    DebugLog(
                        DBG_NORMAL,
                        L"[TaskbarCompat][TapSite] VTC_RECT: сессия сменилась — пропуск Apply (ref Release)");
                }
                w->Release();
                return 0;
            }
            if (msg == WM_TIMER && wParam == kTrayRedrawDeferredTimerId)
            {
                if (IsTapShutdownRequested())
                    return 0;
                KillTimer(hwnd, kTrayRedrawDeferredTimerId);
                RedrawShellTraysAfterTaskbarXamlPatch();
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][TapSite] отложенная RedrawWindow Shell_*Tray* после Advise");
                return 0;
            }
            if (msg == WM_TIMER && wParam == kTrayBridgeRescanTimerDeferredId)
            {
                if (IsTapShutdownRequested())
                    return 0;
                KillTimer(hwnd, kTrayBridgeRescanTimerDeferredId);
                InvokeWndCompatRescanTaskbarDesktopBridges();
                InvokeActiveWatcherReapplyPatchedBrushes();
                RetryActiveWatcherPendingRectangles();
                RedrawShellTraysAfterTaskbarXamlPatch();
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][TapSite] отложенный Rescan + reapply + redraw (~2.8s)");
                return 0;
            }
            if (msg == WM_TIMER && wParam == kTrayBridgeRescanTimerDelayedSecondId)
            {
                if (IsTapShutdownRequested())
                    return 0;
                KillTimer(hwnd, kTrayBridgeRescanTimerDelayedSecondId);
                InvokeWndCompatRescanTaskbarDesktopBridges();
                InvokeActiveWatcherReapplyPatchedBrushes();
                RetryActiveWatcherPendingRectangles();
                RedrawShellTraysAfterTaskbarXamlPatch();
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][TapSite] второй Rescan + reapply + redraw (~5.2s)");
                return 0;
            }
            if (msg == WM_TIMER && wParam == kTrayPatchReassertTimerId)
            {
                if (IsTapShutdownRequested())
                    return 0;
                KillTimer(hwnd, kTrayPatchReassertTimerId);

                const Registry::Settings settings = Registry::ReadFromRegistry();
                if (!settings.patchBorderStroke)
                {
                    s_patchReassertRoundsRemaining = 0;
                    return 0;
                }

                InvokeActiveWatcherReapplyPatchedBrushes();
                RedrawShellTraysAfterTaskbarXamlPatch();

                const unsigned remainingBeforeTick = s_patchReassertRoundsRemaining;
                if (remainingBeforeTick == 0)
                    return 0;

                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][TapSite] Reassert XAML brush: проход %u/%u",
                    kPatchReassertRoundCount - remainingBeforeTick + 1,
                    kPatchReassertRoundCount);

                --s_patchReassertRoundsRemaining;
                if (s_patchReassertRoundsRemaining > 0)
                {
                    if (SetTimer(hwnd, kTrayPatchReassertTimerId, 4000, nullptr) == 0)
                    {
                        DebugLog(
                            DBG_WARNING,
                            L"[TaskbarCompat][TapSite] SetTimer(ReassertPatches) failed GLE=%u",
                            static_cast<unsigned>(GetLastError()));
                        s_patchReassertRoundsRemaining = 0;
                    }
                }
                return 0;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        void EnsureTaskbarMarshalWindow() noexcept
        {
            if (s_marshalHwnd && IsWindow(s_marshalHwnd))
                return;

            const HMODULE hMod = MarshalModuleHandle();

            WNDCLASSEXW existing{};
            existing.cbSize = sizeof(existing);
            const bool classExists =
                GetClassInfoExW(
                    hMod, TaskbarCompatMsg::kTaskbarXamlShutdownWndClass, &existing)
                != 0;
            const bool procStale =
                classExists && existing.lpfnWndProc != TaskbarXamlMarshalWndProc;

            if (procStale)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][TapSite] marshal-класс от прошлой загрузки — re-register");
                UnregisterMarshalClass();

                WNDCLASSEXW recheck{};
                recheck.cbSize = sizeof(recheck);
                if (GetClassInfoExW(
                        hMod, TaskbarCompatMsg::kTaskbarXamlShutdownWndClass, &recheck)
                    && recheck.lpfnWndProc != TaskbarXamlMarshalWndProc)
                {
                    // CreateWindowExW → WM_NCCREATE в выгруженный код → краш explorer.
                    DebugLog(
                        DBG_ERROR,
                        L"[TaskbarCompat][TapSite] stale marshal-класс не снят — окно не создаём");
                    s_marshalHwnd = nullptr;
                    return;
                }
            }

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = TaskbarXamlMarshalWndProc;
            wc.hInstance = hMod;
            wc.lpszClassName = TaskbarCompatMsg::kTaskbarXamlShutdownWndClass;
            if (RegisterClassExW(&wc) == 0)
            {
                const DWORD gle = GetLastError();
                if (gle != ERROR_CLASS_ALREADY_EXISTS)
                {
                    DebugLog(
                        DBG_WARNING,
                        L"[TaskbarCompat][TapSite] RegisterClassEx/TaskbarMarshal failed GLE=%u",
                        static_cast<unsigned>(gle));
                    return;
                }
            }

            const HWND created = CreateWindowExW(
                0,
                TaskbarCompatMsg::kTaskbarXamlShutdownWndClass,
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
            if (!created)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][TapSite] CreateWindowEx/TaskbarMarshal failed GLE=%u",
                    static_cast<unsigned>(GetLastError()));
                return;
            }

            s_marshalHwnd = created;
            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][TapSite] HWND_MESSAGE marshal hwnd=0x%p tid=%u",
                created,
                static_cast<unsigned>(GetCurrentThreadId()));
        }
    }

    void TaskbarTapSite::SetPendingSettings(Registry::Settings settings) noexcept
    {
        g_pendingSettings = settings;
        g_pendingSettingsValid.store(true, std::memory_order_release);
    }

    TaskbarTapSite::TaskbarTapSite() noexcept
    {
        TaskbarCompatCom::g_objectCount.fetch_add(1, std::memory_order_relaxed);
    }

    HRESULT STDMETHODCALLTYPE TaskbarTapSite::QueryInterface(REFIID riid, void** ppvObject) noexcept
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

    ULONG STDMETHODCALLTYPE TaskbarTapSite::AddRef() noexcept
    {
        return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE TaskbarTapSite::Release() noexcept
    {
        const ULONG c = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0)
        {
            TaskbarCompatCom::g_objectCount.fetch_sub(1, std::memory_order_relaxed);
            delete this;
        }
        return c;
    }

    HRESULT STDMETHODCALLTYPE TaskbarTapSite::SetSite(IUnknown* pUnkSite) noexcept
    {
        // SEH снаружи Impl: AV в Advise/QI/логах не должны валить explorer.
        // В handler — только OutputDebugString (DebugLog/SendLog сами могут AV при мёртвом pipe).
        __try
        {
            return SetSiteImpl(pUnkSite);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            const DWORD code = GetExceptionCode();
            wchar_t synBuf[192]{};
            swprintf_s(
                synBuf,
                L"[TaskbarCompat][TapSite] SEH in SetSite code=0x%08X tid=%u\n",
                static_cast<unsigned>(code),
                static_cast<unsigned>(GetCurrentThreadId()));
            OutputDebugStringW(synBuf);
            g_setSiteBusy.store(false, std::memory_order_release);
            TaskbarCompat_ResetIxdeSchedulingAfterFailure();
            return E_FAIL;
        }
    }

    HRESULT TaskbarTapSite::SetSiteImpl(IUnknown* pUnkSite) noexcept
    {
        // Entry без GlassLogClient: при старте explorer pipe/логер иногда даёт AV (дамп @ DebugLog).
        {
            wchar_t synBuf[160]{};
            swprintf_s(
                synBuf,
                L"[TaskbarCompat][TapSite] SetSite: site=%p tid=%u\n",
                static_cast<void*>(pUnkSite),
                static_cast<unsigned>(GetCurrentThreadId()));
            SafeOutputDebugStringW(synBuf);
        }

        if (!pUnkSite)
        {
            // Cleanup: SetSite(nullptr) — Unadvise через ShutdownActiveWatcher().
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
            // Не DebugLog: на старте explorer pipe/логер даёт AV (дамп @ return E_UNEXPECTED).
            SafeOutputDebugStringW(L"[TaskbarCompat][TapSite] SetSite re-enter — отказ\n");
            return E_UNEXPECTED;
        }

        struct BusyGuard
        {
            ~BusyGuard() noexcept
            {
                g_setSiteBusy.store(false, std::memory_order_release);
            }
        } busyGuard;

        // Только одна активная сессия (как в TranslucentTB).
        {
            std::lock_guard lock(g_sessionMx);
            if (g_activeWatcher)
            {
                // Не DebugLog — тот же AV при отказе вторичной сессии (дамп line ~615).
                SafeOutputDebugStringW(
                    L"[TaskbarCompat][TapSite] active watcher already exists — отказ\n");
                return E_UNEXPECTED;
            }
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
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][TapSite] QueryInterface(IXamlDiagnostics) failed hr=0x%08X",
                static_cast<unsigned>(hr));
            return hr;
        }

        Registry::Settings settings{};
        if (g_pendingSettingsValid.load(std::memory_order_acquire))
            settings = g_pendingSettings;

        auto* watcher = new (std::nothrow) TaskbarVisualTreeWatcher(diag, settings);
        if (!watcher)
        {
            diag->Release();
            return E_OUTOFMEMORY;
        }

        IVisualTreeService3* vts3 = nullptr;
        hr = diag->QueryInterface(IID_IVisualTreeService3, reinterpret_cast<void**>(&vts3));
        diag->Release();
        diag = nullptr;
        if (FAILED(hr) || !vts3)
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][TapSite] QueryInterface(IVisualTreeService3) failed hr=0x%08X",
                static_cast<unsigned>(hr));
            watcher->Release();
            return hr;
        }

        // HWND_MESSAGE до Advise (PostMessage VTC). g_active* — только после успешного Advise,
        // иначе Sleep/ретраи + marshal дают гонку с Reload/таймерами на полуживом watcher.
        EnsureTaskbarMarshalWindow();

        constexpr int kAdviseMaxAttempts = 8;
        HRESULT       hrAdvise           = E_UNEXPECTED;

        for (int attempt = 0; attempt < kAdviseMaxAttempts; ++attempt)
        {
            hrAdvise = vts3->AdviseVisualTreeChange(watcher);
            if (SUCCEEDED(hrAdvise))
                break;
            if (hrAdvise != E_UNEXPECTED)
                break;

            const DWORD backoffMs = 20u + static_cast<DWORD>(attempt) * 25u;
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][TapSite] AdviseVisualTreeChange E_UNEXPECTED attempt=%d/%d backoff=%ums tid=%u",
                attempt + 1,
                kAdviseMaxAttempts,
                static_cast<unsigned>(backoffMs),
                static_cast<unsigned>(GetCurrentThreadId()));
            Sleep(backoffMs);
        }

        if (FAILED(hrAdvise))
        {
            DebugLog(
                DBG_CRITICAL,
                L"[TaskbarCompat][TapSite] AdviseVisualTreeChange failed после ретраев hr=0x%08X",
                static_cast<unsigned>(hrAdvise));
            TaskbarCompat_ResetIxdeSchedulingAfterFailure();
            vts3->Release();
            watcher->Release();
            return hrAdvise;
        }

        {
            std::lock_guard lock(g_sessionMx);
            if (g_activeWatcher)
            {
                // Не DebugLog — AV pipe при старте (как на отказе вторичной сессии).
                SafeOutputDebugStringW(
                    L"[TaskbarCompat][TapSite] race: active watcher during Advise — Unadvise\n");
                (void)vts3->UnadviseVisualTreeChange(
                    static_cast<IVisualTreeServiceCallback*>(watcher));
                vts3->Release();
                watcher->Release();
                return E_UNEXPECTED;
            }
            g_activeWatcher = watcher;
            g_activeVts = vts3;
        }

        DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][TapSite] AdviseVisualTreeChange OK");

        if (s_marshalHwnd && IsWindow(s_marshalHwnd))
        {
            KillTimer(s_marshalHwnd, kTrayRedrawDeferredTimerId);
            KillTimer(s_marshalHwnd, kTrayBridgeRescanTimerDeferredId);
            KillTimer(s_marshalHwnd, kTrayBridgeRescanTimerDelayedSecondId);
            KillTimer(s_marshalHwnd, kTrayPatchReassertTimerId);
            if (SetTimer(s_marshalHwnd, kTrayRedrawDeferredTimerId, 1200, nullptr) == 0)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][TapSite] SetTimer(TrayRedrawDeferred) failed GLE=%u",
                    static_cast<unsigned>(GetLastError()));
            }
            if (SetTimer(s_marshalHwnd, kTrayBridgeRescanTimerDeferredId, 2800, nullptr) == 0)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][TapSite] SetTimer(TrayBridgeRescanDeferred) failed GLE=%u",
                    static_cast<unsigned>(GetLastError()));
            }
            if (SetTimer(s_marshalHwnd, kTrayBridgeRescanTimerDelayedSecondId, 5200, nullptr) == 0)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][TapSite] SetTimer(TrayBridgeRescanDelayed2) failed GLE=%u",
                    static_cast<unsigned>(GetLastError()));
            }

            StartOgPatchReassertTimerOnMarshalHwnd();
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE TaskbarTapSite::GetSite(REFIID riid, void** ppvSite) noexcept
    {
        if (!ppvSite)
            return E_POINTER;
        *ppvSite = nullptr;
        if (!m_site)
            return E_FAIL;
        return m_site->QueryInterface(riid, ppvSite);
    }

    void TaskbarTapSite::ShutdownActiveWatcher() noexcept
    {
        CancelMarshalTimersOnSta();

        TaskbarVisualTreeWatcher* watcher = nullptr;
        IVisualTreeService3*      vts     = nullptr;
        {
            std::lock_guard lock(g_sessionMx);
            watcher = g_activeWatcher;
            vts     = g_activeVts;
            g_activeWatcher = nullptr;
            g_activeVts = nullptr;
        }

        if (!watcher && !vts)
        {
            DebugLog(DBG_NORMAL, L"[TaskbarCompat][TapSite] Shutdown: nothing to stop");
            return;
        }

        DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][TapSite] ShutdownActiveWatcher: BEGIN");

        const HWND hMsg = (s_marshalHwnd && IsWindow(s_marshalHwnd)) ? s_marshalHwnd : nullptr;
        DWORD      wndTid = 0;
        if (hMsg)
            wndTid = GetWindowThreadProcessId(hMsg, nullptr);

        const bool needMarshal = hMsg && wndTid != 0 && GetCurrentThreadId() != wndTid;
        if (needMarshal)
        {
            auto* payload = new (std::nothrow) ShutdownPayload{ watcher, vts };
            if (!payload)
            {
                DebugLog(DBG_ERROR, L"[TaskbarCompat][TapSite] Shutdown: нет памяти для payload — прямой вызов");
                RunShutdownSession(watcher, vts);
                CoFreeUnusedLibraries();
                DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][TapSite] ShutdownActiveWatcher: END");
                return;
            }

            DWORD_PTR result = 0;
            constexpr UINT kTimeoutMs = 15'000;
            if (SendMessageTimeoutW(
                    hMsg,
                    TaskbarCompatMsg::WM_GLASS_TASKBAR_XAML_SHUTDOWN,
                    0,
                    reinterpret_cast<LPARAM>(payload),
                    SMTO_ABORTIFHUNG,
                    kTimeoutMs,
                    &result) != 0)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[TaskbarCompat][TapSite] Shutdown marshalled на tid=%u",
                    static_cast<unsigned>(wndTid));
                DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][TapSite] ShutdownActiveWatcher: END");
                return;
            }

            DebugLog(DBG_WARNING,
                L"[TaskbarCompat][TapSite] SendMessageTimeout marshal GLE=%u — повтор 30s",
                static_cast<unsigned>(GetLastError()));
            if (SendMessageTimeoutW(
                    hMsg,
                    TaskbarCompatMsg::WM_GLASS_TASKBAR_XAML_SHUTDOWN,
                    0,
                    reinterpret_cast<LPARAM>(payload),
                    SMTO_ABORTIFHUNG,
                    30'000,
                    &result) != 0)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[TaskbarCompat][TapSite] Shutdown marshalled (повтор) tid=%u",
                    static_cast<unsigned>(wndTid));
                DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][TapSite] ShutdownActiveWatcher: END");
                return;
            }

            DebugLog(DBG_CRITICAL,
                L"[TaskbarCompat][TapSite] marshal не удался — Restore/Unadvise с чужого потока");
            delete payload;
        }

        CancelMarshalTimersOnSta();
        RunShutdownSession(watcher, vts);
        CoFreeUnusedLibraries();

        if (!needMarshal && hMsg && wndTid != 0 && GetCurrentThreadId() == wndTid)
            DestroyMarshalWindowOnOwnerThread(hMsg);

        DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][TapSite] ShutdownActiveWatcher: END");
    }

    bool TaskbarTapSite_PostVtcRectangleAdd(
        TaskbarVisualTreeWatcher* watcher,
        std::uint64_t             parentHandle,
        std::uint64_t             elementHandle,
        bool                      isBackgroundFill) noexcept
    {
        return PostVtcRectangleAdd(watcher, parentHandle, elementHandle, isBackgroundFill);
    }

    void TaskbarTapSite::ReapplyActiveWatcherXamlBackgroundPatches() noexcept
    {
        InvokeActiveWatcherReapplyPatchedBrushes();
        RetryActiveWatcherPendingRectangles();
    }

    void TaskbarTapSite::ReloadSettingsAndReapply() noexcept
    {
        if (IsTapShutdownRequested())
            return;
        if (s_marshalHwnd && IsWindow(s_marshalHwnd))
        {
            const DWORD marshalTid = GetWindowThreadProcessId(s_marshalHwnd, nullptr);
            if (marshalTid != 0 && marshalTid != GetCurrentThreadId())
            {
                DWORD_PTR sendResult = 0;
                const LRESULT sm = SendMessageTimeoutW(
                    s_marshalHwnd,
                    TaskbarCompatMsg::WM_GLASS_TASKBAR_REAPPLY_SETTINGS,
                    0,
                    0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                    3000,
                    &sendResult);
                if (sm == 0)
                {
                    DebugLog(
                        DBG_WARNING,
                        L"[TaskbarCompat][TapSite] SendMessageTimeout(REAPPLY_SETTINGS) failed GLE=%u — inline",
                        static_cast<unsigned>(GetLastError()));
                    ReloadSettingsAndReapplyOnMarshalThread();
                }
                return;
            }
        }

        ReloadSettingsAndReapplyOnMarshalThread();
    }

    void TaskbarTapSite::StartOgPatchReassertIfNeeded() noexcept
    {
        if (IsTapShutdownRequested())
            return;
        if (!s_marshalHwnd || !IsWindow(s_marshalHwnd))
            return;

        const DWORD marshalTid = GetWindowThreadProcessId(s_marshalHwnd, nullptr);
        if (marshalTid != 0 && marshalTid != GetCurrentThreadId())
        {
            if (PostMessageW(
                    s_marshalHwnd,
                    TaskbarCompatMsg::WM_GLASS_TASKBAR_START_OG_REASSERT,
                    0,
                    0) == 0)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][TapSite] PostMessage(OG reassert) failed GLE=%u",
                    static_cast<unsigned>(GetLastError()));
            }
            return;
        }

        StartOgPatchReassertTimerOnMarshalHwnd();
    }
}
