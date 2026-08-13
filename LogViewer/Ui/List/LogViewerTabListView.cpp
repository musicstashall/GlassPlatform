#include "SharedPch.h"

#include "framework.h"
#include "Ui/List/LogViewerTabListView.h"
#include "Ui/Theme/LogViewerListTheme.h"
#include "LogViewerLevelColors.h"
#include <commctrl.h>

HWND CreateLogViewerTabListView(HWND /*hwndParent*/, HWND hTabCtrl, HFONT monoFont)
{
    RECT rcTab;
    GetClientRect(hTabCtrl, &rcTab);
    TabCtrl_AdjustRect(hTabCtrl, FALSE, &rcTab);

    HWND hList = CreateWindowExW(
        WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
        WS_CHILD | WS_CLIPSIBLINGS | LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        rcTab.left, rcTab.top,
        rcTab.right - rcTab.left, rcTab.bottom - rcTab.top,
        hTabCtrl, nullptr,
        GetModuleHandleW(nullptr), nullptr
    );

    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(hList, LogViewerListBgBase);
    ListView_SetTextBkColor(hList, CLR_NONE);
    ListView_SetTextColor(hList, LogViewerColors::kLevelFg[0]);

    int ci = 0;
    auto addCol = [&](const wchar_t* n, int w, int fmt = LVCFMT_LEFT)
        {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            c.pszText = const_cast<wchar_t*>(n);
            c.cx = w; c.fmt = fmt;
            ListView_InsertColumn(hList, ci++, &c);
        };
    addCol(L"#", 52, LVCFMT_RIGHT);
    addCol(L"Время", 118);
    addCol(L"PID", 60, LVCFMT_RIGHT);
    addCol(L"TID", 60, LVCFMT_RIGHT);
    addCol(L"Ур.", 50);
    addCol(L"Текст", 800);

    SendMessageW(hList, WM_SETFONT, (WPARAM)monoFont, TRUE);
    return hList;
}
