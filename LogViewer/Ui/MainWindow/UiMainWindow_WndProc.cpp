#include "SharedPch.h"

#include "framework.h"
#include "UiMainWindow.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "Ui/MainWindow/UiMainWindowInternal.h"
#include "Ui/MainWindow/UiMainWindowSubclasses.h"
#include "Ui/Messages/LogViewerAppMessages.h"
#include "Ui/Helpers/LogViewerUiHelpers.h"
#include "Ui/Theme/LogViewerListTheme.h"
#include "UiConsts.h"
#include "UiRebarSetup.h"
#include "LogViewerGlassHost.h"
#include "LogViewerGlassAttach.h"
#include <GlassModulePath.h>
#include "LogViewerWndCompatScan.h"
#include "LogViewerComboLBoxSubclass.h"
#include "LogViewerBackdrop.h"
#include "LogClient.h"
#include "LogViewerProcessDump.h"
#include "GlassLogServer.h"
#include "MessageRouter.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

extern UiMainWindow* g_mainWindow;

/// Дренаж очереди LogEntry: батч + бюджет времени; продолжение через IDT_LOG_DRAIN (не цепочка PostMessage).
static void ProcessLogIncomingBatch(HWND hwnd, UiMainWindowImpl* impl)
{
    if (!impl)
        return;

    LARGE_INTEGER freq{};
    LARGE_INTEGER t0{};
    (void)QueryPerformanceFrequency(&freq);
    (void)QueryPerformanceCounter(&t0);
    const double sliceTicks =
        kMaxLogUiSliceMs * 0.001 * static_cast<double>(freq.QuadPart);

    auto entries = DrainLogEntryQueueUpTo(kMaxLogEntriesPerUiTick);

    std::size_t i = 0;
    for (; i < entries.size(); ++i)
    {
        if (i > 0)
        {
            LARGE_INTEGER tNow{};
            (void)QueryPerformanceCounter(&tNow);
            if (static_cast<double>(tNow.QuadPart - t0.QuadPart) >= sliceTicks)
                break;
        }
        impl->ApplyStagedLogEntry(std::move(entries[i]));
    }

    if (i < entries.size())
    {
        std::vector<LogEntry> rest;
        rest.reserve(entries.size() - i);
        for (std::size_t j = i; j < entries.size(); ++j)
            rest.push_back(std::move(entries[j]));
        RequeueLogEntriesFront(std::move(rest));
    }

    const std::uint64_t dropped = ConsumeLogQueueDroppedCount();
    const std::uint64_t droppedBridge =
        Shared::Log::GlassLogServer::Instance().ConsumeInboundDroppedCount();
    if (dropped > 0 || droppedBridge > 0)
    {
        wchar_t msg[288]{};
        if (dropped > 0)
        {
            swprintf_s(
                msg,
                L"[LogViewer] Отброшено %llu старых сообщений из-за переполнения внутренней очереди приёма.",
                static_cast<unsigned long long>(dropped));

            LogEntry syn;
            syn.level = DBG_WARNING;
            syn.processId = GetCurrentProcessId();
            syn.threadId = GetCurrentThreadId();
            syn.text = msg;
            ParseLogTags(syn.text, syn.module, syn.subsystem);
            impl->ApplyStagedLogEntry(std::move(syn));
        }
        if (droppedBridge > 0)
        {
            swprintf_s(
                msg,
                L"[LogViewer] Отброшено %llu строк: переполнение очереди GlassLogServer между пайпами инжектированных процессов и LogViewer.",
                static_cast<unsigned long long>(droppedBridge));

            LogEntry syn2;
            syn2.level = DBG_WARNING;
            syn2.processId = GetCurrentProcessId();
            syn2.threadId = GetCurrentThreadId();
            syn2.text = msg;
            ParseLogTags(syn2.text, syn2.module, syn2.subsystem);
            impl->ApplyStagedLogEntry(std::move(syn2));
        }
    }

    impl->CommitListView();

    if (HasPendingLogEntries())
        (void)SetTimer(hwnd, IDT_LOG_DRAIN, kLogDrainCoalesceMs, nullptr);
    else
        (void)KillTimer(hwnd, IDT_LOG_DRAIN);
}

void LogViewerFilterToolbar_Attach(HWND hFilterTb, UiMainWindowImpl* impl) noexcept;
void LogViewerFilterToolbar_ApplyPostDropdownSync(HWND hTb) noexcept;

namespace
{
constexpr wchar_t kFilterTbHostClassName[] = L"LogViewer.FilterTbHost";

/// Дочернее окно-контейнер тулбара фильтров: фон — как у родителя (Rebar).
LRESULT CALLBACK LogViewerFilterTbHostWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_ERASEBKGND:
    {
        HWND hParent = ::GetParent(hWnd);
        if (hParent && ::IsWindow(hParent) != FALSE)
        {
            auto* hdc = reinterpret_cast<HDC>(wParam);
            POINT pt{};
            (void)::MapWindowPoints(hWnd, hParent, &pt, 1);
            (void)::OffsetWindowOrgEx(hdc, pt.x, pt.y, nullptr);
            const LRESULT lr = ::SendMessageW(hParent, WM_ERASEBKGND, wParam, 0);
            (void)::OffsetWindowOrgEx(hdc, -pt.x, -pt.y, nullptr);
            return lr;
        }
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        (void)::BeginPaint(hWnd, &ps);
        (void)::EndPaint(hWnd, &ps);
        return 0;
    }
    default:
        return ::DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }
}

/// После инициализации/ресайза: высота окна тулбара = высота кнопок (TB_GETBUTTONSIZE), по вертикали по центру в хосте.
/// Если useTbGetMaxSize == false — TB_GETMAXSIZE не вызывается (до скрытия главного окна при старте).
void LayoutFilterToolbarInHost(UiMainWindowImpl* impl, bool useTbGetMaxSize = true) noexcept
{
    if (!impl || !impl->hFilterTbHost || !impl->hFilterTb)
        return;
    if (::IsWindow(impl->hFilterTbHost) == FALSE || ::IsWindow(impl->hFilterTb) == FALSE)
        return;

    RECT rcHost{};
    if (::GetClientRect(impl->hFilterTbHost, &rcHost) == FALSE)
        return;
    const int hostW = (std::max)(1, static_cast<int>(rcHost.right - rcHost.left));
    const int hostH = (std::max)(1, static_cast<int>(rcHost.bottom - rcHost.top));

    const DWORD btnSz = static_cast<DWORD>(::SendMessageW(impl->hFilterTb, TB_GETBUTTONSIZE, 0, 0));
    int btnW = static_cast<int>(LOWORD(btnSz));
    int btnH = static_cast<int>(HIWORD(btnSz));
    if (btnH <= 0)
        btnH = CTRL_TB_H;
    if (btnW < 0)
        btnW = 0;

    (void)::SendMessageW(impl->hFilterTb, TB_SETBUTTONSIZE, 0, MAKELPARAM(btnW, btnH));
    (void)::SendMessageW(impl->hFilterTb, TB_AUTOSIZE, 0, 0);

    int tbW;
    int tbH;
    if (useTbGetMaxSize)
    {
        SIZE szMax{};
        (void)::SendMessageW(impl->hFilterTb, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&szMax));
        tbW = static_cast<int>(szMax.cx);
        tbH = static_cast<int>(szMax.cy);
        if (tbW < 1)
            tbW = hostW;
        if (tbH < 1)
            tbH = btnH;
        tbW = (std::max)(tbW, hostW);
    }
    else
    {
        tbW = hostW;
        tbH = btnH;
    }

    const int y = (std::max)(0, (hostH - tbH) / 2);
    (void)::SetWindowPos(
        impl->hFilterTb,
        nullptr,
        0,
        y,
        tbW,
        tbH,
        SWP_NOZORDER | SWP_NOACTIVATE);
}
} // namespace

[[nodiscard]] bool RegisterLogViewerFilterTbHostClass(HINSTANCE hInst) noexcept
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = LogViewerFilterTbHostWndProc;
    wc.hInstance = hInst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kFilterTbHostClassName;
    if (::RegisterClassW(&wc) != 0)
        return true;
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "uxtheme.lib")

LRESULT CALLBACK UiMainWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    UiMainWindowImpl* impl = GetImpl(hwnd);

    switch (msg)
    {
    case WM_NCCREATE:
    {
        auto* cs = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        auto* passed = reinterpret_cast<UiMainWindowImpl*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)passed);
        passed->hwnd = hwnd;
        return TRUE;
    }

    case WM_CREATE:
    {
        impl = GetImpl(hwnd);
        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

        RECT rc;
        GetClientRect(hwnd, &rc);

        HFONT uiFont = CreateUiFont();
        HFONT monoFont = CreateMonoFont();
        impl->m_font = uiFont;
        impl->m_monoFont = monoFont;

        // ===========================================================
        // Рябар и 3 ленты
        // ===========================================================

        const int iconSz = 16;
        impl->hImgList = CreateLogViewerToolbarImages(iconSz, iconSz);

        impl->hCtrlTb = CreateCtrlToolbar(hwnd, impl->hImgList, uiFont);
        impl->hSearchTb = CreateSearchToolbar(hwnd, uiFont);

        // --- Edit поиска (скрыт) ---
        impl->hSearch = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | ES_AUTOHSCROLL,
            0, -200, 200, BAND_H, hwnd,
            (HMENU)(UINT_PTR)IDC_SEARCH, hInst, nullptr);
        SendMessageW(impl->hSearch, WM_SETFONT, (WPARAM)uiFont, FALSE);
        SendMessageW(impl->hSearch, EM_SETCUEBANNER, TRUE,
            (LPARAM)L"\u041f\u043e\u0438\u0441\u043a...");
        SetWindowSubclass(impl->hSearch, LogViewerMainWndEditSubclassProc, 2, (DWORD_PTR)impl);

        // --- Rebar (стиль/тема как у неактивной навигационной полосы) ---
        static constexpr DWORD kRebarWindowStyle = 0x5602EA69UL;
        static constexpr LONG_PTR kRebarExStyle = static_cast<LONG_PTR>(0x00010080UL);
        impl->hRebar = CreateWindowExW(
            0,
            REBARCLASSNAMEW,
            nullptr,
            kRebarWindowStyle,
            0, 0, rc.right, 0,
            hwnd, (HMENU)(UINT_PTR)IDC_REBAR, hInst, nullptr);
        if (impl->hRebar)
        {
            (void)::SetWindowLongPtrW(impl->hRebar, GWL_EXSTYLE, kRebarExStyle);
            (void)::SetWindowTheme(impl->hRebar, L"InactiveNavbarComposited", nullptr);
        }

        impl->hFilterTbHost = CreateWindowExW(
            0,
            kFilterTbHostClassName,
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0,
            0,
            1,
            BAND_H,
            impl->hRebar,
            (HMENU)(UINT_PTR)IDC_FILTER_TB_HOST,
            hInst,
            nullptr);
        impl->hFilterTb = CreateFilterToolbar(impl->hFilterTbHost, impl->hImgList, uiFont);
        // WM_NOTIFY/TBN_DROPDOWN — на главное окно; HWND родителя для отрисовки остаётся hFilterTbHost.
        (void)::SendMessageW(impl->hFilterTb, TB_SETPARENT, reinterpret_cast<WPARAM>(hwnd), 0);
        LogViewerFilterToolbar_Attach(impl->hFilterTb, impl);

        impl->UpdateButtons();

        auto tbWidth = [](HWND hTb) -> int {
            SIZE sz{};
            SendMessageW(hTb, TB_GETMAXSIZE, 0, (LPARAM)&sz);
            return sz.cx + 4;
            };

        int ctrlW = (std::max)(tbWidth(impl->hCtrlTb), 60);
        // Ширина средней полосы до TB_GETMAXSIZE (после ShowWindow(SW_HIDE)): временная, заменится в конце WM_CREATE.
        const int clientW = static_cast<int>(rc.right);
        const int availMid = clientW - ctrlW - impl->m_searchW;
        const int filterWPlaceholder = (std::max)(240, (std::min)(availMid, clientW * 3 / 4));
        int searchW = impl->m_searchW;

        impl->m_ctrlW = ctrlW;

        SetWindowPos(impl->hCtrlTb, nullptr, 0, 0, ctrlW, BAND_H, SWP_NOMOVE | SWP_NOZORDER);
        SetWindowPos(impl->hSearchTb, nullptr, 0, 0, searchW, BAND_H, SWP_NOMOVE | SWP_NOZORDER);
        // Band 0: Start/Stop + Pause — фиксированный, без гриппера
        AddRebarBand(impl->hRebar, impl->hCtrlTb, nullptr,
            ctrlW, BAND_H, BAND_H, 0);

        // Band 1: хост на всю высоту ленты; тулбар внутри хоста центрируем сами (LayoutFilterToolbarInHost).
        {
            REBARBANDINFOW rbb{};
            rbb.cbSize = sizeof(REBARBANDINFOW);
            rbb.fMask = RBBIM_STYLE | RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_SIZE;
            rbb.fStyle = RBBS_NOGRIPPER;
            rbb.hwndChild = impl->hFilterTbHost;
            rbb.cyChild = BAND_H;
            rbb.cyMaxChild = BAND_H;
            rbb.cyMinChild = BAND_H;
            rbb.cxMinChild = filterWPlaceholder;
            rbb.cx = filterWPlaceholder;
            SendMessageW(impl->hRebar, RB_INSERTBAND, (WPARAM)-1, (LPARAM)&rbb);
        }

        // Band 2: поиск — с гриппером
        {
            REBARBANDINFOW rbb{};
            rbb.cbSize = sizeof(REBARBANDINFOW);
            rbb.fMask = RBBIM_STYLE | RBBIM_CHILD | RBBIM_CHILDSIZE | RBBIM_SIZE;
            rbb.fStyle = RBBS_GRIPPERALWAYS;
            rbb.hwndChild = impl->hSearchTb;
            rbb.cyChild = BAND_H;
            rbb.cyMinChild = BAND_H;
            rbb.cxMinChild = MIN_SEARCH_W;
            rbb.cx = searchW;
            SendMessageW(impl->hRebar, RB_INSERTBAND, (WPARAM)-1, (LPARAM)&rbb);
        }

        // TB_AUTOSIZE только для Band 0 и Band 2; средний тулбар — внутри хоста (см. LayoutFilterToolbarInHost).
        SendMessageW(impl->hCtrlTb, TB_AUTOSIZE, 0, 0);
        SendMessageW(impl->hSearchTb, TB_AUTOSIZE, 0, 0);
        SendMessageW(impl->hRebar, WM_SIZE, 0, 0);
        impl->UpdateSearchButtonWidth();
        LayoutFilterToolbarInHost(impl, false);

        // ===========================================================
        // Таб-контрол
        // ===========================================================
        // Позиция будет корректно задана в WM_SIZE;
        // при первом вызове CreateTabListView рябар ещё не размещён,
        // поэтому создаём таб-контрол с временными координатами.
        const int rawTabH = static_cast<int>(rc.bottom) - STATUS_H;
        const int tabCtrlInitH = rawTabH > 50 ? rawTabH : 50;
        impl->hTabCtrl = CreateWindowExW(0, WC_TABCONTROLW, nullptr,
            static_cast<DWORD>(LOGVIEWER_TABCTRL_STYLE),
            0, 0, rc.right, tabCtrlInitH,
            hwnd, (HMENU)(UINT_PTR)IDC_TABCTRL,
            hInst, nullptr);
        SendMessageW(impl->hTabCtrl, WM_SETFONT, (WPARAM)uiFont, TRUE);
        // Подкласс: WM_NOTIFY / WM_CONTEXTMENU / WM_MOUSEWHEEL от дочерних ListView
        //           пробрасываем в главное окно (hwnd).
        SetWindowSubclass(impl->hTabCtrl, LogViewerMainWndTabCtrlSubclassProc, 1, (DWORD_PTR)hwnd);

        // Псевдо-вкладка "+" всегда в конце
        TCITEMW tci{}; tci.mask = TCIF_TEXT; tci.pszText = (LPWSTR)L"+";
        TabCtrl_InsertItem(impl->hTabCtrl, 0, &tci);

        // ===========================================================
        // Строка статуса (msctls_statusbar32 — GlassCore StatusBarHooks)
        // ===========================================================
        impl->hStatus = CreateWindowExW(
            0,
            STATUSCLASSNAME,
            nullptr,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, rc.bottom - STATUS_H, rc.right, STATUS_H,
            hwnd, (HMENU)(UINT_PTR)IDC_STATUS,
            hInst, nullptr);
        SendMessageW(impl->hStatus, WM_SETFONT, (WPARAM)uiFont, TRUE);
        SendMessageW(impl->hStatus, SB_SIMPLE, TRUE, 0);
        SendMessageW(impl->hStatus, WM_SIZE, 0, 0);

        // Создать первую вкладку "Основной"
        impl->AddTab();

        impl->UpdateButtons();
        impl->UpdateStatus();

        if (LogViewerGlass::IsGlassCoreLoaded())
        {
            if (impl->hTabCtrl && IsWindow(impl->hTabCtrl))
                LogViewerGlassAttach::AttachTabControl(impl->hTabCtrl);
            LogViewerGlassAttach::AttachRebar(impl->hRebar);
            if (LogViewerGlass::GetWndCompatHandleWinEvent())
                LogViewerWndCompatScan::AttachLogViewerWindowTree(hwnd, nullptr);
            if (impl->hTabCtrl && IsWindow(impl->hTabCtrl))
                LogViewerGlassAttach::RequestTabControlGlassRepaint(impl->hTabCtrl);
        }

        // Периодический опрос состояния платформы (старт/стоп извне, краш)
        SetTimer(hwnd, IDT_PLATFORM_POLL, 1000, nullptr);

        // TB_GETMAXSIZE для тулбара фильтров — только когда главное окно скрыто (корректные метрики без первого кадра).
        (void)::ShowWindow(hwnd, SW_HIDE);

        (void)::SendMessageW(impl->hRebar, WM_SIZE, 0, 0);
        impl->UpdateSearchButtonWidth();
        LayoutFilterToolbarInHost(impl);

        if (impl->hFilterTb && ::IsWindow(impl->hFilterTb) == TRUE && impl->hRebar
            && ::IsWindow(impl->hRebar) == TRUE)
        {
            SIZE szTb{};
            (void)::SendMessageW(impl->hFilterTb, TB_GETMAXSIZE, 0, reinterpret_cast<LPARAM>(&szTb));
            const int filterMinW =
                (std::max)(1, static_cast<int>(szTb.cx) + 4 + BAND_FILTER_MIN_EXTRA_PX);
            impl->m_filterW = filterMinW;

            REBARBANDINFOW rbi{};
            rbi.cbSize = sizeof(rbi);
            rbi.fMask = RBBIM_CHILDSIZE | RBBIM_SIZE;
            if (::SendMessageW(impl->hRebar, RB_GETBANDINFO, BAND_FILTER, reinterpret_cast<LPARAM>(&rbi)) == TRUE)
            {
                rbi.cxMinChild = static_cast<UINT>(filterMinW);
                if (static_cast<int>(rbi.cx) < filterMinW)
                    rbi.cx = static_cast<UINT>(filterMinW);
                (void)::SendMessageW(impl->hRebar, RB_SETBANDINFO, BAND_FILTER, reinterpret_cast<LPARAM>(&rbi));
            }
        }

        (void)::SendMessageW(impl->hRebar, WM_SIZE, 0, 0);
        impl->UpdateSearchButtonWidth();
        LayoutFilterToolbarInHost(impl);

        RECT rcClientFinal{};
        (void)::GetClientRect(hwnd, &rcClientFinal);
        (void)::SendMessageW(
            hwnd,
            WM_SIZE,
            static_cast<WPARAM>(SIZE_RESTORED),
            MAKELPARAM(rcClientFinal.right, rcClientFinal.bottom));

        (void)::ShowWindow(hwnd, SW_SHOW);

        LogViewer::ApplyWindowBackdrop(hwnd);

        return 0;
    }

    case WM_SIZE:
    {
        if (!impl) break;
        if (wParam == SIZE_MINIMIZED) return 0;  // при минимизации ничего не делаем

        RECT rc;
        GetClientRect(hwnd, &rc);
        int cx = rc.right;
        int cy = rc.bottom;

        // --- Растянуть рябар по ширине ---
        if (impl->hRebar)
            SetWindowPos(impl->hRebar, nullptr, 0, 0, cx, 0,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);

        // --- Получить реальную высоту рябара ---
        int rebarH = 0;
        if (impl->hRebar)
        {
            RECT rcRebar;
            GetWindowRect(impl->hRebar, &rcRebar);
            rebarH = rcRebar.bottom - rcRebar.top;
        }

        // --- Band 1: ширина ленты через RB_SETBANDINFO; тулбар в хосте — LayoutFilterToolbarInHost ---
        if (impl->hRebar)
        {
            REBARBANDINFOW rbiQ{};
            rbiQ.cbSize = sizeof(rbiQ);
            rbiQ.fMask = RBBIM_SIZE;
            SendMessageW(impl->hRebar, RB_GETBANDINFO, BAND_SEARCH, (LPARAM)&rbiQ);
            int band2cx = (int)rbiQ.cx;
            if (band2cx < MIN_SEARCH_W) band2cx = MIN_SEARCH_W;

            int filterW = cx - impl->m_ctrlW - band2cx;
            if (filterW < impl->m_filterW) filterW = impl->m_filterW;

            REBARBANDINFOW rbi{};
            rbi.cbSize = sizeof(rbi);
            rbi.fMask = RBBIM_SIZE | RBBIM_CHILDSIZE;
            if (SendMessageW(impl->hRebar, RB_GETBANDINFO, BAND_FILTER, (LPARAM)&rbi) == TRUE)
            {
                rbi.cx = static_cast<UINT>(filterW);
                if (impl->m_filterW > 0)
                    rbi.cxMinChild = static_cast<UINT>(impl->m_filterW);
                SendMessageW(impl->hRebar, RB_SETBANDINFO, BAND_FILTER, (LPARAM)&rbi);
            }
        }

        // --- Кнопка поиска заполняет всю ширину Band 2 ---
        impl->UpdateSearchButtonWidth();
        LayoutFilterToolbarInHost(impl);

        // --- Позиционировать TabCtrl и StatusBar ---
        int tabH = cy - rebarH - STATUS_H;
        if (tabH < 0) tabH = 0;

        MoveWindow(impl->hTabCtrl, 0, rebarH, cx, tabH, TRUE);
        MoveWindow(impl->hStatus, 0, cy - STATUS_H, cx, STATUS_H, TRUE);
        SendMessageW(impl->hStatus, WM_SIZE, 0, 0);

        // --- Переместить ListViews (дочерние окна TabCtrl, координаты в его пространстве) ---
        {
            RECT rcTab;
            GetClientRect(impl->hTabCtrl, &rcTab);
            TabCtrl_AdjustRect(impl->hTabCtrl, FALSE, &rcTab);
            // MapWindowPoints не нужен — ListView теперь дочернее окно hTabCtrl

            for (auto& tab : impl->m_tabs)
                MoveWindow(tab.hList, rcTab.left, rcTab.top,
                    rcTab.right - rcTab.left, rcTab.bottom - rcTab.top, TRUE);

            // Растянуть последнюю колонку активной вкладки
            if (!impl->m_tabs.empty())
            {
                HWND hActive = impl->ActiveTab().hList;
                RECT rcL;
                GetClientRect(hActive, &rcL);
                int fixedW = 52 + 118 + 60 + 60 + 50 + 4;
                int textColW = rcL.right - fixedW - GetSystemMetrics(SM_CXVSCROLL) - 6;
                if (textColW > 200)
                    ListView_SetColumnWidth(hActive, 5, textColW);
            }
        }

        // --- Переместить Edit если виден ---
        if (impl->m_editVisible)
            impl->RepositionEdit();

        return 0;
    }

    case WM_DPICHANGED:
    {
        const auto* prc = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, prc->left, prc->top,
            prc->right - prc->left, prc->bottom - prc->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_DWMCOLORIZATIONCHANGED:
        break;

    // Инвалидация ListView после любого изменения размера или первого показа.
    // DefWindowProcW внутри WM_WINDOWPOSCHANGED отправляет WM_SIZE (layouting),
    // поэтому к моменту нашего кода все контролы уже расставлены.
    case WM_WINDOWPOSCHANGED:
    {
        const auto* pos = reinterpret_cast<const WINDOWPOS*>(lParam);
        // Сначала дефолтная обработка — она отправит WM_SIZE → наш layout
        const LRESULT r = DefWindowProcW(hwnd, msg, wParam, lParam);
        // Инвалидируем, если окно не свёрнуто И (изменился размер ИЛИ окно стало видимым)
        if (impl && !IsIconic(hwnd))
        {
            const bool sizeChanged = !(pos->flags & SWP_NOSIZE);
            const bool becameVisible = !!(pos->flags & SWP_SHOWWINDOW);
            if (sizeChanged || becameVisible)
                impl->InvalidateList();
        }
        return r;
    }

    // Минимальная ширина окна = сумма лент (Band 1 ≥ m_filterW после TB_GETMAXSIZE) + рамка
    case WM_GETMINMAXINFO:
    {
        if (impl)
        {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lParam);
            int minW = impl->m_ctrlW + impl->m_filterW + impl->m_searchW
                + GetSystemMetrics(SM_CXFRAME) * 2;
            if (mm->ptMinTrackSize.x < minW)
                mm->ptMinTrackSize.x = minW;
        }
        break;
    }

    // Выпадающий фильтр: фокус на ListView внутри owned popup даёт WM_NCACTIVATE(FALSE) на главном —
    // рамка/заголовок рисуются «неактивными», хотя foreground всё ещё наше окно. Принудительно
    // оставляем NC активным, пока открыт попап и пользователь не переключился на другое приложение.
    case WM_NCACTIVATE:
    {
        if (impl && impl->IsFilterPopupOpen() && !wParam)
        {
            HWND const fg = ::GetForegroundWindow();
            if (fg == hwnd || (fg && ::GetWindow(fg, GW_OWNER) == hwnd))
                return ::DefWindowProcW(hwnd, WM_NCACTIVATE, TRUE, lParam);
        }
        break;
    }

    // F3 — далее; Ctrl+F3 / Shift+F3 — назад (когда фокус на главном окне, не в Edit/ListView)
    case WM_KEYDOWN:
    {
        if (!impl) break;
        if (wParam == VK_F3)
        {
            impl->DoSearch(!LogViewerSearchNavigateBackwardFromModifiers());
            return 0;
        }
        break;
    }

    // Активация: подтянуть ink из реестра (после «Применить» в GlassSettings); деактивация — скрыть Edit.
    case WM_ACTIVATE:
    {
        if (LOWORD(wParam) != WA_INACTIVE)
            LogViewerGlass::ReloadRenderTextInkFromRegistryThrottled(500);
        if (LOWORD(wParam) == WA_INACTIVE && impl)
            impl->CloseEdit();
        break;
    }

    // Отложенный показ Edit (PostMessage из WM_COMMAND IDC_SEARCH_BTN)
    case WM_APP_SHOW_EDIT:
    {
        if (impl) impl->ShowEdit();
        return 0;
    }

    // Скрытие Edit (PostMessage из LogViewerMainWndEditSubclassProc или WM_ACTIVATE)
    case WM_APP_HIDE_EDIT:
    {
        if (impl) impl->CloseEdit();
        return 0;
    }

    case WM_APP_CLOSE_FILTER:
    {
        if (impl && impl->IsFilterPopupOpen())
            impl->CloseFilterPopup(true);
        return 0;
    }

    case WM_APP_SWITCH_FILTER:
    {
        if (impl)
            impl->SwitchFilterPopupIfOpen(static_cast<int>(wParam));
        return 0;
    }

    case WM_APP_FILTER_RESUME_DROPDOWN:
    {
        if (impl)
            impl->OpenFilterDropdownModal(static_cast<int>(wParam));
        return 0;
    }

    case WM_APP_FILTER_TOOLBAR_POST_SYNC:
    {
        if (impl && impl->hFilterTb && ::IsWindow(impl->hFilterTb))
            LogViewerFilterToolbar_ApplyPostDropdownSync(impl->hFilterTb);
        return 0;
    }

    // ---------------------------------------------------------------------------
    // NM_CUSTOMDRAW, LVN_GETDISPINFO, LVN_KEYDOWN, TCN_SELCHANGE
    // ---------------------------------------------------------------------------
    case WM_NOTIFY:
    {
        if (!impl) break;
        auto* hdr = reinterpret_cast<LPNMHDR>(lParam);

        // RBS_FIXEDORDER запрещает перестановку лент.
        // RBN_BEGINDRAG не перехватываем — пусть Rebar обрабатывает drag нативно.

        // --- Рябар: при перетаскивании гриппера Band 2 обновить кнопку поиска ---
        if (hdr->hwndFrom == impl->hRebar &&
            (hdr->code == RBN_HEIGHTCHANGE || hdr->code == RBN_ENDDRAG))
        {
            impl->UpdateSearchButtonWidth();
            return 0;
        }

        // --- Таб-контрол: переключение вкладок ---
        if (hdr->hwndFrom == impl->hTabCtrl && hdr->code == TCN_SELCHANGE)
        {
            int sel = TabCtrl_GetCurSel(impl->hTabCtrl);
            if (sel == (int)impl->m_tabs.size())
                impl->AddTab();      // "+" выбрана — добавить новую вкладку
            else
                impl->SwitchToTab(sel);
            return 0;
        }

        // --- Тулбар фильтров: BTNS_DROPDOWN (стрелка) → TBN_DROPDOWN и модальный цикл ---
        if (hdr->hwndFrom == impl->hFilterTb && hdr->code == TBN_DROPDOWN)
        {
            const auto* nmtb = reinterpret_cast<const NMTOOLBARW*>(lParam);
            int kind = -1;
            switch (nmtb->iItem)
            {
            case IDC_PROC_BTN: kind = 0; break;
            case IDC_MOD_BTN:  kind = 1; break;
            case IDC_SUB_BTN:  kind = 2; break;
            default: break;
            }
            if (kind < 0)
                return TBDDRET_DEFAULT;
            impl->OpenFilterDropdownModal(kind);
            // WorkerW.au3 TBN_DROPDOWN: Return 0 = TBDDRET_DEFAULT после своего TrackPopupMenu — сброс внутренней сессии
            // split у comctl32; двухфазный hot-switch (WM_APP_FILTER_RESUME_DROPDOWN) опирается на это же.
            return TBDDRET_DEFAULT;
        }

        // Фильтр-тулбар: см. LogViewerFilterPopup.cpp. При открытом попапе — RequestFilterPopupHotSwitch (только Close +
        // отложенное открытие), без OpenFilterPopup в том же NOTIFY.
        if (hdr->hwndFrom == impl->hFilterTb && hdr->code == TBN_HOTITEMCHANGE)
        {
            const auto* hot = reinterpret_cast<const NMTBHOTITEM*>(lParam);
            impl->m_filterTbHotIdNew = hot->idNew;
            impl->m_filterTbHotIdOld = hot->idOld;
            impl->m_filterTbHotDwFlags = hot->dwFlags;
            if (impl->IsFilterPopupOpen())
            {
                if ((hot->dwFlags & HICF_LEAVING) == 0)
                {
                    int switchKind = -1;
                    switch (hot->idNew)
                    {
                    case IDC_PROC_BTN: switchKind = 0; break;
                    case IDC_MOD_BTN:  switchKind = 1; break;
                    case IDC_SUB_BTN:  switchKind = 2; break;
                    default: break;
                    }
                    if (switchKind >= 0)
                        impl->RequestFilterPopupHotSwitch(switchKind);
                }
            }
            return FALSE;
        }

        // --- ListView: находим вкладку по HWND ---
        TabView* tab = impl->FindTab(hdr->hwndFrom);
        if (!tab) break;

        if (hdr->code == LVN_ENDSCROLL)
        {
            const int n = (int)tab->m_visibleIdx.size();
            tab->m_autoScroll = ListViewIsScrolledToBottom(tab->hList, n);
            break;
        }

        if (hdr->code == NM_CUSTOMDRAW)
        {
            auto* cd = reinterpret_cast<LPNMLVCUSTOMDRAW>(lParam);

            switch (cd->nmcd.dwDrawStage)
            {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;

            case CDDS_ITEMPREPAINT:
            {
                int vi = (int)cd->nmcd.dwItemSpec;
                if (vi < 0 || vi >= (int)tab->m_visibleIdx.size())
                    return CDRF_DODEFAULT;

                const LogEntry& e = impl->m_allLogs[tab->m_visibleIdx[vi]];

                COLORREF bg = impl->GetThreadBg(e.processId, e.threadId);

                bool selected = (ListView_GetItemState(tab->hList, vi, LVIS_SELECTED) & LVIS_SELECTED) != 0;
                bool focused = (GetFocus() == tab->hList);

                if (selected && focused)
                {
                    constexpr int A = 89;  // 35% of 255
                    bg = RGB(
                        (GetRValue(bg) * (255 - A) + GetRValue(LogViewerListBgSel) * A) / 255,
                        (GetGValue(bg) * (255 - A) + GetGValue(LogViewerListBgSel) * A) / 255,
                        (GetBValue(bg) * (255 - A) + GetBValue(LogViewerListBgSel) * A) / 255
                    );
                }
                else if (selected)
                {
                    bg = LogViewerListBgSelInactive;
                }

                cd->clrTextBk = bg;
                cd->clrText = LogViewerLevelFg(e.level);

                return CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
            }

            case CDDS_SUBITEM | CDDS_ITEMPREPAINT:
                return CDRF_DODEFAULT;

            case CDDS_ITEMPOSTPAINT:
            {
                // Тёмная горизонтальная линия под каждой строкой
                RECT rcItem{};
                ListView_GetItemRect(tab->hList, (int)cd->nmcd.dwItemSpec, &rcItem, LVIR_BOUNDS);

                HPEN pen = CreatePen(PS_SOLID, 1, LogViewerListGridLine);
                HPEN op = (HPEN)SelectObject(cd->nmcd.hdc, pen);
                MoveToEx(cd->nmcd.hdc, rcItem.left, rcItem.bottom - 1, nullptr);
                LineTo(cd->nmcd.hdc, rcItem.right, rcItem.bottom - 1);
                SelectObject(cd->nmcd.hdc, op);
                DeleteObject(pen);
                return CDRF_DODEFAULT;
            }
            }

            return CDRF_DODEFAULT;
        }

        if (hdr->code == LVN_GETDISPINFO)
        {
            auto* disp = reinterpret_cast<NMLVDISPINFOW*>(lParam);
            int vi = disp->item.iItem;
            if (vi < 0 || vi >= (int)tab->m_visibleIdx.size())
                break;

            static wchar_t sBuf[2048];

            if (disp->item.mask & LVIF_TEXT)
            {
                std::wstring cell = impl->GetCellText(*tab, vi, disp->item.iSubItem);
                wcsncpy_s(sBuf, cell.c_str(), _TRUNCATE);
                disp->item.pszText = sBuf;
            }
            break;
        }

        if (hdr->code == LVN_KEYDOWN)
        {
            if (tab != &impl->ActiveTab()) break;  // только активная вкладка
            auto* kd = reinterpret_cast<LPNMLVKEYDOWN>(lParam);
            if (kd->wVKey == L'A' && (GetKeyState(VK_CONTROL) & 0x8000))
            {
                ListView_SetItemState(tab->hList, -1, LVIS_SELECTED, LVIS_SELECTED);
                return 0;
            }
            if (kd->wVKey == L'C' && (GetKeyState(VK_CONTROL) & 0x8000))
            {
                impl->DoCopy(false);
                return 0;
            }
            if (kd->wVKey == VK_F3)
            {
                impl->DoSearch(!LogViewerSearchNavigateBackwardFromModifiers());
                return 0;
            }
            break;
        }

        break;
    }

    case WM_COMMAND:
    {
        if (!impl) break;
        int id = LOWORD(wParam);
        int notif = HIWORD(wParam);

        switch (id)
        {
        case IDC_START_STOP:
            // BTNS_CHECK: тулбар уже переключил состояние до WM_COMMAND
            if (SendMessageW(impl->hCtrlTb, TB_ISBUTTONCHECKED, IDC_START_STOP, 0))
                impl->StartPlatform();
            else
                impl->StopPlatform();
            return 0;
        case IDC_GLASS_SETTINGS:
        {
            std::wstring settingsExe =
                GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
                    nullptr, L"GlassSettings.exe");
            if (settingsExe.empty())
                return 0;
            const std::wstring workDir =
                GlassShared::GlassModulePath::GetInstallRootFromPath(settingsExe.c_str());
            ShellExecuteW(
                hwnd,
                L"open",
                settingsExe.c_str(),
                nullptr,
                workDir.empty() ? nullptr : workDir.c_str(),
                SW_SHOWNORMAL);
            return 0;
        }
        case IDC_PAUSE_BTN: impl->TogglePause();        return 0;
        case IDC_CLEAR:     impl->DoClear();            return 0;
        case IDC_INVERT:    impl->DoInvertSelection();  return 0;
        case IDC_FIND_NEXT: impl->DoSearch(true);       return 0;
        case IDC_FIND_PREV: impl->DoSearch(false);      return 0;

            // --- Кнопки фильтра: BTNS_DROPDOWN — открытие списка через TBN_DROPDOWN (клик по стрелке) ---
        case IDC_PROC_BTN:
        case IDC_MOD_BTN:
        case IDC_SUB_BTN:
            return 0;

        // --- Кнопка «Поиск в документе» — показать Edit ---
        case IDC_SEARCH_BTN:
        {
            PostMessageW(hwnd, WM_APP_SHOW_EDIT, 0, 0);
            return 0;
        }

        // --- Edit поиска: при изменении текста — запустить таймер задержки ---
        case IDC_SEARCH:
            if (notif == EN_CHANGE)
            {
                // Перезапускаем таймер — поиск начнётся через 1 сек после последнего ввода
                SetTimer(hwnd, IDT_SEARCH, 1000, nullptr);
                return 0;
            }
            break;

        case IDM_COPY:       impl->DoCopy(false);        return 0;
        case IDM_COPY_ALL:   impl->DoCopy(true);         return 0;
        case IDM_COPY_CELL:  impl->DoCopyCell();         return 0;
        case IDM_SAVE_PROCESS_DUMP: impl->DoSaveProcessDump(); return 0;
        case IDM_CLEAR:      impl->DoClear();            return 0;
        case IDM_INVERT_SEL: impl->DoInvertSelection();  return 0;
        }

        if (id >= IDC_LVL_BASE && id < IDC_LVL_BASE + 5 && notif == BN_CLICKED)
        {
            impl->OnLevelCheckChanged(id - IDC_LVL_BASE);
            return 0;
        }

        break;
    }

    // ---------------------------------------------------------------------------
    // Контекстное меню
    // ---------------------------------------------------------------------------
    case WM_CONTEXTMENU:
    {
        if (!impl) break;

        // Проверяем, что клик по одному из наших ListViews
        TabView* ctxTab = impl->FindTab((HWND)wParam);
        if (!ctxTab) break;

        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        // Определяем строку и столбец правого клика
        POINT ptList = pt;
        ScreenToClient(ctxTab->hList, &ptList);
        LVHITTESTINFO ht{};
        ht.pt = ptList;
        ListView_SubItemHitTest(ctxTab->hList, &ht);
        ctxTab->m_ctxMenuRow = ht.iItem;
        ctxTab->m_ctxMenuCol = ht.iSubItem;

        HMENU hMenu = CreatePopupMenu();
        AppendMenuW(hMenu, MF_STRING, IDM_COPY, L"Копировать выделенное\tCtrl+C");
        AppendMenuW(hMenu, MF_STRING, IDM_COPY_ALL, L"Копировать всё");
        AppendMenuW(hMenu, MF_STRING, IDM_COPY_CELL, L"Копировать текст в ячейке");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_SAVE_PROCESS_DUMP, L"Сохранить дамп процесса\u2026");
        EnableMenuItem(
            hMenu,
            IDM_SAVE_PROCESS_DUMP,
            MF_BYCOMMAND | (ht.iItem >= 0 ? MF_ENABLED : MF_GRAYED));
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_INVERT_SEL, L"Инвертировать выделение");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, IDM_CLEAR, L"Очистить");

        TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        DestroyMenu(hMenu);
        return 0;
    }

    // ---------------------------------------------------------------------------
    // Устаревший сигнал WM_LOG_PACKET — тот же дренаж, что и по IDT_LOG_DRAIN
    // ---------------------------------------------------------------------------
    case WM_LOG_PACKET:
    {
        if (!impl) break;
        ProcessLogIncomingBatch(hwnd, impl);
        return 0;
    }

    case WM_APP_PID_LABELS_FLUSH:
    {
        if (!impl) break;
        std::vector<std::pair<uint32_t, std::wstring>> batch;
        TakePendingPidLabelResults(batch);
        for (auto& pr : batch)
            impl->m_pidLabels[pr.first] = std::move(pr.second);
        impl->UpdateProcButtonText();
        impl->UpdateModButtonText();
        impl->UpdateSubButtonText();
        impl->UpdateStatus();
        return 0;
    }

    case WM_APP_PROCESS_DUMP_DONE:
    {
        if (!impl) break;
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));

        auto result = std::unique_ptr<LogViewerProcessDumpResult>(
            reinterpret_cast<LogViewerProcessDumpResult*>(lParam));
        if (!result)
            return 0;

        if (result->ok)
        {
            wchar_t msg[MAX_PATH + 64]{};
            swprintf_s(msg, L"Дамп сохранён:\n%s", result->path);
            MessageBoxW(hwnd, msg, L"LogViewer", MB_OK | MB_ICONINFORMATION);
        }
        else
        {
            wchar_t msg[512]{};
            swprintf_s(
                msg,
                L"Не удалось сохранить дамп:\n%s\n\n%s",
                result->path,
                result->error[0] ? result->error : L"Неизвестная ошибка");
            MessageBoxW(hwnd, msg, L"LogViewer", MB_OK | MB_ICONERROR);
        }
        return 0;
    }

    // ---------------------------------------------------------------------------
    // Состояние соединения
    // ---------------------------------------------------------------------------
    case WM_LOG_CONNECTED:
    {
        if (!impl) break;
        impl->m_connected = (wParam == 1);
        // GlassPlatform.exe не отправляет LOG_LEVEL_DISCONNECT для себя —
        // при закрытии трубы считаем все процессы неактивными.
        if (!impl->m_connected)
            impl->m_activePids.clear();
        impl->UpdateStatus();
        return 0;
    }

    // ---------------------------------------------------------------------------
    // Команды MessageRouter
    // ---------------------------------------------------------------------------
    case WM_APP_COMMAND:
    {
        if (!impl) break;
        auto cmd = (LogViewerCommand)wParam;
        switch (cmd)
        {
        case LogViewerCommand::Clear:
            impl->DoClear();
            break;
        case LogViewerCommand::PlatformStarted:
            impl->m_platformRunning = true;
            impl->UpdateButtons();
            break;
        case LogViewerCommand::PlatformStopped:
            impl->m_platformRunning = false;
            impl->UpdateButtons();
            break;
        default: break;
        }
        return 0;
    }

    // ---------------------------------------------------------------------------
    // Прокрутка: следим за автоскроллом активной вкладки
    // ---------------------------------------------------------------------------
    case WM_MOUSEWHEEL:
        // Автопрокрутка синхронизируется с LVN_ENDSCROLL у ListView логов
        break;

    // -----------------------------------------------------------------------
    // Таймеры: опрос завершения при Stop, периодическая синхронизация состояния
    // -----------------------------------------------------------------------
    case WM_TIMER:
    {
        if (wParam == IDT_STOP_POLL && impl && impl->m_stopping)
        {
            impl->UpdateButtons();
            if (!impl->m_stopping)
                KillTimer(hwnd, IDT_STOP_POLL);
        }
        if (wParam == IDT_PLATFORM_POLL && impl)
        {
            bool actual = IsGlassPlatformRunning();
            if (actual != impl->m_platformRunning)
            {
                impl->m_platformRunning = actual;
                impl->UpdateButtons();
            }
        }
        if (wParam == IDT_LOGVIEWER_WINEVENT_PING && impl)
        {
            KillTimer(hwnd, IDT_LOGVIEWER_WINEVENT_PING);
            if (IsWindow(hwnd))
            {
                NotifyWinEvent(EVENT_OBJECT_SHOW, hwnd, OBJID_WINDOW, CHILDID_SELF);
                (void)PostMessageW(hwnd, WM_NULL, 0, 0);
            }
            return 0;
        }
        if (wParam == IDT_SEARCH && impl)
        {
            KillTimer(hwnd, IDT_SEARCH);
            impl->DoSearch(true);  // поиск вниз после задержки
        }
        if (wParam == IDT_LOG_DRAIN && impl)
        {
            ProcessLogIncomingBatch(hwnd, impl);
            return 0;
        }
        return 0;
    }

    case WM_CLOSE:
    {
        if (!impl)
            break;
        // Платформа запущена (хост держит mutex) — спросить, останавливать ли её при выходе.
        if (!IsGlassPlatformRunning())
            break;
        const int answer = MessageBoxW(
            hwnd,
            L"Платформа Glass сейчас запущена.\n\n"
            L"Да — остановить платформу и закрыть Log Viewer.\n"
            L"Нет — закрыть Log Viewer; платформа продолжит работу.\n"
            L"Отмена — вернуться к окну.",
            L"Glass Log Viewer",
            MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3);
        if (answer == IDCANCEL)
            return 0;
        if (answer == IDYES)
            impl->StopPlatform();
        DestroyWindow(hwnd);
        return 0;
    }

    case WM_DESTROY:
    {
        StopLogClientSupportThreads();
        if (impl)
        {
            KillTimer(hwnd, IDT_STOP_POLL);
            KillTimer(hwnd, IDT_PLATFORM_POLL);
            KillTimer(hwnd, IDT_LOGVIEWER_WINEVENT_PING);
            KillTimer(hwnd, IDT_SEARCH);
            KillTimer(hwnd, IDT_LOG_DRAIN);
            KillTimer(hwnd, IDT_FILTER_TOOLBAR_LEGACY);
            if (impl->hTabCtrl) RemoveWindowSubclass(impl->hTabCtrl, LogViewerMainWndTabCtrlSubclassProc, 1);
            if (impl->hSearch)  RemoveWindowSubclass(impl->hSearch, LogViewerMainWndEditSubclassProc, 2);
            if (impl->m_font)     DeleteObject(impl->m_font);
            if (impl->m_monoFont) DeleteObject(impl->m_monoFont);
            if (impl->hImgList)   ImageList_Destroy(impl->hImgList);
            // ListViews и прочие дочерние окна — уничтожаются автоматически
            // КРИТИЧНО: обнуляем GWLP_USERDATA до delete — после WM_DESTROY Windows
            // продолжает доставлять WM_NOTIFY (NM_CUSTOMDRAW и др.) от дочерних окон.
            // GetImpl() без обнуления вернёт висячий указатель → краш в FindTab.
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete impl;
        }
        g_mainWindow = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
