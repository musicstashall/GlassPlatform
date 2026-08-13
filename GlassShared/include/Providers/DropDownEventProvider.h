#pragma once
#include <windows.h>
#include <Providers/DropDownContext.h>

namespace WndCompat::DropDown
{
    struct DropDownEventProvider
    {
        // ── ListviewPopup paint ──────────────────────────────────────────────
        void (*OnPaintBegin)(HWND hWnd) = nullptr;
        void (*OnPaintEnd)(HWND hWnd) = nullptr;

        void (*OnDrawItemBegin)(HWND hWnd, const DRAWITEMSTRUCT* dis) = nullptr;
        void (*OnDrawItemEnd)(HWND hWnd) = nullptr;

        // ── Attach notifications ─────────────────────────────────────────────
        void (*OnListviewPopupAttach)(HWND hWndListview, HWND hWndRoot) = nullptr;
        void (*OnViewControlAttach)(HWND hWnd) = nullptr;

        // ── Toolbar paint ────────────────────────────────────────────────────
        void (*OnToolbarPaintBegin)(HWND hWnd) = nullptr;
        void (*OnToolbarPaintEnd)(HWND hWnd) = nullptr;

        // ── DropDown show (DropDownSubclass → WM_SHOWWINDOW) ─────────────────
        void (*OnDropDownShow)(HWND hWnd) = nullptr;

        /// Per-popup defer NC-рамки во время flyout-анимации (общий с Menu через FlyoutFrame).
        bool (*IsFlyoutBorderDeferred)(HWND hWnd) noexcept = nullptr;
        void (*ClearFlyoutBorderDeferred)(HWND hWnd) noexcept = nullptr;

        /// Снимок g_dropDownCtx из GlassCore.dll (WndCompat и GlassCore — разные копии TLS).
        bool (*QueryDropDownContext)(WndCompat::DropDownContext* out) noexcept = nullptr;

        // ── SysMonthCal32 ────────────────────────────────────────────────────
        // Вызывается один раз при обнаружении SysMonthCal32.
        // hWndDropDown != nullptr → календарь дочерний к DropDown-popup;
        // hWndDropDown == nullptr → embedded-календарь в обычном окне.
        void (*OnMonthCalAttach)(HWND hWndMonthCal, HWND hWndDropDown) = nullptr;

        // WM_PRINTCLIENT — прямое включение/выключение IAT-хуков.
        // PaintDispatcher не перехватывает DC, переданный через wParam напрямую.
        // AnimateWindow использует WM_PRINTCLIENT для захвата снимка содержимого.
        void (*OnMonthCalPrintClientBegin)(HWND hWnd) = nullptr;
        void (*OnMonthCalPrintClientEnd)(HWND hWnd) = nullptr;

        // ── ComboLBox show (WM_SHOWWINDOW) ──────────────────────────────────
        // Вызывается из ComboLBoxSubclassProc при WM_SHOWWINDOW wParam=1.
        // Применяет backdrop effect и (если isComboBoxAnimation) анимацию.
        // isComboBoxAnimation = результат SPI_GETCOMBOBOXANIMATION.
        void (*OnComboLBoxShow)(HWND hWnd, bool isComboBoxAnimation) = nullptr;

        // ── ComboLBox WM_PRINTCLIENT (прямое включение/выключение хуков) ─────
        // PaintDispatcher не перехватывает DC переданный через wParam напрямую.
        // Вызываются из ComboLBoxSubclassProc при WM_PRINTCLIENT до и после DefSubclassProc.
        void (*OnComboLBoxPrintClientBegin)(HWND hWnd) = nullptr;
        void (*OnComboLBoxPrintClientEnd)(HWND hWnd) = nullptr;

        // ── ComboLBox detach (WM_NCDESTROY cleanup) ──────────────────────────
        void (*OnComboLBoxDetach)(HWND hWnd) = nullptr;

        // ── ComboBox ──────────────────────────────────────────────────────────
        // CBN_DROPDOWN → hook AnimateWindow в comctl32, чтобы подменить
        // системную slide-анимацию ComboLBox на ShowWindow(SW_SHOW).
        void (*OnComboBoxDropDown)(HWND hComboBox) = nullptr;

        // ── Net UI Tool Window / Layered (ленточные popup) ───────────────────
        // Реализации в GlassCore::UiRibbon; вызов из NetUISubclass после GlassMarkWindow.
        void (*OnNetUiToolWindowHooksAttach)(HWND hWnd) = nullptr;
        void (*OnNetUiToolWindowAttach)(HWND hWnd) = nullptr;
        void (*OnNetUiToolWindowDetach)(HWND hWnd) = nullptr;
    };

    void SetDropDownEventProvider(const DropDownEventProvider& provider);
    const DropDownEventProvider* GetDropDownEventProvider();
}