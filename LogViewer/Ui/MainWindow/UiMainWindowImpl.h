#pragma once

#include "framework.h"
#include "UiConsts.h"
#include "Ui/Model/LogViewerLogModel.h"
#include "MessageRouter.h"
#include <commctrl.h>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

LRESULT CALLBACK LogViewerFilterDropDownWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK LogViewerFilterListviewPopupWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

class UiMainWindowImpl
{
    friend LRESULT CALLBACK LogViewerFilterDropDownWndProc(HWND, UINT, WPARAM, LPARAM);
    friend LRESULT CALLBACK LogViewerFilterListviewPopupWndProc(HWND, UINT, WPARAM, LPARAM);

public:
    HWND hwnd = nullptr;

    HWND hRebar = nullptr;
    HWND hCtrlTb = nullptr;
    /// Контейнер (класс LogViewer.FilterTbHost) в Band 1 Rebar; `hFilterTb` — его дочерний тулбар (TB_SETPARENT → hwnd для WM_NOTIFY).
    HWND hFilterTbHost = nullptr;
    HWND hFilterTb = nullptr;
    HWND hSearchTb = nullptr;

    HWND hSearch = nullptr;

    HWND m_filterPopup = nullptr;
    HWND m_filterListviewPopup = nullptr;
    HWND m_filterPopupList = nullptr;
    int  m_filterPopupKind = 0;
    /// После смены hot при открытом дропе: kind для WM_APP_FILTER_RESUME_DROPDOWN (−1 — нет отложенного открытия).
    int  m_filterResumeKind = -1;
    std::vector<std::wstring> m_filterPopupRowStrings;

    bool m_editVisible = false;

    int m_ctrlW = 0;
    int m_filterW = 0;
    int m_searchW = 200;

    HWND hStatus = nullptr;
    HWND hTabCtrl = nullptr;

    HIMAGELIST hImgList = nullptr;

    std::vector<LogEntry> m_allLogs;

    std::vector<TabView> m_tabs;
    int                  m_activeTab = 0;

    std::set<uint32_t>     m_seenPids;
    std::set<std::wstring> m_seenModules;
    std::set<std::wstring> m_seenSubsystems;
    std::set<uint32_t>     m_activePids;
    std::map<uint32_t, std::wstring> m_pidLabels;

    bool m_paused = false;
    bool m_connected = false;
    bool m_platformRunning = false;
    bool m_stopping = false;

    /// Последнее TBN_HOTITEMCHANGE на тулбаре фильтров (диагностика).
    int m_filterTbHotIdNew = -1;
    int m_filterTbHotIdOld = -1;
    DWORD m_filterTbHotDwFlags = 0;

    std::map<std::pair<uint32_t, uint32_t>, int> m_tidColorMap;
    int m_nextColorIdx = 1;

    HFONT m_font = nullptr;
    HFONT m_monoFont = nullptr;

    TabView& ActiveTab() { return m_tabs[m_activeTab]; }
    TabView* FindTab(HWND hList);

    bool PassFilter(const LogEntry& e, const TabView& tab) const
    {
        if (!tab.m_showLevel[e.level < 5 ? e.level : 0]) return false;
        if (tab.m_restrictByPid && !tab.m_filterPids.contains(e.processId)) return false;
        if (tab.m_restrictByModule && !tab.m_filterModules.contains(e.module)) return false;
        if (tab.m_restrictBySubsystem && !tab.m_filterSubsystems.contains(e.subsystem)) return false;
        return true;
    }

    /// Каскад опций выпадающих фильтров (без учёта «нижних» осей): уровень → PID → модуль → подсистема.
    [[nodiscard]] bool PassesLevelForFilters(const LogEntry& e, const TabView& tab) const noexcept;
    [[nodiscard]] bool PassesForModuleFilterOptions(const LogEntry& e, const TabView& tab) const noexcept;
    [[nodiscard]] bool PassesForSubsystemFilterOptions(const LogEntry& e, const TabView& tab) const noexcept;
    void PruneTabFiltersForCascade(TabView& tab);
    COLORREF GetThreadBg(uint32_t pid, uint32_t tid);
    std::wstring GetCellText(const TabView& tab, int visibleRow, int col) const;
    std::wstring GetRowText(const TabView& tab, int visibleRow) const;

    void UpdateControlsFromTab(const TabView& tab);
    void SwitchToTab(int idx);

    void AppendEntry(LogEntry e);
    void InvalidateList();
    void CommitListView();
    void RebuildVisible();
    /// Применить запись из очереди приёма (GUI-поток); OpenProcess для PID — только через EnqueuePidForLabelResolution.
    void ApplyStagedLogEntry(LogEntry&& e);
    void FlushPauseBuffer(TabView& tab);
    void EnsurePidLabel(uint32_t pid);

    void UpdateButtons();
    void UpdateProcButtonText();
    void UpdateModButtonText();
    void UpdateSubButtonText();
    void UpdateStatus();
    void UpdateSearchButtonWidth();
    void ShowEdit();
    void CloseEdit();
    void RepositionEdit();

    void DoCopy(bool all);
    void DoCopyCell();
    void DoSaveProcessDump();
    void DoClear();
    void DoInvertSelection();
    void DoSearch(bool forward);

    void OnLevelCheckChanged(int lvl);
    void TogglePause();

    void AddTab();

    void RegisterFilterPopupClass();
    void EnsureFilterPopupCreated();
    void FillFilterPopupList();
    void OpenFilterPopup(int kind);
    /// Открыть выпадающий фильтр и блокировать до закрытия окна (как модальный цикл в DropDown.au3).
    void OpenFilterDropdownModal(int kind);
    void RunFilterPopupModalLoop();
    /// Прочитать состояние чекбоксов списка фильтра и обновить вкладку / список логов (пока окно фильтра ещё существует).
    void ApplyFilterPopupSelection();
    /// restoreKeyboardFocus — вернуть фокус на список логов (ложь при переключении kind внутри OpenFilterPopup).
    void CloseFilterPopup(bool apply, bool restoreKeyboardFocus = true);
    [[nodiscard]] bool IsFilterPopupOpen() const noexcept;
    [[nodiscard]] int  FilterPopupKind() const noexcept { return m_filterPopupKind; }
    /// Если выпадающий фильтр уже открыт — переключить на другой kind без мерцания рамки главного окна.
    void SwitchFilterPopupIfOpen(int kind);
    /// Закрыть попап и запланировать вторую фазу (PostMessage), не открывая новый в том же TBN_DROPDOWN (модель как AutoIt WM_CLOSE).
    void RequestFilterPopupHotSwitch(int kind);

    void StartPlatform();
    void StopPlatform();

private:
    /// Полное уничтожение popup фильтра (владелец — главное окно); перед следующим открытием — новое дерево и attach.
    void DestroyFilterPopup() noexcept;
};
