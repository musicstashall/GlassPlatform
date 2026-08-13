#include "WndPch.h"
#include "Tracker/WindowClassifier.h"
#include "Tracker/WindowClassUtils.h"
#include <ExplorerCompatHostPolicy.h>
#include <RootBackdropPolicy.h>
#include <SystemHelper.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

// Дочернее окно внутри дерева Progman (рабочий стол) или панели задач — не классифицируем.
// Иначе SysListView32 рабочего стола и прочие контроли получают ListView_Attach и т.д. (BWM WinHandler).
static bool IsUnderShellDesktopOrTrayRoot(HWND hwnd) noexcept
{
    HWND hRoot = GetAncestor(hwnd, GA_ROOT);
    if (!hRoot || hRoot == hwnd)
        return false;
    return WndCompat::IsWindowClass(hRoot, L"Progman");
}

// Строгое сравнение без учёта регистра, БЕЗ снятия точки.
// Точка сохраняется как признак WinForms-класса.
static bool Equals(const std::wstring& a, const wchar_t* b)
{
    return _wcsicmp(a.c_str(), b) == 0;
}

[[nodiscard]] static bool HasPrefixI(const std::wstring& cls, const wchar_t* pref, size_t n) noexcept
{
    return cls.size() >= n && _wcsnicmp(cls.c_str(), pref, n) == 0;
}

// Подстрока без учёта регистра (для фильтров BWM-стиля).
[[nodiscard]] static bool ContainsSubstrI(const std::wstring& s, const wchar_t* needle) noexcept
{
    const size_t n = wcslen(needle);
    if (s.size() < n)
        return false;
    for (size_t i = 0; i + n <= s.size(); ++i)
    {
        if (_wcsnicmp(s.c_str() + i, needle, n) == 0)
            return true;
    }
    return false;
}

// ATL: + непустой суффикс из [0-9A-Fa-f] — нестабильное имя vtable; GenericContainer + Root_Attach (паритет BWM WinSubclassProc).
[[nodiscard]] static bool IsAtlHexSuffixClass(const std::wstring& cls) noexcept
{
    if (!HasPrefixI(cls, L"ATL:", 4))
        return false;
    const wchar_t* tail = cls.c_str() + 4;
    if (*tail == L'\0')
        return false;
    for (const wchar_t* p = tail; *p; ++p)
    {
        const wchar_t c = *p;
        if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F') || (c >= L'a' && c <= L'f')))
            return false;
    }
    return true;
}

// Шум / неподдерживаемые ATL/WTL → Ignored до контейнера 5.5 (без Root_Attach).
[[nodiscard]] static bool IsAtlWtlNoiseOrUnknown(const std::wstring& cls) noexcept
{
    if (ContainsSubstrI(cls, L".NET-BroadcastEventWindow"))
        return true;
    // Не затрагивает «AfxWnd42u» и подобные без подстроки «Afx:».
    if (ContainsSubstrI(cls, L"Afx:"))
        return true;

    if (HasPrefixI(cls, L"WTL_", 4) && !Equals(cls, L"WTL_SearchBand"))
        return true;

    if (!HasPrefixI(cls, L"ATL:", 4))
        return false;

    // ATL:+hex — WS_CHILD-контейнер (BWM: !IsControlSubClass → WinSubclassProc), не шум.
    if (IsAtlHexSuffixClass(cls))
        return false;

    const wchar_t* tail = cls.c_str() + 4;
    if (*tail == L'\0')
        return true;

    if (Equals(cls, L"ATL:EDIT") ||
        Equals(cls, L"ATL:ToolbarWindow32") ||
        Equals(cls, L"ATL:LISTBOX"))
        return false;

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ЖЁСТКИЙ БЛОК‑ЛИСТ (всегда Ignored)
// ─────────────────────────────────────────────────────────────────────────────
bool WindowClassifier::IsHardBlockedClass(const std::wstring& cls)
{
    // Единый список строк: Shared::RootBackdropPolicy::IsNormalizedClassBackdropExempt (RootBackdropPolicy.cpp).
    return Shared::RootBackdropPolicy::IsNormalizedClassBackdropExempt(cls);
}

// ─────────────────────────────────────────────────────────────────────────────
//  МЯГКИЙ БЛОК‑ЛИСТ (Ignored, если не Root)
// ─────────────────────────────────────────────────────────────────────────────
bool WindowClassifier::IsSoftBlockedClass(const std::wstring& cls)
{
    static const std::array softList
    {
        L"Breadcrumb Parent",
        L"TrayClockWClass",
        L"ApplicationFrameWindow",
        L"ApplicationFrameTitleBarWindow",
        L"SearchEditBoxWrapperClass",
        L"Windows.Internal.Shell.TabProxyWindow",
    };

    return std::ranges::any_of(softList, [&](auto s) { return Equals(cls, s); });
}

// ─────────────────────────────────────────────────────────────────────────────
//  TRAY
// ─────────────────────────────────────────────────────────────────────────────
bool WindowClassifier::IsTrayWindow(const std::wstring& cls)
{
    return Equals(cls, L"Shell_TrayWnd") ||
        Equals(cls, L"Shell_SecondaryTrayWnd");
}

// ─────────────────────────────────────────────────────────────────────────────
//  POPUP (меню, тултипы)
// ─────────────────────────────────────────────────────────────────────────────
bool WindowClassifier::IsPopupWindow(HWND hwnd, const std::wstring& cls)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_POPUP))
        return false;

    if (Equals(cls, L"#32768"))           return true;  // системное меню
    if (Equals(cls, L"tooltips_class32")) return true;  // тултипы Win32
    if (Equals(cls, L"MicrosoftWindowsTooltip")) return true;  // WinUI/оболочка (класс как у tooltips_class32)
    if (Equals(cls, L"THintWindow"))      return true;  // Delphi тултипы
    if (Equals(cls, L"KbxLabelClass"))    return true;  // Delphi label popup

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SPECIAL (DropDown-специфичные popup-окна)
// ─────────────────────────────────────────────────────────────────────────────
bool WindowClassifier::IsSpecialWindow(const std::wstring& cls, ControlKind& outKind)
{
    if (Equals(cls, L"SHELLDLL_DefView") ||
        Equals(cls, L"ShellTabWindowClass") ||
        Equals(cls, L"DetailsPaneHwndHostClass") ||
        Equals(cls, L"FileExplorer_FolderViewIsland"))
    {
        outKind = ControlKind::FileExplorerIslandContainer;
        return true;
    }

    if (Equals(cls, L"ListviewPopup"))
    {
        outKind = ControlKind::ListviewPopup;
        return true;
    }
    if (Equals(cls, L"ViewControlClass"))
    {
        outKind = ControlKind::ViewControl;
        return true;
    }
    if (Equals(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge"))
    {
        outKind = ControlKind::WinUiDesktopBridge;
        return true;
    }
    if (Equals(cls, L"Windows.UI.Composition.DesktopWindowContentBridge"))
    {
        outKind = ControlKind::WinUiContentBridge;
        return true;
    }
    if (Equals(cls, L"Microsoft.UI.Content.PopupWindowSiteBridge"))
    {
        outKind = ControlKind::WinUiPopupBridge;
        return true;
    }
    // ComboLBox — нативный popup без WS_CHILD, классифицируется как Special
    if (Equals(cls, L"ComboLBox"))
    {
        outKind = ControlKind::ComboLBox;
        return true;
    }
    // ATL:LISTBOX — тот же сценарий хуков ComboLBox, отдельный ControlKind (как WinForms_*)
    if (Equals(cls, L"ATL:LISTBOX"))
    {
        outKind = ControlKind::Atl_ListBox;
        return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
//  IsNetUiRibbonToolWindow
// ─────────────────────────────────────────────────────────────────────────────

bool WindowClassifier::IsNetUiRibbonToolWindow(HWND hwnd) noexcept
{
    if (!hwnd || !IsWindow(hwnd))
        return false;
    const std::wstring cls = WndCompat::NormalizeWindowClass(hwnd);
    return GetControlKind(cls) == ControlKind::NetUiToolWindow;
}

// ─────────────────────────────────────────────────────────────────────────────
//  GetControlKind
//
//  Принимает нормализованное имя (NormalizeWindowClass):
//    Win32/Delphi/MFC/wx → "Button", "SysListView32", ...
//    WinForms            → ".BUTTON", ".SysListView32", ...
//
//  Сравнения строгие (_wcsicmp без снятия точки).
//  WinForms-классы возвращают отдельные ControlKind::WinForms_*.
//  Стабильные ATL:/WTL_* — ControlKind::Atl_* / Wtl_*.
// ─────────────────────────────────────────────────────────────────────────────
ControlKind WindowClassifier::GetControlKind(const std::wstring& cls)
{
    // ═══════════════════════════════════════════════════════════════════════
    //  WinForms (точка-префикс) — проверяем первыми, т.к. имена перекрываются
    //  с Win32 после case-fold (например ".Button" vs "Button")
    // ═══════════════════════════════════════════════════════════════════════

    if (Equals(cls, L".BUTTON"))            return ControlKind::WinForms_Button;
    if (Equals(cls, L".STATIC"))            return ControlKind::WinForms_Static;
    if (Equals(cls, L".EDIT"))              return ControlKind::WinForms_Edit;
    if (Equals(cls, L".RichEdit20W"))       return ControlKind::WinForms_RichEdit20W;
    if (Equals(cls, L".RICHEDIT50W"))       return ControlKind::WinForms_RichEdit50W;
    if (Equals(cls, L".LISTBOX"))           return ControlKind::WinForms_ListBox;
    if (Equals(cls, L".SysListView32"))     return ControlKind::WinForms_ListView;
    if (Equals(cls, L".SysTabControl32"))   return ControlKind::WinForms_TabControl;
    if (Equals(cls, L".SysDateTimePick32")) return ControlKind::WinForms_DateTimePicker;
    if (Equals(cls, L".COMBOBOX"))          return ControlKind::WinForms_ComboBox;
    // .Window — WinForms Form/Panel, встроенный как дочернее окно.
    // Особая обработка: снять WS_EX_CLIENTEDGE (см. BWM WinHandler.cpp).
    if (Equals(cls, L".Window"))            return ControlKind::WinForms_Window;
    // ── DirectUI / NetUI ─────────────────────────────────────────────────────
    if (Equals(cls, L"DirectUIHWND"))
        return ControlKind::DirectUI;
    if (Equals(cls, L"NetUIHWND"))
        return ControlKind::NetUI;
    // Popup-окна меню ленты Office (BWM WinHandler: ApplyEffect DropDown)
    if (Equals(cls, L"Net UI Tool Window") ||
        Equals(cls, L"Net UI Tool Window Layered"))
        return ControlKind::NetUiToolWindow;

    // Shell-проводник (GA_ROOT): WinEvent → DispatchExplorerRootAttachToOwnerThread → ExplorerRoot::Attach.
    // Не смешивать с Root_Attach / RootSubclassProc (см. RootSubclass.h, GLASS_DIAG_EXPLORER_USE_ROOT_SUBCLASS).
    if (Equals(cls, L"CabinetWClass") || Equals(cls, L"ExploreWClass"))
        return ControlKind::FileExplorerShellRoot;

    // ── ATL / WTL (стабильные литералы имени класса) ─────────────────────────
    if (Equals(cls, L"ATL:EDIT"))
        return ControlKind::Atl_Edit;
    if (Equals(cls, L"ATL:ToolbarWindow32"))
        return ControlKind::Atl_ToolBar;
    if (Equals(cls, L"ATL:LISTBOX"))
        return ControlKind::Atl_ListBox;
    if (Equals(cls, L"WTL_SearchBand"))
        return ControlKind::Wtl_SearchBand;

    // ═══════════════════════════════════════════════════════════════════════
    //  Win32 / Delphi / MFC / wxWidgets
    // ═══════════════════════════════════════════════════════════════════════

    // ── Button ───────────────────────────────────────────────────────────────
    if (Equals(cls, L"Button") ||
        Equals(cls, L"TButton") ||
        Equals(cls, L"TCheckBox") ||
        Equals(cls, L"TRadioButton"))
        return ControlKind::Button;

    // ── Static / SysLink ─────────────────────────────────────────────────────
    if (Equals(cls, L"Static") ||
        Equals(cls, L"TStatic") ||
        Equals(cls, L"SysLink") ||
        Equals(cls, L"Link Window"))
        return ControlKind::Static;

    // ── Edit ─────────────────────────────────────────────────────────────────
    if (Equals(cls, L"Edit") ||
        Equals(cls, L"TEdit") ||
        Equals(cls, L"TMemo") ||
        Equals(cls, L"TInplaceEditList"))
        return ControlKind::Edit;

    // ── RichEdit ─────────────────────────────────────────────────────────────
    if (Equals(cls, L"RichEdit20W") ||
        Equals(cls, L"TRichEdit"))
        return ControlKind::RichEdit20W;

    if (Equals(cls, L"RICHEDIT50W"))
        return ControlKind::RichEdit50W;

    // ── ComboLBox (popup — классифицируется через IsSpecialWindow; только нативный класс)
    if (Equals(cls, L"ComboLBox"))
        return ControlKind::ComboLBox;

    // ── Internet Explorer / WebBrowser (Trident) ─────────────────────────────
    if (Equals(cls, L"Internet Explorer_Server"))
        return ControlKind::InternetExplorer_Server;

    // ── ListBox ──────────────────────────────────────────────────────────────
    if (Equals(cls, L"ListBox") ||
        Equals(cls, L"TListBox"))
        return ControlKind::ListBox;

    // ── ComboBox ─────────────────────────────────────────────────────────────
    if (Equals(cls, L"ComboBox") ||
        Equals(cls, L"TComboBox"))
        return ControlKind::ComboBox;

    // ── ComboBoxEx32 ────────────────────────────────────────────────────────
    if (Equals(cls, L"ComboBoxEx32"))
        return ControlKind::ComboBoxEx32;

    // ── ListView ─────────────────────────────────────────────────────────────
    if (Equals(cls, L"SysListView32") ||
        Equals(cls, L"TListView") ||
        Equals(cls, L"TSystemListView"))
        return ControlKind::ListView;

    // ── TreeView ─────────────────────────────────────────────────────────────
    if (Equals(cls, L"SysTreeView32") ||
        Equals(cls, L"TTreeView") ||
        Equals(cls, L"TResTreeView"))
        return ControlKind::TreeView;

    // ── TabControl ───────────────────────────────────────────────────────────
    if (Equals(cls, L"SysTabControl32") ||
        Equals(cls, L"TTabControl") ||
        Equals(cls, L"_wx_SysTabCtl32"))
        return ControlKind::TabControl;

    // ── ProgressBar ──────────────────────────────────────────────────────────
    if (Equals(cls, L"msctls_progress32") ||
        Equals(cls, L"TProgressBar"))
        return ControlKind::ProgressBar;

    // ── StatusBar ────────────────────────────────────────────────────────────
    if (Equals(cls, L"msctls_statusbar32") ||
        Equals(cls, L"TStatusBar"))
        return ControlKind::StatusBar;

    // ── TrackBar ─────────────────────────────────────────────────────────────
    if (Equals(cls, L"msctls_trackbar32"))
        return ControlKind::TrackBar;

    // ── UpDown ───────────────────────────────────────────────────────────────
    if (Equals(cls, L"msctls_updown32") ||
        Equals(cls, L"TUpDown"))
        return ControlKind::UpDown;

    // ── Header ───────────────────────────────────────────────────────────────
    if (Equals(cls, L"SysHeader32"))
        return ControlKind::Header;

    // ── Animate ──────────────────────────────────────────────────────────────
    if (Equals(cls, L"SysAnimate32"))
        return ControlKind::Animate;

    // ── ScrollBar ────────────────────────────────────────────────────────────
    if (Equals(cls, L"ScrollBar"))
        return ControlKind::ScrollBar;

    // ── DateTimePicker ───────────────────────────────────────────────────────
    if (Equals(cls, L"SysDateTimePick32"))
        return ControlKind::DateTimePicker;

    // ── MonthCal ─────────────────────────────────────────────────────────────
    if (Equals(cls, L"SysMonthCal32"))
        return ControlKind::MonthCal;

    // ── ReBar ────────────────────────────────────────────────────────────────
    if (Equals(cls, L"ReBarWindow32") || Equals(cls, L"SizeableRebar"))
        return ControlKind::ReBar;

    // ── ToolBar ──────────────────────────────────────────────────────────────
    if (Equals(cls, L"ToolbarWindow32"))
        return ControlKind::ToolBar;

    // ── Delphi-контейнеры ────────────────────────────────────────────────────
    if (Equals(cls, L"TGroupBox") ||
        Equals(cls, L"TTabSheet"))
        return ControlKind::DelphiContainer;

    // ── Прочие WS_CHILD контейнеры ───────────────────────────────────────────
    if (Equals(cls, L"AutoIt v3 GUI") ||
        Equals(cls, L"#32770") ||
        Equals(cls, L"AfxWnd42u") ||
        Equals(cls, L"NamespaceTreeControl"))
        return ControlKind::GenericContainer;

    if (IsAtlHexSuffixClass(cls))
        return ControlKind::GenericContainer;

    return ControlKind::Unknown;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CONTROL (WS_CHILD)
// ─────────────────────────────────────────────────────────────────────────────
bool WindowClassifier::IsControlWindow(HWND hwnd, const std::wstring& cls, ControlKind& outKind)
{
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_CHILD))
        return false;

    outKind = GetControlKind(cls);
    // WS_CHILD всегда считается Control; Unknown доходит в WinEvent CREATE (case Unknown → Root_Attach при Full).
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Classify
// ─────────────────────────────────────────────────────────────────────────────
ClassifyResult WindowClassifier::Classify(HWND hwnd)
{
    if (!IsWindow(hwnd))
        return {};

    // Wizard preview blocks GlassSettings: custom WM_PAINT, без Unknown→Root_Attach.
    if (WndCompat::IsGlassSettingsBitmapSwitchHwnd(hwnd))
        return {};

    std::wstring cls = WndCompat::NormalizeWindowClass(hwnd);

    // 0. Win11 CoreWindow: SEH/ShellHost/SMEH/SearchHost — Special; иначе Ignored (никогда Root).
    //    Класс остаётся в kHardList (SWCA-exempt).
    if (Equals(cls, L"Windows.UI.Core.CoreWindow"))
    {
        const bool ncHost = Shared::RootBackdropPolicy::IsShellNotificationHostProcess();
        const bool startHost = Shared::RootBackdropPolicy::IsStartMenuHostProcess();
        const bool searchHost = Shared::RootBackdropPolicy::IsSearchHostProcess();
        const bool win11 = Shared::SystemHelper::g_buildNumber >= 22000;
        if (win11 && ncHost)
            return { WindowRole::Special, ControlKind::ShellNotificationCore };
        // Start + Search: один Special kind → UICompat Start registry (Windhawk parity).
        if (win11 && (startHost || searchHost))
            return { WindowRole::Special, ControlKind::ShellStartMenuCore };
        return {};
    }

    // 0b. ShellHost Quick Settings (24H2+): ControlCenterWindow — тот же NC-контур UICompat
    //     (Windhawk: SEH→CoreWindow, ShellHost→ControlCenterWindow). Никогда Root.
    if (Equals(cls, L"ControlCenterWindow"))
    {
        const bool win11 = Shared::SystemHelper::g_buildNumber >= 22000;
        if (win11 && Shared::RootBackdropPolicy::IsShellNotificationHostProcess())
            return { WindowRole::Special, ControlKind::ShellNotificationCore };
        return {};
    }

    // 1. Жёсткий блок‑лист
    if (IsHardBlockedClass(cls))
        return {};

    // 1b. ATL/WTL: нестабильные и неизвестные имена — Ignored (до 5.5 и без Root)
    if (IsAtlWtlNoiseOrUnknown(cls))
        return {};

    // 1c. DropDown — top-level оболочка ListviewPopup; attach через ListviewPopup_Attach,
    //     не через WndHandler. Не в kHardList, чтобы GlassAttachGate не блокировал дочерние окна.
    if (Equals(cls, L"DropDown"))
        return {};

    // 1d. WinUI host HWND — только explorer.exe + DesktopChildSiteBridge → ExplorerCompat; иначе Ignored.
    if (Shared::ExplorerCompatHostPolicy::IsHostWindowClass(cls)
        && !Shared::ExplorerCompatHostPolicy::ShouldWndCompatHandleHostWindow(cls))
        return {};

    // 2. Tray
    if (IsTrayWindow(cls))
        return { WindowRole::Tray, ControlKind::ShellTray };

    // 2a. Поддерево Progman / Shell_TrayWnd (в т.ч. ListView рабочего стола)
    if (IsUnderShellDesktopOrTrayRoot(hwnd))
        return {};

    // 3. Popup (меню, тултипы)
    if (IsPopupWindow(hwnd, cls))
        return { WindowRole::Popup };

    // 4. Special (DropDown-popup окна)
    {
        ControlKind kind = ControlKind::Unknown;
        if (IsSpecialWindow(cls, kind))
            return { WindowRole::Special, kind };
    }

    // 4b. Popup ленты Office — WinEvent: WindowRole::Control (как DirectUI).
    //      Top-level не проходит IsControlWindow (нет WS_CHILD); без этого шага попадали бы в Root.
    //      CREATE на shell не обрабатывается отдельно: NetUiToolWindow_Attach из NetUI_Attach (NetUISubclass).
    {
        const ControlKind ribbonPopupKind = GetControlKind(cls);
        if (ribbonPopupKind == ControlKind::NetUiToolWindow)
            return { WindowRole::Control, ribbonPopupKind };
    }

    // 5. Control (WS_CHILD; kind может быть Unknown — см. IsControlWindow)
    {
        ControlKind kind = ControlKind::Unknown;
        if (IsControlWindow(hwnd, cls, kind))
            return { WindowRole::Control, kind };
    }

    // 6. Мягкий блок‑лист
    if (IsSoftBlockedClass(cls))
    {
        if (GetAncestor(hwnd, GA_ROOT) == hwnd)
            return { WindowRole::Root };
        return {};
    }

    // 7. Root (для редких top-level DirectUIHWND/NetUIHWND — передаём kind, иначе WinEvent не вызовет *_Attach)
    if (GetAncestor(hwnd, GA_ROOT) == hwnd)
    {
        // См. GetControlKind: тот же kind — маршрут только через ExplorerRoot::Attach в WinEventHandler.
        if (Equals(cls, L"CabinetWClass") || Equals(cls, L"ExploreWClass"))
            return { WindowRole::Root, ControlKind::FileExplorerShellRoot };

        const ControlKind k = GetControlKind(cls);
        if (k == ControlKind::DirectUI || k == ControlKind::NetUI)
            return { WindowRole::Root, k };
        return { WindowRole::Root, ControlKind::NativeTopLevelRoot };
    }

    return {};
}