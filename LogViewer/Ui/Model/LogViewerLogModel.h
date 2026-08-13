#pragma once

#include "framework.h"
#include <cstdint>
#include <string>
#include <set>
#include <vector>

// Запись лога
struct LogEntry
{
    uint32_t     level;
    uint32_t     processId;
    uint32_t     threadId;
    std::wstring text;
    std::wstring module;
    std::wstring subsystem;
};

void ParseLogTags(const std::wstring& text, std::wstring& module, std::wstring& subsystem);

// Состояние вкладки — фильтры + видимые данные (индексы в глобальный m_allLogs)
struct TabView
{
    HWND         hList = nullptr;
    std::wstring name;

    bool         m_showLevel[5] = { true, true, true, true, true };
    bool                   m_restrictByPid = false;
    bool                   m_restrictByModule = false;
    bool                   m_restrictBySubsystem = false;
    std::set<uint32_t>     m_filterPids;
    std::set<std::wstring> m_filterModules;
    std::set<std::wstring> m_filterSubsystems;

    bool             m_autoScroll = true;
    int              m_lastCommittedVisibleCount = -1;
    std::vector<int> m_visibleIdx;
    std::vector<int> m_pauseBuffer;

    int m_ctxMenuRow = -1;
    int m_ctxMenuCol = -1;
};
