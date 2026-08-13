#include "SharedPch.h"

#include "framework.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "Ui/Messages/LogViewerAppMessages.h"
#include "Ui/Theme/LogViewerListTheme.h"
#include "UiConsts.h"
#include "LogViewerGlassHost.h"
#include "LogViewerWndCompatScan.h"
#include <commctrl.h>
#include <algorithm>
#include <set>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "UxTheme.lib")

// Фильтр-тулбар: BTNS_DROPDOWN + кастомный попап. Hot-switch — двухфазно (Close → TBDDRET_DEFAULT →
// WM_APP_FILTER_RESUME_DROPDOWN → OpenFilterDropdownModal), иначе comctl32 держит сессию первой кнопки.

static constexpr int kFilterPopupPadPx = 1;
/// Вертикальные «отступы 2 пкс» (база 96 DPI) — суммарно к высоте клиента попапа сверх тела списка.
static constexpr int kFilterPopupClientVertMarginPxAt96 = 2;
/// Пустой список — как минимум одна строка по высоте пункта.
static constexpr int kFilterPopupMinVisibleRows = 1;
/// Максимум видимых строк: высота тела списка ≤ itemH × 15, дальше прокрутка.
static constexpr int kFilterPopupMaxVisibleRows = 15;
/// DropDown.au3 $__dm_maxwidth — потолок ширины клиента ListView (база 96 DPI).
static constexpr int kFilterPopupMaxListClientWidthAt96 = 264;

namespace FilterPopupSizing
{
    [[nodiscard]] inline UINT AnchorDpi(HWND hAnchor) noexcept
    {
        if (hAnchor && ::IsWindow(hAnchor))
        {
            const UINT d = ::GetDpiForWindow(hAnchor);
            if (d != 0)
                return d;
        }
        return USER_DEFAULT_SCREEN_DPI;
    }

    [[nodiscard]] inline int ScalePixels(int pxAt96, UINT dpi) noexcept
    {
        return ::MulDiv(pxAt96, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    }

    /// Отступ контента от рамки попапа, масштабированный с DPI (база — 96).
    [[nodiscard]] inline int ContentPadPx(HWND dpiSource) noexcept
    {
        return ScalePixels(kFilterPopupPadPx, AnchorDpi(dpiSource));
    }

    [[nodiscard]] inline int ClientVertMarginPx(HWND dpiSource) noexcept
    {
        return ScalePixels(kFilterPopupClientVertMarginPxAt96, AnchorDpi(dpiSource));
    }
}

static thread_local bool s_inCloseFilterPopup = false;
/// Подавить LVN_ITEMCHANGED→Apply во время пакетного ListView_SetCheckState (клик по строке).
static thread_local bool s_filterPopupBatchCheck = false;

/// Сверка fsState фильтр-кнопок: при закрытом дропе — только TBSTATE_ENABLED (+ELLIPSES/WRAP с кнопки);
/// при открытом — PRESS только у activeCommandId. При activeCommandId==0 всегда шлём TB_SETBUTTONINFO:
/// comctl32 для BTNS_DROPDOWN может кэшировать «подсветку» при совпадении GETBUTTON с желаемым.
static void FilterToolbarReconcileFilterButtonFsState(HWND hTb, UINT activeCommandId) noexcept
{
    static const UINT kFilterCmds[] = { IDC_PROC_BTN, IDC_MOD_BTN, IDC_SUB_BTN };
    for (UINT id : kFilterCmds)
    {
        const int idx = static_cast<int>(::SendMessageW(hTb, TB_COMMANDTOINDEX, static_cast<WPARAM>(id), 0));
        if (idx < 0)
            continue;
        TBBUTTON btn{};
        if (::SendMessageW(hTb, TB_GETBUTTON, static_cast<WPARAM>(idx), reinterpret_cast<LPARAM>(&btn)) == FALSE)
            continue;

        const BYTE styleBits = static_cast<BYTE>(btn.fsState & (TBSTATE_ELLIPSES | TBSTATE_WRAP));
        BYTE desired;
        if (activeCommandId != 0 && id == activeCommandId)
            desired = static_cast<BYTE>(TBSTATE_ENABLED | TBSTATE_PRESSED | styleBits);
        else
            desired = static_cast<BYTE>(TBSTATE_ENABLED | styleBits);

        if (activeCommandId != 0 && desired == btn.fsState)
            continue;

        TBBUTTONINFOW tbi{};
        tbi.cbSize = sizeof(tbi);
        tbi.dwMask = TBIF_STATE;
        tbi.fsState = desired;
        (void)::SendMessageW(hTb, TB_SETBUTTONINFO, static_cast<WPARAM>(id), reinterpret_cast<LPARAM>(&tbi));
    }
}

/// Синхронизация BTNS_DROPDOWN (split) при кастомном попапе (после TBN_DROPDOWN с TBDDRET_DEFAULT): hot, TB_MARKBUTTON, fsState.
/// WorkerW.au3: снятие — суррогатный WM_UNINITMENUPOPUP; у нас — CloseFilterPopup + этот вызов.
static void FilterToolbarSyncDropdownPressed(HWND hTb, UINT activeCommandId) noexcept
{
    if (!hTb || ::IsWindow(hTb) == FALSE)
        return;

    if (::GetCapture() == hTb)
        ::ReleaseCapture();
    (void)::SendMessageW(hTb, WM_CANCELMODE, 0, 0);

    static const UINT kFilterCmds[] = { IDC_PROC_BTN, IDC_MOD_BTN, IDC_SUB_BTN };
    (void)::SendMessageW(hTb, TB_SETHOTITEM, static_cast<WPARAM>(-1), 0);

    for (UINT id : kFilterCmds)
        (void)::SendMessageW(hTb, TB_MARKBUTTON, static_cast<WPARAM>(id), static_cast<LPARAM>(FALSE));

    for (UINT id : kFilterCmds)
        (void)::SendMessageW(hTb, TB_PRESSBUTTON, static_cast<WPARAM>(id), MAKELPARAM(FALSE, 0));

    if (activeCommandId != 0)
    {
        (void)::SendMessageW(hTb, TB_PRESSBUTTON, static_cast<WPARAM>(activeCommandId), MAKELPARAM(TRUE, 0));
        const int idx = static_cast<int>(::SendMessageW(hTb, TB_COMMANDTOINDEX, activeCommandId, 0));
        if (idx >= 0)
            (void)::SendMessageW(hTb, TB_SETHOTITEM, static_cast<WPARAM>(idx), 0);
    }

    FilterToolbarReconcileFilterButtonFsState(hTb, activeCommandId);

    // BTNS_DROPDOWN: один TB_SETHOTITEM(-1) не сбрасывает внутреннюю «горячую» подсветку той кнопки,
    // с которой открыли дроп (переход hot на другую кнопку это чинит). Цикл set/clear по фильтрам
    // синхронизирует состояние с comctl32 без смены выбранной кнопки.
    if (activeCommandId == 0)
    {
        for (UINT id : kFilterCmds)
        {
            const int idx = static_cast<int>(::SendMessageW(hTb, TB_COMMANDTOINDEX, static_cast<WPARAM>(id), 0));
            if (idx >= 0)
            {
                (void)::SendMessageW(hTb, TB_SETHOTITEM, static_cast<WPARAM>(idx), 0);
                (void)::SendMessageW(hTb, TB_SETHOTITEM, static_cast<WPARAM>(-1), 0);
            }
        }
    }

    (void)::RedrawWindow(hTb, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

void LogViewerFilterToolbar_ApplyPostDropdownSync(HWND hTb) noexcept
{
    FilterToolbarSyncDropdownPressed(hTb, 0);
}

namespace
{
    constexpr UINT_PTR kFilterToolbarSubclassId = 7;
    constexpr UINT_PTR kFilterPopupListSubclassId = 8;

    /// Закрытие попапа через очередь главного окна (как в AutoIt: не трогаем состояние тулбара из потока попапа синхронно).
    void PostCloseFilterRequest(UiMainWindowImpl* impl) noexcept
    {
        if (!impl || !impl->hwnd || ::IsWindow(impl->hwnd) == FALSE)
            return;
        (void)::PostMessageW(impl->hwnd, WM_APP_CLOSE_FILTER, 0, 0);
    }

    /// Высота строки report+чекбокс: LVM_GETITEMSPACING в Explorer-теме даёт не высоту пункта (см. DropDown.au3 — LVIR_BOUNDS).
    [[nodiscard]] int MeasureFilterPopupListRowHeight(HWND hList, int cxList, UINT dpi) noexcept
    {
        (void)::SendMessageW(hList, LVM_SETCOLUMNWIDTH, 0, static_cast<LPARAM>(cxList));
        const int n = ListView_GetItemCount(hList);
        if (n > 0)
        {
            RECT rc{};
            rc.left = LVIR_BOUNDS;
            if (::SendMessageW(hList, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rc)))
            {
                const int h = rc.bottom - rc.top;
                const int hMax = FilterPopupSizing::ScalePixels(96, dpi);
                if (h > 0 && h <= hMax)
                    return h;
            }
        }
        const DWORD approx = ListView_ApproximateViewRect(hList, 1, cxList, -1);
        int h = static_cast<int>(HIWORD(approx));
        if (h < 1)
            h = FilterPopupSizing::ScalePixels(19, dpi);
        const int hMax = FilterPopupSizing::ScalePixels(96, dpi);
        if (h > hMax)
            h = hMax;
        return h;
    }

    /// DropDown.au3 DMM_GETWIDTHSCROLLBAR: ширина дорожки OBJID_VSCROLL; иначе SM_CXVSCROLL по DPI.
    [[nodiscard]] int GetVerticalScrollbarTrackWidth(HWND hList, UINT dpi) noexcept
    {
        SCROLLBARINFO sbi{};
        sbi.cbSize = sizeof(SCROLLBARINFO);
        if (::GetScrollBarInfo(hList, OBJID_VSCROLL, &sbi))
        {
            const int tw = sbi.rcScrollBar.right - sbi.rcScrollBar.left;
            if (tw > 0)
                return tw;
        }
        return ::GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
    }

    /// DropDown.au3 DMM_SETAUTOSIZE: LVSCW_AUTOSIZE, поправка LVIR_ICON + отступ как 8×dpi.
    [[nodiscard]] int MeasureFilterPopupColumnContentWidth(HWND hList, UINT dpi) noexcept
    {
        if (!hList || ::IsWindow(hList) == FALSE)
            return FilterPopupSizing::ScalePixels(48, dpi);
        if (ListView_GetItemCount(hList) <= 0)
            return FilterPopupSizing::ScalePixels(48, dpi);

        (void)::SendMessageW(hList, LVM_SETCOLUMNWIDTH, 0, LVSCW_AUTOSIZE);
        int w = static_cast<int>(::SendMessageW(hList, LVM_GETCOLUMNWIDTH, 0, 0));
        if (w < 1)
            w = 1;

        RECT rcIcon{};
        rcIcon.left = LVIR_ICON;
        if (::SendMessageW(hList, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&rcIcon)))
        {
            const int iconSpan = (std::max)(0, static_cast<int>(rcIcon.right) - static_cast<int>(rcIcon.left));
            w = w - iconSpan + FilterPopupSizing::ScalePixels(8, dpi);
        }
        return (std::max)(w, 1);
    }

    LRESULT CALLBACK FilterPopupListSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        if (uMsg == WM_MOUSEACTIVATE)
            return MA_ACTIVATE;
        if (uMsg == WM_NCDESTROY)
            ::RemoveWindowSubclass(hWnd, FilterPopupListSubclassProc, kFilterPopupListSubclassId);
        return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    /// AutoIt: пока попап закрыт — при движении мыши над тулбаром переносим фокус на него до TBN_DROPDOWN (не во время открытого попапа:
    /// иначе срываем фокус со списка и провоцируем лишние активации / TBN_DROPDOWN).
    LRESULT CALLBACK FilterToolbarSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR dwRefData)
    {
        if (uMsg == WM_NCDESTROY)
            ::RemoveWindowSubclass(hWnd, FilterToolbarSubclassProc, kFilterToolbarSubclassId);
        else if (uMsg == WM_MOUSEMOVE)
        {
            auto* impl = reinterpret_cast<UiMainWindowImpl*>(dwRefData);
            HWND hHost = impl ? impl->hFilterTbHost : nullptr;
            if (impl && impl->hwnd && ::IsWindow(impl->hwnd) != FALSE && impl->hFilterTb == hWnd && !impl->m_stopping
                && !impl->IsFilterPopupOpen() && hHost && ::IsWindow(hHost) != FALSE)
            {
                const HWND foc = ::GetFocus();
                if (foc != hHost)
                    (void)::SetFocus(hHost);
            }
        }
        return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

void LogViewerFilterToolbar_Attach(HWND hFilterTb, UiMainWindowImpl* impl) noexcept
{
    if (!hFilterTb || !::IsWindow(hFilterTb))
        return;
    (void)::SetWindowSubclass(
        hFilterTb,
        FilterToolbarSubclassProc,
        kFilterToolbarSubclassId,
        reinterpret_cast<DWORD_PTR>(impl));
}

LRESULT CALLBACK LogViewerFilterDropDownWndProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    auto* impl = reinterpret_cast<UiMainWindowImpl*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (uMsg)
    {
    // DropDown.au3 _DropProc: WM_MOUSEACTIVATE → 1 (MA_ACTIVATE); иначе «первый клик» уходит в активацию.
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && impl && !s_inCloseFilterPopup)
        {
            HWND next = reinterpret_cast<HWND>(lParam);
            if (!next || !IsChild(hWnd, next))
                PostCloseFilterRequest(impl);
        }
        break;
    case WM_SIZE:
        if (impl && impl->m_filterListviewPopup && LOWORD(lParam) > 0 && HIWORD(lParam) > 0)
        {
            const UINT dpi = FilterPopupSizing::AnchorDpi(hWnd);
            const int pad = FilterPopupSizing::ContentPadPx(hWnd);
            const int vm = FilterPopupSizing::ClientVertMarginPx(hWnd);
            const int topM = FilterPopupSizing::ScalePixels(1, dpi);
            const int botM = (std::max)(vm - topM, 0);
            const int cw = LOWORD(lParam);
            const int ch = HIWORD(lParam);
            const int innerW = (std::max)(cw - 2 * pad, 1);
            const int innerH = (std::max)(ch - topM - botM, 1);
            MoveWindow(impl->m_filterListviewPopup, pad, topM, innerW, innerH, TRUE);
        }
        return 0;
    case WM_CLOSE:
        if (impl)
            PostCloseFilterRequest(impl);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK LogViewerFilterListviewPopupWndProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    auto* impl = reinterpret_cast<UiMainWindowImpl*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
    switch (uMsg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NOTIFY:
        if (impl && impl->m_filterPopupList)
        {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr->hwndFrom == impl->m_filterPopupList)
            {
                if (hdr->code == LVN_KEYDOWN)
                {
                    const auto* nmk = reinterpret_cast<const NMLVKEYDOWN*>(lParam);
                    if (nmk->wVKey == VK_ESCAPE && impl->hwnd)
                    {
                        PostCloseFilterRequest(impl);
                        return 0;
                    }
                }
                else if (hdr->code == NM_CLICK)
                {
                    (void)::SetFocus(impl->m_filterPopupList);
                    HWND hList = impl->m_filterPopupList;
                    POINT pt{};
                    GetCursorPos(&pt);
                    ScreenToClient(hList, &pt);
                    LVHITTESTINFO ht{};
                    ht.pt = pt;
                    const int item = ListView_HitTest(hList, &ht);
                    if (item >= 0)
                    {
                        if (!(ht.flags & LVHT_ONITEMSTATEICON))
                        {
                            const int n = ListView_GetItemCount(hList);
                            s_filterPopupBatchCheck = true;
                            for (int j = 0; j < n; ++j)
                                ListView_SetCheckState(hList, j, FALSE);
                            ListView_SetCheckState(hList, item, TRUE);
                            s_filterPopupBatchCheck = false;
                            impl->ApplyFilterPopupSelection();
                        }
                    }
                    return 0;
                }
                else if (hdr->code == LVN_ITEMCHANGED)
                {
                    const auto* nmlv = reinterpret_cast<const NMLISTVIEW*>(lParam);
                    if ((nmlv->uChanged & LVIF_STATE) != 0
                        && ((nmlv->uOldState ^ nmlv->uNewState) & LVIS_STATEIMAGEMASK) != 0
                        && !s_filterPopupBatchCheck && !s_inCloseFilterPopup && impl
                        && impl->m_filterPopup && IsWindowVisible(impl->m_filterPopup))
                    {
                        impl->ApplyFilterPopupSelection();
                    }
                    return 0;
                }
            }
        }
        break;
    case WM_SIZE:
        if (impl && impl->m_filterPopupList && LOWORD(lParam) > 0 && HIWORD(lParam) > 0)
        {
            const int pad = FilterPopupSizing::ContentPadPx(hWnd);
            const int cw = LOWORD(lParam);
            const int ch = HIWORD(lParam);
            const int listW = (std::max)(cw - 2 * pad, 1);
            const int listH = (std::max)(ch, 1);
            MoveWindow(impl->m_filterPopupList, pad, 0, listW, listH, TRUE);
            RECT rcList{};
            if (::GetClientRect(impl->m_filterPopupList, &rcList))
            {
                const int colPx = (std::max)(1, static_cast<int>(rcList.right - rcList.left));
                (void)::SendMessageW(impl->m_filterPopupList, LVM_SETCOLUMNWIDTH, 0, static_cast<LPARAM>(colPx));
            }
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

void UiMainWindowImpl::RegisterFilterPopupClass()
{
    static bool s_done = false;
    if (s_done)
        return;
    HINSTANCE hi = GetModuleHandleW(nullptr);

    WNDCLASSW wcDrop{};
    wcDrop.lpfnWndProc = LogViewerFilterDropDownWndProc;
    wcDrop.hInstance = hi;
    wcDrop.lpszClassName = L"DropDown";
    wcDrop.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcDrop.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wcDrop) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    WNDCLASSW wcLv{};
    wcLv.lpfnWndProc = LogViewerFilterListviewPopupWndProc;
    wcLv.hInstance = hi;
    wcLv.lpszClassName = L"ListviewPopup";
    wcLv.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcLv.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wcLv) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    s_done = true;
}

void UiMainWindowImpl::DestroyFilterPopup() noexcept
{
    if (m_filterPopup && ::IsWindow(m_filterPopup))
        ::DestroyWindow(m_filterPopup);
    m_filterPopup = nullptr;
    m_filterListviewPopup = nullptr;
    m_filterPopupList = nullptr;
    m_filterPopupKind = 0;
    m_filterTbHotIdNew = 0;
    m_filterTbHotIdOld = 0;
    m_filterTbHotDwFlags = 0;
}

void UiMainWindowImpl::EnsureFilterPopupCreated()
{
    if (m_filterPopup || !hwnd)
        return;
    RegisterFilterPopupClass();
    HINSTANCE hi = GetModuleHandleW(nullptr);
    const UINT dpi = FilterPopupSizing::AnchorDpi(hwnd);
    const int pad = FilterPopupSizing::ContentPadPx(hwnd);
    const int topM = FilterPopupSizing::ScalePixels(1, dpi);
    const int initH = FilterPopupSizing::ScalePixels(160, dpi);

    // WS_EX_NOACTIVATE — как выпадающие списки проводника: главное окно остаётся «активным» для рамки/заголовка.
    const int initOuterW = (std::max)(PROC_CBX_W, FilterPopupSizing::ScalePixels(360, dpi));
    m_filterPopup = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"DropDown",
        L"",
        WS_POPUP | WS_BORDER | WS_CLIPCHILDREN,
        0, 0, initOuterW, initH,
        hwnd,
        nullptr,
        hi,
        nullptr);
    if (!m_filterPopup)
        return;
    SetWindowLongPtrW(m_filterPopup, GWLP_USERDATA, (LONG_PTR)this);

    m_filterListviewPopup = CreateWindowExW(
        0,
        L"ListviewPopup",
        L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
        pad,
        topM,
        100,
        100,
        m_filterPopup,
        nullptr,
        hi,
        nullptr);
    if (!m_filterListviewPopup)
        return;
    SetWindowLongPtrW(m_filterListviewPopup, GWLP_USERDATA, (LONG_PTR)this);

    m_filterPopupList = CreateWindowExW(
        0,
        WC_LISTVIEWW,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER
            | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_ALIGNLEFT,
        pad,
        0,
        100,
        100,
        m_filterListviewPopup,
        nullptr,
        hi,
        nullptr);
    ListView_SetExtendedListViewStyle(m_filterPopupList,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ListView_SetBkColor(m_filterPopupList, LogViewerListBgBase);
    ListView_SetTextBkColor(m_filterPopupList, CLR_NONE);

    LVCOLUMNW col{};
    col.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
    col.fmt = LVCFMT_LEFT;
    col.cx = FilterPopupSizing::ScalePixels(280, FilterPopupSizing::AnchorDpi(hwnd));
    wchar_t empty[] = L"";
    col.pszText = empty;
    ListView_InsertColumn(m_filterPopupList, 0, &col);

    if (m_font)
        SendMessageW(m_filterPopupList, WM_SETFONT, (WPARAM)m_font, FALSE);

    // Визуал как у списков проводника (hot track и пр. без собственного CUSTOMDRAW).
    (void)::SetWindowTheme(m_filterPopupList, L"Explorer", nullptr);

    (void)::SetWindowSubclass(m_filterPopupList, FilterPopupListSubclassProc, kFilterPopupListSubclassId, 0);

    if (LogViewerGlass::GetWndCompatHandleWinEvent())
        LogViewerWndCompatScan::AttachLogViewerWindowTree(m_filterPopup, nullptr);

    // ListView_Attach выставляет BkColor 0 — для фильтра оставляем прежнюю палитру списка логов.
    ListView_SetBkColor(m_filterPopupList, LogViewerListBgBase);
    ListView_SetTextBkColor(m_filterPopupList, CLR_NONE);
}

bool UiMainWindowImpl::IsFilterPopupOpen() const noexcept
{
    if (!m_filterPopup || ::IsWindow(m_filterPopup) == FALSE)
        return false;
    // Glass PopupIn держит HWND в DWM cloak — IsWindowVisible даёт FALSE, хотя попап показан.
    return (::GetWindowLongPtrW(m_filterPopup, GWL_STYLE) & WS_VISIBLE) != 0;
}

void UiMainWindowImpl::RequestFilterPopupHotSwitch(int kind)
{
    if (m_tabs.empty() || kind < 0 || kind > 2)
        return;
    if (!IsFilterPopupOpen())
        return;
    if (kind == m_filterPopupKind)
        return;
    m_filterResumeKind = kind;
    CloseFilterPopup(false, false);
}

void UiMainWindowImpl::SwitchFilterPopupIfOpen(int kind)
{
    if (m_tabs.empty() || kind < 0 || kind > 2)
        return;
    if (!IsFilterPopupOpen())
        return;
    if (kind == m_filterPopupKind)
        return;
    RequestFilterPopupHotSwitch(kind);
}

void UiMainWindowImpl::RunFilterPopupModalLoop()
{
    if (!hwnd)
        return;
    for (;;)
    {
        if (!IsFilterPopupOpen() || !m_filterPopup || ::IsWindow(m_filterPopup) == FALSE)
            break;

        (void)::MsgWaitForMultipleObjectsEx(
            0, nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                ::PostQuitMessage(static_cast<int>(msg.wParam));
                return;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
}

void UiMainWindowImpl::OpenFilterDropdownModal(int kind)
{
    HWND hMainCaptured = hwnd;
    if (m_tabs.empty())
        return;
    m_filterResumeKind = -1;
    if (m_filterPopup && ::IsWindow(m_filterPopup) == FALSE)
    {
        m_filterPopup = nullptr;
        m_filterListviewPopup = nullptr;
        m_filterPopupList = nullptr;
    }
    if (IsFilterPopupOpen() && m_filterPopupKind == kind)
    {
        CloseFilterPopup(true);
        if (hwnd && ::IsWindow(hwnd) && hFilterTb && ::IsWindow(hFilterTb))
            (void)::PostMessageW(hwnd, WM_APP_FILTER_TOOLBAR_POST_SYNC, 0, 0);
        return;
    }
    if (hwnd && ::IsWindow(hwnd))
        (void)::KillTimer(hwnd, IDT_FILTER_TOOLBAR_LEGACY);
    OpenFilterPopup(kind);
    RunFilterPopupModalLoop();
    // Модальный цикл мог обработать WM_DESTROY: GWLP_USERDATA обнулён, this уничтожен — не обращаться к полям impl.
    if (!hMainCaptured || ::IsWindow(hMainCaptured) == FALSE
        || ::GetWindowLongPtrW(hMainCaptured, GWLP_USERDATA) == 0)
        return;
    const int resumeKind = m_filterResumeKind;
    m_filterResumeKind = -1;
    if (resumeKind >= 0 && resumeKind <= 2 && hwnd && ::IsWindow(hwnd) != FALSE
        && ::GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0)
    {
        (void)::PostMessageW(hwnd, WM_APP_FILTER_RESUME_DROPDOWN, static_cast<WPARAM>(resumeKind), 0);
    }
    else if (hwnd && ::IsWindow(hwnd) && hFilterTb && ::IsWindow(hFilterTb) && !IsFilterPopupOpen()
        && ::GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0)
    {
        // После возврата из TBN_DROPDOWN comctl32 может снова отрисовать кнопку нажатой — PostSync в следующей итерации очереди.
        (void)::PostMessageW(hwnd, WM_APP_FILTER_TOOLBAR_POST_SYNC, 0, 0);
    }
}

void UiMainWindowImpl::FillFilterPopupList()
{
    if (!m_filterPopupList || m_tabs.empty())
        return;
    ListView_DeleteAllItems(m_filterPopupList);
    m_filterPopupRowStrings.clear();

    TabView& tab = ActiveTab();

    if (m_filterPopupKind == 0)
    {
        std::set<uint32_t> pidsInScope;
        for (const auto& e : m_allLogs)
        {
            if (PassesLevelForFilters(e, tab))
                pidsInScope.insert(e.processId);
        }
        const bool allOn = !tab.m_restrictByPid;
        for (uint32_t pid : pidsInScope)
        {
            EnsurePidLabel(pid);
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = ListView_GetItemCount(m_filterPopupList);
            it.pszText = const_cast<wchar_t*>(m_pidLabels[pid].c_str());
            it.lParam = static_cast<LPARAM>(static_cast<ULONG_PTR>(pid));
            const int idx = ListView_InsertItem(m_filterPopupList, &it);
            if (idx >= 0)
                ListView_SetCheckState(m_filterPopupList, idx,
                    (allOn || tab.m_filterPids.contains(pid)) ? TRUE : FALSE);
        }
    }
    else if (m_filterPopupKind == 1)
    {
        std::set<std::wstring> modsInScope;
        for (const auto& e : m_allLogs)
        {
            if (!PassesForModuleFilterOptions(e, tab))
                continue;
            if (!e.module.empty())
                modsInScope.insert(e.module);
        }
        const bool allOn = !tab.m_restrictByModule;
        for (const auto& mod : modsInScope)
        {
            m_filterPopupRowStrings.push_back(mod);
            const size_t k = m_filterPopupRowStrings.size() - 1;
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = ListView_GetItemCount(m_filterPopupList);
            it.pszText = const_cast<wchar_t*>(m_filterPopupRowStrings[k].c_str());
            it.lParam = static_cast<LPARAM>(k);
            const int idx = ListView_InsertItem(m_filterPopupList, &it);
            if (idx >= 0)
                ListView_SetCheckState(m_filterPopupList, idx,
                    (allOn || tab.m_filterModules.contains(mod)) ? TRUE : FALSE);
        }
    }
    else if (m_filterPopupKind == 2)
    {
        std::set<std::wstring> subsInScope;
        for (const auto& e : m_allLogs)
        {
            if (!PassesForSubsystemFilterOptions(e, tab))
                continue;
            if (!e.subsystem.empty())
                subsInScope.insert(e.subsystem);
        }
        const bool allOn = !tab.m_restrictBySubsystem;
        for (const auto& sub : subsInScope)
        {
            m_filterPopupRowStrings.push_back(sub);
            const size_t k = m_filterPopupRowStrings.size() - 1;
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = ListView_GetItemCount(m_filterPopupList);
            it.pszText = const_cast<wchar_t*>(m_filterPopupRowStrings[k].c_str());
            it.lParam = static_cast<LPARAM>(k);
            const int idx = ListView_InsertItem(m_filterPopupList, &it);
            if (idx >= 0)
                ListView_SetCheckState(m_filterPopupList, idx,
                    (allOn || tab.m_filterSubsystems.contains(sub)) ? TRUE : FALSE);
        }
    }
}

void UiMainWindowImpl::OpenFilterPopup(int kind)
{
    if (m_tabs.empty())
        return;

    if (m_filterPopup && ::IsWindow(m_filterPopup) == FALSE)
    {
        m_filterPopup = nullptr;
        m_filterListviewPopup = nullptr;
        m_filterPopupList = nullptr;
    }
    CloseFilterPopup(false, false);
    EnsureFilterPopupCreated();
    if (!m_filterPopup || !m_filterPopupList || !m_filterListviewPopup)
        return;

    m_filterPopupKind = kind;
    SetWindowLongPtrW(m_filterPopup, GWLP_USERDATA, (LONG_PTR)this);
    SetWindowLongPtrW(m_filterListviewPopup, GWLP_USERDATA, (LONG_PTR)this);

    FillFilterPopupList();

    UINT btnId = IDC_PROC_BTN;
    if (kind == 1)
        btnId = IDC_MOD_BTN;
    else if (kind == 2)
        btnId = IDC_SUB_BTN;

    // DropDown.au3 / WorkerW: PressButton до показа окна; TBN_DROPDOWN не даёт WM_UNINITMENUPOPUP как у HMENU.
    if (hFilterTb)
        FilterToolbarSyncDropdownPressed(hFilterTb, btnId);

    RECT r{};
    SendMessageW(hFilterTb, TB_GETRECT, btnId, (LPARAM)&r);
    MapWindowPoints(hFilterTb, nullptr, (LPPOINT)&r, 2);
    const int btnMinW = (std::max)(1, static_cast<int>(r.right - r.left));

    const UINT dpi = FilterPopupSizing::AnchorDpi(m_filterPopupList);
    const int pad = FilterPopupSizing::ContentPadPx(hwnd);
    const int vm = FilterPopupSizing::ClientVertMarginPx(hwnd);

    const int nItems = ListView_GetItemCount(m_filterPopupList);
    const int visibleRows = (std::min)(
        (std::max)(nItems, kFilterPopupMinVisibleRows), kFilterPopupMaxVisibleRows);
    const bool needsVertScroll = nItems > visibleRows;
    const int vsw = needsVertScroll ? GetVerticalScrollbarTrackWidth(m_filterPopupList, dpi) : 0;

    const int colContent = MeasureFilterPopupColumnContentWidth(m_filterPopupList, dpi);
    const int listTotalW = colContent + vsw;
    const int maxListW = FilterPopupSizing::ScalePixels(kFilterPopupMaxListClientWidthAt96, dpi);
    int idealListW = (std::max)((std::min)(listTotalW, maxListW), btnMinW);
    int colW = idealListW - (needsVertScroll ? vsw : 0);
    if (colW < 1)
        colW = 1;

    const int itemH = MeasureFilterPopupListRowHeight(m_filterPopupList, colW, dpi);
    const int listH = itemH * visibleRows;
    const int clientH = listH + vm;

    // Listview: ширина окна = idealListW; клиент DropDown: +2×pad до оболочки +2×pad до внешнего края.
    const int dropClientW = idealListW + 4 * pad;

    RECT adjOuter{ 0, 0, dropClientW, clientH };
    const DWORD wStyle = static_cast<DWORD>(GetWindowLongPtrW(m_filterPopup, GWL_STYLE));
    const DWORD wEx = static_cast<DWORD>(GetWindowLongPtrW(m_filterPopup, GWL_EXSTYLE));
    AdjustWindowRectEx(&adjOuter, wStyle, FALSE, wEx);
    const int outerW = adjOuter.right - adjOuter.left;
    const int outerH = adjOuter.bottom - adjOuter.top;

    int yTop = r.bottom;
    MONITORINFO mi{ sizeof mi };
    const HMONITOR mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (mon && ::GetMonitorInfoW(mon, &mi))
    {
        if (yTop + outerH > mi.rcWork.bottom)
            yTop = mi.rcWork.bottom - outerH;
        if (yTop < mi.rcWork.top)
            yTop = mi.rcWork.top;
    }

    // Финальная геометрия без показа: сначала ApplyEffect / g_dropDownCtx / PopupIn, иначе первый
    // видимый кадр — чёрная NC/erase до DWM и дубль WM_SHOWWINDOW гасится дедупом OnDropDownShow.
    SetWindowPos(m_filterPopup, HWND_TOP, r.left, yTop, outerW, outerH, SWP_NOACTIVATE);

    if (LogViewerGlass::IsGlassCoreLoaded())
    {
        if (const auto pApply = LogViewerGlass::GetGlassCoreApplyDropDownShow())
            pApply(m_filterPopup);
    }

    SetWindowPos(m_filterPopup, HWND_TOP, r.left, yTop, outerW, outerH,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);

    if (m_filterPopupList && ::IsWindow(m_filterPopupList))
        (void)::SetFocus(m_filterPopupList);
}

void UiMainWindowImpl::ApplyFilterPopupSelection()
{
    if (!m_filterPopupList || m_tabs.empty())
        return;

    TabView& tab = ActiveTab();
    const int n = ListView_GetItemCount(m_filterPopupList);
    int checked = 0;
    for (int i = 0; i < n; ++i)
    {
        if (ListView_GetCheckState(m_filterPopupList, i))
            ++checked;
    }

    if (m_filterPopupKind == 0)
    {
        tab.m_filterPids.clear();
        if (n == 0 || checked == n)
            tab.m_restrictByPid = false;
        else if (checked == 0)
            tab.m_restrictByPid = true;
        else
        {
            tab.m_restrictByPid = true;
            for (int i = 0; i < n; ++i)
            {
                if (!ListView_GetCheckState(m_filterPopupList, i))
                    continue;
                LVITEMW it{};
                it.mask = LVIF_PARAM;
                it.iItem = i;
                ListView_GetItem(m_filterPopupList, &it);
                tab.m_filterPids.insert(static_cast<uint32_t>(static_cast<ULONG_PTR>(it.lParam)));
            }
        }
    }
    else if (m_filterPopupKind == 1)
    {
        tab.m_filterModules.clear();
        if (n == 0 || checked == n)
            tab.m_restrictByModule = false;
        else if (checked == 0)
            tab.m_restrictByModule = true;
        else
        {
            tab.m_restrictByModule = true;
            for (int i = 0; i < n; ++i)
            {
                if (!ListView_GetCheckState(m_filterPopupList, i))
                    continue;
                LVITEMW it{};
                it.mask = LVIF_PARAM;
                it.iItem = i;
                ListView_GetItem(m_filterPopupList, &it);
                const size_t k = static_cast<size_t>(it.lParam);
                if (k < m_filterPopupRowStrings.size())
                    tab.m_filterModules.insert(m_filterPopupRowStrings[k]);
            }
        }
    }
    else if (m_filterPopupKind == 2)
    {
        tab.m_filterSubsystems.clear();
        if (n == 0 || checked == n)
            tab.m_restrictBySubsystem = false;
        else if (checked == 0)
            tab.m_restrictBySubsystem = true;
        else
        {
            tab.m_restrictBySubsystem = true;
            for (int i = 0; i < n; ++i)
            {
                if (!ListView_GetCheckState(m_filterPopupList, i))
                    continue;
                LVITEMW it{};
                it.mask = LVIF_PARAM;
                it.iItem = i;
                ListView_GetItem(m_filterPopupList, &it);
                const size_t k = static_cast<size_t>(it.lParam);
                if (k < m_filterPopupRowStrings.size())
                    tab.m_filterSubsystems.insert(m_filterPopupRowStrings[k]);
            }
        }
    }

    PruneTabFiltersForCascade(tab);
    RebuildVisible();
    UpdateProcButtonText();
    UpdateModButtonText();
    UpdateSubButtonText();
}

void UiMainWindowImpl::CloseFilterPopup(bool apply, bool restoreKeyboardFocus)
{
    if (s_inCloseFilterPopup)
        return;

    if (m_filterPopup && ::IsWindow(m_filterPopup) == FALSE)
    {
        m_filterPopup = nullptr;
        m_filterListviewPopup = nullptr;
        m_filterPopupList = nullptr;
    }

    if (m_filterPopup)
    {
        s_inCloseFilterPopup = true;

        if (apply && m_filterPopupList && !m_tabs.empty())
            ApplyFilterPopupSelection();

        DestroyFilterPopup();

        s_inCloseFilterPopup = false;
    }

    // DropDown.au3 при WM_CLOSE дропа: WM_UNINITMENUPOPUP + WM_MENUSELECT(0,0xFFFF) на hwndParent.
    if (hwnd)
        (void)::SendMessageW(hwnd, WM_MENUSELECT, MAKEWPARAM(0, 0xFFFF), 0);

    // Сессия дропдауна завершена: снять устаревший legacy-таймер тулбара (если был).
    if (hwnd && ::IsWindow(hwnd))
        (void)::KillTimer(hwnd, IDT_FILTER_TOOLBAR_LEGACY);

    // WorkerW.au3 WM_UNINITMENUPOPUP: PressButton False; у нас HMENU нет — тот же смысл через TB_*.
    if (hFilterTb)
        FilterToolbarSyncDropdownPressed(hFilterTb, 0);

    if (restoreKeyboardFocus && hwnd && !m_tabs.empty())
    {
        HWND hList = ActiveTab().hList;
        if (hList && ::IsWindow(hList))
        {
            HWND fg = ::GetForegroundWindow();
            if (fg == hwnd || ::IsChild(hwnd, fg))
                (void)::SetFocus(hList);
        }
    }
}
