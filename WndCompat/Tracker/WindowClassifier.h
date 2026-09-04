#pragma once
#include <windows.h>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  WindowRole — верхний уровень классификации окна
// ─────────────────────────────────────────────────────────────────────────────
enum class WindowRole
{
    Root,       // корневое окно приложения
    Popup,      // всплывающее (меню, тултипы)
    Control,    // дочерний контрол (WS_CHILD)
    Tray,       // панель задач
    Special,    // специальные popup DropDown (ListviewPopup, ViewControlClass...)
    Ignored     // не обрабатывается
};

// ─────────────────────────────────────────────────────────────────────────────
//  ControlKind — уточняющий тип контрола
//
//  Соглашение об именовании:
//    Без префикса     — Win32 / Delphi / MFC / wxWidgets
//    WinForms_*       — WindowsForms10.* (нормализованные с точкой: ".Button")
//                       Отрисовка WinForms отличается от Win32, поэтому они
//                       различимы на уровне ControlKind.
//    Atl_*            — стабильные литералы класса ATL:… (не нестабильный ATL:+hex)
//    Wtl_*            — осмысленные классы WTL_* (не совпадающие с таблицей ATL)
// ─────────────────────────────────────────────────────────────────────────────
enum class ControlKind
{
    Unknown = 0,

    // ── Win32 / Delphi / MFC / wxWidgets ─────────────────────────────────────
    Button,             // Button, TButton, TCheckBox, TRadioButton
    Static,             // Static, TStatic, SysLink, Link Window
    Edit,               // Edit, TEdit, TMemo
    RichEdit20W,        // RichEdit20W, TRichEdit
    RichEdit50W,        // RICHEDIT50W
    ListBox,            // ListBox, TListBox, ComboLBox
    ComboBox,           // ComboBox, TComboBox
    ComboBoxEx32,       // ComboBoxEx32 (extended combobox; root-subclass для WM_CTLCOLOR*)
    ListView,           // SysListView32, TListView, TSystemListView
    TreeView,           // SysTreeView32, TTreeView, TResTreeView
    TabControl,         // SysTabControl32, TTabControl, _wx_SysTabCtl32
    ProgressBar,        // msctls_progress32, TProgressBar
    StatusBar,          // msctls_statusbar32, TStatusBar
    TrackBar,           // msctls_trackbar32
    UpDown,             // msctls_updown32, TUpDown
    Header,             // SysHeader32
    Animate,            // SysAnimate32
    ScrollBar,          // ScrollBar
    DateTimePicker,     // SysDateTimePick32
    MonthCal,           // SysMonthCal32
    ReBar,              // ReBarWindow32, SizeableRebar
    ToolBar,            // ToolbarWindow32
    DirectUI,           // DirectUIHWND (DUser.dll/wdc.dll, Windows 10+)
    NetUI,              // NetUIHWND, UIRibbonCommandBar (Office ribbon / UIRibbon.dll)
    NetUiToolWindow,    // «Net UI Tool Window», «Net UI Tool Window Layered» — WinEvent: Control (как DirectUI)
    AMCCustomTab,       // AMCCustomTab — кастомный C#/WinForms таб-контрол mmc.exe (MFC42u.dll)

    // Delphi-контейнеры (WS_CHILD, но не leaf-контролы)
    DelphiContainer,    // TGroupBox, TTabSheet, TInplaceEditList

    // Прочие WS_CHILD контейнеры
    GenericContainer,   // AutoIt v3 GUI, AfxWnd42u

    // ── WinForms (Windows Forms 10.*) ────────────────────────────────────────
    // Имена получены через NormalizeWindowClass → ".ClassName"
    WinForms_Button,        // .BUTTON
    WinForms_Static,        // .STATIC
    WinForms_Edit,          // .EDIT
    WinForms_RichEdit20W,   // .RichEdit20W
    WinForms_RichEdit50W,   // .RICHEDIT50W
    WinForms_ListBox,       // .LISTBOX
    WinForms_ListView,      // .SysListView32
    WinForms_TabControl,    // .SysTabControl32
    WinForms_DateTimePicker,// .SysDateTimePick32
    WinForms_ComboBox,      // .COMBOBOX
    WinForms_Window,        // .Window  (WinForms Form/Panel встроенный в окно;
    //           особая обработка: снять WS_EX_CLIENTEDGE)

    // ── ATL / WTL (стабильные имена; ATL:+hex и прочий шум → Ignored в Classify) ──
    Atl_Edit,           // ATL:EDIT
    Atl_ToolBar,        // ATL:ToolbarWindow32
    Atl_ListBox,        // ATL:LISTBOX (popup списка Combo; role Special, хуки как ComboLBox)
    Wtl_SearchBand,     // WTL_SearchBand (полоса поиска; см. BWM WSBWinHandler)

    // ── DropDown-специфичные (role == Special) ───────────────────────────────
    WinUiDesktopBridge, // Microsoft.UI.Content.DesktopChildSiteBridge (WinUI island host в explorer и др.)
    WinUiContentBridge,
    WinUiPopupBridge,

    ListviewPopup,      // popup-окно ListView в DropDown
    ViewControl,        // ViewControlClass внутри DropDown
    ComboLBox,          // нативный ComboLBox (dropdown popup ComboBox)

    // MSHTML / WebBrowser (Trident): клиентская область документа
    InternetExplorer_Server,

    // Явные роли вместо Unknown (логи / WinEvent; не смешивать с «неизвестным» контролом)
    ShellTray,                      // Shell_TrayWnd / Shell_SecondaryTrayWnd
    /// Win11 NC/Action Center: CoreWindow в SEH/ShellHost (UWP XAML → UICompat).
    ShellNotificationCore,
    /// Win11 Start Menu: CoreWindow в StartMenuExperienceHost (UWP XAML → UICompat).
    ShellStartMenuCore,
    NativeTopLevelRoot,             // top-level GA_ROOT, не DirectUI/NetUI
    /// Top-level проводник (GA_ROOT): CabinetWClass / ExploreWClass — отдельный root-субкласс.
    FileExplorerShellRoot,
    FileExplorerIslandContainer,    // SHELLDLL_DefView, ShellTabWindowClass — общая ветка для GDI-подготовки
};

// ─────────────────────────────────────────────────────────────────────────────
//  ClassifyResult
// ─────────────────────────────────────────────────────────────────────────────
struct ClassifyResult
{
    WindowRole  role{ WindowRole::Ignored };
    ControlKind kind{ ControlKind::Unknown };
};

// ─────────────────────────────────────────────────────────────────────────────
//  WindowClassifier
// ─────────────────────────────────────────────────────────────────────────────
class WindowClassifier
{
public:
    // Полная классификация окна.
    static ClassifyResult Classify(HWND hwnd);

    // «Net UI Tool Window» / «Layered» — оболочка popup ленты (не DropDownSubclass / не #32768 меню).
    [[nodiscard]] static bool IsNetUiRibbonToolWindow(HWND hwnd) noexcept;

    // Отображение нормализованного имени класса → ControlKind.
    // Принимает вывод NormalizeWindowClass (может быть ".Button" или "Button").
    // Точка сохраняется как признак WinForms — сравнения строгие (без strip).
    static ControlKind GetControlKind(const std::wstring& cls);

private:
    static bool IsHardBlockedClass(const std::wstring& cls);
    static bool IsSoftBlockedClass(const std::wstring& cls);
    static bool IsTrayWindow(const std::wstring& cls);
    static bool IsPopupWindow(HWND hwnd, const std::wstring& cls);
    static bool IsSpecialWindow(const std::wstring& cls, ControlKind& outKind);
    static bool IsControlWindow(HWND hwnd, const std::wstring& cls, ControlKind& outKind);
};