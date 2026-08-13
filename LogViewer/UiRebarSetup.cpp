#include "SharedPch.h"

#include "framework.h"
#include "UiConsts.h"
#include "UiRebarSetup.h"
#include "LogViewerLevelColors.h"
#include <commctrl.h>
#include <Uxtheme.h>

#pragma comment(lib, "uxtheme.lib")

// Стиль Band 0 (Ctrl) — CCS_NORESIZE (0x04) присутствует в обоих.
// 0x5600B84D = WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN
//            | TBSTYLE_TRANSPARENT|TBSTYLE_CUSTOMERASE|TBSTYLE_LIST|TBSTYLE_FLAT
//            | CCS_NODIVIDER|CCS_NOPARENTALIGN|CCS_NORESIZE|CCS_TOP
// Используется для Band 1 (Filter) и Band 2 (Search).
// CCS_NORESIZE: тулбар не отвечает на авто-ресайз родителя → сохраняет высоту CTRL_TB_H.
// Когда cyChild(CTRL_TB_H) < cyMinChild(BAND_H) в REBARBANDINFO, Rebar ЦЕНТРИРУЕТ тулбар.
static constexpr DWORD TB_STYLE_FLEX = 0x5600B84DUL;

// Стиль Band 0 (Ctrl) — дополнительно WS_TABSTOP и TBSTYLE_TOOLTIPS вместо LIST/CUSTOMERASE.
// 0x5601894D = ...| TBSTYLE_TRANSPARENT|TBSTYLE_TOOLTIPS|TBSTYLE_FLAT
//            | CCS_NODIVIDER|CCS_NOPARENTALIGN|CCS_NORESIZE|CCS_TOP
static constexpr DWORD TB_STYLE_FIXED = 0x5601894DUL;

// ---------------------------------------------------------------------------
// ImageList: Start/Stop (круги), параметры отрисовки, средний тулбар (круги/квадраты)
// ---------------------------------------------------------------------------
HIMAGELIST CreateLogViewerToolbarImages(int cx, int cy)
{
    HIMAGELIST himl = ImageList_Create(cx, cy, ILC_COLOR32 | ILC_MASK, 14, 4);
    if (!himl)
        return nullptr;
    HDC hdcScreen = GetDC(nullptr);
    auto fillBlackBg = [&](HDC hdc) {
        HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
        RECT   rc = { 0, 0, cx, cy };
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);
    };
    auto addCircle = [&](COLORREF clr) {
        HDC     hdc = CreateCompatibleDC(hdcScreen);
        HBITMAP bmp = CreateCompatibleBitmap(hdcScreen, cx, cy);
        HBITMAP old = (HBITMAP)SelectObject(hdc, bmp);
        fillBlackBg(hdc);
        HBRUSH br = CreateSolidBrush(clr);
        HPEN   pn = CreatePen(PS_SOLID, 1, clr);
        SelectObject(hdc, br);
        SelectObject(hdc, pn);
        Ellipse(hdc, 1, 1, cx - 1, cy - 1);
        DeleteObject(br);
        DeleteObject(pn);
        SelectObject(hdc, old);
        ImageList_AddMasked(himl, bmp, RGB(0, 0, 0));
        DeleteObject(bmp);
        DeleteDC(hdc);
    };
    auto addSquare = [&](COLORREF fill, COLORREF border) {
        HDC     hdc = CreateCompatibleDC(hdcScreen);
        HBITMAP bmp = CreateCompatibleBitmap(hdcScreen, cx, cy);
        HBITMAP old = (HBITMAP)SelectObject(hdc, bmp);
        fillBlackBg(hdc);
        HBRUSH br = CreateSolidBrush(fill);
        HPEN   pn = CreatePen(PS_SOLID, 1, border);
        SelectObject(hdc, br);
        SelectObject(hdc, pn);
        Rectangle(hdc, 2, 2, cx - 2, cy - 2);
        DeleteObject(br);
        DeleteObject(pn);
        SelectObject(hdc, old);
        ImageList_AddMasked(himl, bmp, RGB(0, 0, 0));
        DeleteObject(bmp);
        DeleteDC(hdc);
    };

    addCircle(RGB(128, 128, 128));   // 0: старт (серый)
    addCircle(RGB(50, 180, 80));     // 1: стоп (зелёный)
    addSquare(RGB(70, 150, 230), RGB(40, 100, 200)); // 2: параметры отрисовки
    addCircle(RGB(60, 120, 220));    // 3: процессы
    addCircle(RGB(40, 160, 130));    // 4: модули
    addCircle(RGB(220, 140, 40));    // 5: подсистемы
    addSquare(RGB(220, 70, 70), RGB(160, 40, 40)); // 6: очистить
    addSquare(RGB(180, 90, 220), RGB(120, 50, 160)); // 7: инверсия
    auto addLevelSquare = [&](std::size_t idx) {
        const COLORREF fill = LogViewerColors::ToolbarSquareRgb(idx);
        const BYTE       r = GetRValue(fill), g = GetGValue(fill), b = GetBValue(fill);
        const COLORREF     border =
            RGB((r > 55 ? r - 40 : 0), (g > 55 ? g - 40 : 0), (b > 55 ? b - 40 : 0));
        addSquare(fill, border);
    };
    for (std::size_t i = 0; i < 5; ++i)
        addLevelSquare(i); // 8..12: N I W E C — как цвет текста DBG_* в списке

    ReleaseDC(nullptr, hdcScreen);
    return himl;
}

// ---------------------------------------------------------------------------
// AddRebarBand
// ---------------------------------------------------------------------------
void AddRebarBand(HWND hRebar, HWND hChild,
    const wchar_t* pszText,
    int cx, int cyChild, int cyBand,
    DWORD extraStyle)
{
    if (cyBand < cyChild) cyBand = cyChild;
    REBARBANDINFOW rbi{};
    rbi.cbSize = sizeof(REBARBANDINFOW);
    rbi.fMask = RBBIM_STYLE | RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_SIZE;
    rbi.fStyle = RBBS_NOGRIPPER | RBBS_FIXEDSIZE | extraStyle;
    rbi.hwndChild = hChild;
    rbi.cyChild = cyChild;    // реальная высота → Rebar центрирует если < cyMinChild
    rbi.cyMinChild = cyBand;
    rbi.cxMinChild = cx;
    rbi.cx = cx;
    if (pszText && pszText[0]) {
        rbi.fMask |= RBBIM_TEXT;
        rbi.lpText = const_cast<wchar_t*>(pszText);
    }
    SendMessageW(hRebar, RB_INSERTBAND, (WPARAM)-1, (LPARAM)&rbi);
}

// ---------------------------------------------------------------------------
// Band 0: Start/Stop + Pause  (кнопки BAND_H)
// ---------------------------------------------------------------------------
HWND CreateCtrlToolbar(HWND hwndParent, HIMAGELIST hImgList, HFONT hFont)
{
    // TBSTYLE_LIST: строка «иконка + текст» как единый блок с нормальной вертикалью относительно BAND_H.
    HWND hTb = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        TB_STYLE_FIXED | TBSTYLE_LIST, 0, 0, 0, BAND_H,
        hwndParent, (HMENU)(UINT_PTR)IDC_CTRL_TB,
        GetModuleHandleW(nullptr), nullptr);

    SendMessageW(hTb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(hTb, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);
    SendMessageW(hTb, TB_SETIMAGELIST, 0, (LPARAM)hImgList);
    SendMessageW(hTb, TB_SETBITMAPSIZE, 0, MAKELPARAM(16, 16));
    if (hFont) SendMessageW(hTb, WM_SETFONT, (WPARAM)hFont, FALSE);
    SendMessageW(hTb, TB_SETPADDING, 0, MAKELPARAM(8, 0));
    SendMessageW(hTb, TB_SETBUTTONSIZE, 0, MAKELPARAM(0, BAND_H));
    // Вертикальное центрирование подписей; без DT_END_ELLIPSIS — иначе тема иногда сжимает rect по высоте.
    SendMessageW(hTb, TB_SETDRAWTEXTFLAGS,
        DT_LEFT | DT_CENTER | DT_RIGHT | DT_TOP | DT_BOTTOM | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX,
        DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetWindowTheme(hTb, L"Explorer", nullptr);

    TBBUTTON btns[] = {
        { 0, IDC_START_STOP, TBSTATE_ENABLED,
          BTNS_CHECK | BTNS_SHOWTEXT | BTNS_AUTOSIZE, {}, 0,
          (INT_PTR)L"\u0417\u0430\u043f\u0443\u0441\u0442\u0438\u0442\u044c" },
        { 4, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0 },
        { I_IMAGENONE, IDC_PAUSE_BTN, TBSTATE_ENABLED,
          BTNS_CHECK | BTNS_AUTOSIZE, {}, 0,
          (INT_PTR)L"\u23F8 \u041f\u0430\u0443\u0437\u0430" },
        { 4, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0 },
        { I_IMAGENONE, IDC_GLASS_SETTINGS, TBSTATE_ENABLED,
          BTNS_BUTTON | BTNS_AUTOSIZE, {}, 0,
          (INT_PTR)L"\u041f\u0430\u0440\u0430\u043c\u0435\u0442\u0440\u044b" },
    };
    SendMessageW(hTb, TB_ADDBUTTONS, (WPARAM)_countof(btns), (LPARAM)btns);
    SendMessageW(hTb, TB_AUTOSIZE, 0, 0);
    // TB_AUTOSIZE может уменьшить высоту кнопок до «идеальной» — возвращаем высоту ленты рябара.
    SendMessageW(hTb, TB_SETBUTTONSIZE, 0, MAKELPARAM(0, BAND_H));
    return hTb;
}

// ---------------------------------------------------------------------------
// Band 1: Proc + Mod | Clear + Invert | N/I/W/E/C  (кнопки CTRL_TB_H)
// Rebar центрирует тулбар вертикально (cyChild=CTRL_TB_H < cyBand=BAND_H).
// ---------------------------------------------------------------------------
HWND CreateFilterToolbar(HWND hwndParent, HIMAGELIST hImgList, HFONT hFont)
{
    static constexpr DWORD kFilterTbWindowStyle = 0x5600B84DUL;
    static constexpr DWORD kFilterTbExtendedStyle = 0x481UL;

    HWND hTb = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        kFilterTbWindowStyle, 0, 0, 0, CTRL_TB_H,
        hwndParent, (HMENU)(UINT_PTR)IDC_FILTER_TB,
        GetModuleHandleW(nullptr), nullptr);

    SendMessageW(hTb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(hTb, TB_SETEXTENDEDSTYLE, kFilterTbExtendedStyle, kFilterTbExtendedStyle);
    SetWindowTheme(hTb, L"InactiveBBComposited", nullptr);
    SendMessageW(hTb, TB_SETINDENT, 10, 0);

    SendMessageW(hTb, TB_SETIMAGELIST, 0, (LPARAM)hImgList);
    SendMessageW(hTb, TB_SETBITMAPSIZE, 0, MAKELPARAM(16, 16));
    if (hFont) SendMessageW(hTb, WM_SETFONT, (WPARAM)hFont, FALSE);
    SendMessageW(hTb, TB_SETPADDING, 0, MAKELPARAM(6, 1));
    SendMessageW(hTb, TB_SETDRAWTEXTFLAGS,
        DT_LEFT | DT_CENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_VCENTER | DT_SINGLELINE,
        DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE);

    const int idxTbStrEmpty = (int)SendMessageW(hTb, TB_ADDSTRING, 0, (LPARAM)L"");

    // BTNS_DROPDOWN: TBN_DROPDOWN при клике по стрелке; основная часть — отдельное поведение (пробуем макет).
    const BYTE stFilterDrop = static_cast<BYTE>(TBSTATE_ENABLED | TBSTATE_ELLIPSES);
    const BYTE fsFilterDrop = static_cast<BYTE>(
        BTNS_AUTOSIZE | BTNS_DROPDOWN | BTNS_NOPREFIX | BTNS_SHOWTEXT);

    TBBUTTON btns[] = {
        { 3, IDC_PROC_BTN, stFilterDrop,
          fsFilterDrop, {}, 0,
          (INT_PTR)L"\u0412\u0441\u0435 \u043f\u0440\u043e\u0446\u0435\u0441\u0441\u044b" },
        { 4, IDC_MOD_BTN, stFilterDrop,
          fsFilterDrop, {}, 0,
          (INT_PTR)L"\u0412\u0441\u0435 \u043c\u043e\u0434\u0443\u043b\u0438" },
        { 5, IDC_SUB_BTN, stFilterDrop,
          fsFilterDrop, {}, 0,
          (INT_PTR)L"\u0412\u0441\u0435 \u043f\u043e\u0434\u0441\u0438\u0441\u0442\u0435\u043c\u044b" },
        { 8, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0 },
        { 6, IDC_CLEAR, TBSTATE_ENABLED,
          BTNS_BUTTON | BTNS_AUTOSIZE, {}, 0,
          (INT_PTR)L"\u2715 \u041e\u0447\u0438\u0441\u0442\u0438\u0442\u044c" },
        { 4, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0 },
        { 7, IDC_INVERT, TBSTATE_ENABLED,
          BTNS_BUTTON | BTNS_AUTOSIZE, {}, 0,
          (INT_PTR)L"\u21C4 \u0418\u043d\u0432\u0435\u0440\u0441\u0438\u044f" },
        { 8, 0, TBSTATE_ENABLED, BTNS_SEP, {}, 0, 0 },
        { 8, (int)(IDC_LVL_BASE + 0), TBSTATE_ENABLED | TBSTATE_CHECKED,
          BTNS_CHECK | BTNS_AUTOSIZE, {}, 0, (INT_PTR)idxTbStrEmpty },
        { 9, (int)(IDC_LVL_BASE + 1), TBSTATE_ENABLED | TBSTATE_CHECKED,
          BTNS_CHECK | BTNS_AUTOSIZE, {}, 0, (INT_PTR)idxTbStrEmpty },
        { 10, (int)(IDC_LVL_BASE + 2), TBSTATE_ENABLED | TBSTATE_CHECKED,
          BTNS_CHECK | BTNS_AUTOSIZE, {}, 0, (INT_PTR)idxTbStrEmpty },
        { 11, (int)(IDC_LVL_BASE + 3), TBSTATE_ENABLED | TBSTATE_CHECKED,
          BTNS_CHECK | BTNS_AUTOSIZE, {}, 0, (INT_PTR)idxTbStrEmpty },
        { 12, (int)(IDC_LVL_BASE + 4), TBSTATE_ENABLED | TBSTATE_CHECKED,
          BTNS_CHECK | BTNS_AUTOSIZE, {}, 0, (INT_PTR)idxTbStrEmpty },
    };
    SendMessageW(hTb, TB_ADDBUTTONS, (WPARAM)_countof(btns), (LPARAM)btns);

    auto fixWidth = [&](UINT id, WORD w) {
        TBBUTTONINFOW tbi{};
        tbi.cbSize = sizeof(tbi);
        tbi.dwMask = TBIF_SIZE;
        tbi.cx = w;
        SendMessageW(hTb, TB_SETBUTTONINFO, id, (LPARAM)&tbi);
        };
    fixWidth(IDC_PROC_BTN, PROC_CBX_W);
    fixWidth(IDC_MOD_BTN, MOD_CBX_W);
    fixWidth(IDC_SUB_BTN, SUB_CBX_W);
    for (int i = 0; i < 5; ++i)
        fixWidth(static_cast<UINT>(IDC_LVL_BASE + i), 26);

    TBMETRICS metrics{};
    metrics.cbSize = sizeof(metrics);
    metrics.dwMask = 0;
    SendMessageW(hTb, TB_SETMETRICS, 0, reinterpret_cast<LPARAM>(&metrics));

    constexpr int kButtonHeight = CTRL_TB_H;
    constexpr int kButtonWidth = 0;
    SendMessageW(hTb, TB_SETBUTTONSIZE, 0, MAKELPARAM(kButtonWidth, kButtonHeight));

    SendMessageW(hTb, TB_AUTOSIZE, 0, 0);
    return hTb;
}

// ---------------------------------------------------------------------------
// Band 2: одна кнопка «Поиск», BAND_H; Edit накладывается поверх по клику.
// Ширина кнопки задаётся динамически через UpdateSearchButtonWidth().
// ---------------------------------------------------------------------------
HWND CreateSearchToolbar(HWND hwndParent, HFONT hFont)
{
    HWND hTb = CreateWindowExW(0, TOOLBARCLASSNAMEW, nullptr,
        TB_STYLE_FLEX,
        0, 0, 0, BAND_H,
        hwndParent, (HMENU)(UINT_PTR)IDC_SEARCH_TB,
        GetModuleHandleW(nullptr), nullptr);

    SendMessageW(hTb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(hTb, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);
    SendMessageW(hTb, TB_SETIMAGELIST, 0, (LPARAM)nullptr);
    if (hFont) SendMessageW(hTb, WM_SETFONT, (WPARAM)hFont, FALSE);
    SendMessageW(hTb, TB_SETPADDING, 0, MAKELPARAM(8, 0));
    SendMessageW(hTb, TB_SETBUTTONSIZE, 0, MAKELPARAM(0, BAND_H));
    // Текст слева, обрезка с многоточием при нехватке места
    SendMessageW(hTb, TB_SETDRAWTEXTFLAGS, DT_CENTER | DT_END_ELLIPSIS, DT_LEFT | DT_END_ELLIPSIS | DT_SINGLELINE);
    SetWindowTheme(hTb, L"Explorer", nullptr);

    TBBUTTON btn = { I_IMAGENONE, IDC_SEARCH_BTN,
                     static_cast<BYTE>(TBSTATE_ENABLED | TBSTATE_ELLIPSES),
                     BTNS_BUTTON, {}, 0,
                     (INT_PTR)L"\u041f\u043e\u0438\u0441\u043a \u0432 \u0434\u043e\u043a\u0443\u043c\u0435\u043d\u0442\u0435" };
    SendMessageW(hTb, TB_ADDBUTTONS, 1, (LPARAM)&btn);
    SendMessageW(hTb, TB_AUTOSIZE, 0, 0);
    return hTb;
}