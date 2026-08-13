#include "SharedPch.h"

#include "framework.h"
#include "UiMainWindow.h"
#include "LogClient.h"
#include "GlassLogServer.h"
#include "GlassLogBridge.h"
#include "LogViewerGlassHost.h"
#include "LogViewerGlassAttach.h"
#include "LogViewerComboLBoxSubclass.h"

// Выводит текст в панель Output Visual Studio (видно при отладке без LogViewer).
static void DbgOut(const wchar_t* msg)
{
    OutputDebugStringW(L"[LogViewer] ");
    OutputDebugStringW(msg);
    OutputDebugStringW(L"\n");
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    DbgOut(L"wWinMain: start");

    DbgOut(L"LoadLibrary Msftedit");
    LoadLibraryW(L"Msftedit.dll");

    DbgOut(L"LogViewerGlass::LoadGlassCoreDll");
    if (!LogViewerGlass::LoadGlassCoreDll())
        DbgOut(L"LoadGlassCoreDll failed (GlassCore.dll не найден рядом с exe — стекло без хоста недоступно)");
    else
    {
        DbgOut(L"LogViewerGlass::EnsureWndCompatRuntime");
        if (!LogViewerGlass::EnsureWndCompatRuntime())
            DbgOut(L"EnsureWndCompatRuntime failed (WndCompat.dll не найден — только GlassCore, без WndCompat)");
    }

    // GlassLogServer создаёт пайп \\.\pipe\GlassLog и ждёт подключения LogClientThread.
    // ВАЖНО: Start() НЕ должен блокироваться до подключения клиента — клиент
    // запускается позже (StartLogClientThread). Сервер стартует в фоне.
    DbgOut(L"GlassLogServer::Start");
    Shared::Log::GlassLogServer::Instance().Start();
    DbgOut(L"GlassLogServer::Start done");

    // GlassLogBridge принимает подключения от инжектированных DLL-модулей.
    DbgOut(L"GlassLogBridge::Start");
    Shared::Log::GlassLogBridge::Instance().Start();
    DbgOut(L"GlassLogBridge::Start done");

    DbgOut(L"UiMainWindow::Create");
    UiMainWindow window;
    window.Create(hInstance);
    DbgOut(L"UiMainWindow::Create done");

    // LogClientThread подключается к пайпу GlassLogServer и читает пакеты.
    DbgOut(L"StartLogClientThread");
    StartLogClientThread(window.GetHwnd());
    DbgOut(L"StartLogClientThread done");

    DbgOut(L"entering message loop");
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DbgOut(L"message loop exited, unloading Glass runtime");
    LogViewer::ComboLBoxUi::DetachAll();
    LogViewer::ComboLBoxUi::FlushPendingUiMessages(200);

    LogViewerGlassAttach::ShutdownCleanup();
    LogViewerGlass::ShutdownGlassCoreDll();

    LogViewer::ComboLBoxUi::ClearCachedGlassProviders();
    Shared::Log::GlassLogBridge::Instance().Stop();
    Shared::Log::GlassLogServer::Instance().Stop();

    DbgOut(L"wWinMain: exit");
    return 0;
}
