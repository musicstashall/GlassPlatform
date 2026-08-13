#include "SharedPch.h"

#include "framework.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "Ui/Helpers/LogViewerUiHelpers.h"
#include "Ui/Theme/LogViewerListTheme.h"
#include "Ui/Model/LogViewerLogModel.h"
#include "LogClient.h"

TabView* UiMainWindowImpl::FindTab(HWND hList)
{
    for (auto& t : m_tabs)
        if (t.hList == hList) return &t;
    return nullptr;
}

COLORREF UiMainWindowImpl::GetThreadBg(uint32_t pid, uint32_t tid)
{
    auto key = std::make_pair(pid, tid);
    auto it = m_tidColorMap.find(key);
    if (it == m_tidColorMap.end())
    {
        int idx = m_nextColorIdx;
        m_tidColorMap[key] = idx;
        m_nextColorIdx = (idx % (kLogViewerThreadPaletteCount - 1)) + 1;
        return kLogViewerThreadPalette[idx % kLogViewerThreadPaletteCount];
    }
    return kLogViewerThreadPalette[it->second % kLogViewerThreadPaletteCount];
}

std::wstring UiMainWindowImpl::GetCellText(const TabView& tab, int visibleRow, int col) const
{
    if (visibleRow < 0 || visibleRow >= (int)tab.m_visibleIdx.size())
        return {};
    const LogEntry& e = m_allLogs[tab.m_visibleIdx[visibleRow]];
    wchar_t b[32]{};
    switch (col)
    {
    case 0:
        swprintf_s(b, L"%d", tab.m_visibleIdx[visibleRow] + 1);
        return b;
    case 1:
    {
        size_t ob = e.text.find(L'[');
        size_t cb = e.text.find(L']');
        if (ob != std::wstring::npos && cb != std::wstring::npos && cb > ob)
            return e.text.substr(ob + 1, cb - ob - 1);
        return {};
    }
    case 2: swprintf_s(b, L"%u", e.processId); return b;
    case 3: swprintf_s(b, L"%u", e.threadId);  return b;
    case 4: return LogViewerLevelName(e.level);
    case 5:
    {
        size_t skip = e.text.find(L' ');
        return (skip != std::wstring::npos) ? e.text.substr(skip + 1) : e.text;
    }
    default: return {};
    }
}

std::wstring UiMainWindowImpl::GetRowText(const TabView& tab, int visibleRow) const
{
    static constexpr int kCols = 6;
    std::wstring row;
    for (int c = 1; c < kCols; ++c)
    {
        if (c > 1)
            row += L'\t';
        row += GetCellText(tab, visibleRow, c);
    }
    return row;
}

void UiMainWindowImpl::UpdateControlsFromTab(const TabView& tab)
{
    for (int i = 0; i < 5; ++i)
    {
        TBBUTTONINFOW tbi{};
        tbi.cbSize = sizeof(tbi);
        tbi.dwMask = TBIF_STATE;
        tbi.fsState = TBSTATE_ENABLED | (tab.m_showLevel[i] ? TBSTATE_CHECKED : 0);
        SendMessageW(hFilterTb, TB_SETBUTTONINFO, IDC_LVL_BASE + i, (LPARAM)&tbi);
    }

    UpdateProcButtonText();
    UpdateModButtonText();
    UpdateSubButtonText();
}

void UiMainWindowImpl::SwitchToTab(int idx)
{
    if (idx < 0 || idx >= (int)m_tabs.size() || idx == m_activeTab) return;
    ShowWindow(m_tabs[m_activeTab].hList, SW_HIDE);
    m_activeTab = idx;
    ShowWindow(m_tabs[m_activeTab].hList, SW_SHOW);
    TabCtrl_SetCurSel(hTabCtrl, idx);
    UpdateControlsFromTab(m_tabs[m_activeTab]);
    CommitListView();
    InvalidateRect(hTabCtrl, nullptr, TRUE);
    RedrawWindow(m_tabs[m_activeTab].hList, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}

void UiMainWindowImpl::AppendEntry(LogEntry e)
{
    GetThreadBg(e.processId, e.threadId);
    int newIdx = (int)m_allLogs.size();
    m_allLogs.push_back(std::move(e));
    const LogEntry& entry = m_allLogs.back();
    for (auto& tab : m_tabs)
    {
        if (m_paused)
            tab.m_pauseBuffer.push_back(newIdx);
        else if (PassFilter(entry, tab))
            tab.m_visibleIdx.push_back(newIdx);
    }
}

void UiMainWindowImpl::InvalidateList()
{
    if (m_tabs.empty()) return;
    HWND h = ActiveTab().hList;
    InvalidateRect(h, nullptr, TRUE);
    UpdateWindow(h);
}

void UiMainWindowImpl::CommitListView()
{
    if (m_tabs.empty()) return;
    TabView& tab = ActiveTab();
    HWND hList = tab.hList;
    const int newCount = (int)tab.m_visibleIdx.size();
    const int prevCount = tab.m_lastCommittedVisibleCount;

    const int oldVisForScroll = (prevCount >= 0) ? prevCount : ListView_GetItemCount(hList);
    const bool wasAtBottom = ListViewIsScrolledToBottom(hList, oldVisForScroll);
    const bool countUnchanged = (prevCount >= 0 && newCount == prevCount);

    if (countUnchanged)
    {
        ListView_SetItemCountEx(hList, newCount, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
    }
    else if (!tab.m_autoScroll || m_paused)
    {
        ListView_SetItemCountEx(hList, newCount, LVSICF_NOSCROLL);
    }
    else
    {
        ListView_SetItemCount(hList, newCount);
    }

    tab.m_lastCommittedVisibleCount = newCount;

    const bool shouldStickBottom = tab.m_autoScroll && !m_paused && newCount > 0
        && wasAtBottom
        && !countUnchanged;
    if (shouldStickBottom)
        ListView_EnsureVisible(hList, newCount - 1, FALSE);

    UpdateStatus();
}

bool UiMainWindowImpl::PassesLevelForFilters(const LogEntry& e, const TabView& tab) const noexcept
{
    const uint32_t L = e.level < 5 ? e.level : 0;
    return tab.m_showLevel[L];
}

bool UiMainWindowImpl::PassesForModuleFilterOptions(const LogEntry& e, const TabView& tab) const noexcept
{
    if (!PassesLevelForFilters(e, tab))
        return false;
    if (tab.m_restrictByPid && !tab.m_filterPids.contains(e.processId))
        return false;
    return true;
}

bool UiMainWindowImpl::PassesForSubsystemFilterOptions(const LogEntry& e, const TabView& tab) const noexcept
{
    if (!PassesForModuleFilterOptions(e, tab))
        return false;
    if (tab.m_restrictByModule && !tab.m_filterModules.contains(e.module))
        return false;
    return true;
}

void UiMainWindowImpl::PruneTabFiltersForCascade(TabView& tab)
{
    std::set<uint32_t> levelPids;
    for (const auto& e : m_allLogs)
    {
        if (PassesLevelForFilters(e, tab))
            levelPids.insert(e.processId);
    }
    if (tab.m_restrictByPid)
    {
        for (auto it = tab.m_filterPids.begin(); it != tab.m_filterPids.end();)
        {
            if (!levelPids.contains(*it))
                it = tab.m_filterPids.erase(it);
            else
                ++it;
        }
        if (tab.m_filterPids.empty())
            tab.m_restrictByPid = false;
    }

    std::set<std::wstring> okMods;
    for (const auto& e : m_allLogs)
    {
        if (!PassesForModuleFilterOptions(e, tab))
            continue;
        if (!e.module.empty())
            okMods.insert(e.module);
    }
    if (tab.m_restrictByModule)
    {
        for (auto it = tab.m_filterModules.begin(); it != tab.m_filterModules.end();)
        {
            if (!okMods.contains(*it))
                it = tab.m_filterModules.erase(it);
            else
                ++it;
        }
        if (tab.m_filterModules.empty())
            tab.m_restrictByModule = false;
    }

    std::set<std::wstring> okSubs;
    for (const auto& e : m_allLogs)
    {
        if (!PassesForSubsystemFilterOptions(e, tab))
            continue;
        if (!e.subsystem.empty())
            okSubs.insert(e.subsystem);
    }
    if (tab.m_restrictBySubsystem)
    {
        for (auto it = tab.m_filterSubsystems.begin(); it != tab.m_filterSubsystems.end();)
        {
            if (!okSubs.contains(*it))
                it = tab.m_filterSubsystems.erase(it);
            else
                ++it;
        }
        if (tab.m_filterSubsystems.empty())
            tab.m_restrictBySubsystem = false;
    }
}

void UiMainWindowImpl::RebuildVisible()
{
    TabView& tab = ActiveTab();
    tab.m_lastCommittedVisibleCount = -1;
    tab.m_visibleIdx.clear();
    tab.m_visibleIdx.reserve(m_allLogs.size());
    for (int i = 0; i < (int)m_allLogs.size(); ++i)
        if (PassFilter(m_allLogs[i], tab))
            tab.m_visibleIdx.push_back(i);
    CommitListView();
}

void UiMainWindowImpl::ApplyStagedLogEntry(LogEntry&& e)
{
    if (e.level == LOG_LEVEL_DISCONNECT)
    {
        m_activePids.erase(e.processId);
        UpdateStatus();
        return;
    }

    if (m_seenPids.insert(e.processId).second)
    {
        m_activePids.insert(e.processId);
        if (!m_pidLabels.contains(e.processId))
            EnqueuePidForLabelResolution(e.processId);
    }
    if (!e.module.empty())
        m_seenModules.insert(e.module);
    if (!e.subsystem.empty())
        m_seenSubsystems.insert(e.subsystem);

    AppendEntry(std::move(e));
}

void UiMainWindowImpl::FlushPauseBuffer(TabView& tab)
{
    for (int idx : tab.m_pauseBuffer)
        if (PassFilter(m_allLogs[idx], tab))
            tab.m_visibleIdx.push_back(idx);
    tab.m_pauseBuffer.clear();
}

void UiMainWindowImpl::EnsurePidLabel(uint32_t pid)
{
    if (m_pidLabels.contains(pid))
        return;
    wchar_t exeName[MAX_PATH] = L"unknown";
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc)
    {
        DWORD sz = MAX_PATH;
        QueryFullProcessImageNameW(hProc, 0, exeName, &sz);
        CloseHandle(hProc);
        if (wchar_t* sl = wcsrchr(exeName, L'\\'))
            wcscpy_s(exeName, sl + 1);
    }
    wchar_t buf[MAX_PATH + 32];
    swprintf_s(buf, L"%s (%u)", exeName, pid);
    m_pidLabels[pid] = buf;
}
