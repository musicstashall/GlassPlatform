#include "TaskbarPch.h"
#include "config/TaskbarRegistry.h"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/TaskbarTapSite.hpp"

#include <Helpers/BorderDisplayHelper.h>
#include <Helpers/ThemeHelper.h>
#include <InjectProcessMap.h>

#include <atomic>
#include <objbase.h>

namespace
{
    std::atomic<bool> g_initialized{ false };
    std::atomic<bool> g_shutdownComplete{ false };
}

extern "C" __declspec(dllexport) BOOL WINAPI TaskbarCompat_Initialize(HMODULE)
{
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true))
        return TRUE;

    (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagLifecycleTaskbar,
        true);

    g_shutdownComplete.store(false, std::memory_order_release);
    TaskbarCompatCom::g_shutdownDone.store(false, std::memory_order_release);

    // 1) Подготовка TAP-пути как можно раньше: WinUi attach из explorer может случиться параллельно с этим потоком;
    //    раньше g_tapPathReady выставлялся после лога/реестра и Notify терялся навсегда (см. taskbar-logs).
    const HRESULT hrPrepEarly = TaskbarCompat_PrepareXamlDiagnostics();

    DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][Core] Initialize: BEGIN");
    DebugLog(
        DBG_IMPORTANT,
        L"[TaskbarCompat][Core] BuildMarker: %S %S",
        __DATE__,
        __TIME__);

    if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
    {
        using ResetCacheFn = void(WINAPI*)(void);
        if (const auto pReset = reinterpret_cast<ResetCacheFn>(
                GetProcAddress(hWndCompat, "WndCompat_ResetTaskbarCompatNotifyProcCache")))
        {
            pReset();
        }
    }

    if (FAILED(hrPrepEarly))
    {
        DebugLog(
            DBG_WARNING,
            L"[TaskbarCompat][Core] PrepareXamlDiagnostics hr=0x%08X",
            static_cast<unsigned>(hrPrepEarly));
    }

    // 2) Реестр: значение публикуется до первого SetSite (поток IXDE).
    const auto settings = TaskbarCompat::Registry::ReadFromRegistry();
    TaskbarCompat::TaskbarTapSite::SetPendingSettings(settings);

    {
        const bool darkMode = Shared::ThemeHelper::IsTrayXamlDarkMode();
        const auto inherited =
            Shared::BorderDisplayHelper::QueryInheritedBorderForBwmSubKey(L"Tray", darkMode);
        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][Core] Initialize policy: patchStroke=%u BorderColor=0x%08X "
            L"inheritedDefault=0x%08X source=%ls theme=%ls",
            settings.patchBorderStroke ? 1u : 0u,
            settings.borderColorArgb,
            inherited.displayArgb,
            Shared::BorderDisplayHelper::InheritedBorderSourceName(inherited.source),
            darkMode ? L"dark" : L"light");
    }

    // 3) WndCompat инициализируется до TaskbarCompat — сбросить очередь отложенных UWP-мостов.
    if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
    {
        using FlushPendingFn = void(WINAPI*)();
        if (const auto pFlush = reinterpret_cast<FlushPendingFn>(
                GetProcAddress(hWndCompat, "WndCompat_FlushPendingDesktopBridges")))
        {
            pFlush();
        }
    }

    DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][Core] Initialize: END");
    (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagLifecycleTaskbar,
        false);
    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagTaskbar,
        true);
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI TaskbarCompat_ReapplyXamlBackgroundPatches() noexcept
{
    TaskbarCompat::TaskbarTapSite::ReloadSettingsAndReapply();
}

extern "C" __declspec(dllexport) void WINAPI TaskbarCompat_ReloadSettingsAndReapply() noexcept
{
    TaskbarCompat::TaskbarTapSite::ReloadSettingsAndReapply();
}

// Для вызовов из hook-колбэков: только поставить запрос в очередь marshal-потока.
extern "C" __declspec(dllexport) void WINAPI TaskbarCompat_RequestReloadSettingsAndReapply() noexcept
{
    TaskbarCompat::TaskbarTapSite::RequestReloadSettingsAndReapply();
}

extern "C" __declspec(dllexport) BOOL WINAPI TaskbarCompat_Shutdown()
{
    if (!g_initialized.load(std::memory_order_acquire))
        return TRUE;

    DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][Core] Shutdown: BEGIN");
    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagTaskbar,
        false);

    TaskbarCompat_StopXamlDiagnostics();

    DebugLog(
        DBG_IMPORTANT,
        L"[TaskbarCompat][Core] Stop: COM refs objectCount=%ld serverLocks=%ld",
        static_cast<long>(TaskbarCompatCom::g_objectCount.load(std::memory_order_acquire)),
        static_cast<long>(TaskbarCompatCom::g_serverLocks.load(std::memory_order_acquire)));

    CoFreeUnusedLibraries();
    for (int i = 0; i < 3; ++i)
    {
        Sleep(10);
        CoFreeUnusedLibraries();
    }

    DebugLog(
        DBG_IMPORTANT,
        L"[TaskbarCompat][Core] CoFreeUnusedLibraries: after objectCount=%ld serverLocks=%ld",
        static_cast<long>(TaskbarCompatCom::g_objectCount.load(std::memory_order_acquire)),
        static_cast<long>(TaskbarCompatCom::g_serverLocks.load(std::memory_order_acquire)));

    g_shutdownComplete.store(true, std::memory_order_release);
    g_initialized.store(false, std::memory_order_release);

    DebugLog(DBG_IMPORTANT, L"[TaskbarCompat][Core] Shutdown: END");

    // Гарантия — никаких рабочих потоков TaskbarCompat не остаётся (см. CLAUDE.md, Логирование).
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER) && defined(_DEBUG)
    Shared::Log::GlassLogClient::Instance().Stop();
#endif
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI TaskbarCompat_IsShutdownComplete()
{
    return g_shutdownComplete.load(std::memory_order_acquire) ? TRUE : FALSE;
}

// COM-счётчики для residual FreeLibrary в GlassCompat (IXDE LoadLibrary-pin вне free-list).
// bits 0-15: objectCount, bits 16-31: serverLocks, bit 32: shutdownDone.
extern "C" __declspec(dllexport) unsigned long long WINAPI TaskbarCompat_DebugComState() noexcept
{
    const unsigned long long oc =
        static_cast<unsigned long long>(TaskbarCompatCom::g_objectCount.load(std::memory_order_acquire)) & 0xFFFFULL;
    const unsigned long long sl =
        static_cast<unsigned long long>(TaskbarCompatCom::g_serverLocks.load(std::memory_order_acquire)) & 0xFFFFULL;
    const unsigned long long sd =
        TaskbarCompatCom::g_shutdownDone.load(std::memory_order_acquire) ? 1ULL : 0ULL;
    return oc | (sl << 16) | (sd << 32);
}

extern "C" __declspec(dllexport) void WINAPI TaskbarCompat_HandleWinEvent(
    DWORD,
    HWND,
    LONG,
    LONG,
    DWORD,
    DWORD)
{
    // IXDE: ModuleHandler (Windows.UI.Xaml) + интерес трея через TrackBar (Full) и DesktopWindowContentBridge.
    // Заглушка для GetProcAddress в GlassCompat.cpp.
}
