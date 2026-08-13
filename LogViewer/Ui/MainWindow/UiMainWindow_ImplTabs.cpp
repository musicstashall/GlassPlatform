#include "SharedPch.h"

#include "framework.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "Ui/List/LogViewerTabListView.h"
#include "LogViewerGlassHost.h"
#include "LogViewerWndCompatScan.h"
#include <commctrl.h>

void UiMainWindowImpl::AddTab()
{
    int newIdx = (int)m_tabs.size();
    wchar_t name[32];
    swprintf_s(name, newIdx == 0 ? L"Основной" : L"Вид %d", newIdx + 1);

    TCITEMW tci{}; tci.mask = TCIF_TEXT; tci.pszText = name;
    TabCtrl_InsertItem(hTabCtrl, newIdx, &tci);

    TabView tab;
    tab.name = name;
    tab.hList = CreateLogViewerTabListView(hwnd, hTabCtrl, m_monoFont);
    if (LogViewerGlass::IsGlassCoreLoaded() && LogViewerGlass::GetWndCompatHandleWinEvent())
    {
        // Вкладка 0: то же поддерево уже проходит синтетический CREATE в WM_CREATE
        // (AttachLogViewerWindowTree по корню главного окна). Повторный вызов здесь
        // давал двойной WndHandler и «двойные» логи/хуки на ListView.
        if (newIdx > 0)
            LogViewerWndCompatScan::AttachLogViewerControlSubtree(tab.hList, nullptr);
    }

    tab.m_visibleIdx.reserve(m_allLogs.size());
    for (int i = 0; i < (int)m_allLogs.size(); ++i)
        if (PassFilter(m_allLogs[i], tab))
            tab.m_visibleIdx.push_back(i);

    if (!m_tabs.empty())
        ShowWindow(m_tabs[m_activeTab].hList, SW_HIDE);

    m_tabs.push_back(std::move(tab));
    m_activeTab = newIdx;

    ShowWindow(m_tabs[m_activeTab].hList, SW_SHOW);
    TabCtrl_SetCurSel(hTabCtrl, newIdx);
    UpdateControlsFromTab(m_tabs[m_activeTab]);
    CommitListView();
}
