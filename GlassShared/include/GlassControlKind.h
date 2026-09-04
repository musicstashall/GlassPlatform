#pragma once
#include <windows.h>

// Постоянный маркер типа контрола на HWND.
// Устанавливается при Attach (один раз), снимается при Detach / WM_NCDESTROY.
//
// Позволяет PaintDispatcher определить тип контрола непосредственно в момент
// GDI-вызова (BeginPaint / GetDC / CreateCompatibleDC) без зависимости от
// предварительной «взводки» из SubclassProc.
//
// Использование:
//   GlassMarkWindow(hWnd, GlassControlKind::Button)   — при Attach
//   GlassGetWindowKind(hWnd)                           — в провайдерах GlassCore
//   GlassUnmarkWindow(hWnd)                            — при Detach

namespace GlassShared
{
    enum class GlassControlKind : ULONG_PTR
    {
        None           = 0,
        Button         = 1,
        WinForms_Button = 2,
        Static         = 3,
        ComboLBox      = 4,
        ComboBox       = 5,
        Toolbar        = 6,
        MonthCal       = 7,
        DirectUI       = 8,
        TreeView       = 9,
        ListView       = 10,
        Rebar          = 11,
        TabControl     = 12,
        Header         = 13,   // SysHeader32
        WinForms_ListView = 14, // .SysListView32
        WinForms_Window   = 15, // .Window — контейнер WinForms (Root_Attach, не всегда GA_ROOT)
        NetUI             = 16, // NetUIHWND / UIRibbonCommandBar — NetUI_Attach; UiRibbonHooks + PaintDispatcher
        NetUiToolWindow   = 17, // Net UI Tool Window / Layered — NetUiToolWindow_Attach с NetUIHWND (WinEvent), не DropDown_Attach; IAT UIRibbon как у NetUI
        InternetExplorer_Server = 18, // MSHTML Trident / MMC DocObject — GlassMark (IeServer_Attach), сессия PaintDispatcher + IAT StretchBlt (GlassCore)
        Edit                    = 19, // Edit, TEdit, TMemo
        WinForms_Edit           = 20, // .EDIT (WindowsForms10)
        RichEdit20W             = 21, // RichEdit20W, TRichEdit
        WinForms_RichEdit20W    = 22, // .RichEdit20W
        RichEdit50W             = 23, // RICHEDIT50W
        WinForms_RichEdit50W    = 24, // .RICHEDIT50W
        ScrollBar               = 25, // класс ScrollBar — ScrollBarHost (WndCompat) + BitBlt/AlphaBlend в ScrollBarThemeHooks (GlassCore)
        ListBox                 = 26, // ListBox, TListBox (нативный)
        WinForms_ListBox        = 27, // .LISTBOX (WinForms)
        StatusBar               = 28, // msctls_statusbar32
        ProgressBar             = 29, // msctls_progress32
        DateTimePicker          = 30, // SysDateTimePick32 (нативный)
        // 31: зарезервировано (раньше отдельный kind превью текста — не используется).
        // Превью «Текст» в GlassSettings: обычный Edit (GlassMarkWindow Edit), RAM-капсула — preview API GlassCore.
        TrackBar                  = 32, // msctls_trackbar32 — маркер в WndCompat::TrackBar_Attach (TrackBarHost)
        WinForms_Static           = 33, // .STATIC (WindowsForms10)
        WinForms_ComboBox         = 34, // .COMBOBOX (WindowsForms10)
        WinForms_DateTimePicker   = 35, // .SysDateTimePick32 (WindowsForms10)
        UpDown                    = 36, // msctls_updown32, TUpDown — UpDownHost + DrawThemeBackground Spin (GlassCore)
        Root                      = 37, // top-level / GA_ROOT в g_rootWindows (WndCompat RootSubclass)
        AMCCustomTab              = 38, // AMCCustomTab — кастомный C#/WinForms таб-контрол mmc.exe (MFC42u.dll), BWM PaintHandler
    };

    // Атом для имени property — создаётся один раз в процессе.
    // GlobalAddAtomW возвращает одинаковый атом при повторных вызовах с той же строкой.
    inline ATOM GetControlKindAtom() noexcept
    {
        static const ATOM s_atom = GlobalAddAtomW(L"GlassPlatform.ControlKind");
        return s_atom;
    }

    // Устанавливает маркер типа контрола на окно. Вызывать при Attach.
    inline void GlassMarkWindow(HWND hWnd, GlassControlKind kind) noexcept
    {
        SetPropW(hWnd, MAKEINTATOM(GetControlKindAtom()),
            reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(kind)));
    }

    // Возвращает тип контрола окна. None — если маркер не установлен.
    [[nodiscard]] inline GlassControlKind GlassGetWindowKind(HWND hWnd) noexcept
    {
        return static_cast<GlassControlKind>(
            reinterpret_cast<ULONG_PTR>(
                GetPropW(hWnd, MAKEINTATOM(GetControlKindAtom()))));
    }

    // Снимает маркер с окна. Вызывать при Detach / WM_NCDESTROY.
    inline void GlassUnmarkWindow(HWND hWnd) noexcept
    {
        RemovePropW(hWnd, MAKEINTATOM(GetControlKindAtom()));
    }
}
