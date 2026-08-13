#include "WndPch.h"
#include "WinEventHandler.h"
#include "Tracker/BackdropPolicyRescan.h"
#include "Tracker/SubclassDiagPublish.h"

#include <WndCompat.h>
#include <GlassControlKind.h>
#include "Tracker/WindowClassifier.h"
#include "Root/RootSubclass.h"
#include "Root/RootExplorerSubclassBridge.h"
#include "Root/Win32ChromeRestore.h"
#include "Root/TaskbarHost.h"
#include "Root/ExplorerRootSubclass.h"
#include "Popup/Menu/MenuSubclass.h"
//#include "Popup/Menu/MenuNTSubclass.h"
#include <MenuUtils.h>

#include <atomic>
#include <mutex>
#include <cstdio>
#include <unordered_map>
#include <TlHelp32.h>
#include "WindowClassUtils.h"
#include <WinApiUtils.h>
#include <Helpers/AddressBarPieUtils.h>
#include <GlassHostRegistry.h>
#include <RootBackdropPolicy.h>
#include <RootPolicyQuery.h>
#include <ProcessBlockList.h>
#include <OpenGlassHostState.h>
#include <TrayBorderDrawMode.h>
#include "Tracker/GlassAttachGate.h"
#include <Popup/DropDown/ListviewPopupSubclass.h>
#include <Popup/DropDown/ViewControlSubclass.h>
#include <Controls/MonthCalSubclass.h>
#include <Controls/TreeViewSubclass.h>
#include <Controls/TabControlSubclass.h>
#include <Controls/TrackBarHost.h>
#include <Controls/UpDownHost.h>
#include <Popup/DropDown/DropDownSubclass.h>
#include <Controls/ToolbarSubclass.h>
#include <Controls/ButtonSubclass.h>
#include <GlassControlKind.h>
#include <SystemHelper.h>
#include <Controls/ListViewSubclass.h>
#include <Controls/EditSubclass.h>
#include <Controls/StaticSubclass.h>
#include <Controls/RebarSubclass.h>
#include <Controls/HeaderSubclass.h>
#include <Controls/ListBoxSubclass.h>
#include <Controls/StatusBarSubclass.h>
#include <Controls/ProgressBarSubclass.h>
#include <Controls/DateTimePickerSubclass.h>
#include <Mshtml/IeServerHost.h>
#include <WinUiBridge/WinUiDesktopBridgeHost.h>
#include <WinUiBridge/WinUiContentBridgeHost.h>
#include <WinUiBridge/CoreWindowHost.h>
#include <Controls/ScrollBarHost.h>
#include <cstdio>
#include <Popup/ComboLBox/ComboLBoxSubclass.h>
#include <Popup/ComboLBox/ComboBoxSubclass.h>
#include <Popup/Tooltip/TooltipSubclass.h>
#include <Popup/Tooltip/KbxLabelSubclass.h>
#include <DropDownEventProvider.h>
#include <CommCtrl.h>
#include <Controls/DirectUISubclass.h>
#include <Controls/NetUISubclass.h>

extern HMODULE g_hModule;

#include <Explorer/ExplorerIslandContainerSubclass.h>

namespace WndCompat
{
    void (*g_installGetMsgHookForThread_ptr)(DWORD tid) noexcept = nullptr;

    void EnsureGetMsgHookForHwnd(HWND hWnd) noexcept
    {
        if (!hWnd)
            return;
        const DWORD tid = GetWindowThreadProcessId(hWnd, nullptr);
        if (tid && g_installGetMsgHookForThread_ptr)
            g_installGetMsgHookForThread_ptr(tid);
    }
}

namespace
{
    // Хук для событий окон: CREATE / DESTROY / SHOW (SHOW — повторный аттач тултипов).
    static HWINEVENTHOOK     g_winEventHook = nullptr;

    static std::atomic<bool> g_hookInstalled{ false };
    static std::mutex        g_hookMutex;

    static std::atomic<long> g_inCallback{ 0 };
    static HANDLE            g_noCallbacksEvent = nullptr;

    static Shared::InjectionMode  g_mode = Shared::InjectionMode::Full;

    void EnsureExplorerCompatForWinUiMenu(HWND hwnd, DWORD event) noexcept
    {
        if (g_mode != Shared::InjectionMode::Full)
            return;

        const HMODULE hGlassCompat = GetModuleHandleW(L"GlassCompat.dll");
        if (!hGlassCompat)
            return;

        using EnsureFn = void(WINAPI*)() noexcept;
        const auto ensure = reinterpret_cast<EnsureFn>(
            GetProcAddress(hGlassCompat, "GlassCompat_EnsureExplorerCompatLoaded"));
        if (!ensure)
            return;

        // Только lifecycle-триггер. HWND не участвует в матчинге XAML и не
        // регистрируется в ExplorerCompat: патчер по-прежнему работает по VTC.
        (void)hwnd;
        (void)event;
        ensure();
    }

    // Forward declaration — определение ниже, после CallbackGuard
    void CALLBACK WndHandler(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);

    // ── Per-thread WH_GETMESSAGE хуки для cross-thread Root_Attach ──────────
    // При initial scan: для каждого чужого GUI-потока устанавливается хук,
    // затем в окно этого потока PostMessage(WM_GLASS_ATTACH_ROOT).
    // GetMsgHookProc принимает сообщение в правильном потоке и маршрутизирует
    // в WndHandler — там применяются все те же проверки (InjectionMode,
    // WindowClassifier, IsShuttingDown), что и при обычном EVENT_OBJECT_CREATE.
    // Хуки снимаются в WinEventHook_Shutdown вместе с основным WinEvent-хуком.
    static std::unordered_map<DWORD, HHOOK> g_perThreadGetMsgHooks;
    static std::mutex                        g_perThreadHooksMutex;

    struct CallbackGuard
    {
        CallbackGuard()
        {
            g_inCallback.fetch_add(1, std::memory_order_relaxed);
            if (g_noCallbacksEvent)
                ResetEvent(g_noCallbacksEvent);
        }
        ~CallbackGuard()
        {
            if (g_inCallback.fetch_sub(1, std::memory_order_relaxed) == 1)
            {
                if (g_noCallbacksEvent)
                    SetEvent(g_noCallbacksEvent);
            }
        }
    };

    // Лог перед Root_Attach из WinEvent: имя класса окна (как у GetClassNameW).
    void LogWinEventRootAttach(const wchar_t* scenario, HWND hWnd) noexcept
    {
        wchar_t cn[256]{};
        if (::GetClassNameW(hWnd, cn, static_cast<int>(_countof(cn))) <= 0)
            cn[0] = L'\0';
        DebugLog(L"[WndCompat][WinEvent] Root_Attach (%s): hwnd=0x%p class=\"%s\"", scenario, hWnd, cn);
    }

#ifndef WM_DWMCOLORIZATIONCHANGED
#define WM_DWMCOLORIZATIONCHANGED 0x0320
#endif

    // Trailing debounce light↔dark / accent: пачка THEMECHANGED+ImmersiveColorSet.
    constexpr UINT_PTR kTaskbarVisualReapplyTimerId = 0x47545352u; // 'GTSR'
    // После #65 / IThemeManager2 uxtheme #132+#139 settle дольше, чем 200 ms.
    constexpr UINT kTaskbarVisualReapplyDebounceMs = 450;

    void ReapplyTrackedTaskbarOnVisualChange(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd) || !WndCompat::TaskbarHost_IsTracked(hwnd))
            return;

        if (Shared::OpenGlassHostState::IsPipelineAffectingBorders()
            && Shared::TrayBorderDrawMode::ReadFromRegistry()
                == Shared::TrayBorderDrawMode::Mode::Xaml)
        {
            const HMODULE hTb = GetModuleHandleW(L"TaskbarCompat.dll");
            if (hTb)
            {
                // Только Request*: вызов приходит из GetMsgHookProc, то есть изнутри
                // kernel-callback от PeekMessage. Синхронный reapply реэнтрантно
                // входил в XAML core на середине его операции — AV в
                // CDependencyObject::AddRef на CValue-float (дамп explorer 13.08).
                using ReloadFn = void(WINAPI*)() noexcept;
                const auto pfn = reinterpret_cast<ReloadFn>(
                    GetProcAddress(hTb, "TaskbarCompat_RequestReloadSettingsAndReapply"));
                if (pfn)
                    pfn();
            }
            return;
        }

        WndCompat::TaskbarHost_HandleReapplyBackdropMessage(hwnd);
    }

    [[nodiscard]] bool IsTaskbarVisualChangeMessage(UINT msg, LPARAM lParam) noexcept
    {
        if (msg == WM_DWMCOLORIZATIONCHANGED || msg == WM_THEMECHANGED)
            return true;
        if (msg != WM_SETTINGCHANGE)
            return false;
        // Зеркало host ThemeMessageSuggestsDwmPolicyReload.
        const wchar_t* s = reinterpret_cast<const wchar_t*>(lParam);
        if (!s)
            return true;
        return _wcsicmp(s, L"ImmersiveColorSet") == 0;
    }

    void ScheduleTrackedTaskbarVisualReapply(HWND hwnd) noexcept
    {
        if (!hwnd || !IsWindow(hwnd) || !WndCompat::TaskbarHost_IsTracked(hwnd))
            return;
        KillTimer(hwnd, kTaskbarVisualReapplyTimerId);
        SetTimer(hwnd, kTaskbarVisualReapplyTimerId, kTaskbarVisualReapplyDebounceMs, nullptr);
    }

    // Обработчик WM_GLASS_ATTACH_ROOT в потоке-владельце окна.
    // SetWindowSubclass здесь безопасен: мы в правильном потоке.
    // Маршрутизируем через WndHandler, чтобы Classify/IsHardBlockedClass
    // применялись так же, как при обычном EVENT_OBJECT_CREATE.
    LRESULT CALLBACK GetMsgHookProc(int code, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (code == HC_ACTION && wParam == PM_REMOVE)
        {
            MSG* msg = reinterpret_cast<MSG*>(lParam);
            if (msg && msg->message == WndCompat::WM_GLASS_DETACH_TASKBAR_TRAY && IsWindow(msg->hwnd))
            {
                KillTimer(msg->hwnd, kTaskbarVisualReapplyTimerId);
                WndCompat::TaskbarHost_Detach(msg->hwnd);
                msg->message = WM_NULL;
            }
            else if (msg && msg->message == WndCompat::WM_GLASS_REAPPLY_TASKBAR_BACKDROP && IsWindow(msg->hwnd))
            {
                WndCompat::TaskbarHost_HandleReapplyBackdropMessage(msg->hwnd);
                msg->message = WM_NULL;
            }
            else if (msg && msg->message == WndCompat::WM_GLASS_ATTACH_ROOT && IsWindow(msg->hwnd))
            {
                // Проходим через WndHandler — там применяется Classify со всеми
                // фильтрами (IsHardBlockedClass, IsSoftBlockedClass, роль окна).
                WndHandler(nullptr, EVENT_OBJECT_CREATE, msg->hwnd,
                    OBJID_WINDOW, CHILDID_SELF, 0, 0);
                msg->message = WM_NULL; // поглощаем — до WndProc не доходит
            }
            else if (msg && msg->message == WndCompat::WM_GLASS_SYNTHETIC_CREATE && IsWindow(msg->hwnd))
            {
                WndHandler(nullptr, EVENT_OBJECT_CREATE, msg->hwnd,
                    OBJID_WINDOW, CHILDID_SELF, 0, 0);
                msg->message = WM_NULL;
            }
            else if (msg && msg->message == WndCompat::WM_GLASS_ATTACH_IESERVER && IsWindow(msg->hwnd))
            {
                // InternetExplorer_Server: GlassMark в потоке-владельце HWND.
                WndCompat::IeServer_Attach(msg->hwnd);
                msg->message = WM_NULL;
            }
            else if (msg && msg->message == WM_TIMER
                && msg->wParam == kTaskbarVisualReapplyTimerId
                && IsWindow(msg->hwnd)
                && WndCompat::TaskbarHost_IsTracked(msg->hwnd))
            {
                KillTimer(msg->hwnd, kTaskbarVisualReapplyTimerId);
                ReapplyTrackedTaskbarOnVisualChange(msg->hwnd);
                msg->message = WM_NULL;
            }
            else if (msg && IsWindow(msg->hwnd)
                && WndCompat::TaskbarHost_IsTracked(msg->hwnd)
                && IsTaskbarVisualChangeMessage(msg->message, msg->lParam))
            {
                ScheduleTrackedTaskbarVisualReapply(msg->hwnd);
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Установить WH_GETMESSAGE хук для потока tid (идемпотентно).
    void InstallGetMsgHookForThread(DWORD tid) noexcept
    {
        std::lock_guard lock(g_perThreadHooksMutex);
        if (g_perThreadGetMsgHooks.contains(tid))
            return;

        // hMod = nullptr: целевой поток принадлежит текущему процессу, поэтому
        // LoadLibrary не вызывается и рефкаунт WndCompat.dll не растёт.
        // Передача g_hModule здесь была бы ошибкой — UnhookWindowsHookEx поставил бы
        // FreeLibrary-нотификацию в очередь потока, которую тот может никогда не обработать,
        // и DLL не выгрузилась бы.
        HHOOK hHook = SetWindowsHookExW(WH_GETMESSAGE, GetMsgHookProc, nullptr, tid);
        if (hHook)
        {
            g_perThreadGetMsgHooks[tid] = hHook;
            DebugLog(L"[WndCompat][WinEventHook] GetMsgHook: установлен для tid=%u", tid);
        }
        else
        {
            DebugLog(DBG_WARNING,
                L"[WndCompat][WinEventHook] GetMsgHook: SetWindowsHookExW FAILED tid=%u GLE=%u",
                tid, GetLastError());
        }
    }

    struct InstallGetMsgHookPtrRegistrar
    {
        InstallGetMsgHookPtrRegistrar() noexcept
        {
            WndCompat::g_installGetMsgHookForThread_ptr = &InstallGetMsgHookForThread;
        }
    } g_installGetMsgHookPtrRegistrar;

    // Снять все per-thread WH_GETMESSAGE хуки (вызывается из WinEventHook_Shutdown).
    void UninstallAllGetMsgHooks() noexcept
    {
        std::lock_guard lock(g_perThreadHooksMutex);
        for (auto& [tid, hHook] : g_perThreadGetMsgHooks)
        {
            UnhookWindowsHookEx(hHook);
            DebugLog(L"[WndCompat][WinEventHook] GetMsgHook: снят для tid=%u", tid);
        }
        g_perThreadGetMsgHooks.clear();
    }

    struct WindowScanStats
    {
        unsigned topDirect = 0;
        unsigned topPosted = 0;
        unsigned childDirect = 0;
        unsigned childPosted = 0;
    };
} // namespace

namespace WndCompat
{
    void DispatchRootAttachToOwnerThread(HWND hRoot) noexcept
    {
        if (!IsWindow(hRoot))
            return;
        if (WndCompat::IsShuttingDown())
            return;
        DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);
        const DWORD curTid = GetCurrentThreadId();
        if (wndTid != 0 && wndTid != curTid)
        {
            InstallGetMsgHookForThread(wndTid);
            if (!PostMessageW(hRoot, WndCompat::WM_GLASS_ATTACH_ROOT, 0, 0))
            {
                DebugLog(DBG_WARNING,
                    L"[WndCompat][WinEvent] DispatchRootAttachToOwnerThread: PostMessage failed hwnd=0x%p GLE=%u",
                    hRoot,
                    GetLastError());
            }
            return;
        }
        WndCompat::Root_Attach(hRoot);
    }

    // File Explorer shell root (CabinetWClass / ExploreWClass): отдельный subclass proc — см. ExplorerRootSubclass.cpp.
    void DispatchExplorerRootAttachToOwnerThread(HWND hRoot) noexcept
    {
        if (!IsWindow(hRoot))
            return;
        if (WndCompat::IsShuttingDown())
            return;
        const DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);
        const DWORD curTid = GetCurrentThreadId();
        if (wndTid != 0 && wndTid != curTid)
        {
            InstallGetMsgHookForThread(wndTid);
            if (!PostMessageW(hRoot, WndCompat::WM_GLASS_ATTACH_ROOT, 0, 0))
            {
                DebugLog(DBG_WARNING,
                    L"[WndCompat][WinEvent] DispatchExplorerRootAttachToOwnerThread: PostMessage failed hwnd=0x%p GLE=%u",
                    hRoot,
                    GetLastError());
            }
            return;
        }
        WndCompat::ExplorerRoot::Attach(hRoot);
    }
}

namespace
{
    using WndCompat::DispatchExplorerRootAttachToOwnerThread;
    using WndCompat::DispatchRootAttachToOwnerThread;

    void CALLBACK WndHandler(
        HWINEVENTHOOK,
        DWORD event,
        HWND hWnd,
        LONG idObject,
        LONG idChild,
        DWORD,
        DWORD)
    {
        CallbackGuard guard;

        if (WndCompat::IsShuttingDown())
            return;

        if (!IsWindow(hWnd))
            return;

        DWORD thisPid = GetCurrentProcessId();
        DWORD wndPid = 0;
        GetWindowThreadProcessId(hWnd, &wndPid);
        if (wndPid != thisPid)
            return;

        // Wizard preview blocks: custom paint; skip Unknown→Root_Attach (incl. rescan bypass).
        if (WndCompat::IsGlassSettingsBitmapSwitchHwnd(hWnd)
            && (event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_SHOW))
            return;

        switch (event)
        {
            // ── Создание/уничтожение окон ─────────────────────────────────────────
            // Фильтруем строго по OBJID_WINDOW: нас интересуют только окна,
            // а не дочерние объекты доступности.
        case EVENT_SYSTEM_FOREGROUND:
        {
            // H44: StartMenu CoreWindow — SHOW часто нет; process-wide FOREGROUND hook.
            if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
                break;
            if (g_mode != Shared::InjectionMode::Full)
                break;
            if (!WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd))
                break;
            const auto [roleFg, kindFg] = WindowClassifier::Classify(hWnd);
            if (roleFg == WindowRole::Special
                && (kindFg == ControlKind::ShellNotificationCore
                    || kindFg == ControlKind::ShellStartMenuCore))
            {
                WndCompat::CoreWindowHost_Attach(hWnd);
            }
            break;
        }

        case EVENT_OBJECT_UNCLOAKED:
        {
            // SEH NC: SHOW часто cloaked. QS ControlCenter — обычно не cloaked (H12).
            if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
                break;
            if (g_mode != Shared::InjectionMode::Full)
                break;
            if (!WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd))
                break;
            const auto [roleUc, kindUc] = WindowClassifier::Classify(hWnd);
            if (roleUc == WindowRole::Special
                && (kindUc == ControlKind::ShellNotificationCore
                    || kindUc == ControlKind::ShellStartMenuCore))
            {
                DebugLog(
                    L"[WndCompat][WinEvent] CoreWindowHost_Attach (UNCLOAKED): hwnd=0x%p",
                    hWnd);
                WndCompat::CoreWindowHost_Attach(hWnd);
            }
            break;
        }

        case EVENT_OBJECT_LOCATIONCHANGE:
        {
            // ShellHost QS: первый SHOW при cx=cy=0 → IXDE defer; размер приходит
            // LOCATIONCHANGE (не UNCLOAKED). Один Attach на hwnd после ненулевого client.
            // SEH: CREATE часто 1×1; NC ~360×1032 → Attach для IXDE/reapply (лог H12 defer).
            if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
                break;
            if (g_mode != Shared::InjectionMode::Full)
                break;

            const bool isCc =
                WndCompat::IsWindowClass(hWnd, L"ControlCenterWindow");
            const bool isSehCw =
                Shared::RootBackdropPolicy::IsShellExperienceHostProcess()
                && WndCompat::IsWindowClass(hWnd, L"Windows.UI.Core.CoreWindow");
            if (!isCc && !isSehCw)
                break;
            if (!WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd))
                break;

            RECT rc{};
            if (!GetClientRect(hWnd, &rc))
                break;
            const int cx = rc.right - rc.left;
            const int cy = rc.bottom - rc.top;
            if (cx <= 0 || cy <= 0)
                break;
            // SEH: ждём высоту панели NC (не toast ~171 и не 1×1).
            if (isSehCw && cy < 600)
                break;

            if (isCc)
            {
                static HWND s_ccSizedAttachHwnd = nullptr;
                if (s_ccSizedAttachHwnd == hWnd)
                    break;
                s_ccSizedAttachHwnd = hWnd;
            }
            else
            {
                static HWND s_sehNcSizedAttachHwnd = nullptr;
                if (s_sehNcSizedAttachHwnd == hWnd)
                    break;
                s_sehNcSizedAttachHwnd = hWnd;
            }

            DebugLog(
                L"[WndCompat][WinEvent] CoreWindowHost_Attach (LOCATIONCHANGE sized): "
                L"hwnd=0x%p cx=%d cy=%d",
                hWnd,
                cx,
                cy);
            WndCompat::CoreWindowHost_Attach(hWnd);
            break;
        }

        case EVENT_OBJECT_SHOW:
        {
            // Повторный показ тултипа: HWND уже существовал, CREATE не приходит,
            // а субкласс мог быть снят в WM_NCDESTROY при предыдущем скрытии.
            //
            // Первый показ popup после инжекта: EVENT_OBJECT_CREATE мог быть потерян
            // (GlassCompat до g_hWndCompat) — дублируем attach по SHOW (идемпотентно).
            if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
                break;

            if (WndCompat::IsWindowClass(hWnd, TOOLTIPS_CLASSW) ||
                WndCompat::IsWindowClass(hWnd, L"MicrosoftWindowsTooltip"))
            {
                WndCompat::Tooltip_AttachIfNeeded(hWnd);
            }
            else if (WndCompat::IsWindowClass(hWnd, L"THintWindow"))
            {
                WndCompat::Tooltip_AttachHintWindow(hWnd);
            }
            else
            {
                auto [role, kind] = WindowClassifier::Classify(hWnd);
                switch (role)
                {
                case WindowRole::Popup:
                    if (Shared::MenuUtils::IsPopupMenu(hWnd))
                    {
                        WndCompat::Menu_AttachIfNeeded(hWnd);
                    }
                    else if (WndCompat::IsWindowClass(hWnd, L"KbxLabelClass"))
                    {
                        WndCompat::KbxLabel_Attach(hWnd);
                    }
                    else if (WndCompat::IsWindowClass(hWnd, L"DropDown"))
                    {
                        WndCompat::DropDown_AttachIfNeeded(hWnd);
                    }
                    break;

                case WindowRole::Special:
                {
                    const bool allowGlassShow = WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd);
                    switch (kind)
                    {
                    case ControlKind::FileExplorerIslandContainer:
                        if (allowGlassShow)
                            WndCompat::ExplorerIslandContainer_Attach(hWnd);
                        break;
                    case ControlKind::ListviewPopup:
                        if (allowGlassShow)
                            WndCompat::ListviewPopup_AttachIfNeeded(hWnd);
                        break;
                    case ControlKind::ViewControl:
                        if (allowGlassShow)
                            WndCompat::ViewControl_AttachIfNeeded(hWnd);
                        break;
                    case ControlKind::Atl_ListBox:
                        if (allowGlassShow)
                            WndCompat::ComboLBox_Attach(hWnd);
                        break;
                    case ControlKind::WinUiPopupBridge:
                        // HWND используется только для ранней lazy-загрузки ExplorerCompat.
                        // Само меню патчится по типу XAML-элемента, без HWND-bridge.
                        EnsureExplorerCompatForWinUiMenu(hWnd, EVENT_OBJECT_SHOW);
                        break;
                    case ControlKind::ShellNotificationCore:
                    case ControlKind::ShellStartMenuCore:
                        // H32/H33: SHOW (в т.ч. remote OOC). Visible не обязателен — гонка с paint.
                        if (allowGlassShow && g_mode == Shared::InjectionMode::Full)
                        {
                            DebugLog(L"[WndCompat][WinEvent] CoreWindowHost_Attach (SHOW): hwnd=0x%p", hWnd);
                            WndCompat::CoreWindowHost_Attach(hWnd);
                        }
                        break;
                    default:
                        break;
                    }
                    break;
                }

                default:
                    break;
                }
            }
            break;
        }

        case EVENT_OBJECT_CREATE:
        {
            if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
                break;

            auto [role, kind] = WindowClassifier::Classify(hWnd);
            WCHAR className[256] = L"";
            GetClassNameW(hWnd, className, sizeof(className) / sizeof(className[0]));
            DebugLog(DBG_CRITICAL, L"[WndCompat][WinEvent] EVENT_OBJECT_CREATE: hwnd=0x%p role=%d kind=%d class=%ls",
                hWnd, static_cast<int>(role), static_cast<int>(kind), className);
            switch (role)
            {
                // ── Popup (меню, тултипы) ─────────────────────────────────────
            case WindowRole::Popup:
                if (Shared::MenuUtils::IsPopupMenu(hWnd))
                {
                    DebugLog(L"[WndCompat][WinEvent] Menu_Attach: hwnd=0x%p", hWnd);
                    WndCompat::Menu_Attach(hWnd);
                }
                else if (WndCompat::IsWindowClass(hWnd, L"KbxLabelClass"))
                {
                    DebugLog(L"[WndCompat][WinEvent] KbxLabel_Attach: hwnd=0x%p", hWnd);
                    WndCompat::KbxLabel_Attach(hWnd);
                }
                else if (WndCompat::IsWindowClass(hWnd, TOOLTIPS_CLASSW) ||
                    WndCompat::IsWindowClass(hWnd, L"MicrosoftWindowsTooltip"))
                {
                    DebugLog(L"[WndCompat][WinEvent] Tooltip_AttachIfNeeded (tooltips_class32 / MicrosoftWindowsTooltip): hwnd=0x%p", hWnd);
                    WndCompat::Tooltip_AttachIfNeeded(hWnd);
                }
                else if (WndCompat::IsWindowClass(hWnd, L"THintWindow"))
                {
                    DebugLog(L"[WndCompat][WinEvent] Tooltip_AttachHintWindow: hwnd=0x%p", hWnd);
                    WndCompat::Tooltip_AttachHintWindow(hWnd);
                }
                else if (WndCompat::IsWindowClass(hWnd, L"DropDown"))
                {
                    WndCompat::DropDown_AttachIfNeeded(hWnd);
                }
                break;

                // ── Special (DropDown-popup окна) ──────────────────────────────
                case WindowRole::Special:
            {
                const bool allowGlassCreate = WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd);
                switch (kind)
                {
                case ControlKind::ListviewPopup:
                    if (allowGlassCreate)
                    {
                        DebugLog(L"[WndCompat][WinEvent] ListviewPopup_Attach: hwnd=0x%p", hWnd);
                        WndCompat::ListviewPopup_Attach(hWnd);
                    }
                    break;

                case ControlKind::ViewControl:
                    if (allowGlassCreate)
                    {
                        DebugLog(L"[WndCompat][WinEvent] ViewControl_Attach: hwnd=0x%p", hWnd);
                        WndCompat::ViewControl_Attach(hWnd);
                    }
                    break;

                case ControlKind::ComboLBox:
                    break;
                case ControlKind::Atl_ListBox:
                    if (allowGlassCreate)
                    {
                        DebugLog(L"[WndCompat][WinEvent] ComboLBox_Attach (Special): hwnd=0x%p kind=%d", hWnd,
                            static_cast<int>(kind));
                        WndCompat::ComboLBox_Attach(hWnd); // под вопросом, возможно вызовет зависание при применении анимации.
                    }
                    break;

				case ControlKind::WinUiPopupBridge:
                    EnsureExplorerCompatForWinUiMenu(hWnd, EVENT_OBJECT_CREATE);
                    break;

					// WinUI content bridge: контент панели задач в Windows 11
                case ControlKind::WinUiContentBridge:
                     if (allowGlassCreate && g_mode == Shared::InjectionMode::Full)
                    {
                        DebugLog(L"[WndCompat][WinEvent] WinUiContentBridge_Attach: hwnd=0x%p", hWnd);
                        WndCompat::WinUiContentBridge_Attach(hWnd);
                    }
					 break;
                case ControlKind::ShellNotificationCore:
                    // HR7: CREATE+visible на dormant NC CoreWindow = H49 AV.
                    // Attach только по SHOW (см. EVENT_OBJECT_SHOW выше).
                    break;
                case ControlKind::ShellStartMenuCore:
                    // Donor CreateWindowInBand(CoreWindow): init на CREATE без
                    // Visible (Search часто cloaked на CREATE). Search →
                    // PostMessage defer IXDE; Start → sync UI IXDE.
                    // Visible требовать нельзя: после restart SearchHost CREATE
                    // пропускался → нет повторных патчей.
                    if (allowGlassCreate && g_mode == Shared::InjectionMode::Full)
                    {
                        DebugLog(
                            L"[WndCompat][WinEvent] CoreWindowHost_Attach (CREATE): hwnd=0x%p",
                            hWnd);
                        WndCompat::CoreWindowHost_Attach(hWnd);
                    }
                    break;
                // WinUI island host: ранний триггер InstallUdk (ExplorerCompat_NotifyDesktopBridgeWindow), без субкласса HWND.
                case ControlKind::WinUiDesktopBridge:
                    if (allowGlassCreate && g_mode == Shared::InjectionMode::Full)
                    {
                        DebugLog(L"[WndCompat][WinEvent] WinUiDesktopBridge_Attach: hwnd=0x%p", hWnd);
                        WndCompat::WinUiDesktopBridge_Attach(hWnd);
                    }
                    //break;
                case ControlKind::FileExplorerIslandContainer:
                    if (allowGlassCreate && g_mode == Shared::InjectionMode::Full)
                        WndCompat::ExplorerIslandContainer_Attach(hWnd);
                    break;
                default:
                    break;
                }
                break;
            }

                // ── Control (WS_CHILD) ─────────────────────────────────────────
            case WindowRole::Control:
            {
                const bool isFull = (g_mode == Shared::InjectionMode::Full);
                const bool allowGlassForHwnd = WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd);

                switch (kind)
                {
                // ── Общие для обоих режимов ────────────────────────────────────

                case ControlKind::MonthCal:
                {
                    HWND parent = GetParent(hWnd);
                    const bool parentIsDropDown = WndCompat::IsWindowClass(parent, L"DropDown");
                    if (allowGlassForHwnd && (parentIsDropDown || isFull))
                        WndCompat::MonthCal_Attach(hWnd, parent, parentIsDropDown);
                    break;
                }

                case ControlKind::ComboBox:
                    // Full: все ComboBox; PopupOnly: только FloatNotifySink (AnimateWindow hook).
                    // Исключение: внутренний ComboBox в «пироге» msctls_progress32→ComboBoxEx32 — без сессии GlassCore
                    // (только ComboLBox/AnimateWindow); иначе чёрная заливка кнопки дропдауна.
                    if (Shared::AddressBarPie::IsInProgressPieComboSubtree(hWnd))
                        break;
                    if (allowGlassForHwnd && (isFull || WndCompat::IsWindowClass(GetParent(hWnd), L"FloatNotifySink")))
                    {
                        DebugLog(L"[WndCompat][WinEvent] ComboBox_Attach: hwnd=0x%p", hWnd);
                        WndCompat::ComboBox_Attach(hWnd);
                    }
                    break;

                case ControlKind::WinForms_ComboBox:
                    if (Shared::AddressBarPie::IsInProgressPieComboSubtree(hWnd))
                        break;
                    if (allowGlassForHwnd && (isFull || WndCompat::IsWindowClass(GetParent(hWnd), L"FloatNotifySink")))
                    {
                        DebugLog(L"[WndCompat][WinEvent] ComboBox_Attach (WinForms): hwnd=0x%p", hWnd);
                        WndCompat::ComboBox_Attach(hWnd, GlassShared::GlassControlKind::WinForms_ComboBox);
                    }
                    break;

                case ControlKind::ListBox:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] ListBox_Attach (native): hwnd=0x%p", hWnd);
                        WndCompat::ListBox_Attach(hWnd, GlassShared::GlassControlKind::ListBox);
                    }
                    break;

                case ControlKind::WinForms_ListBox:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] ListBox_Attach (WinForms): hwnd=0x%p", hWnd);
                        WndCompat::ListBox_Attach(hWnd, GlassShared::GlassControlKind::WinForms_ListBox);
                    }
                    break;

                case ControlKind::StatusBar:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] StatusBar_Attach: hwnd=0x%p", hWnd);
                        WndCompat::StatusBar_Attach(hWnd);
                    }
                    break;

                case ControlKind::ProgressBar:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] ProgressBar_Attach: hwnd=0x%p", hWnd);
                        WndCompat::ProgressBar_Attach(hWnd);
                    }
                    break;

                case ControlKind::DateTimePicker:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] DateTimePicker_Attach: hwnd=0x%p kind=%d", hWnd,
                            static_cast<int>(kind));
                        WndCompat::DateTimePicker_Attach(hWnd, GlassShared::GlassControlKind::DateTimePicker);
                    }
                    break;

                case ControlKind::WinForms_DateTimePicker:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] DateTimePicker_Attach (WinForms): hwnd=0x%p", hWnd);
                        WndCompat::DateTimePicker_Attach(hWnd, GlassShared::GlassControlKind::WinForms_DateTimePicker);
                    }
                    break;

                case ControlKind::ReBar:
                {
                    // 1) Проводник / диалоги: родитель WorkerW или SizeableRebar.
                    // 2) Full + хост UI (GlassSettings и т.п.): ReBar прямой дочерний элемент top-level (GA_ROOT).
                    HWND parent = GetParent(hWnd);
                    if (!allowGlassForHwnd || !parent)
                        break;
                    const bool shellRebar =
                        WndCompat::IsWindowClass(parent, L"WorkerW")
                        || WndCompat::IsWindowClass(parent, L"SizeableRebar");
                    const bool topLevelHostRebar =
                        isFull && (GetAncestor(parent, GA_ROOT) == parent);
                    if (isFull && (shellRebar || topLevelHostRebar))
                    {
                        DebugLog(L"[WndCompat][WinEvent] Rebar_Attach: hwnd=0x%p shell=%d toplevelHost=%d",
                            hWnd,
                            shellRebar ? 1 : 0,
                            topLevelHostRebar ? 1 : 0);
                        WndCompat::Rebar_Attach(hWnd);
                    }
                    break;
                }

                case ControlKind::ToolBar:
                case ControlKind::Atl_ToolBar:
                case ControlKind::Wtl_SearchBand:
                    // Full: все ToolBar; PopupOnly: только внутри ViewControlClass (ATL/WTL — те же условия)
                    if (allowGlassForHwnd &&
                        (isFull || WndCompat::IsWindowClass(GetParent(hWnd), L"ViewControlClass") /*&& !(WndCompat::IsWindowClass(GetParent(hWnd), L"ViewControlClass"))*/))
                    {
                        DebugLog(L"[WndCompat][WinEvent] ToolBar_Attach: hwnd=0x%p kind=%d", hWnd, static_cast<int>(kind));
                        WndCompat::ToolBar_Attach(hWnd);
                    }
                    break;

                case ControlKind::TrackBar:
                    // Full: везде; PopupOnly: только внутри ViewControlClass
                    if (allowGlassForHwnd && isFull)
                    {
                        DebugLog(L"[WndCompat][WinEvent] TrackBar_Attach: hwnd=0x%p", hWnd);
                        
                        WndCompat::TrackBar_Attach(hWnd);
                            
                    }
                    break;

                case ControlKind::UpDown:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] UpDown_Attach: hwnd=0x%p", hWnd);
                        WndCompat::UpDown_Attach(hWnd);
                    }
                    break;

                case ControlKind::ScrollBar:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] ScrollBar_Attach: hwnd=0x%p", hWnd);
                        WndCompat::ScrollBar_Attach(hWnd);
                    }
                    break;

                case ControlKind::ComboBoxEx32:
                    // Root_Attach для обработки WM_CTLCOLOR* — в обоих режимах (BWM pattern)
                    if (allowGlassForHwnd)
                    {
                        LogWinEventRootAttach(L"ComboBoxEx32", hWnd);
                        DispatchRootAttachToOwnerThread(hWnd);
                    }
                    // Full: сессия GlassCore для темы Combobox — но не для «пирога» адресной строки
                    // (ComboBoxEx32 внутри msctls_progress32 в комдиалогах / старом проводнике).
                    if (isFull && allowGlassForHwnd && !WndCompat::IsWindowClass(GetParent(hWnd), L"msctls_progress32"))
                    {
                        DebugLog(L"[WndCompat][WinEvent] ComboBox_Attach (ComboBoxEx32): hwnd=0x%p", hWnd);
                        WndCompat::ComboBox_Attach(hWnd);
                    }
                    break;

                case ControlKind::GenericContainer:
                    // Full: Root_Attach для WM_CTLCOLOR*. PopupOnly: не аттачить —
                    // AutoIt/#32770 и т.п. иначе получают subclass + hooks cache на load.
                    if (isFull && allowGlassForHwnd)
                    {
                        LogWinEventRootAttach(L"GenericContainer", hWnd);
                        DispatchRootAttachToOwnerThread(hWnd);
                    }
                    break;

                // ── Только Full mode ───────────────────────────────────────────

                case ControlKind::DirectUI:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] DirectUI_Attach: hwnd=0x%p", hWnd);
                        WndCompat::DirectUI_Attach(hWnd);
                    }
                    break;

                case ControlKind::NetUI:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] NetUI_Attach: hwnd=0x%p", hWnd);
                        WndCompat::NetUI_Attach(hWnd);
                    }
                    break;

                case ControlKind::Button:
                    if (isFull && allowGlassForHwnd)
                    {
                        // Кнопки с иконкой/битмапом пропускаем — они рисуют изображение,
                        // стеклянный фон не нужен (BWM pattern).
                        const LONG style = GetWindowLongW(hWnd, GWL_STYLE);
                        if (!(style & (BS_BITMAP | BS_ICON)))
                        {
                            DebugLog(L"[WndCompat][WinEvent] Button_Attach (native): hwnd=0x%p", hWnd);
                            WndCompat::Button_Attach(hWnd, GlassShared::GlassControlKind::Button);
                        }
                    }
                    break;

                case ControlKind::WinForms_Button:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] Button_Attach (WinForms): hwnd=0x%p", hWnd);
                        WndCompat::Button_Attach(hWnd, GlassShared::GlassControlKind::WinForms_Button);
                    }
                    break;

                case ControlKind::WinForms_Window:
                    if (isFull && allowGlassForHwnd)
                    {
                        // WinForms Form/Panel — снимаем WS_EX_CLIENTEDGE для корректной рамки
                        {
                            wchar_t wfCn[256]{};
                            if (::GetClassNameW(hWnd, wfCn, static_cast<int>(_countof(wfCn))) <= 0)
                                wfCn[0] = L'\0';
                            DebugLog(
                                L"[WndCompat][WinEvent] WinForms_Window: removed WS_EX_CLIENTEDGE hwnd=0x%p class=\"%s\"",
                                hWnd,
                                wfCn);
                        }
                        SetWindowLongW(hWnd, GWL_EXSTYLE,
                            GetWindowLongW(hWnd, GWL_EXSTYLE) & ~WS_EX_CLIENTEDGE);
                        DispatchRootAttachToOwnerThread(hWnd);
                        bool markedWinFormsWindow = false;
                        if (GlassShared::GlassGetWindowKind(hWnd) == GlassShared::GlassControlKind::None)
                        {
                            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::WinForms_Window);
                            markedWinFormsWindow = true;
                        }
                        if (markedWinFormsWindow)
                            WndCompat::ScheduleThemedChromeRemigrateAfterWinFormsWindowMarked(hWnd);
                    }
                    break;

                // ── Новые кейсы из BWM (только Full) ──────────────────────────

                case ControlKind::TreeView:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] TreeView_Attach: hwnd=0x%p", hWnd);
                        WndCompat::TreeView_Attach(hWnd);
                    }
                    break;

                case ControlKind::ListView:
                case ControlKind::WinForms_ListView:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] ListView_Attach: hwnd=0x%p", hWnd);
                        WndCompat::ListView_Attach(hWnd);
                    }
                    break;

                case ControlKind::Header:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] Header_Attach: hwnd=0x%p", hWnd);
                        WndCompat::Header_Attach(hWnd);
                    }
                    break;

                case ControlKind::Edit:
                case ControlKind::WinForms_Edit:
                case ControlKind::Atl_Edit:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] Edit_Attach: hwnd=0x%p kind=%d", hWnd, static_cast<int>(kind));
                        WndCompat::Edit_Attach(hWnd);
                        DebugLog(DBG_IMPORTANT,
                            L"[WndCompat][WinEvent][Trace] Edit_Attach returned ok hwnd=0x%p tid=%u", hWnd,
                            GetCurrentThreadId());
                    }
                    break;

                case ControlKind::RichEdit20W:
                case ControlKind::WinForms_RichEdit20W:
                    if (isFull && allowGlassForHwnd)
                    {
                        auto glassKind = (kind == ControlKind::WinForms_RichEdit20W)
                            ? GlassShared::GlassControlKind::WinForms_RichEdit20W
                            : GlassShared::GlassControlKind::RichEdit20W;
                        DebugLog(L"[WndCompat][WinEvent] RichEdit_Attach: hwnd=0x%p kind=%d", hWnd, static_cast<int>(kind));
                        WndCompat::RichEdit_Attach(hWnd, glassKind);
                    }
                    break;

                case ControlKind::RichEdit50W:
                case ControlKind::WinForms_RichEdit50W:
                    if (isFull && allowGlassForHwnd)
                    {
                        auto glassKind = (kind == ControlKind::WinForms_RichEdit50W)
                            ? GlassShared::GlassControlKind::WinForms_RichEdit50W
                            : GlassShared::GlassControlKind::RichEdit50W;
                        DebugLog(L"[WndCompat][WinEvent] RichEdit_Attach: hwnd=0x%p kind=%d", hWnd, static_cast<int>(kind));
                        WndCompat::RichEdit_Attach(hWnd, glassKind);
                    }
                    break;

                case ControlKind::Static:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] Static_Attach: hwnd=0x%p", hWnd);
                        WndCompat::Static_Attach(hWnd, GlassShared::GlassControlKind::Static);
                        // Static с изображением (SS_BITMAP/SS_ICON) — доп. субкласс для STM_SETIMAGE.
                        if (WndCompat::StaticHasImageStyle(hWnd))
                            WndCompat::StaticImage_Attach(hWnd);
                    }
                    break;

                case ControlKind::WinForms_Static:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] Static_Attach (WinForms): hwnd=0x%p", hWnd);
                        WndCompat::Static_Attach(hWnd, GlassShared::GlassControlKind::WinForms_Static);
                        if (WndCompat::StaticHasImageStyle(hWnd))
                            WndCompat::StaticImage_Attach(hWnd);
                    }
                    break;

                case ControlKind::TabControl:
                case ControlKind::WinForms_TabControl:
                    if (isFull && allowGlassForHwnd)
                    {
                        DebugLog(L"[WndCompat][WinEvent] TabControl_Attach: hwnd=0x%p", hWnd);
                        WndCompat::TabControl_Attach(hWnd);
                    }
                    break;

                case ControlKind::InternetExplorer_Server:
                    if (isFull && allowGlassForHwnd)
                    {
                        const DWORD curTid = GetCurrentThreadId();
                        DWORD       wndPid = 0;
                        const DWORD wndTid = GetWindowThreadProcessId(hWnd, &wndPid);
                        if (wndTid != 0 && wndTid != curTid)
                        {
                            InstallGetMsgHookForThread(wndTid);
                            if (!PostMessageW(hWnd, WndCompat::WM_GLASS_ATTACH_IESERVER, 0, 0))
                            {
                                DebugLog(
                                    DBG_WARNING,
                                    L"[WndCompat][WinEvent] WM_GLASS_ATTACH_IESERVER PostMessage failed hwnd=0x%p GLE=%u",
                                    hWnd,
                                    GetLastError());
                            }
                        }
                        else
                        {
                            WndCompat::IeServer_Attach(hWnd);
                        }
                    }
                    break;

				case ControlKind::Unknown:
                    if (isFull && allowGlassForHwnd)
                    {
                        LogWinEventRootAttach(L"Unknown", hWnd);
                        DispatchRootAttachToOwnerThread(hWnd);
                    }
                    break;
                default:
                    break;
                }

                break; // предотвращает fall-through в case WindowRole::Root
            }

            // ── Tray (панель задач) — Tray_Attach → Root_Attach + GlassCore backdrop только в Full ──
            case WindowRole::Tray:
                 if (g_mode == Shared::InjectionMode::Full)
                 {
                     LogWinEventRootAttach(L"Tray", hWnd);
                     WndCompat::Tray_Attach(hWnd);
                 }
                 break;

                // ── Root (top-level): две дорожки к сабклассу — только здесь, без смешивания в Root_Attach.
            case WindowRole::Root:
                if (g_mode == Shared::InjectionMode::Full)
                {
                    if (!WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd))
                        break;

                    // Win11: shell-проводник → отдельный ExplorerRootSubclassProc (DwmExtendFrame IAT и т.д.).
                    // Win10: shell-проводник идёт по общему пути RootSubclassProc (такая же SWCA-политика, как у прочих root).
                    if (kind == ControlKind::FileExplorerShellRoot
                        && Shared::SystemHelper::g_buildNumber >= 22000)
                    {
                        LogWinEventRootAttach(L"Root.ExplorerShell→ExplorerRootSubclassProc", hWnd);
                        DispatchExplorerRootAttachToOwnerThread(hWnd);
                    }
                    else
                    {
                        LogWinEventRootAttach(L"Root.OtherTopLevel→RootSubclassProc", hWnd);
                        DispatchRootAttachToOwnerThread(hWnd);
                        if (WindowClassifier::GetControlKind(WndCompat::NormalizeWindowClass(hWnd))
                            == ControlKind::WinForms_Window)
                        {
                            bool markedWinFormsWindow = false;
                            if (GlassShared::GlassGetWindowKind(hWnd) == GlassShared::GlassControlKind::None)
                            {
                                GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::WinForms_Window);
                                markedWinFormsWindow = true;
                            }
                            if (markedWinFormsWindow)
                                WndCompat::ScheduleThemedChromeRemigrateAfterWinFormsWindowMarked(hWnd);
                        }
                    }
                }
                break;

            default:
                break;
            }
            break;
        }

        case EVENT_OBJECT_DESTROY:
        {
            if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
                break;

            Shared::GlassHostRegistry::Remove(hWnd);

            auto [role, kind] = WindowClassifier::Classify(hWnd);
            if (g_mode == Shared::InjectionMode::Full && role == WindowRole::Tray)
            {
                WndCompat::DispatchTrayDetachToOwnerThread(hWnd);
            }
            // Только оболочка ленты Office — не путать с системным #32768 (IsPopupMenu): для меню — Menu_Detach ниже.
            if (kind == ControlKind::NetUiToolWindow)
            {
                WndCompat::NetUiToolWindow_Detach(hWnd);
                DebugLog(L"[WndCompat][WinEvent] NetUiToolWindow_Detach: hwnd=0x%p", hWnd);
            }
            if (role == WindowRole::Popup)
            {
                // Safety net: основная очистка уже выполнена в WM_NCDESTROY.
                WndCompat::Menu_Detach(hWnd);
                DebugLog(L"[WndCompat][WinEvent] Menu_Detach (safety): hwnd=0x%p", hWnd);
                if (WndCompat::IsWindowClass(hWnd, TOOLTIPS_CLASSW) ||
                    WndCompat::IsWindowClass(hWnd, L"MicrosoftWindowsTooltip") ||
                    WndCompat::IsWindowClass(hWnd, L"THintWindow"))
                    WndCompat::Tooltip_Detach(hWnd);
                if (WndCompat::IsWindowClass(hWnd, L"KbxLabelClass"))
                    WndCompat::KbxLabel_Detach(hWnd);
            }
            if (kind == ControlKind::DirectUI)
                WndCompat::DirectUI_Detach(hWnd);
            // Субкласс WinForms-кнопки убран — снимаем маркер и запись в списке при уничтожении окна.
            if (kind == ControlKind::Button || kind == ControlKind::WinForms_Button)
                WndCompat::Button_Detach(hWnd);
            if (kind == ControlKind::InternetExplorer_Server)
                WndCompat::IeServer_Detach(hWnd);
            if (kind == ControlKind::WinUiContentBridge)
                WndCompat::WinUiContentBridge_Detach(hWnd);
            if (kind == ControlKind::WinUiDesktopBridge)
                WndCompat::WinUiDesktopBridge_Detach(hWnd);
            // Detach NC-surface только в host-процессе (не трогать CoreWindow в explorer).
            if (kind == ControlKind::ShellNotificationCore
                || kind == ControlKind::ShellStartMenuCore
                || (Shared::RootBackdropPolicy::IsCoreWindowHostProcess()
                    && WndCompat::IsWindowClass(hWnd, L"Windows.UI.Core.CoreWindow"))
                || (Shared::RootBackdropPolicy::IsShellNotificationHostProcess()
                    && WndCompat::IsWindowClass(hWnd, L"ControlCenterWindow")))
                WndCompat::CoreWindowHost_Detach(hWnd);
            if (kind == ControlKind::TrackBar)
                WndCompat::TrackBar_Detach(hWnd);
            if (kind == ControlKind::UpDown)
                WndCompat::UpDown_Detach(hWnd);
            if (kind == ControlKind::ScrollBar)
                WndCompat::ScrollBar_Detach(hWnd);
            if (kind == ControlKind::ListBox || kind == ControlKind::WinForms_ListBox)
                WndCompat::ListBox_Detach(hWnd);
            if (kind == ControlKind::StatusBar)
                WndCompat::StatusBar_Detach(hWnd);
            if (kind == ControlKind::ProgressBar)
                WndCompat::ProgressBar_Detach(hWnd);
            if (kind == ControlKind::DateTimePicker || kind == ControlKind::WinForms_DateTimePicker)
                WndCompat::DateTimePicker_Detach(hWnd);
            if (kind == ControlKind::ComboBox || kind == ControlKind::WinForms_ComboBox)
                WndCompat::ComboBox_Detach(hWnd);
            break;
        }
        case EVENT_OBJECT_HIDE:
        {
            if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF)
                break;
            auto [role, kind] = WindowClassifier::Classify(hWnd);
            break;
        }

        default:
            break;
        }
    }
}

namespace WndCompat
{
    void DispatchTrayAttachToOwnerThread(HWND hRoot) noexcept
    {
        if (!IsWindow(hRoot))
            return;
        if (WndCompat::IsShuttingDown())
            return;
        DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);
        const DWORD curTid = GetCurrentThreadId();
        if (wndTid != 0 && wndTid != curTid)
        {
            if (g_installGetMsgHookForThread_ptr)
                g_installGetMsgHookForThread_ptr(wndTid);
            if (!PostMessageW(hRoot, WndCompat::WM_GLASS_ATTACH_ROOT, 0, 0))
            {
                DebugLog(DBG_WARNING,
                    L"[WndCompat][WinEvent] DispatchTrayAttachToOwnerThread: PostMessage failed hwnd=0x%p GLE=%u",
                    hRoot,
                    GetLastError());
            }
            return;
        }
        WndCompat::TaskbarHost_Attach(hRoot);
    }

    void DispatchTrayDetachToOwnerThread(HWND hWnd) noexcept
    {
        if (!IsWindow(hWnd))
            return;
        // При shutdown платформы IsShuttingDown уже true до WinEventHook_Shutdown — detach трея обязаны выполнять всё равно.
        const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
        const DWORD curTid = GetCurrentThreadId();
        if (wndTid != 0 && wndTid != curTid)
        {
            if (g_installGetMsgHookForThread_ptr)
                g_installGetMsgHookForThread_ptr(wndTid);
            if (!PostMessageW(hWnd, WndCompat::WM_GLASS_DETACH_TASKBAR_TRAY, 0, 0))
            {
                DebugLog(DBG_WARNING,
                    L"[WndCompat][WinEvent] DispatchTrayDetachToOwnerThread: PostMessage failed hwnd=0x%p GLE=%u",
                    hWnd,
                    GetLastError());
            }
            return;
        }
        WndCompat::TaskbarHost_Detach(hWnd);
    }

    void DispatchTrayReapplyBackdropToOwnerThread(HWND hTrayRoot) noexcept
    {
        if (!IsWindow(hTrayRoot))
            return;
        if (WndCompat::IsShuttingDown())
            return;

        const DWORD wndTid = GetWindowThreadProcessId(hTrayRoot, nullptr);
        const DWORD curTid = GetCurrentThreadId();
        if (wndTid != 0 && wndTid != curTid)
        {
            if (g_installGetMsgHookForThread_ptr)
                g_installGetMsgHookForThread_ptr(wndTid);

            if (!PostMessageW(hTrayRoot, WndCompat::WM_GLASS_REAPPLY_TASKBAR_BACKDROP, 0, 0))
            {
                DebugLog(
                    DBG_WARNING,
                    L"[WndCompat][WinEvent] DispatchTrayReapplyBackdrop: PostMessage failed hwnd=0x%p GLE=%u",
                    hTrayRoot,
                    GetLastError());
            }
            return;
        }

        WndCompat::TaskbarHost_HandleReapplyBackdropMessage(hTrayRoot);
    }

    bool WinEventHook_Initialize(Shared::InjectionMode mode)
    {
        // H9: initial scan делает SendMessageTimeout/WndHandler — нельзя держать g_hookMutex
        // (дедлок с чужим GUI-потоком / GetMsgHook). Скан — вне lock (как Detach на Shutdown).
        {
            std::lock_guard<std::mutex> lock(g_hookMutex);

            if (g_hookInstalled.load())
                return true;

            g_mode = mode;

            DebugLog(L"[WndCompat][WinEvent] WinEventHook_Initialize: mode=%u", static_cast<DWORD>(mode));

            if (!g_noCallbacksEvent)
            {
                g_noCallbacksEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
                if (!g_noCallbacksEvent)
                {
                    DebugLog(L"[WndCompat][ERROR] CreateEventW failed, GLE=%u", GetLastError());
                    return false;
                }
            }

            // SetWinEventHook убран: глобальный INCONTEXT-хук теперь устанавливает
            // GlassCompat (EntryRundll.cpp, Main /start) для всех процессов сразу.
            // WndCompat только делает начальный скан окон текущего процесса.

            g_hookInstalled.store(true);
        }

        DebugLog(L"[WndCompat][WinEvent] WinEventHook_Initialize: initial scan, mode=%u",
            static_cast<DWORD>(mode));

        // Начальный скан: прогоняем все существующие окна процесса.
        // Top-level того же потока — напрямую WndHandler; чужого потока — PostMessage(WM_GLASS_ATTACH_ROOT).
        // Дочерние окна: если поток ребёнка ≠ поток скана — тот же PostMessage.
        BackdropPolicyRescan::WindowScanContext ctx{};
        ctx.pid = GetCurrentProcessId();
        ctx.scanTid = GetCurrentThreadId();
        ctx.mode = g_mode;
        ctx.verbose = true;
        ctx.topLevelOnly = true; // H12: не блокировать DoInitialize полным subtree-сканом
        ctx.phase = L"Initial scan";

        BackdropPolicyRescan::RunProcessWindowScan(ctx);
        WndCompat::NotifyInitialRootScanCompleted();

        DebugLog(
            L"[WndCompat][WinEvent] %s: topDirect=%u topPosted=%u childDirect=%u childPosted=%u mode=%u",
            ctx.phase,
            ctx.stats.topDirect,
            ctx.stats.topPosted,
            ctx.stats.childDirect,
            ctx.stats.childPosted,
            static_cast<unsigned>(ctx.mode));

        return true;
    }

    void WinEventHook_ShutdownHostExit()
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);

        if (!g_hookInstalled.load() && !g_winEventHook)
            return;

        if (g_winEventHook)
        {
            UnhookWinEvent(g_winEventHook);
            g_winEventHook = nullptr;

            MSG flushMsg{};
            while (PeekMessageW(&flushMsg, nullptr, 0, 0, PM_REMOVE))
            {
                if (flushMsg.message == WM_QUIT)
                {
                    PostQuitMessage(0);
                    break;
                }
                TranslateMessage(&flushMsg);
                DispatchMessageW(&flushMsg);
            }

            SendNotifyMessageW(HWND_BROADCAST, WM_NULL, 0, 0);
        }

        g_hookInstalled.store(false);
        UninstallAllGetMsgHooks();

        DebugLog(L"[WndCompat][WinEvent] WinEventHook_ShutdownHostExit complete");
    }

    namespace
    {
        struct RemainingGlassMarkCtx final
        {
            DWORD pid{};
            unsigned rootMarks{};
            unsigned otherMarks{};
        };

        BOOL CALLBACK EnumRemainingGlassMarkSubtree(HWND hwnd, LPARAM lParam) noexcept
        {
            auto* ctx = reinterpret_cast<RemainingGlassMarkCtx*>(lParam);
            DWORD pid = 0;
            (void)GetWindowThreadProcessId(hwnd, &pid);
            if (pid != ctx->pid)
                return TRUE;

            const auto kind = GlassShared::GlassGetWindowKind(hwnd);
            if (kind != GlassShared::GlassControlKind::None)
            {
                if (kind == GlassShared::GlassControlKind::Root)
                    ++ctx->rootMarks;
                else
                    ++ctx->otherMarks;
            }

            (void)EnumChildWindows(hwnd, EnumRemainingGlassMarkSubtree, lParam);
            return TRUE;
        }

        [[nodiscard]] RemainingGlassMarkCtx CountRemainingGlassMarks() noexcept
        {
            RemainingGlassMarkCtx ctx{};
            ctx.pid = GetCurrentProcessId();
            (void)EnumWindows(EnumRemainingGlassMarkSubtree, reinterpret_cast<LPARAM>(&ctx));
            return ctx;
        }
    }

    void WinEventHook_Shutdown()
    {
        // HWiNFO/UAF: раньше early-return при !g_hookInstalled пропускал DetachAll,
        // тогда как WndCompat_HandleWinEvent мог навесить субклассы без Initialize-scan
        // (GlassCompat INCONTEXT). FreeLibrary → вызов в unmapped WndCompat.
        // HR4 (log 561350 @75138): SEH после UICompat_Shutdown зависает здесь на
        // DispatchMessage/HWND_BROADCAST — нет uicompat_free_result / FreeSelf.
        const bool cwHost = Shared::RootBackdropPolicy::IsCoreWindowHostProcess();
        {
            std::lock_guard<std::mutex> lock(g_hookMutex);

            if (g_winEventHook)
            {
                UnhookWinEvent(g_winEventHook);
                g_winEventHook = nullptr;

                // WINEVENT_INCONTEXT + idThread=0: при каждом вызове WinEventProc
                // Windows инкрементировал refcount WndCompat.dll в потоке-источнике
                // события.  После UnhookWinEvent система ставит внутреннее cleanup-
                // сообщение (FreeLibrary-notification) в очередь каждого такого потока.
                if (cwHost)
                {
                    Sleep(50);
                }
                else
                {
                    MSG flushMsg;
                    DWORD deadline = GetTickCount() + 200;
                    while (GetTickCount() < deadline)
                    {
                        while (PeekMessageW(&flushMsg, nullptr, 0, 0, PM_REMOVE))
                        {
                            if (flushMsg.message == WM_QUIT)
                            {
                                PostQuitMessage(0);
                                goto flush_done;
                            }
                            TranslateMessage(&flushMsg);
                            DispatchMessageW(&flushMsg);
                        }
                        Sleep(10);
                    }
                flush_done:;
                    SendNotifyMessageW(HWND_BROADCAST, WM_NULL, 0, 0);
                }
            }

            if (g_noCallbacksEvent)
            {
                if (cwHost)
                {
                    (void)WaitForSingleObject(g_noCallbacksEvent, 500);
                }
                else
                {
                    DWORD ncbDeadline = GetTickCount() + 2000;
                    while (true)
                    {
                        DWORD now = GetTickCount();
                        DWORD remaining = (now < ncbDeadline) ? (ncbDeadline - now) : 0;

                        DWORD r = MsgWaitForMultipleObjects(
                            1, &g_noCallbacksEvent,
                            FALSE, remaining, QS_ALLINPUT);

                        if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT || remaining == 0)
                            break;

                        if (r == WAIT_OBJECT_0 + 1)
                        {
                            MSG msg{};
                            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                            {
                                if (msg.message == WM_QUIT)
                                {
                                    PostQuitMessage(0);
                                    goto weh_shutdown_unhook_done;
                                }
                                TranslateMessage(&msg);
                                DispatchMessageW(&msg);
                            }
                        }
                    }
                }
            }

        weh_shutdown_unhook_done:
            g_hookInstalled.store(false);
        }

        // Detach вне g_hookMutex: SendMessageTimeout на чужие GUI-потоки не должен
        // держать lock (иначе риск дедлока с Initialize/SetMode).
        TaskbarHost_DetachAll();
        UninstallAllGetMsgHooks();
        RestoreTrackedControlsWin32Chrome(Win32ChromeRestoreScope::ShutdownDetach);
        const RemainingGlassMarkCtx marks = CountRemainingGlassMarks();
        WndCompat::SubclassDiag::ClearAll();

        DebugLog(L"[WndCompat][WinEvent] WinEventHook_Shutdown complete");
    }

    void WinEventHook_Uninstall()
    {
        WinEventHook_Shutdown();
    }

    bool WinEventHook_IsInstalled()
    {
        return g_hookInstalled.load();
    }

    Shared::InjectionMode WinEventHook_GetMode()
    {
        return g_mode;
    }

    void WinEventHook_SetMode(Shared::InjectionMode mode) noexcept
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        if (g_mode == mode)
            return;

        g_mode = mode;
        DebugLog(L"[WndCompat][WinEvent] WinEventHook_SetMode: mode=%u", static_cast<DWORD>(mode));
    }

    void WinEventHook_ApplyInjectionModeVisuals(
        Shared::InjectionMode oldMode,
        Shared::InjectionMode newMode) noexcept
    {
        const bool wasFull = (oldMode == Shared::InjectionMode::Full);
        const bool isFull = (newMode == Shared::InjectionMode::Full);
        if (wasFull == isFull)
            return;

        if (wasFull && !isFull)
        {
            // Единый NoBackdrop OFF-sweep после SetInjectionMode(non-Full):
            // IsRootGlassVisualsAllowed / Wnd Full-gate → RestoreWin32Chrome.
            DebugLog(DBG_IMPORTANT,
                L"[WndCompat][WinEvent] ApplyInjectionModeVisuals: Full→%u → ReapplyOpenRootBackdrops (unified OFF)",
                static_cast<DWORD>(newMode));

            using ReapplyFn = void(WINAPI*)() noexcept;
            if (const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll"))
            {
                if (const auto pfn = reinterpret_cast<ReapplyFn>(
                        GetProcAddress(hCore, "GlassCore_ReapplyOpenRootBackdrops")))
                {
                    pfn();
                    return;
                }
            }
            // Fallback: тот же applier, что вызывает Reapply при OFF.
            ApplyBackdropPolicyToTrackedRoots();
            return;
        }

        // не-Full → Full: стартовый скан (как WinEventHook_Initialize) + apply tracked roots.
        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][WinEvent] ApplyInjectionModeVisuals: %u→Full → initial-style scan + ApplyBackdropPolicy",
            static_cast<DWORD>(oldMode));

        BackdropPolicyRescan::WindowScanContext ctx{};
        ctx.pid = GetCurrentProcessId();
        ctx.scanTid = GetCurrentThreadId();
        ctx.mode = Shared::InjectionMode::Full;
        ctx.verbose = true;
        ctx.topLevelOnly = true;
        ctx.phase = L"AttachList Full upgrade scan";

        BackdropPolicyRescan::RunProcessWindowScan(ctx);
        NotifyInitialRootScanCompleted();
        ApplyBackdropPolicyToTrackedRoots();

        DebugLog(
            L"[WndCompat][WinEvent] %s: topDirect=%u topPosted=%u childDirect=%u childPosted=%u",
            ctx.phase,
            ctx.stats.topDirect,
            ctx.stats.topPosted,
            ctx.stats.childDirect,
            ctx.stats.childPosted);
    }

    void RescanMissingControlsUnderTrackedRoots(const std::vector<HWND>& roots) noexcept
    {
        BackdropPolicyRescan::RescanMissingControlsUnderTrackedRoots(roots);
    }

    void Tray_Attach(HWND hRoot) noexcept
    {
        DispatchTrayAttachToOwnerThread(hRoot);
    }
}

namespace
{
    [[nodiscard]] GlassShared::GlassControlKind TryResolveGlassMarkKind(
        HWND hWnd,
        WindowRole role,
        ControlKind kind) noexcept
    {
        using namespace GlassShared;
        using GK = GlassControlKind;

        if (role == WindowRole::Root)
        {
            if (WindowClassifier::GetControlKind(WndCompat::NormalizeWindowClass(hWnd))
                == ControlKind::WinForms_Window)
            {
                return GK::WinForms_Window;
            }
            return GK::None;
        }

        if (role != WindowRole::Control)
            return GK::None;

        const bool isFull = (WndCompat::WinEventHook_GetMode() == Shared::InjectionMode::Full);
        const bool allowGlassForHwnd = WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd(hWnd);
        if (!allowGlassForHwnd)
            return GK::None;

        switch (kind)
        {
        case ControlKind::MonthCal:
        {
            const HWND parent = GetParent(hWnd);
            const bool parentIsDropDown = WndCompat::IsWindowClass(parent, L"DropDown");
            if (parentIsDropDown || isFull)
                return GK::MonthCal;
            break;
        }

        case ControlKind::ComboBox:
            if (Shared::AddressBarPie::IsInProgressPieComboSubtree(hWnd))
                break;
            if (isFull || WndCompat::IsWindowClass(GetParent(hWnd), L"FloatNotifySink"))
                return GK::ComboBox;
            break;

        case ControlKind::WinForms_ComboBox:
            if (Shared::AddressBarPie::IsInProgressPieComboSubtree(hWnd))
                break;
            if (isFull || WndCompat::IsWindowClass(GetParent(hWnd), L"FloatNotifySink"))
                return GK::WinForms_ComboBox;
            break;

        case ControlKind::ListBox:
            if (isFull)
                return GK::ListBox;
            break;

        case ControlKind::WinForms_ListBox:
            if (isFull)
                return GK::WinForms_ListBox;
            break;

        case ControlKind::StatusBar:
            if (isFull)
                return GK::StatusBar;
            break;

        case ControlKind::ProgressBar:
            if (isFull)
                return GK::ProgressBar;
            break;

        case ControlKind::DateTimePicker:
            if (isFull)
                return GK::DateTimePicker;
            break;

        case ControlKind::WinForms_DateTimePicker:
            if (isFull)
                return GK::WinForms_DateTimePicker;
            break;

        case ControlKind::ReBar:
        {
            const HWND parent = GetParent(hWnd);
            if (!parent)
                break;
            const bool shellRebar =
                WndCompat::IsWindowClass(parent, L"WorkerW")
                || WndCompat::IsWindowClass(parent, L"SizeableRebar");
            const bool topLevelHostRebar =
                isFull && (GetAncestor(parent, GA_ROOT) == parent);
            if (isFull && (shellRebar || topLevelHostRebar))
                return GK::Rebar;
            break;
        }

        case ControlKind::ToolBar:
        case ControlKind::Atl_ToolBar:
        case ControlKind::Wtl_SearchBand:
            if (isFull || WndCompat::IsWindowClass(GetParent(hWnd), L"ViewControlClass"))
                return GK::Toolbar;
            break;

        case ControlKind::TrackBar:
            if (isFull)
                return GK::TrackBar;
            break;

        case ControlKind::UpDown:
            if (isFull)
                return GK::UpDown;
            break;

        case ControlKind::ScrollBar:
            if (isFull)
                return GK::ScrollBar;
            break;

        case ControlKind::DirectUI:
            if (isFull)
                return GK::DirectUI;
            break;

        case ControlKind::NetUI:
            if (isFull)
                return GK::NetUI;
            break;

        case ControlKind::Button:
            if (isFull)
            {
                const LONG style = GetWindowLongW(hWnd, GWL_STYLE);
                if (!(style & (BS_BITMAP | BS_ICON)))
                    return GK::Button;
            }
            break;

        case ControlKind::WinForms_Button:
            if (isFull)
                return GK::WinForms_Button;
            break;

        case ControlKind::WinForms_Window:
            if (isFull)
                return GK::WinForms_Window;
            break;

        case ControlKind::TreeView:
            if (isFull)
                return GK::TreeView;
            break;

        case ControlKind::ListView:
            if (isFull)
                return GK::ListView;
            break;

        case ControlKind::WinForms_ListView:
            if (isFull)
                return GK::WinForms_ListView;
            break;

        case ControlKind::Header:
            if (isFull)
                return GK::Header;
            break;

        case ControlKind::Edit:
        case ControlKind::Atl_Edit:
            if (isFull)
                return GK::Edit;
            break;

        case ControlKind::WinForms_Edit:
            if (isFull)
                return GK::WinForms_Edit;
            break;

        case ControlKind::RichEdit20W:
            if (isFull)
                return GK::RichEdit20W;
            break;

        case ControlKind::WinForms_RichEdit20W:
            if (isFull)
                return GK::WinForms_RichEdit20W;
            break;

        case ControlKind::RichEdit50W:
            if (isFull)
                return GK::RichEdit50W;
            break;

        case ControlKind::WinForms_RichEdit50W:
            if (isFull)
                return GK::WinForms_RichEdit50W;
            break;

        case ControlKind::Static:
            if (isFull)
                return GK::Static;
            break;

        case ControlKind::WinForms_Static:
            if (isFull)
                return GK::WinForms_Static;
            break;

        case ControlKind::TabControl:
        case ControlKind::WinForms_TabControl:
            if (isFull)
                return GK::TabControl;
            break;

        case ControlKind::InternetExplorer_Server:
            if (isFull)
                return GK::InternetExplorer_Server;
            break;

        case ControlKind::ComboBoxEx32:
        case ControlKind::GenericContainer:
        case ControlKind::Unknown:
        default:
            break;
        }

        return GK::None;
    }
} // namespace

// Catch-up GlassMarkWindow без *_Attach / Root_Attach (ThemePaint Register bucket).
extern "C" __declspec(dllexport)
void WINAPI WndCompat_EnsureWinFormsWindowMarked(HWND hWnd) noexcept
{
    if (!hWnd || !IsWindow(hWnd) || WndCompat::IsShuttingDown())
        return;
    if (GlassShared::GlassGetWindowKind(hWnd) != GlassShared::GlassControlKind::None)
        return;

    const auto [role, kind] = WindowClassifier::Classify(hWnd);
    const GlassShared::GlassControlKind gk = TryResolveGlassMarkKind(hWnd, role, kind);
    if (gk == GlassShared::GlassControlKind::None)
        return;

    GlassShared::GlassMarkWindow(hWnd, gk);
    DebugLog(DBG_IMPORTANT,
        L"[WndCompat][MarkCatchUp] hWnd=0x%p role=%d kind=%d glassKind=%u",
        hWnd,
        static_cast<int>(role),
        static_cast<int>(kind),
        static_cast<unsigned>(static_cast<std::uint8_t>(gk)));
    if (gk == GlassShared::GlassControlKind::WinForms_Window)
        WndCompat::ScheduleThemedChromeRemigrateAfterWinFormsWindowMarked(hWnd);
}

// ── Провайдер для GlassCompat ────────────────────────────────────────────────
//
// GlassCompat устанавливает глобальный INCONTEXT WinEvent-хук и при каждом
// событии делегирует сюда.  Функция является публичным эквивалентом
// внутреннего WinEventProc: выполняет ту же классификацию и dispatch.
//
extern "C" __declspec(dllexport)
void WINAPI WndCompat_HandleWinEvent(
    DWORD event,
    HWND  hWnd,
    LONG  idObject,
    LONG  idChild,
    DWORD dwEventThread,
    DWORD dwmsEventTime)
{
    // Маршрутизируем через внутренний WinEventProc — там все фильтры,
    // классификация окон и вызовы *_Attach/*_Detach.
    WndHandler(nullptr, event, hWnd, idObject, idChild, dwEventThread, dwmsEventTime);
}
