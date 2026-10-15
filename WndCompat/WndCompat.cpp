#include "WndPch.h"
#include "WndCompat.h"
#include <GlassMessages.h>
#include <GlassControlKind.h>
#include "Tracker/WinEventHandler.h"
#include "UiDispatcher.h"

#include <windows.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <BitmapUtils.h>
#include "Popup/Menu/MenuSubclass.h"
#include "MenuEventProvider.h"
#include "DropDownEventProvider.h"
#include "MenuContext.h"
#include <RootEventProvider.h>
#include "Root/RootSubclass.h"
#include "Tracker/WinEventHandler.h"
#include <DirectUIEventProvider.h>
#include <RebarEventProvider.h>
#include "TooltipEventProvider.h"
#include "WinUiBridge/WinUiDesktopBridgeHost.h"
#include "WinUiBridge/WinUiContentBridgeHost.h"
#include "WinUiBridge/CoreWindowHost.h"
#include "Root/TaskbarHost.h"
#include <Helpers/ThemeHelper.h>
namespace WndCompat
{
    // ============================================
    // Глобальное состояние
    // ============================================

    static std::atomic<bool>  g_initialized{ false };
    static std::atomic<bool>  g_shuttingDown{ false };
    static std::atomic<bool>  g_shutdownComplete{ false };

    static HANDLE             g_shutdownEvent = nullptr;
    static std::atomic<DWORD> g_guiThreadId{ 0 };

    static HHOOK              g_getMsgHook = nullptr;
    static HWND               g_controlWindow = nullptr;

    HWND WndCompat::GetControlWindow()
    {
        return g_controlWindow;
    }

    static const wchar_t* kControlClassName = L"WndCompat.ControlWindow";

    // ============================================
    // Вперёд объявление
    // ============================================

    namespace Internal
    {
        void ExecuteShutdownCore();
        void EnsureControlWindow();
    }

    // ============================================
    // Control window
    // ============================================

    static LRESULT CALLBACK ControlWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_GLASS_DWMCOLORIZATION_DISPATCH)
        {
            if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
                return 0;

            WPARAM maxWave = wParam;
            MSG peek{};
            HWND const filterHwnd = hWnd;
            while (PeekMessageW(
                &peek,
                filterHwnd,
                WM_GLASS_DWMCOLORIZATION_DISPATCH,
                WM_GLASS_DWMCOLORIZATION_DISPATCH,
                PM_REMOVE))
            {
                if (peek.wParam > maxWave)
                    maxWave = peek.wParam;
            }

            if (const Root::RootEventProvider* const hp = Root::GetRootEventProvider())
            {
                if (hp->OnRootDwmColorizationChanged)
                    hp->OnRootDwmColorizationChanged(nullptr, maxWave, WndCompat::kGlassLiveRootScopeInfer);
            }
            return 0;
        }

        if (msg == WM_APP_GUI_SHUTDOWN)
        {
            DebugLog(L"[WndCompat][Core] ControlWndProc: WM_APP_GUI_SHUTDOWN");

            // 1) Снять хуки пока message pump жив
            Internal::ExecuteShutdownCore(); // внутри: UnhookWindowsHookEx + UnhookWinEvent

            // 2) Дать системе “доделать” снятие INCONTEXT (очистить внутренние сообщения)
            MSG m;
            for (int i = 0; i < 64; ++i)
            {
                while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&m);
                    DispatchMessageW(&m);
                }
                Sleep(0);
            }

            // 3) Теперь можно завершать loop
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }

    namespace Internal
    {
        void EnsureControlWindow()
        {
            if (g_controlWindow && IsWindow(g_controlWindow))
                return;

            HINSTANCE hInst = reinterpret_cast<HINSTANCE>(
                GetModuleHandleW(L"WndCompat.dll"));

            static std::once_flag s_regFlag;
            std::call_once(s_regFlag, [hInst]()
                {
                    WNDCLASSEXW wc{};
                    wc.cbSize = sizeof(wc);
                    wc.lpfnWndProc = ControlWndProc;
                    wc.hInstance = hInst;
                    wc.lpszClassName = kControlClassName;

                    if (!RegisterClassExW(&wc))
                    {
                        DWORD gle = GetLastError();
                        DebugLog(L"[WndCompat][Core] Failed to register control window class, GLE=%u", gle);
                    }
                    else
                    {
                        DebugLog(L"[WndCompat][Core] Control window class registered");
                    }
                });

            g_controlWindow = CreateWindowExW(
                0,
                kControlClassName,
                L"",
                0,
                0, 0, 0, 0,
                HWND_MESSAGE,
                nullptr,
                hInst,
                nullptr
            );

            if (!g_controlWindow)
            {
                DWORD gle = GetLastError();
                DebugLog(L"[WndCompat][ERROR] Failed to create control window, GLE=%u", gle);
            }
            else
            {
                DebugLog(L"[WndCompat][Core] Control window created: 0x%p",
                    g_controlWindow);
            }
        }
    }

    // ============================================
    // WH_GETMESSAGE hook
    // ============================================

    static void UninstallGuiMessageHook();

    static LRESULT CALLBACK GetMsgHookProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code >= 0)
        {
            MSG* pMsg = reinterpret_cast<MSG*>(lParam);
            if (pMsg)
            {
                const UINT   msg = pMsg->message;
                const WPARAM wp = pMsg->wParam;
                const LPARAM lp = pMsg->lParam;

                if (msg == WM_APP_GUI_SHUTDOWN)
                {
                    DebugLog(L"[WndCompat][GetMsgHook] WM_APP_GUI_SHUTDOWN");

                    Internal::ExecuteShutdownCore();
                    pMsg->message = WM_NULL;
                }
                else
                {
                    UiDispatcher::ProcessWindowsMessage(msg, wp, lp);
                }
            }
        }

        return CallNextHookEx(g_getMsgHook, code, wParam, lParam);
    }

    static void InstallGuiMessageHook(DWORD guiThreadId)
    {
        if (g_getMsgHook)
        {
            DebugLog(L"[WndCompat][Core] GetMsg hook already installed");
            return;
        }

        DebugLog(L"[WndCompat][Core] Installing WH_GETMESSAGE hook for GUI thread");

        g_getMsgHook = SetWindowsHookExW(
            WH_GETMESSAGE,
            GetMsgHookProc,
            nullptr,
            guiThreadId
        );

        if (!g_getMsgHook)
        {
            DWORD err = GetLastError();
            DebugLog(L"[WndCompat][ERROR] Failed to install WH_GETMESSAGE hook, GLE=%u", err);
        }
        else
        {
            DebugLog(L"[WndCompat][Core] WH_GETMESSAGE hook installed: 0x%p", g_getMsgHook);
        }
    }

    static void UninstallGuiMessageHook()
    {
        if (!g_getMsgHook)
            return;

        DebugLog(L"[WndCompat][Core] Uninstalling WH_GETMESSAGE hook: 0x%p", g_getMsgHook);

        if (!UnhookWindowsHookEx(g_getMsgHook))
        {
            DWORD err = GetLastError();
            if (err == ERROR_INVALID_HANDLE)
            {
                DebugLog(L"[WndCompat][Core] UnhookWindowsHookEx: handle already invalid (GLE=%u), ignoring", err);
            }
            else
            {
                DebugLog(L"[WndCompat][ERROR] UnhookWindowsHookEx failed, GLE=%u", err);
            }
        }
        else
        {
            DebugLog(L"[WndCompat][Core] WH_GETMESSAGE hook removed");
        }

        g_getMsgHook = nullptr;
    }

    // ============================================
    // Shutdown core
    // ============================================

    namespace Internal
    {
        void ExecuteShutdownCore()
        {
            bool expected = false;
            if (!g_shuttingDown.compare_exchange_strong(expected, true))
            {
                DebugLog(L"[WndCompat][Core] ExecuteShutdownCore: already in progress or completed");
                return;
            }

            DebugLog(L"[WndCompat][Core] ExecuteShutdownCore: BEGIN");

            // 1) Снимаем WH_GETMESSAGE
            UninstallGuiMessageHook();

            // 2) Снимаем WinEventHook (включая *_DetachAll — провайдеры GlassCore ещё живы)
            WinEventHook_Shutdown();

            // 3) Обнуляем провайдеры GlassCore — после DetachAll, но ДО FreeLibrary(GlassCore).
            //    Все *_DetachAll уже отработали и вызвали OnRootDetach/OnRootHooksDetach.
            //    Теперь указатели на функции GlassCore больше не нужны.
            //    Без этого FreeLibrary(GlassCore) оставляет висячие указатели в провайдерах,
            //    и любой вызов (например, из оставшегося субкласса) → use-after-free → краш.
            WndCompat::Root::SetRootEventProvider({});
            WndCompat::DropDown::SetDropDownEventProvider({});
            WndCompat::Menu::SetMenuEventProvider({});
            WndCompat::DirectUI::SetDirectUIEventProvider({});
            WndCompat::Rebar::SetRebarEventProvider({});
            WndCompat::Tooltip::SetTooltipEventProvider({});

            DebugLog(L"[WndCompat][Core] ExecuteShutdownCore: providers cleared");

            // 4) Отмечаем завершение
            g_shutdownComplete.store(true, std::memory_order_release);

            if (g_shutdownEvent)
                SetEvent(g_shutdownEvent);

            DebugLog(L"[WndCompat][Core] ExecuteShutdownCore: END");
        }
    }

    // ============================================
    // Публичный API
    // ============================================

    void Initialize(Shared::InjectionMode mode)
    {
        bool expected = false;
        if (!g_initialized.compare_exchange_strong(expected, true))
        {
            DebugLog(L"[WndCompat][Core] Already initialized");
            return;
        }

        DWORD tid = GetCurrentThreadId();
        g_guiThreadId.store(tid, std::memory_order_release);

        DebugLog(L"[WndCompat][Core] Initialize: BEGIN, mode=%u",
            static_cast<DWORD>(mode));

        Shared::ThemeHelper::TryEnableProcessDarkMode();

        g_shuttingDown.store(false, std::memory_order_release);
        g_shutdownComplete.store(false, std::memory_order_release);

        g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!g_shutdownEvent)
        {
            DWORD err = GetLastError();
            DebugLog(L"[WndCompat][Core] WARNING: Failed to create shutdown event: %u", err);
        }

        UiDispatcher::Initialize();

        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (hCore)
        {
            using PfnGetProviders = BOOL(WINAPI*)(GlassCoreProviders*);
            auto pfnGetProviders = reinterpret_cast<PfnGetProviders>(
                GetProcAddress(hCore, "GlassCore_GetProviders"));

            if (pfnGetProviders)
            {
                GlassCoreProviders providers{};
                if (pfnGetProviders(&providers))
                {
                    WndCompat::Menu::SetMenuEventProvider(providers.menu);
                    WndCompat::DropDown::SetDropDownEventProvider(providers.dropdown);
                    WndCompat::Root::SetRootEventProvider(providers.root);
                    WndCompat::DirectUI::SetDirectUIEventProvider(providers.directui);
                    WndCompat::Rebar::SetRebarEventProvider(providers.rebar);
                    WndCompat::Tooltip::SetTooltipEventProvider(providers.tooltip);
                }
            }

            using RegisterRootsApplierFn = void(WINAPI*)(void(WINAPI*)() noexcept);
            if (const auto pfnRegister = reinterpret_cast<RegisterRootsApplierFn>(
                    GetProcAddress(hCore, "GlassCore_RegisterTrackedRootsBackdropApplier")))
            {
                pfnRegister(&WndCompat::ApplyBackdropPolicyToTrackedRoots);
            }

            using RegisterRootIsAttachedFn = void(WINAPI*)(bool(WINAPI*)(HWND) noexcept);
            if (const auto pfnRootAttached = reinterpret_cast<RegisterRootIsAttachedFn>(
                    GetProcAddress(hCore, "GlassCore_RegisterRootIsAttachedQuery")))
            {
                pfnRootAttached(&WndCompat::Root_IsAttached);
            }

            using RegisterRootAttachRequestFn = void(WINAPI*)(void(WINAPI*)(HWND) noexcept);
            if (const auto pfnRootAttachRequest = reinterpret_cast<RegisterRootAttachRequestFn>(
                    GetProcAddress(hCore, "GlassCore_RegisterRootAttachRequest")))
            {
                pfnRootAttachRequest(&WndCompat::RequestRootAttachForHwnd);
            }
        }
        else
        {
            DebugLog(
                DBG_WARNING,
                L"[WndCompat][Core] Initialize: GlassCore.dll ещё нет — providers later; "
                L"WinEventHook_Initialize всё равно обязателен (иначе Stop без DetachAll)");
        }

        InstallGuiMessageHook(tid);

        // mode передаётся в WinEventHook — там определяет поведение WinEventProc.
        // Всегда: иначе HandleWinEvent навешивает субклассы, а Shutdown early-return без Detach.
        if (!WinEventHook_Initialize(mode))
        {
            DebugLog(L"[WndCompat][ERROR] WinEventHook_Initialize failed");
        }

        Internal::EnsureControlWindow();

        DebugLog(L"[WndCompat][Core] Initialize: END");
    }

    void FinalizeGlassCoreRootActivationVisualUpdate(HWND hRoot, BOOL ncActivateFromWmNcActivate) noexcept
    {
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND, BOOL);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_FinalizeRootActivationVisualUpdate"));
        if (pfn)
            pfn(hRoot, ncActivateFromWmNcActivate);
    }

    void RemoveGlassCoreThemePaintRootNcState(HWND hRoot) noexcept
    {
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RemoveThemePaintRootNcState"));
        if (pfn)
            pfn(hRoot);
    }

    void RemoveTextGlowClientsForRootWithGlassCore(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RemoveTextGlowClientsForRoot"));
        if (pfn)
            pfn(hRoot);
    }

    void RemoveTextInkClientsForRootWithGlassCore(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RemoveTextInkClientsForRoot"));
        if (pfn)
            pfn(hRoot);
    }

    void RegisterThemedChromeClientWithGlassCore(HWND hControl) noexcept
    {
        if (!hControl)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        DebugLog(L"[WndCompat][Core] RegisterThemedChromeClientWithGlassCore 0x%p", hControl);
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RegisterThemedChromeClient"));
        if (pfn)
            pfn(hControl);
    }

    void UnregisterThemedChromeClientWithGlassCore(HWND hControl) noexcept
    {
        if (!hControl)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_UnregisterThemedChromeClient"));
        if (pfn)
            pfn(hControl);
    }

    void RebindThemedChromeClientsAfterWinFormsWindowMarkedWithGlassCore(HWND hMarkedWinFormsWindow) noexcept
    {
        if (!hMarkedWinFormsWindow)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RebindThemedChromeClientsAfterWinFormsWindowMarked"));
        if (pfn)
            pfn(hMarkedWinFormsWindow);
    }

    void ScheduleThemedChromeRemigrateAfterWinFormsWindowMarked(HWND hMarkedWinFormsWindow) noexcept
    {
        if (!hMarkedWinFormsWindow || !IsWindow(hMarkedWinFormsWindow))
            return;
        if (GlassShared::GlassGetWindowKind(hMarkedWinFormsWindow)
            != GlassShared::GlassControlKind::WinForms_Window)
        {
            return;
        }
        HWND gaRoot = GetAncestor(hMarkedWinFormsWindow, GA_ROOT);
        if (!gaRoot)
            gaRoot = hMarkedWinFormsWindow;
        if (!IsWindow(gaRoot))
            return;
        if (!PostMessageW(
                gaRoot,
                WndCompat::WM_GLASS_REMIGRATE_THEMED_CHROME,
                reinterpret_cast<WPARAM>(hMarkedWinFormsWindow),
                0))
        {
            DebugLog(DBG_WARNING,
                L"[WndCompat][Core] ScheduleThemedChromeRemigrate PostMessage failed hMarked=0x%p gaRoot=0x%p GLE=%u",
                hMarkedWinFormsWindow,
                gaRoot,
                GetLastError());
        }
    }

    void RunDeferredThemedChromeSoftInvalidateWithGlassCore(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RunDeferredThemedChromeSoftInvalidate"));
        if (pfn)
            pfn(hRoot);
    }

    void TryRegisterTextGlowClientWithGlassCore(HWND hControl) noexcept
    {
        if (!hControl)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_TryRegisterTextGlowClient"));
        if (pfn)
            pfn(hControl);
    }

    void RegisterTextGlowPopupClientWithGlassCore(HWND hControl, std::uint8_t category) noexcept
    {
        if (!hControl)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND, BYTE);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RegisterTextGlowPopupClient"));
        if (pfn)
            pfn(hControl, category);
    }

    void UnregisterTextGlowClientWithGlassCore(HWND hControl) noexcept
    {
        if (!hControl)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_UnregisterTextGlowClient"));
        if (pfn)
            pfn(hControl);
    }

    void TryRegisterMenuBarTextGlowOwnerWithGlassCore(HWND hOwner) noexcept
    {
        if (!hOwner)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_TryRegisterMenuBarTextGlowOwner"));
        if (pfn)
            pfn(hOwner);
    }

    void RunDeferredTextGlowSoftInvalidateWithGlassCore(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RunDeferredTextGlowSoftInvalidate"));
        if (pfn)
            pfn(hRoot);
    }

    void RunDeferredTextInkSoftInvalidateWithGlassCore(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        using PFN = void(WINAPI*)(HWND);
        const auto pfn = reinterpret_cast<PFN>(
            GetProcAddress(hCore, "GlassCore_RunDeferredTextInkSoftInvalidate"));
        if (pfn)
            pfn(hRoot);
    }

    void WaitForShutdownComplete(DWORD timeoutMs)
    {
        // Событие должно существовать всегда, но на всякий случай создадим
        if (!g_shutdownEvent)
        {
            g_shutdownEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
            DebugLog(L"[WndCompat][Core] WaitForShutdownComplete: created missing shutdown event");
        }

        DebugLog(L"[WndCompat][Core] Waiting for shutdown completion (timeout=%ums)...", timeoutMs);

        DWORD result = WaitForSingleObject(g_shutdownEvent, timeoutMs);

        switch (result)
        {
        case WAIT_OBJECT_0:
            DebugLog(L"[WndCompat][Core] Shutdown wait completed");
            break;

        case WAIT_TIMEOUT:
            DebugLog(L"[WndCompat][Core] Shutdown wait timeout - continuing anyway");
            break;

        default:
            DebugLog(L"[WndCompat][Core] Shutdown wait error (GLE=%u)", GetLastError());
            break;
        }
    }

    void Shutdown()
    {
        DebugLog(L"[WndCompat][Core] Shutdown: BEGIN");

        // 1) Запускаем shutdown (идемпотентно)
        Internal::ExecuteShutdownCore();

        // 2) Локально ждём завершения (больше не нужно ждать из GlassCompat)
        WaitForShutdownComplete();

        // 3) Дальше — локальная очистка
        UiDispatcher::Shutdown();

        if (g_controlWindow)
        {
            DestroyWindow(g_controlWindow);
            g_controlWindow = nullptr;
        }

        {
            HINSTANCE hInst = reinterpret_cast<HINSTANCE>(
                GetModuleHandleW(L"WndCompat.dll"));

            if (hInst)
            {
                if (!UnregisterClassW(kControlClassName, hInst))
                {
                    DWORD gle = GetLastError();
                    DebugLog(L"[WndCompat][Core] UnregisterClassW failed for control window class, GLE=%u", gle);
                }
                else
                {
                    DebugLog(L"[WndCompat][Core] Control window class unregistered");
                }
            }
        }

        g_guiThreadId.store(0, std::memory_order_release);

        if (g_shutdownEvent)
        {
            CloseHandle(g_shutdownEvent);
            g_shutdownEvent = nullptr;
        }

        Shared::BitmapUtils::EarlyShutdown();

        g_initialized.store(false, std::memory_order_release);

        DebugLog(L"[WndCompat][Core] Shutdown: END");

#ifdef GLASS_ENABLE_LOGGING
        // Завершаем логгер один раз, локально
        Shared::Log::GlassLogClient::Instance().Stop();
#endif
    }

    void ShutdownHostExit()
    {
        bool expected = false;
        if (!g_shuttingDown.compare_exchange_strong(expected, true))
        {
            DebugLog(L"[WndCompat][Core] ShutdownHostExit: already in progress or completed");
            return;
        }

        DebugLog(L"[WndCompat][Core] ShutdownHostExit: BEGIN");

        UninstallGuiMessageHook();
        WinEventHook_ShutdownHostExit();
        UiDispatcher::ShutdownHostExit();

        WndCompat::Root::SetRootEventProvider({});
        WndCompat::DropDown::SetDropDownEventProvider({});
        WndCompat::Menu::SetMenuEventProvider({});
        WndCompat::DirectUI::SetDirectUIEventProvider({});
        WndCompat::Rebar::SetRebarEventProvider({});
        WndCompat::Tooltip::SetTooltipEventProvider({});

        g_shutdownComplete.store(true, std::memory_order_release);
        if (g_shutdownEvent)
            SetEvent(g_shutdownEvent);

        DebugLog(L"[WndCompat][Core] ShutdownHostExit: END");
    }

    void PostDwmColorizationDispatch(WPARAM colorizationWave) noexcept
    {
        if (g_shuttingDown.load(std::memory_order_acquire))
            return;
        Internal::EnsureControlWindow();
        if (!g_controlWindow || !IsWindow(g_controlWindow))
            return;
        (void)PostMessageW(
            g_controlWindow,
            WM_GLASS_DWMCOLORIZATION_DISPATCH,
            colorizationWave,
            0);
    }

    bool IsShuttingDown()
    {
        return g_shuttingDown.load(std::memory_order_acquire);
    }

    bool IsShutdownComplete()
    {
        return g_shutdownComplete.load(std::memory_order_acquire);
    }

    bool IsInitialized()
    {
        return g_initialized.load(std::memory_order_acquire);
    }
}

// ============================================
// EXPORTS
// ============================================

extern "C"
{
    __declspec(dllexport) BOOL WINAPI WndCompat_Initialize(DWORD mode)
    {
        WndCompat::Initialize(static_cast<Shared::InjectionMode>(mode));
        return TRUE;
    }

    __declspec(dllexport) void WINAPI WndCompat_SetInjectionMode(DWORD mode) noexcept
    {
        WndCompat::WinEventHook_SetMode(static_cast<Shared::InjectionMode>(mode));
    }

    __declspec(dllexport) void WINAPI WndCompat_ApplyInjectionModeVisuals(
        DWORD oldMode,
        DWORD newMode) noexcept
    {
        WndCompat::WinEventHook_ApplyInjectionModeVisuals(
            static_cast<Shared::InjectionMode>(oldMode),
            static_cast<Shared::InjectionMode>(newMode));
    }

    __declspec(dllexport) void WINAPI WndCompat_BeginShutdown()
    {
        DebugLog(L"[WndCompat][Core] Exported BeginShutdown called");
        // ВАЖНО: синхронный shutdown, без фонового потока.
        WndCompat::Shutdown();
    }

    __declspec(dllexport) void WINAPI WndCompat_BeginHostExitShutdown()
    {
        DebugLog(L"[WndCompat][Core] Exported BeginHostExitShutdown called");
        WndCompat::ShutdownHostExit();
    }

    __declspec(dllexport) BOOL WINAPI WndCompat_IsShutdownComplete()
    {
        return WndCompat::IsShutdownComplete() ? TRUE : FALSE;
    }

    __declspec(dllexport) BOOL WINAPI WndCompat_IsInitialized()
    {
        return WndCompat::IsInitialized() ? TRUE : FALSE;
    }

    __declspec(dllexport) void WINAPI WndCompat_WaitForShutdownComplete()
    {
        // Оставляем экспорт для совместимости/диагностики,
        // но GlassCompat больше не обязан его вызывать.
        WndCompat::WaitForShutdownComplete();
    }

    __declspec(dllexport) HWND WINAPI WndCompat_GetControlWindow()
    {
        return WndCompat::g_controlWindow;
    }

    __declspec(dllexport) void WINAPI WndCompat_FlushPendingDesktopBridges()
    {
        WndCompat::WinUiDesktopBridge_FlushPendingAfterExplorerCompatReady();
        WndCompat::WinUiContentBridge_FlushPendingAfterTaskbarCompatReady();
    }

    __declspec(dllexport) void WINAPI WndCompat_FlushPendingCoreWindows()
    {
        WndCompat::CoreWindowHost_FlushPendingAfterUICompatReady();
    }

    __declspec(dllexport) void WINAPI WndCompat_AttachExistingCoreWindows()
    {
        WndCompat::CoreWindowHost_AttachExistingInProcess();
    }

    __declspec(dllexport) void WINAPI WndCompat_OnCoreWindowIxdeReady() noexcept
    {
        WndCompat::CoreWindowHost_OnIxdeReady();
    }

    __declspec(dllexport) void WINAPI WndCompat_RescanTaskbarDesktopWindowContentBridges() noexcept
    {
        WndCompat::WinUiContentBridge_RescanShellTraysDesktopBridges();
    }

    __declspec(dllexport) void WINAPI WndCompat_ResetTaskbarCompatNotifyProcCache(void) noexcept
    {
        WndCompat::WinUi_ResetTaskbarCompatNotifyProcCache();
    }

    __declspec(dllexport) void WINAPI WndCompat_ResetUICompatNotifyProcCache(void) noexcept
    {
        WndCompat::CoreWindowHost_ResetUICompatNotifyProcCache();
    }

    __declspec(dllexport) void WINAPI WndCompat_ReapplyTaskbarBackdrops() noexcept
    {
        WndCompat::TaskbarHost_ReapplyBackdropToTracked();
    }
}