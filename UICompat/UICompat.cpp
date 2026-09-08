#include "UiPch.h"
#include "config/UiRegistry.h"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/UiTapSite.hpp"

#include <InjectProcessMap.h>
#include <atomic>
#include <objbase.h>

namespace
{
    std::atomic<bool> g_initialized{ false };
    std::atomic<bool> g_shutdownComplete{ false };
}

extern "C" __declspec(dllexport) BOOL WINAPI UICompat_Initialize(HMODULE)
{
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true))
        return TRUE;

    (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagLifecycleUICompat,
        true);

    g_shutdownComplete.store(false, std::memory_order_release);
    UICompatCom::g_shutdownDone.store(false, std::memory_order_release);

    const HRESULT hrPrepEarly = UICompat_PrepareXamlDiagnostics();

    DebugLog(DBG_IMPORTANT, L"[UICompat][Core] Initialize: BEGIN");
    DebugLog(
        DBG_IMPORTANT,
        L"[UICompat][Core] BuildMarker: %S %S",
        __DATE__,
        __TIME__);

    if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
    {
        using ResetCacheFn = void(WINAPI*)(void);
        if (const auto pReset = reinterpret_cast<ResetCacheFn>(
                GetProcAddress(hWndCompat, "WndCompat_ResetUICompatNotifyProcCache")))
        {
            pReset();
        }
    }

    if (FAILED(hrPrepEarly))
    {
        DebugLog(
            DBG_WARNING,
            L"[UICompat][Core] PrepareXamlDiagnostics hr=0x%08X",
            static_cast<unsigned>(hrPrepEarly));
    }

    const auto settings = UICompat::Registry::ReadFromRegistry();
    UICompat::UiTapSite::SetPendingSettings(settings);

    if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
    {
        using FlushPendingFn = void(WINAPI*)();
        if (const auto pFlush = reinterpret_cast<FlushPendingFn>(
                GetProcAddress(hWndCompat, "WndCompat_FlushPendingCoreWindows")))
        {
            pFlush();
        }
        // AttachExisting на init запрещён: dormant CoreWindow может дать SEH AV,
        // после чего повторный NC появляется уже с кастомом.
    }

    DebugLog(DBG_IMPORTANT, L"[UICompat][Core] Initialize: END");
    (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagLifecycleUICompat,
        false);
    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagUICompat,
        true);
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI UICompat_ReloadSettingsAndReapply() noexcept
{
    UICompat::UiTapSite::ReloadSettingsAndReapply();
}

extern "C" __declspec(dllexport) void WINAPI UICompat_RefreshBrushesAfterThemeChange() noexcept
{
    UICompat::UiTapSite::RefreshBrushesAfterThemeChange();
}

extern "C" __declspec(dllexport) BOOL WINAPI UICompat_Shutdown()
{
    // IXDE/TAP может поднять UICompat без UICompat_Initialize (лог SEH Stop:
    // no_uicompat, нет restore_all — ранний return оставлял NC с кастомом).
    // StopXaml/Restore всегда, даже если Initialize не вызывали.
    const bool wasInit = g_initialized.load(std::memory_order_acquire);

    DebugLog(
        DBG_IMPORTANT,
        L"[UICompat][Core] Shutdown: BEGIN (initialized=%u)",
        wasInit ? 1u : 0u);
    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagUICompat,
        false);

    // Как TaskbarCompat_Shutdown: StopXaml (TAP Unadvise + drain IXDE STA) → CoFree → complete.
    UICompat_StopXamlDiagnostics();

    DebugLog(
        DBG_IMPORTANT,
        L"[UICompat][Core] Stop: COM refs objectCount=%ld serverLocks=%ld",
        static_cast<long>(UICompatCom::g_objectCount.load(std::memory_order_acquire)),
        static_cast<long>(UICompatCom::g_serverLocks.load(std::memory_order_acquire)));

    CoFreeUnusedLibraries();
    for (int i = 0; i < 3; ++i)
    {
        Sleep(10);
        CoFreeUnusedLibraries();
    }

    DebugLog(
        DBG_IMPORTANT,
        L"[UICompat][Core] CoFreeUnusedLibraries: after objectCount=%ld serverLocks=%ld",
        static_cast<long>(UICompatCom::g_objectCount.load(std::memory_order_acquire)),
        static_cast<long>(UICompatCom::g_serverLocks.load(std::memory_order_acquire)));

    g_shutdownComplete.store(true, std::memory_order_release);
    g_initialized.store(false, std::memory_order_release);

    DebugLog(DBG_IMPORTANT, L"[UICompat][Core] Shutdown: END");
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER) && defined(_DEBUG)
    Shared::Log::GlassLogClient::Instance().Stop();
#endif
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI UICompat_IsShutdownComplete()
{
    return g_shutdownComplete.load(std::memory_order_acquire) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) unsigned long long WINAPI UICompat_DebugComState() noexcept
{
    const unsigned long long oc =
        static_cast<unsigned long long>(UICompatCom::g_objectCount.load(std::memory_order_acquire)) & 0xFFFFULL;
    const unsigned long long sl =
        static_cast<unsigned long long>(UICompatCom::g_serverLocks.load(std::memory_order_acquire)) & 0xFFFFULL;
    const unsigned long long sd =
        UICompatCom::g_shutdownDone.load(std::memory_order_acquire) ? 1ULL : 0ULL;
    return oc | (sl << 16) | (sd << 32);
}

extern "C" __declspec(dllexport) void WINAPI UICompat_HandleWinEvent(
    DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
}
