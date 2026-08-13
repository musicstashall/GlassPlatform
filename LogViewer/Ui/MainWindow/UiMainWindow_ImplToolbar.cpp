#include "SharedPch.h"

#include "framework.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "Ui/Helpers/LogViewerUiHelpers.h"
#include "UiConsts.h"
#include <algorithm>

void UiMainWindowImpl::UpdateButtons()
{
    bool actualRunning = IsGlassPlatformRunning();
    if (m_stopping && !actualRunning)
        m_stopping = false;
    m_platformRunning = actualRunning;

    if (!hCtrlTb) return;

    TBBUTTONINFOW tbi{};
    tbi.cbSize = sizeof(tbi);
    tbi.dwMask = TBIF_STATE | TBIF_IMAGE | TBIF_TEXT;

    wchar_t btnText[32];
    if (m_stopping)
    {
        tbi.fsState = TBSTATE_CHECKED;
        tbi.iImage = 0;
        wcscpy_s(btnText, L"Остановка\u2026");
    }
    else if (m_platformRunning)
    {
        tbi.fsState = TBSTATE_ENABLED | TBSTATE_CHECKED;
        tbi.iImage = 1;
        wcscpy_s(btnText, L"Остановить");
    }
    else
    {
        tbi.fsState = TBSTATE_ENABLED;
        tbi.iImage = 0;
        wcscpy_s(btnText, L"Запустить");
    }
    tbi.pszText = btnText;
    SendMessageW(hCtrlTb, TB_SETBUTTONINFO, IDC_START_STOP, (LPARAM)&tbi);
    SendMessageW(hCtrlTb, TB_AUTOSIZE, 0, 0);
}

void UiMainWindowImpl::UpdateProcButtonText()
{
    if (!hFilterTb || m_tabs.empty()) return;
    const TabView& t = ActiveTab();
    wchar_t buf[96]{};
    FormatFilterToolbarLabel(buf, _countof(buf), t.m_restrictByPid, t.m_filterPids.size(),
        L"\u0412\u0441\u0435 \u043f\u0440\u043e\u0446\u0435\u0441\u0441\u044b",
        L"\u043f\u0440\u043e\u0446\u0435\u0441\u0441",
        L"\u043f\u0440\u043e\u0446\u0435\u0441\u0441\u0430",
        L"\u043f\u0440\u043e\u0446\u0435\u0441\u0441\u043e\u0432");
    TBBUTTONINFOW tbi{};
    tbi.cbSize = sizeof(tbi);
    tbi.dwMask = TBIF_TEXT;
    tbi.pszText = buf;
    SendMessageW(hFilterTb, TB_SETBUTTONINFO, IDC_PROC_BTN, (LPARAM)&tbi);
}

void UiMainWindowImpl::UpdateModButtonText()
{
    if (!hFilterTb || m_tabs.empty()) return;
    const TabView& t = ActiveTab();
    wchar_t buf[96]{};
    FormatFilterToolbarLabel(buf, _countof(buf), t.m_restrictByModule, t.m_filterModules.size(),
        L"\u0412\u0441\u0435 \u043c\u043e\u0434\u0443\u043b\u0438",
        L"\u043c\u043e\u0434\u0443\u043b\u044c",
        L"\u043c\u043e\u0434\u0443\u043b\u044f",
        L"\u043c\u043e\u0434\u0443\u043b\u0435\u0439");
    TBBUTTONINFOW tbi{};
    tbi.cbSize = sizeof(tbi);
    tbi.dwMask = TBIF_TEXT;
    tbi.pszText = buf;
    SendMessageW(hFilterTb, TB_SETBUTTONINFO, IDC_MOD_BTN, (LPARAM)&tbi);
}

void UiMainWindowImpl::UpdateSubButtonText()
{
    if (!hFilterTb || m_tabs.empty()) return;
    const TabView& t = ActiveTab();
    wchar_t buf[96]{};
    FormatFilterToolbarLabel(buf, _countof(buf), t.m_restrictBySubsystem, t.m_filterSubsystems.size(),
        L"\u0412\u0441\u0435 \u043f\u043e\u0434\u0441\u0438\u0441\u0442\u0435\u043c\u044b",
        L"\u043f\u043e\u0434\u0441\u0438\u0441\u0442\u0435\u043c\u0430",
        L"\u043f\u043e\u0434\u0441\u0438\u0441\u0442\u0435\u043c\u044b",
        L"\u043f\u043e\u0434\u0441\u0438\u0441\u0442\u0435\u043c");
    TBBUTTONINFOW tbi{};
    tbi.cbSize = sizeof(tbi);
    tbi.dwMask = TBIF_TEXT;
    tbi.pszText = buf;
    SendMessageW(hFilterTb, TB_SETBUTTONINFO, IDC_SUB_BTN, (LPARAM)&tbi);
}

void UiMainWindowImpl::UpdateStatus()
{
    if (!hStatus)
        return;
    wchar_t buf[256];
    const wchar_t* conn = m_connected ? L"подключён" : L"нет соединения";
    const wchar_t* paused = m_paused ? L"  ⏸ ПАУЗА" : L"";
    swprintf_s(buf,
        L"  Акт. процессов: %u / %u всего  |  Канал: %s%s",
        (unsigned)m_activePids.size(),
        (unsigned)m_seenPids.size(),
        conn, paused
    );
    SendMessageW(hStatus, SB_SETTEXT, SB_SIMPLEID, reinterpret_cast<LPARAM>(buf));
}

void UiMainWindowImpl::UpdateSearchButtonWidth()
{
    if (!hSearchTb || !hRebar) return;
    RECT rcBand{};
    SendMessageW(hRebar, RB_GETRECT, BAND_SEARCH, (LPARAM)&rcBand);
    int bandW = rcBand.right - rcBand.left;
    if (bandW < MIN_SEARCH_W) bandW = MIN_SEARCH_W;
    m_searchW = bandW;
    int w = (std::max)(bandW - 2, MIN_SEARCH_W);
    TBBUTTONINFOW tbi{};
    tbi.cbSize = sizeof(tbi);
    tbi.dwMask = TBIF_SIZE;
    tbi.cx = (WORD)w;
    SendMessageW(hSearchTb, TB_SETBUTTONINFO, IDC_SEARCH_BTN, (LPARAM)&tbi);
    SendMessageW(hSearchTb, TB_SETBUTTONSIZE, 0, MAKELPARAM(w, BAND_H));
    SendMessageW(hSearchTb, TB_AUTOSIZE, 0, 0);
}

void UiMainWindowImpl::ShowEdit()
{
    if (!hSearch || !hSearchTb) return;

    RECT rBtn{};
    SendMessageW(hSearchTb, TB_GETRECT, IDC_SEARCH_BTN, (LPARAM)&rBtn);
    MapWindowPoints(hSearchTb, hwnd, (LPPOINT)&rBtn, 2);

    ShowWindow(hSearchTb, SW_HIDE);

    int editH = CTRL_TB_H;
    int btnH = rBtn.bottom - rBtn.top;
    const int btnW = rBtn.right - rBtn.left;
    const int editW = (std::max)(MIN_SEARCH_W, btnW - SEARCH_EDIT_WIDTH_TRIM_PX);
    int editY = rBtn.top + (btnH - editH) / 2;
    SetWindowPos(hSearch, HWND_TOP,
        rBtn.left, editY,
        editW, editH,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);
    m_editVisible = true;
    SetFocus(hSearch);
    SendMessageW(hSearch, EM_SETSEL, 0, -1);
}

void UiMainWindowImpl::CloseEdit()
{
    if (!m_editVisible) return;
    m_editVisible = false;
    ShowWindow(hSearch, SW_HIDE);
    SetWindowTextW(hSearch, L"");
    ShowWindow(hSearchTb, SW_SHOW);
}

void UiMainWindowImpl::RepositionEdit()
{
    if (!m_editVisible || !hSearch || !hSearchTb) return;
    ShowWindow(hSearchTb, SW_SHOW);
    RECT rBtn{};
    SendMessageW(hSearchTb, TB_GETRECT, IDC_SEARCH_BTN, (LPARAM)&rBtn);
    MapWindowPoints(hSearchTb, hwnd, (LPPOINT)&rBtn, 2);
    ShowWindow(hSearchTb, SW_HIDE);

    int editH = CTRL_TB_H;
    int btnH = rBtn.bottom - rBtn.top;
    const int btnW = rBtn.right - rBtn.left;
    const int editW = (std::max)(MIN_SEARCH_W, btnW - SEARCH_EDIT_WIDTH_TRIM_PX);
    int editY = rBtn.top + (btnH - editH) / 2;
    SetWindowPos(hSearch, nullptr,
        rBtn.left, editY,
        editW, editH,
        SWP_NOZORDER | SWP_NOACTIVATE);
}
