#include "SharedPch.h"

#include "framework.h"
#include "UiMainWindow.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "Ui/MainWindow/UiMainWindowInternal.h"
#include "Ui/Messages/LogViewerAppMessages.h"
#include <commctrl.h>

#pragma comment(lib, "Comctl32.lib")

extern UiMainWindow* g_mainWindow; // определение — MessageRouter.cpp

UiMainWindowImpl* GetImpl(HWND hwnd)
{
    return reinterpret_cast<UiMainWindowImpl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

HFONT CreateMonoFont(int ptSize)
{
    return CreateFontW(
        -MulDiv(ptSize, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas"
    );
}

HFONT CreateUiFont()
{
    // Как стандартные диалоги: шрифт из текущей темы (lfMessageFont),
    // а не захардкоженное имя — иначе Edit/поиск визуально расходятся с «обычными» процессами.
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        return CreateFontIndirectW(&ncm.lfMessageFont);
    return CreateFontW(
        -MulDiv(9, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 72),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"MS Shell Dlg");
}

void UiMainWindow::Create(HINSTANCE hInst)
{
    INITCOMMONCONTROLSEX icc{
        sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_COOL_CLASSES | ICC_BAR_CLASSES
    };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.hInstance = hInst;
    wc.lpszClassName = L"LogViewerWindow";
    wc.lpfnWndProc = UiMainWindow::WndProc;
    wc.hIcon = LoadIconW(hInst, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassW(&wc);
    (void)RegisterLogViewerFilterTbHostClass(hInst);

    auto* impl = new UiMainWindowImpl();

    m_hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Glass Log Viewer",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1280, 750,
        nullptr, nullptr, hInst, impl
    );

    g_mainWindow = this;

    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void UiMainWindow::SetPlatformRunning(bool running)
{
    auto* impl = GetImpl(m_hwnd);
    if (!impl) return;
    impl->m_platformRunning = running;
    impl->UpdateButtons();
}
