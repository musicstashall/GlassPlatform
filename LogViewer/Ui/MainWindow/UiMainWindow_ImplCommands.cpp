#include "SharedPch.h"

#include "framework.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "UiConsts.h"
#include "LogViewerProcessDump.h"
#include <commctrl.h>
#include <cstring>

void UiMainWindowImpl::DoCopy(bool all)
{
    if (m_tabs.empty()) return;
    TabView& tab = ActiveTab();
    std::wstring result;

    auto appendRow = [&](int visibleRow)
        {
            std::wstring t = GetRowText(tab, visibleRow);
            while (!t.empty() && (t.back() == L'\n' || t.back() == L'\r'))
                t.pop_back();
            if (!t.empty())
                result += t + L"\r\n";
        };

    if (all)
    {
        const int n = (int)tab.m_visibleIdx.size();
        for (int i = 0; i < n; ++i)
            appendRow(i);
    }
    else
    {
        int item = -1;
        while ((item = ListView_GetNextItem(tab.hList, item, LVNI_SELECTED)) != -1)
            if (item < (int)tab.m_visibleIdx.size())
                appendRow(item);
    }

    if (result.empty()) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (result.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem)
    {
        wchar_t* ptr = (wchar_t*)GlobalLock(hMem);
        if (ptr) { memcpy(ptr, result.c_str(), bytes); GlobalUnlock(hMem); SetClipboardData(CF_UNICODETEXT, hMem); }
    }
    CloseClipboard();
}

void UiMainWindowImpl::DoCopyCell()
{
    if (m_tabs.empty()) return;
    TabView& tab = ActiveTab();
    std::wstring text = GetCellText(tab, tab.m_ctxMenuRow, tab.m_ctxMenuCol);
    if (text.empty()) return;
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem)
    {
        wchar_t* p = (wchar_t*)GlobalLock(hMem);
        if (p) { wcscpy_s(p, text.size() + 1, text.c_str()); GlobalUnlock(hMem); SetClipboardData(CF_UNICODETEXT, hMem); }
    }
    CloseClipboard();
}

void UiMainWindowImpl::DoSaveProcessDump()
{
    if (m_tabs.empty())
        return;

    TabView& tab = ActiveTab();
    if (tab.m_ctxMenuRow < 0 || tab.m_ctxMenuRow >= (int)tab.m_visibleIdx.size())
        return;

    const int vi = tab.m_visibleIdx[tab.m_ctxMenuRow];
    if (vi < 0 || vi >= (int)m_allLogs.size())
        return;

    const uint32_t pid = m_allLogs[vi].processId;
    EnsurePidLabel(pid);

    std::wstring labelHint;
    if (auto it = m_pidLabels.find(pid); it != m_pidLabels.end())
        labelHint = it->second;

    const std::wstring defaultName = BuildDefaultDumpFileName(pid, labelHint);

    wchar_t defaultDir[MAX_PATH]{};
    if (!GetDefaultDumpDirectory(defaultDir, _countof(defaultDir)))
        wcscpy_s(defaultDir, L".\\");

    wchar_t filePath[MAX_PATH * 2]{};
    swprintf_s(filePath, L"%s\\%s", defaultDir, defaultName.c_str());

    if (!ShowSaveDumpFileDialog(hwnd, defaultDir, defaultName.c_str(), filePath, _countof(filePath)))
        return;

    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    StartProcessDumpAsync(hwnd, pid, filePath);
}

void UiMainWindowImpl::DoClear()
{
    m_allLogs.clear();
    for (auto& tab : m_tabs)
    {
        tab.m_visibleIdx.clear();
        tab.m_pauseBuffer.clear();
        tab.m_lastCommittedVisibleCount = -1;
    }
    CommitListView();
    UpdateProcButtonText();
    UpdateModButtonText();
    UpdateSubButtonText();
}

void UiMainWindowImpl::DoInvertSelection()
{
    if (m_tabs.empty()) return;
    TabView& tab = ActiveTab();
    int n = (int)tab.m_visibleIdx.size();
    for (int i = 0; i < n; ++i)
    {
        UINT cur = ListView_GetItemState(tab.hList, i, LVIS_SELECTED);
        ListView_SetItemState(tab.hList, i,
            (cur & LVIS_SELECTED) ? 0 : LVIS_SELECTED, LVIS_SELECTED);
    }
}

void UiMainWindowImpl::DoSearch(bool forward)
{
    if (m_tabs.empty()) return;
    TabView& tab = ActiveTab();
    wchar_t pat[256]{};
    GetWindowTextW(hSearch, pat, 255);
    if (!pat[0]) return;

    int count = (int)tab.m_visibleIdx.size();
    if (count == 0) return;

    int sel = ListView_GetNextItem(tab.hList, -1, LVNI_SELECTED);
    int start = forward
        ? (sel >= 0 ? sel + 1 : 0)
        : (sel > 0 ? sel - 1 : count - 1);

    auto matches = [&](int vi) {
        return m_allLogs[vi].text.find(pat) != std::wstring::npos;
        };

    if (forward)
    {
        for (int i = start; i < count; ++i)
            if (matches(tab.m_visibleIdx[i]))
            {
                ListView_SetItemState(tab.hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(tab.hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(tab.hList, i, FALSE);
                return;
            }
    }
    else
    {
        for (int i = start; i >= 0; --i)
            if (matches(tab.m_visibleIdx[i]))
            {
                ListView_SetItemState(tab.hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_SetItemState(tab.hList, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(tab.hList, i, FALSE);
                return;
            }
    }
}

void UiMainWindowImpl::OnLevelCheckChanged(int lvl)
{
    if (m_tabs.empty()) return;
    LRESULT state = SendMessageW(hFilterTb, TB_GETSTATE, IDC_LVL_BASE + lvl, 0);
    ActiveTab().m_showLevel[lvl] = (state & TBSTATE_CHECKED) != 0;
    PruneTabFiltersForCascade(ActiveTab());
    RebuildVisible();
}

void UiMainWindowImpl::TogglePause()
{
    m_paused = !m_paused;

    if (hCtrlTb)
    {
        TBBUTTONINFOW tbi{};
        tbi.cbSize = sizeof(tbi);
        tbi.dwMask = TBIF_STATE;
        tbi.fsState = TBSTATE_ENABLED | (m_paused ? TBSTATE_CHECKED : 0);
        SendMessageW(hCtrlTb, TB_SETBUTTONINFO, IDC_PAUSE_BTN, (LPARAM)&tbi);
    }

    if (!m_paused)
    {
        for (auto& tab : m_tabs)
            FlushPauseBuffer(tab);
        CommitListView();
    }
    UpdateStatus();
}
