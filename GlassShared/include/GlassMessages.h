#pragma once
#include <windows.h>

// Централизованный реестр всех WM_APP+ констант платформы GlassPlatform.
//
// Правило распределения диапазонов:
//   0x200..0x2FF — WndCompat: cross-thread управление субклассами
//   0x700..0x7FF — WndCompat: внутренние shutdown-сообщения
//   0x800..0x8FF — ExplorerCompat (зарезервировано)
//   0x900..0x9FF — TaskbarCompat (зарезервировано)
//
// LogViewer (WM_APP+1, WM_APP+2, WM_APP+100) — отдельный процесс,
// коллизий с WndCompat нет, объявления остаются в LogViewer.

namespace WndCompat
{
    // ── Cross-thread управление субклассами (0x200..0x2FF) ───────────────────

    // 0x200 зарезервирован

    // PostMessage → GetMsgHookProc (поток-владелец) → Root_Attach
    inline constexpr UINT WM_GLASS_ATTACH_ROOT      = WM_APP + 0x201;

    // SendMessageTimeout → RootSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_ROOT      = WM_APP + 0x202;

    // SendMessageTimeout → ComboLBoxSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_COMBOLVOX = WM_APP + 0x203;

    // SendMessageTimeout → ComboBoxSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_COMBOBOX  = WM_APP + 0x204;

    // SendMessageTimeout → TooltipSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_TOOLTIP   = WM_APP + 0x205;

    // SendMessageTimeout → KbxLabelSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_KBXLABEL  = WM_APP + 0x206;

    // 0x207 зарезервирован (ранее WM_GLASS_DETACH_BUTTON — SubclassProc удалён)

    // 0x208 зарезервирован (ранее WM_GLASS_DETACH_TREEVIEW — SubclassProc удалён)

    // SendMessageTimeout → TabControlSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_TABCONTROL = WM_APP + 0x209;

    // Зарезервировано (ранее субкласс трекбара; сейчас TrackBarHost без WM_APP-detach).
    inline constexpr UINT WM_GLASS_DETACH_TRACKBAR   = WM_APP + 0x20A;

    // SendMessageTimeout → DateTimePickerSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_DATETIMEPICKER = WM_APP + 0x20B;

    // SendMessageTimeout → RebarSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_REBAR      = WM_APP + 0x20C;

    // 0x211–0x212 зарезервированы (ранее IE_Server APPLY/DETACH при субклассе; сессия — PaintDispatcher + GlassMark).

    // 0x213 зарезервирован (ранее WM_GLASS_IESERVER_NATIVE_SWEEP — откатили)

    // PostMessage → GetMsgHookProc (поток-владелец HWND) → IeServer_Attach
    inline constexpr UINT WM_GLASS_ATTACH_IESERVER = WM_APP + 0x214;

    // PostMessage → GetMsgHookProc → TaskbarHost_Detach (Shell_*TrayWnd без субкласса).
    inline constexpr UINT WM_GLASS_DETACH_TASKBAR_TRAY = WM_APP + 0x217;

    // PostMessage → GetMsgHookProc → TaskbarHost_HandleReapplyBackdropMessage (CornerType/SWCA).
    inline constexpr UINT WM_GLASS_REAPPLY_TASKBAR_BACKDROP = WM_APP + 0x219;

    // SendMessageTimeout (AnimationWorker) → MenuSubclassProc → OnPaintFlyoutHostBorder + TraverseLog NC.
    inline constexpr UINT WM_GLASS_PAINT_FLYOUT_HOST_BORDER = WM_APP + 0x21A;

    // SendMessageTimeout → RootSubclassProc → ApplyRootBackdropReapply на потоке-владельце (GlassGuiThread sweep).
    inline constexpr UINT WM_GLASS_REAPPLY_ROOT_BACKDROP = WM_APP + 0x21B;

    // SendMessageTimeout → RootSubclassProc → RedrawWindow top-level (ThemePaint reload с чужого потока).
    inline constexpr UINT WM_GLASS_INVALIDATE_TOPLEVEL_VISUAL = WM_APP + 0x21C;

    // PostMessage → RootSubclassProc → GlassCore SoftInvalidateTree (InvalidateRect chrome вне nested NCACTIVATE).
    inline constexpr UINT WM_GLASS_DEFER_THEMED_CHROME_INVALIDATE = WM_APP + 0x21D;

    // PostMessage → RootSubclassProc → GlassCore SoftInvalidateTextGlowClientsForRoot (Moderate/Global only).
    inline constexpr UINT WM_GLASS_DEFER_TEXT_GLOW_INVALIDATE = WM_APP + 0x222;

    // PostMessage → RootSubclassProc → GlassCore SoftInvalidateTextInkClientsForGaRootTree (Edit/RichEdit + WinForms).
    inline constexpr UINT WM_GLASS_DEFER_TEXT_INK_INVALIDATE = WM_APP + 0x223;

    // PostMessage → RootSubclassProc → remigrate chrome buckets после mark WinForms_Window.
    inline constexpr UINT WM_GLASS_REMIGRATE_THEMED_CHROME = WM_APP + 0x21E;

    // PostMessage (GetMsgHook) / RootSubclassProc → Root_AttachGlass / ExplorerRoot::AttachGlass.
    inline constexpr UINT WM_GLASS_ATTACH_ROOT_GLASS = WM_APP + 0x21F;

    // SendMessageTimeout / RootSubclassProc → Root_DetachGlass / ExplorerRoot::DetachGlass (реестр сохраняется).
    inline constexpr UINT WM_GLASS_DETACH_ROOT_GLASS = WM_APP + 0x220;

    // PostMessage → GetMsgHookProc (поток-владелец) → WndHandler(EVENT_OBJECT_CREATE) для Control/Special/Popup.
    // Не использовать для top-level Root — только WM_GLASS_ATTACH_ROOT.
    inline constexpr UINT WM_GLASS_SYNTHETIC_CREATE = WM_APP + 0x221;

    // PostMessage → ControlWndProc: один process-wide OnRootDwmColorizationChanged (coalesce wave).
    inline constexpr UINT WM_GLASS_DWMCOLORIZATION_DISPATCH = WM_APP + 0x218;

    /// lParam live PostMessage WM_DWMCOLORIZATIONCHANGED (wParam=0): pivot Root\\Active|Inactive + флаги.
    inline constexpr LPARAM kGlassLiveRootScopeInfer    = 0;
    inline constexpr LPARAM kGlassLiveRootScopeActive   = 1;
    inline constexpr LPARAM kGlassLiveRootScopeInactive = 2;
    /// Только border/gradient/corner — без DWM blur/SBT reset (live preview border/gradient).
    inline constexpr LPARAM kGlassLiveRootScopeCompositionOnly = 0x10000;
    /// Только перерисовка клиента — без border/SWCA и без RDW_FRAME (live preview text glow).
    inline constexpr LPARAM kGlassLiveRootScopeRepaintOnly = 0x20000;

    [[nodiscard]] inline constexpr LPARAM GlassLiveRootScopePivot(LPARAM liveRootScope) noexcept
    {
        return liveRootScope & 0xFFFF;
    }

    [[nodiscard]] inline constexpr bool GlassLiveRootScopeIsCompositionOnly(LPARAM liveRootScope) noexcept
    {
        return (liveRootScope & kGlassLiveRootScopeCompositionOnly) != 0;
    }

    [[nodiscard]] inline constexpr bool GlassLiveRootScopeIsRepaintOnly(LPARAM liveRootScope) noexcept
    {
        return (liveRootScope & kGlassLiveRootScopeRepaintOnly) != 0;
    }

    // 0x20D зарезервирован (ранее WM_GLASS_DETACH_TOOLBAR — SubclassProc удалён)

    // SendMessageTimeout → EditSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_EDIT       = WM_APP + 0x20E;

    // SendMessageTimeout → IslandContainerSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_EXPLORER_ISLAND = WM_APP + 0x215;

    // 0x216 зарезервирован (ранее WM_GLASS_DETACH_DIRECTUI — субкласс DirectUI отключён)
    
    // SendMessageTimeout → MonthCalSubclassProc → RemoveWindowSubclass
    inline constexpr UINT WM_GLASS_DETACH_MONTHCAL   = WM_APP + 0x20F;

    // 0x210 зарезервирован (ранее WM_GLASS_DETACH_HEADER — SubclassProc удалён)

    // SendMessageTimeout → StaticImageSubclassProc → RemoveWindowSubclass (Static с изображением).
    inline constexpr UINT WM_GLASS_DETACH_STATIC     = WM_APP + 0x224;

    // ── WndCompat: внутренний shutdown (0x700..0x7FF) ────────────────────────

    // PostMessage → ControlWndProc → ExecuteShutdownCore
    inline constexpr UINT WM_APP_GUI_SHUTDOWN       = WM_APP + 0x701;

    // ── ExplorerCompat (0x800..0x8FF) — см. также namespace ExplorerCompatMsg ──

    // inline constexpr UINT WM_EXPLORERCOMPAT_SHUTDOWN = WM_APP + 0x800;
}

// Сообщения ExplorerCompat вне namespace WndCompat (избегаем смешения префиксов).
namespace ExplorerCompatMsg
{
    // SendMessageTimeout → XamlShutdownMarshal::ShutdownWndProc → ShutdownVisualTreeWatcher (STA XAML).
    inline constexpr UINT WM_GLASS_EXPLORER_XAML_SHUTDOWN = WM_APP + 0x801;

    // PostMessage на поток SetSite: отложенный AdviseVisualTreeChange (избегаем взаимной блокировки с WinEvent).
    inline constexpr UINT WM_GLASS_EXPLORER_XAML_ADVISE = WM_APP + 0x802;

    // PostMessage на CabinetWClass/ExploreWClass: перерисовка NC/DWM после WinUI SetProperty (только поток-владелец HWND).
    inline constexpr UINT WM_GLASS_EXPLORER_CHROME_REDRAW = WM_APP + 0x803;

    // SendMessageTimeout → UwpShutdownMarshal::ShutdownWndProc → UwpXamlTapSite::ShutdownWatcher (UWP STA XAML).
    inline constexpr UINT WM_GLASS_EXPLORER_XAML_UWP_SHUTDOWN = WM_APP + 0x804;

    // PostMessage после ExplorerRoot::Attach: SWCA+chrome, если WM_SHOWWINDOW пришёл до SetWindowSubclass.
    inline constexpr UINT WM_GLASS_EXPLORER_SHELL_BACKDROP_REFRESH = WM_APP + 0x805;

    // SendMessageTimeout → ShutdownWndProc → OnRootBackdropEnabled/Disabled (STA XAML).
    // wParam: 0 = OFF (NoBackdrop), nonzero = ON.
    inline constexpr UINT WM_GLASS_EXPLORER_ROOT_BACKDROP_POLICY = WM_APP + 0x806;

    // Post SyncExplorerFrame при live NoBackdrop OFF — канонический redraw (как shutdown).
    inline constexpr UINT WM_GLASS_EXPLORER_POST_LIVE_DISABLE_REDRAW = WM_APP + 0x807;

    // SendMessageTimeout на CabinetWClass/ExploreWClass: полный RedrawWindow root+children (OFF/STOP/attach).
    inline constexpr UINT WM_GLASS_EXPLORER_ROOT_FULL_REDRAW = WM_APP + 0x808;

    // SendMessageTimeout на потоке InitializeXamlDiagnosticsEx → CoFreeUnusedLibraries (STA IXDE).
    inline constexpr UINT WM_GLASS_EXPLORER_IXDE_COM_RECLAIM = WM_APP + 0x809;

    // SendMessageTimeout на потоке-владельце HWND_MESSAGE → DestroyWindow (нельзя DestroyWindow с чужого tid).
    inline constexpr UINT WM_GLASS_EXPLORER_DESTROY_SHUTDOWN_WND = WM_APP + 0x80A;

    // Класс HWND_MESSAGE маршалинга; WndCompat::Root_Attach не должен субклассить (см. RootSubclass.cpp).
    inline constexpr wchar_t kExplorerXamlShutdownWndClass[] = L"GlassExplorerCompatXamlShutdown";

    // Класс HWND_MESSAGE для CoFreeUnusedLibraries на потоке IXDE (WinEvent/bridge notify).
    inline constexpr wchar_t kExplorerIxdeComReclaimWndClass[] = L"GlassExplorerCompatIxdeComReclaim";

    // Класс HWND_MESSAGE для UWP shutdown маршалинга.
    inline constexpr wchar_t kExplorerXamlUwpShutdownWndClass[] = L"GlassExplorerCompatXamlUwpShutdown";
}

namespace GlassSettingsMsg
{
    // PostMessage worker → GlobalPage: завершение register/unregister shell extension.
    // wParam: 1 = success, 0 = failure; lParam unused.
    inline constexpr UINT WM_GLASS_SHELL_EXT_OP_COMPLETE = WM_APP + 0xA01;

    // PostMessage worker → GlobalPage: завершение install/remove задачи BWM в Планировщике.
    // wParam: 1 = success, 0 = failure; lParam unused.
    inline constexpr UINT WM_GLASS_PLATFORM_TASK_OP_COMPLETE = WM_APP + 0xA02;

    // PostMessage ShellExt / second instance → MainWindow: restore + foreground (WinUI AppWindow).
    // wParam/lParam unused.
    inline constexpr UINT WM_GLASS_SETTINGS_ACTIVATE = WM_APP + 0xA03;
}

namespace GlassCompatMsg
{
    // PostThreadMessage → GlassGuiThread: повторная загрузка модулей после per-PID soft unload.
    inline constexpr UINT WM_GLASS_MODULES_REINJECT = WM_APP + 0xB01;

    // PostThreadMessage → GlassGuiThread: LoadLibrary TaskbarCompat после Tray-аттача (ShellTray).
    inline constexpr UINT WM_GLASS_ENSURE_TASKBAR_COMPAT = WM_APP + 0xB02;

    // PostThreadMessage → GlassGuiThread: LoadLibrary ExplorerCompat после FileExplorerShellRoot.
    inline constexpr UINT WM_GLASS_ENSURE_EXPLORER_COMPAT = WM_APP + 0xB03;

    // PostThreadMessage → GlassGuiThread: LoadLibrary UICompat после CoreWindow-attach.
    inline constexpr UINT WM_GLASS_ENSURE_UI_COMPAT = WM_APP + 0xB04;

    // PostThreadMessage → GlassGuiThread: OOC/remote CoreWindow SHOW (wParam = HWND).
    inline constexpr UINT WM_GLASS_COREWINDOW_REMOTE_SHOW = WM_APP + 0xB05;

    // PostThreadMessage → GlassGuiThread: AttachExisting CoreWindow (cloaked OK) после initDone.
    inline constexpr UINT WM_GLASS_COREWINDOW_ATTACH_EXISTING = WM_APP + 0xB06;
}

namespace UICompatMsg
{
    // SendMessageTimeout → UiTapSite marshal — Restore + Unadvise на STA IXDE.
    inline constexpr UINT WM_GLASS_UI_XAML_SHUTDOWN = WM_APP + 0xC01;

    // PostMessage → marshal — ReloadSettingsAndReapply на STA.
    inline constexpr UINT WM_GLASS_UI_REAPPLY_SETTINGS = WM_APP + 0xC02;

    // PostMessage (отложенный) → CatchUpExistingNcPanels на UI-потоке marshal
    // CatchUp NC на UI-потоке через marshal
    // (CreateThread + Window.Current даёт hasWindow=0).
    inline constexpr UINT WM_GLASS_UI_CATCHUP_NC = WM_APP + 0xC03;
    // ShellSurface: classify/capture/apply после выхода из Advise (не на стеке VTC).
    inline constexpr UINT WM_GLASS_UI_SHELL_PREPARE = WM_APP + 0xC04;
    // Theme/visual style: force re-brush без ClearValue (не общий REAPPLY_SETTINGS).
    inline constexpr UINT WM_GLASS_UI_THEME_BRUSH_REFRESH = WM_APP + 0xC05;

    inline constexpr wchar_t kUiXamlShutdownWndClass[] = L"GlassUICompatXamlShutdown";
}

namespace TaskbarCompatMsg
{
    // SendMessageTimeout → TaskbarTapSite marshal WndProc — RestoreFill + Unadvise на потоке SetSite/IXDE.
    inline constexpr UINT WM_GLASS_TASKBAR_XAML_SHUTDOWN = WM_APP + 0x901;

    // PostMessage → marshal WndProc — применение патча Rectangle (BackgroundFill/Stroke) на STA-потоке SetSite.
    inline constexpr UINT WM_GLASS_TASKBAR_VTC_APPLY_RECT = WM_APP + 0x902;

    // PostMessage → marshal WndProc — ReloadSettingsAndReapply на STA-потоке TAP (fix RPC_E_WRONG_THREAD).
    inline constexpr UINT WM_GLASS_TASKBAR_REAPPLY_SETTINGS = WM_APP + 0x903;

    // SendMessageTimeout на STA IXDE-потоке → CoFreeUnusedLibraries после shutdown.
    inline constexpr UINT WM_GLASS_TASKBAR_IXDE_COM_RECLAIM = WM_APP + 0x904;

    // PostMessage → marshal — запуск reassert 12×4s при OG (из чужого потока).
    inline constexpr UINT WM_GLASS_TASKBAR_START_OG_REASSERT = WM_APP + 0x905;

    // SendMessage → marshal — снять отложенные таймеры до RunShutdownSession (Stop платформы).
    inline constexpr UINT WM_GLASS_TASKBAR_CANCEL_TIMERS = WM_APP + 0x906;

    // Класс HWND_MESSAGE; WndCompat::Root_Attach не должен субклассить (как ExplorerCompat XAML shutdown).
    inline constexpr wchar_t kTaskbarXamlShutdownWndClass[] = L"GlassTaskbarCompatXamlShutdown";
}
