#include "SharedPch.h"

#include "framework.h"
#include "Ui/MainWindow/UiMainWindowImpl.h"
#include "UiConsts.h"
#include <GlassModulePath.h>
#include <GlassPlatformTask.h>
#include <shellapi.h>
#include <string>

#pragma comment(lib, "Shell32.lib")

static std::wstring GetGlassCompatPath()
{
    std::wstring path =
        GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(nullptr, L"GlassCompat.dll");
    if (path.empty())
        return L"GlassCompat.dll";
    return path;
}

void UiMainWindowImpl::StartPlatform()
{
    m_stopping = false;
    KillTimer(hwnd, IDT_STOP_POLL);
    KillTimer(hwnd, IDT_LOGVIEWER_WINEVENT_PING);
    m_platformRunning = true;
    UpdateButtons();
    std::wstring dllPath = GetGlassCompatPath();
    std::wstring params = L"\"" + dllPath + L"\",Main /start";
    ShellExecuteW(nullptr, L"open", L"Rundll32.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    SetTimer(hwnd, IDT_LOGVIEWER_WINEVENT_PING, 350, nullptr);
}

void UiMainWindowImpl::StopPlatform()
{
    m_stopping = true;
    UpdateButtons();
    SetTimer(hwnd, IDT_STOP_POLL, 500, nullptr);
    (void)GlassShared::GlassPlatformTask::SignalPlatformStop();
}
