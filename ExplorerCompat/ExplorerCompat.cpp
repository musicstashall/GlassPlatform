#include "ExplorerPch.h"
#include "ExplorerCompat.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/XamlTapSite.hpp"
#include "xaml/XamlShutdownMarshal.hpp"
#include "xaml/Patcher/XamlHostBarPatcherState.h"
#include "xaml/XamlComLifetime.hpp"
#include <DebugLog.h>
#include <ExplorerCompatHostPolicy.h>
#include <InjectProcessMap.h>

#include <atomic>

// Типы функций из WndCompat
using Wnd_Initialize_Fn = BOOL(WINAPI*)();
using Wnd_Shutdown_Fn = void (WINAPI*)();
using Wnd_BeginShutdown_Fn = void (WINAPI*)();
using Wnd_IsDone_Fn = BOOL(WINAPI*)();
using Wnd_Wait_Fn = void (WINAPI*)();
using Wnd_SetGuiThreadId_Fn = void (WINAPI*)(DWORD);

static HMODULE g_hWndCompat = nullptr;
static std::atomic<DWORD> g_guiThreadId{ 0 };

// Указатели на функции
static Wnd_Initialize_Fn     pWnd_Initialize = nullptr;
static Wnd_Shutdown_Fn       pWnd_Shutdown = nullptr;
static Wnd_BeginShutdown_Fn  pWnd_BeginShutdown = nullptr;
static Wnd_IsDone_Fn         pWnd_IsDone = nullptr;
static Wnd_Wait_Fn           pWnd_Wait = nullptr;
static Wnd_SetGuiThreadId_Fn pWnd_SetGuiThreadId = nullptr;

// === НОВАЯ АРХИТЕКТУРА ===
// Shutdown больше не зависит от UI thread
static std::atomic<bool> g_shutdownRequested{ false };
static std::atomic<bool> g_shutdownComplete{ false };
static std::atomic<bool> g_initialized{ false };

static void ExecuteThreadAgnosticShutdown() noexcept
{
    DebugLog(L"[ExplorerCompat][Core] ExecuteThreadAgnosticShutdown: BEGIN");
    g_shutdownComplete.store(true, std::memory_order_release);
    DebugLog(L"[ExplorerCompat][Core] Thread-agnostic shutdown completed in thread %u",
        GetCurrentThreadId());
}

// === ГЛАВНАЯ ФУНКЦИЯ SHUTDOWN ===
// Thread-agnostic, non-blocking, idempotent
extern "C"
BOOL WINAPI ExplorerCompat_Shutdown()
{
    DebugLog(L"[ExplorerCompat][Core] ExplorerCompat_Shutdown called");
    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagExplorer,
        false);

    // Проверяем, был ли уже shutdown
    bool expected = false;
    if (!g_shutdownRequested.compare_exchange_strong(expected, true))
    {
        DebugLog(L"[ExplorerCompat][Core] Shutdown already requested or completed");
        return TRUE; // Идемпотентность
    }

    // Проверяем, была ли инициализация
    if (!g_initialized.load(std::memory_order_acquire))
    {
        DebugLog(L"[ExplorerCompat][Core] Not initialized, shutdown is no-op");
        g_shutdownComplete.store(true, std::memory_order_release);
        return TRUE;
    }

    // XAML Diagnostics: снять Advises и восстановить Fill до прочих detach-колбэков.
    // Выгрузка vs WndCompat: WndCompat_BeginShutdown синхронно снимает только Win32-субклассы/хуки —
    // внешних COM-удержаний нет, поэтому после FreeLibrary refcount падает. Здесь WinUI/FrameworkUdk
    // может удерживать IUnknown на TAP/диагностику после Unadvise — DllCanUnloadNow остаётся S_FALSE
    // до тех пор, пока оболочка не отпустит ссылки; это не баг порядка Shutdown в GlassCompat.
    DebugLog(L"[ExplorerCompat][Core] ExplorerCompat_Shutdown: вызываю StopXamlDiagnostics");
    ExplorerCompat_StopXamlDiagnostics();
    DebugLog(L"[ExplorerCompat][Core] ExplorerCompat_Shutdown: StopXamlDiagnostics вернулся");

    // Выполняем thread-agnostic shutdown
    ExecuteThreadAgnosticShutdown();
    // Позволяет reinject после soft unload, если FreeLibrary не снял образ (IXDE pin).
    g_initialized.store(false, std::memory_order_release);
#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER) && defined(_DEBUG)
    Shared::Log::GlassLogClient::Instance().Stop();
#endif

    return TRUE;
}

// COM-счётчики для residual FreeLibrary в GlassCompat (IXDE LoadLibrary-pin вне free-list).
// bits 0-15: objectCount, bits 16-31: serverLocks, bit 32: shutdownDone.
extern "C" __declspec(dllexport) unsigned long long WINAPI ExplorerCompat_DebugComState() noexcept
{
    const unsigned long long oc =
        static_cast<unsigned long long>(GlassExplorerCom::g_objectCount.load(std::memory_order_acquire)) & 0xFFFFULL;
    const unsigned long long sl =
        static_cast<unsigned long long>(GlassExplorerCom::g_serverLocks.load(std::memory_order_acquire)) & 0xFFFFULL;
    const unsigned long long sd =
        GlassExplorerCom::g_shutdownDone.load(std::memory_order_acquire) ? 1ULL : 0ULL;
    return oc | (sl << 16) | (sd << 32);
}

// === ОСТАВЛЯЕМ инициализацию без изменений (но упрощаем) ===
extern "C"
BOOL WINAPI ExplorerCompat_Initialize()
{
    DebugLog(L"[ExplorerCompat][Core] Initialize: BEGIN");

    // Soft unload может оставить DLL mapped (IXDE residual): g_initialized=true + shutdown done.
    // Без сброса reinject — no-op, а diag/g_hExplorer выглядят «живыми» → AV на Stop (H6).
    if (g_initialized.load(std::memory_order_acquire))
    {
        if (g_shutdownRequested.load(std::memory_order_acquire) ||
            g_shutdownComplete.load(std::memory_order_acquire))
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][Core] Re-init after soft unload (reset shutdown flags)");
            g_shutdownRequested.store(false, std::memory_order_release);
            g_shutdownComplete.store(false, std::memory_order_release);
            g_initialized.store(false, std::memory_order_release);
        }
        else
        {
            DebugLog(L"[ExplorerCompat][Core] Already initialized");
            return TRUE;
        }
    }

    g_guiThreadId.store(GetCurrentThreadId());

    g_initialized.store(true, std::memory_order_release);

    // XAML Diagnostics: запуск событийной модели (WinEvent + bootstrap scan).
    // Инициализация IXDE происходит позже в ExplorerCompat_NotifyDesktopBridgeWindow при создании первого острова.
    const HRESULT hrDiag = ExplorerCompat_StartXamlDiagnostics();
    if (FAILED(hrDiag))
    {
        DebugLog(DBG_ERROR,
            L"[ExplorerCompat][Core] ExplorerCompat_StartXamlDiagnostics failed hr=0x%08X",
            hrDiag);
    }

    // Taskbar IXDE изолирован в отдельной DLL TaskbarCompat.
    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][Core] UWP taskbar IXDE выключен в ExplorerCompat (изолирован в TaskbarCompat)");

    DebugLog(L"[ExplorerCompat][Core] Initialize: END (thread-agnostic shutdown ready)");
    (void)Shared::InjectProcessMap::TrySetDiagFlag(
        GetCurrentProcessId(),
        Shared::InjectProcessMap::DiagExplorer,
        true);
    return TRUE;
}

extern "C"
void WINAPI ExplorerCompat_HandleWinEvent(
    DWORD event,
    HWND hwnd,
    LONG idObject,
    LONG idChild,
    DWORD dwEventThread,
    DWORD dwmsEventTime)
{
    UNREFERENCED_PARAMETER(idChild);
    UNREFERENCED_PARAMETER(dwEventThread);
    UNREFERENCED_PARAMETER(dwmsEventTime);

    if (!hwnd || idObject != OBJID_WINDOW)
        return;
    if (event != EVENT_OBJECT_CREATE)
        return;

    wchar_t cls[256]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0]))) == 0)
        return;
    if (wcscmp(cls, L"CabinetWClass") == 0)
    {
        DebugLog(DBG_NORMAL, L"[ExplorerCompat][WinEvent] CabinetWClass create hwnd=0x%p", hwnd);
        return;
    }

    if (wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0 ||
        wcscmp(cls, L"Microsoft.UI.Content.DesktopWindowHost") == 0)
    {
        ExplorerCompat_NotifyDesktopBridgeWindow(hwnd);
    }
}

extern "C"
void WINAPI ExplorerCompat_OnRootBackdropPolicyChanged(BOOL enabled) noexcept
{
    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][Core] OnRootBackdropPolicyChanged enabled=%d",
        enabled ? 1 : 0);
    if (!enabled)
        g_disableHostBarPatching.store(true, std::memory_order_release);
    else
        g_disableHostBarPatching.store(false, std::memory_order_release);
    XamlShutdownMarshal::RunRootBackdropPolicyChangeOnXamlThread(enabled);
}

extern "C"
void WINAPI ExplorerCompat_PostLiveDisableChromeRedraw() noexcept
{
    XamlTapSite::PostLiveDisableChromeRedraw();
}

// === STUB-функции для обратной совместимости ===

// === STUB-функции для обратной совместимости ===
//extern "C"
//void WINAPI ExplorerCompat_OnExplorerWindowDetected(HWND hwnd)
//{
//    UNREFERENCED_PARAMETER(hwnd);
//    DebugLog(L"[ExplorerCompat][Core] OnExplorerWindowDetected (stub)");
//}
//
//extern "C"
//void WINAPI ExplorerCompat_OnExplorerWindowClosed(HWND hwnd)
//{
//    UNREFERENCED_PARAMETER(hwnd);
//    DebugLog(L"[ExplorerCompat][Core] OnExplorerWindowClosed (stub)");
//}