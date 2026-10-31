#include "CompatPch.h"
#include "GlassCompat.h"
#include <windows.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "dwmapi.lib")
#include <atomic>
#include <string>
#include <format>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>

#include <DebugLog.h>

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
#include <GlassLogClient.h>
#endif
#include <InjectionMode.h>
#include <ProcessBlockList.h>
#include <RootPolicyQuery.h>
#include <Helpers/ThemeHelper.h>
#include <Helpers/SystemHelper.h>
#include "AttachList.h"
#include "CoreWindowHostInject.h"
#include "HostPidHelper.h"
#include <GlassPlatformTask.h>
#include <GlassModulePath.h>
#include <InjectProcessMap.h>
#include <GlassMessages.h>
#include "CrashGuard.h"
#include "ShortLivedAgent.h"
#include <RootBackdropPolicy.h>
#include <GlassUlTrace.h>
#include <GlassControlKind.h>

// Определён в EXPORTS ниже; watcher polling (H54 WPM).
extern "C" __declspec(dllexport) volatile LONG GlassCompat_CwForceUnloadFlag;
extern "C" __declspec(dllexport) volatile DWORD GlassCompat_GuiThreadId;

namespace
{
    // ---------------- Глобальное состояние ----------------
#include <UnloadResult.h>
    HMODULE g_hSelf = nullptr;
    HMODULE g_hCore = nullptr; // GlassCore.dll
    // H64: после Load/Ensure — TRUE только если GlassCore_IsEngineReady.
    // false после SEH-abort: FreeLibrary Core с таймаутом (CRT hang после AV в ucrtbased).
    bool g_coreEngineReady = false;
    HMODULE g_hWndCompat = nullptr; // WndCompat.dll
    HMODULE g_hExplorer = nullptr; // ExplorerCompat.dll (lazy: FileExplorerShellRoot, Win11)
    HMODULE g_hTaskbar = nullptr; // TaskbarCompat.dll (lazy: Tray-attach, Win11)
    HMODULE g_hUICompat = nullptr; // UICompat.dll (lazy: CoreWindow host, Win11)
    // Идемпотентный PostThreadMessage(WM_GLASS_ENSURE_*) до завершения Load.
    std::atomic<bool> g_taskbarEnsurePosted{ false };
    std::atomic<bool> g_explorerEnsurePosted{ false };
    std::atomic<bool> g_uiCompatEnsurePosted{ false };

    [[nodiscard]] bool IsWin11OrNewerBuild() noexcept
    {
        return Shared::SystemHelper::g_buildNumber >= 22000;
    }

    std::atomic<bool> g_initialized{ false };
    std::atomic<bool> g_shutdownRequested{ false };
    std::atomic<bool> g_shutdownComplete{ false };
    // CreateRemoteThread(LoadLibrary) даёт лишний pin; FreeSelf должен снять его отдельно.
    std::atomic<bool> g_remoteLoadLibraryPin{ false };
    // Единый предохранитель: ExecuteShutdown должен стартовать только один раз
    // даже если почти одновременно пришли StopEvent и HostLivenessLost.
    std::atomic<bool> g_shutdownStarted{ false };
    std::atomic<ULONG> g_winEventCallbacksInFlight{ 0 };
    // GLASS-ULTRACE: только наблюдение (H86/H89), на FreeLibrary не влияет.
    std::atomic<unsigned> g_guiGeneration{ 0 };
    bool g_taskbarOwnedPin = false;
    bool g_explorerOwnedPin = false;
    bool g_uiCompatOwnedPin = false;
    const char* g_ultraceShutdownReason = "unknown";

    constexpr size_t kWinEventCallbackTargetCapacity = 64;

    struct WinEventCallbackTarget final
    {
        std::atomic<DWORD> threadId{ 0 };
        std::atomic<HWND> hwnd{ nullptr };
    };

    WinEventCallbackTarget g_winEventCallbackTargets[kWinEventCallbackTargetCapacity]{};

    struct WinEventCallbackProbeGuard final
    {
        WinEventCallbackProbeGuard() noexcept
        {
            g_winEventCallbacksInFlight.fetch_add(1, std::memory_order_acq_rel);
        }

        ~WinEventCallbackProbeGuard()
        {
            g_winEventCallbacksInFlight.fetch_sub(1, std::memory_order_acq_rel);
        }
    };

    void RegisterWinEventCallbackTarget(HWND hwnd, DWORD threadId) noexcept
    {
        for (auto& target : g_winEventCallbackTargets)
        {
            DWORD current = target.threadId.load(std::memory_order_acquire);
            if (current == threadId)
            {
                if (hwnd)
                    target.hwnd.store(hwnd, std::memory_order_release);
                return;
            }
            if (current == 0 &&
                target.threadId.compare_exchange_strong(
                    current, threadId, std::memory_order_acq_rel, std::memory_order_acquire))
            {
                target.hwnd.store(hwnd, std::memory_order_release);
                return;
            }
        }
    }

    [[nodiscard]] ULONGLONG WakeWinEventCallbackThreads() noexcept
    {
        ULONG targetCount = 0;
        ULONG wakeCount = 0;
        const DWORD pid = GetCurrentProcessId();

        for (const auto& target : g_winEventCallbackTargets)
        {
            const DWORD threadId = target.threadId.load(std::memory_order_acquire);
            if (!threadId)
                continue;

            ++targetCount;
            bool woke = false;
            const HWND hwnd = target.hwnd.load(std::memory_order_acquire);
            if (hwnd)
            {
                DWORD windowPid = 0;
                const DWORD windowTid = GetWindowThreadProcessId(hwnd, &windowPid);
                if (windowPid == pid && windowTid == threadId)
                    woke = PostMessageW(hwnd, WM_NULL, 0, 0) != FALSE;
            }
            if (!woke)
                woke = PostThreadMessageW(threadId, WM_NULL, 0, 0) != FALSE;
            if (woke)
                ++wakeCount;
        }

        return
            (static_cast<ULONGLONG>(targetCount) << 32) |
            static_cast<ULONGLONG>(wakeCount);
    }

    // H72: deferred FreeLibrary после Unhook обрабатывается потоками процесса (часто UI),
    // не обязательно GlassGuiThread. SleepEx только на GUI-потоке → FreeSelf при OS-pin
    // (ollama/conhost/igfxEM/NhNotif). HWND_BROADCAST с хоста не будит потоки без HWND.
    // Без DispatchMessage (H68/H69 explorer).
    [[nodiscard]] ULONGLONG WakeAllProcessThreadsWmNull() noexcept
    {
        ULONG seen = 0;
        ULONG posted = 0;
        const DWORD pid = GetCurrentProcessId();
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE)
            return 0;

        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te))
        {
            do
            {
                if (te.th32OwnerProcessID != pid)
                    continue;
                ++seen;
                if (PostThreadMessageW(te.th32ThreadID, WM_NULL, 0, 0))
                    ++posted;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
        return (static_cast<ULONGLONG>(seen) << 32) | static_cast<ULONGLONG>(posted);
    }

    struct SyncNullPumpCtx final
    {
        DWORD pid;
        ULONG windows;
        ULONG sentOk;
    };

    BOOL CALLBACK PumpWindowSyncNullProc(HWND hwnd, LPARAM lp) noexcept
    {
        auto* ctx = reinterpret_cast<SyncNullPumpCtx*>(lp);
        DWORD wndPid = 0;
        (void)GetWindowThreadProcessId(hwnd, &wndPid);
        if (wndPid != ctx->pid)
            return TRUE;

        ++ctx->windows;
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(
                hwnd,
                WM_NULL,
                0,
                0,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                200,
                &result) != 0)
        {
            ++ctx->sentOk;
        }
        return TRUE;
    }

    // path=2 Adobe: deferred Unhook снимает WinEvent только когда UI-поток реально
    // обработал user callback (дамп c80cbb: __ClientCallWinEventProc после FLAET).
    [[nodiscard]] ULONGLONG PumpAllProcessWindowsSyncWmNull() noexcept
    {
        SyncNullPumpCtx ctx{ GetCurrentProcessId(), 0, 0 };
        (void)EnumWindows(PumpWindowSyncNullProc, reinterpret_cast<LPARAM>(&ctx));
        return
            (static_cast<ULONGLONG>(ctx.windows) << 32) |
            static_cast<ULONGLONG>(ctx.sentOk);
    }

    // H72 + path=2: inFlight==0 не значит «безопасно FLAET» — deferred Unhook/user
    // callback (KiUserCallbackDispatcher / __ClientCallWinEventProc) может прийти позже.
    void DrainBeforeFreeSelf() noexcept
    {
        constexpr int kMinRounds = 8;
        constexpr int kMaxRounds = 24;
        const ULONGLONG t0 = GetTickCount64();
        const unsigned rc0 = GlassUlTrace::QueryLoadCountByName(L"GlassCompat.dll");
        const ULONG inflight0 = g_winEventCallbacksInFlight.load(std::memory_order_acquire);
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H82",
            "GlassCompat.cpp:DrainBeforeFreeSelf",
            "drain_begin",
            "\"min\":%d,\"max\":%d,\"inflight\":%lu,\"rc\":%u",
            kMinRounds,
            kMaxRounds,
            static_cast<unsigned long>(inflight0),
            rc0);
        // #endregion
        int done = 0;
        for (int i = 0; i < kMaxRounds; ++i)
        {
            const ULONGLONG wAll = WakeAllProcessThreadsWmNull();
            const ULONGLONG wCb = WakeWinEventCallbackThreads();
            (void)PumpAllProcessWindowsSyncWmNull();
            SleepEx(50, TRUE);
            ++done;
            const ULONG inflight = g_winEventCallbacksInFlight.load(std::memory_order_acquire);
            const unsigned rc = GlassUlTrace::QueryLoadCountByName(L"GlassCompat.dll");
            const unsigned woke =
                static_cast<unsigned>(wAll & 0xFFFFFFFFull)
                + static_cast<unsigned>(wCb & 0xFFFFFFFFull);
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H82",
                "GlassCompat.cpp:DrainBeforeFreeSelf",
                "drain_round",
                "\"round\":%d,\"woke\":%u,\"inflight\":%lu,\"rc\":%u",
                done,
                woke,
                static_cast<unsigned long>(inflight),
                rc);
            // #endregion
            if (i + 1 >= kMinRounds
                && g_winEventCallbacksInFlight.load(std::memory_order_acquire) == 0)
            {
                break;
            }
        }
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H82",
            "GlassCompat.cpp:DrainBeforeFreeSelf",
            "drain_end",
            "\"rounds\":%d,\"max\":%d,\"inflight\":%lu,\"rc\":%u,\"ms\":%llu",
            done,
            kMaxRounds,
            static_cast<unsigned long>(g_winEventCallbacksInFlight.load(std::memory_order_acquire)),
            GlassUlTrace::QueryLoadCountByName(L"GlassCompat.dll"),
            GetTickCount64() - t0);
        // #endregion
    }

    // IXDE делает LoadLibrary(TAP), не CoLoadLibrary — CoFreeUnusedLibraries не снимает pin.
    // Residual FreeLibrary безопасен только после Shutdown + objectCount==0 + shutdownDone.
    [[nodiscard]] bool TryDropIxdeResidualModule(
        HMODULE hModule,
        const char* exportName,
        ULONGLONG& comStateOut) noexcept
    {
        comStateOut = 0xFFFFFFFFFFFFFFFFULL;
        if (!hModule || !exportName)
            return false;

        auto pDbg = reinterpret_cast<unsigned long long(WINAPI*)()>(
            GetProcAddress(hModule, exportName));
        if (!pDbg)
            return false;

        comStateOut = pDbg();
        const unsigned long long objectCount = comStateOut & 0xFFFFULL;
        const unsigned long long serverLocks = (comStateOut >> 16) & 0xFFFFULL;
        const bool shutdownDone = ((comStateOut >> 32) & 1ULL) != 0;
        if (!shutdownDone || objectCount != 0 || serverLocks != 0)
        {
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H83",
                "GlassCompat.cpp:TryDropIxdeResidualModule",
                "module_residual_drop",
                "\"export\":\"%s\",\"ok\":0,\"skipped\":1,\"objectCount\":%llu,\"locks\":%llu,\"shutdownDone\":%d,\"rc\":%u",
                exportName,
                objectCount,
                serverLocks,
                shutdownDone ? 1 : 0,
                GlassUlTrace::QueryLoadCount(hModule));
            // #endregion
            return false;
        }
        const unsigned rcBefore = GlassUlTrace::QueryLoadCount(hModule);
        const BOOL freed = FreeLibrary(hModule);
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H83",
            "GlassCompat.cpp:TryDropIxdeResidualModule",
            "module_residual_drop",
            "\"export\":\"%s\",\"ok\":%d,\"skipped\":0,\"objectCount\":%llu,\"locks\":%llu,\"shutdownDone\":%d,\"rc_before\":%u,\"rc_after\":%u",
            exportName,
            freed ? 1 : 0,
            objectCount,
            serverLocks,
            shutdownDone ? 1 : 0,
            rcBefore,
            GlassUlTrace::QueryLoadCount(hModule));
        // #endregion
        return freed != FALSE;
    }

    // Устанавливается после полного завершения DoInitialize (GlassCore + WndCompat + провайдеры).
    // GlassCompat_WinEventProc не делегирует события в WndCompat до этого момента —
    // иначе субкласс вешается на popup, но провайдеры GlassCore ещё не установлены
    // и MN_SIZEWINDOW / политика тултипов пропускает хуки → первый кадр без кастома.
    // Soft unload сбрасывает флаг; WinEvent постит WM_GLASS_MODULES_REINJECT для повторного DoInitialize.
    std::atomic<bool> g_initDone{ false };
    std::atomic<bool> g_softModulesUnloaded{ false };

    HANDLE g_guiThreadHandle = nullptr;
    DWORD  g_guiThreadId = 0;

    HANDLE g_shutdownEvent = nullptr;

    // Поколение последней обработанной перезагрузки настроек отрисовки (см. kReloadGenerationMappingName).
    std::atomic<DWORD> s_lastHandledReloadGen{ 0 };

    // ---------------- Типы функций из WndCompat / ExplorerCompat ----------------

    using WndCompatInitFn = BOOL(WINAPI*)(DWORD);
    using WndCompatBeginShutdownFn = void (WINAPI*)(void);
    using WndCompatWaitShutdownFn = void (WINAPI*)(void);
    using WndCompatIsShutdownCompleteFn = BOOL(WINAPI*)(void);

    using ExplorerCompatInitFn = bool (*)(HMODULE);
    using ExplorerCompatShutdownFn = void (*)();
    using TaskbarCompatInitFn = BOOL(WINAPI*)(HMODULE);
    using TaskbarCompatShutdownFn = BOOL(WINAPI*)();
    using TaskbarCompatIsShutdownCompleteFn = BOOL(WINAPI*)();
    using UICompatInitFn = BOOL(WINAPI*)(HMODULE);
    using UICompatShutdownFn = BOOL(WINAPI*)();
    using UICompatIsShutdownCompleteFn = BOOL(WINAPI*)();

    using GlassCoreOnThemePaintChangedFn = void (WINAPI*)(void);

    // Функция из WndCompat, вызываемая при каждом WinEvent из GlassCompat_WinEventProc
    using HandleWinEventFn = void(WINAPI*)(DWORD, HWND, LONG, LONG, DWORD, DWORD);

    // Кэшированный адрес WndCompat_HandleWinEvent.
    // Читается многими потоками через shared SRWLOCK.
    // Обнуляется GlassGuiThread через exclusive SRWLOCK перед FreeLibrary(g_hWndCompat).
    static std::atomic<HandleWinEventFn> g_pfnHandleWinEvent{ nullptr };

    // Кэш ExplorerCompat_HandleWinEvent (только explorer.exe).
    static std::atomic<HandleWinEventFn> g_pfnExplorerHandleWinEvent{ nullptr };
    static std::atomic<HandleWinEventFn> g_pfnTaskbarHandleWinEvent{ nullptr };

    // Защищает обращение к g_hWndCompat и g_pfnHandleWinEvent:
    //   shared  — в GlassCompat_WinEventProc (много потоков одновременно)
    //   exclusive — в ExecuteShutdown перед FreeLibrary(g_hWndCompat/g_hExplorer)
    // Также защищает g_hExplorer / g_pfnExplorerHandleWinEvent (гонка с WinEvent).
    static SRWLOCK g_wndCompatLock = SRWLOCK_INIT;

    // ---------------- InjectionMode ----------------

    Shared::InjectionMode g_mode = Shared::InjectionMode::Full;

    // Вперёд-объявление (определение после GetProcessNameLower)
    static Shared::InjectionMode ReadModeFromRegistry();

    // ---------------- Утилиты ----------------

    std::wstring GetModuleDirectory(HMODULE hSelf)
    {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(hSelf, path, MAX_PATH))
            return {};

        std::wstring full = path;
        auto pos = full.find_last_of(L"\\/");
        if (pos != std::wstring::npos)
            full.resize(pos + 1);

        return full;
    }

    [[nodiscard]] std::wstring ResolvePeerDllPath(HMODULE hSelf, const wchar_t* fileName) noexcept
    {
        return GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(hSelf, fileName);
    }

    /// Basename для InjectMap UI — без смены регистра (как Toolhelp после stop).
    std::wstring GetProcessBaseName()
    {
        wchar_t path[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, path, MAX_PATH))
            return {};

        std::wstring full = path;
        auto pos = full.find_last_of(L"\\/");
        return (pos != std::wstring::npos) ? full.substr(pos + 1) : full;
    }

    /// Lowercase basename — только для AttachList / BlockList / сравнений политик.
    std::wstring GetProcessNameLower()
    {
        std::wstring name = GetProcessBaseName();
        if (name.empty())
            return {};

        std::transform(name.begin(), name.end(), name.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

        return name;
    }

    [[nodiscard]] std::uint32_t CurrentModuleBits() noexcept
    {
        using namespace Shared::InjectProcessMap;
        std::uint32_t bits = ModuleCompat;
        if (g_hCore)
            bits |= ModuleCore;
        if (g_hWndCompat)
            bits |= ModuleWnd;
        if (g_hExplorer)
            bits |= ModuleExplorer;
        if (g_hTaskbar)
            bits |= ModuleTaskbar;
        if (g_hUICompat)
            bits |= ModuleUICompat;
        return bits;
    }

    void InjectMapUpsert(
        Shared::InjectProcessMap::Kind kind,
        std::uint8_t path,
        Shared::InjectProcessMap::State state,
        std::uint32_t moduleBits) noexcept
    {
        const std::wstring exe = GetProcessBaseName();
        (void)Shared::InjectProcessMap::TryUpsert(
            GetCurrentProcessId(),
            kind,
            path,
            state,
            moduleBits,
            exe.c_str());
    }

    /// Диаг-флаги пишутся из Core/Explorer Startup до финального Upsert — без слота теряются (H3).
    void RepublishDiagFlagsFromLoadedModules() noexcept
    {
        const DWORD pid = GetCurrentProcessId();
        if (g_hCore)
        {
            (void)Shared::InjectProcessMap::TrySetDiagFlag(
                pid, Shared::InjectProcessMap::DiagHooks, true);
            (void)Shared::InjectProcessMap::TrySetDiagFlag(
                pid, Shared::InjectProcessMap::DiagPaintDispatcher, true);
            (void)Shared::InjectProcessMap::TrySetDiagFlag(
                pid, Shared::InjectProcessMap::DiagModuleHandler, true);
        }
        if (g_hExplorer)
        {
            (void)Shared::InjectProcessMap::TrySetDiagFlag(
                pid, Shared::InjectProcessMap::DiagExplorer, true);
        }
        if (g_hTaskbar)
        {
            (void)Shared::InjectProcessMap::TrySetDiagFlag(
                pid, Shared::InjectProcessMap::DiagTaskbar, true);
        }
        if (g_hUICompat)
        {
            (void)Shared::InjectProcessMap::TrySetDiagFlag(
                pid, Shared::InjectProcessMap::DiagUICompat, true);
        }
        if (GlassCompat::IsProcessCrashGuardInstalled())
        {
            (void)Shared::InjectProcessMap::TrySetDiagFlag(
                pid, Shared::InjectProcessMap::DiagCrashGuard, true);
        }
        // WinEventProc: колбэк GlassCompat_WinEventProc (host SetWinEventHook → INCONTEXT).
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagWinEventProc, true);
        // Loader: модули загружены (Running → зелёный).
        if (g_hCore || g_hWndCompat || g_hSelf)
        {
            (void)Shared::InjectProcessMap::TrySetDiagFlag(
                pid, Shared::InjectProcessMap::DiagLoader, true);
        }
    }

    void ClearDiagFlagsInMap() noexcept
    {
        const DWORD pid = GetCurrentProcessId();
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagHooks, false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagPaintDispatcher, false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagModuleHandler, false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagExplorer, false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagTaskbar, false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagUICompat, false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagWinEventProc, false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagCrashGuard, false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagLoader, false);
    }

    // Читает режим из реестра (AttachList), используя имя текущего exe.
    // Заменяет старый ReadModeForPid, который зависел от GlassPlatform.
    static Shared::InjectionMode ReadModeFromRegistry()
    {
        static std::once_flag s_initFlag;
        std::call_once(s_initFlag, []() { AttachList::Initialize(); });

        std::wstring processName = GetProcessNameLower();
        Shared::InjectionMode mode = AttachList::GetProcessMode(processName);

        DebugLog(L"[GlassCompat][Core] ReadModeFromRegistry: process=%s mode=%u",
            processName.c_str(), static_cast<DWORD>(mode));

        return mode;
    }

    void ReloadAttachListFromRegistry() noexcept
    {
        AttachList::Initialize();
        ProcessBlockList::Reload();

        const std::wstring processName = GetProcessNameLower();
        const Shared::InjectionMode oldMode = g_mode;
        const Shared::InjectionMode newMode = AttachList::GetProcessMode(processName);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCompat][AttachList] Reload: process=%s oldMode=%u newMode=%u",
            processName.c_str(),
            static_cast<DWORD>(oldMode),
            static_cast<DWORD>(newMode));

        if (oldMode == newMode)
            return;

        const bool wasFull = (oldMode == Shared::InjectionMode::Full);
        const bool isFull = (newMode == Shared::InjectionMode::Full);

        const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        const HMODULE hWnd = GetModuleHandleW(L"WndCompat.dll");

        using CoreSetModeFn = void(WINAPI*)(DWORD);
        using WndSetModeFn = void(WINAPI*)(DWORD) noexcept;
        using ApplyVisualsFn = void(WINAPI*)(DWORD, DWORD) noexcept;

        const auto pCoreSetMode = hCore
            ? reinterpret_cast<CoreSetModeFn>(GetProcAddress(hCore, "GlassCore_SetInjectionMode"))
            : nullptr;
        const auto pWndSetMode = hWnd
            ? reinterpret_cast<WndSetModeFn>(GetProcAddress(hWnd, "WndCompat_SetInjectionMode"))
            : nullptr;
        const auto pApplyVisuals = hWnd
            ? reinterpret_cast<ApplyVisualsFn>(
                  GetProcAddress(hWnd, "WndCompat_ApplyInjectionModeVisuals"))
            : nullptr;

        g_mode = newMode;

        // Сначала mode, затем единый visual sweep (Full-gate локально в Core/Wnd).
        if (pCoreSetMode)
            pCoreSetMode(static_cast<DWORD>(newMode));
        if (pWndSetMode)
            pWndSetMode(static_cast<DWORD>(newMode));

        if (pApplyVisuals && (wasFull != isFull))
            pApplyVisuals(static_cast<DWORD>(oldMode), static_cast<DWORD>(newMode));
    }

    bool FileExists(const std::wstring& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    HMODULE LoadOptionalModule(const std::wstring& fullPath, const wchar_t* tag)
    {
        if (!FileExists(fullPath))
        {
            DebugLog(std::format(
                L"[GlassCompat][Core] {}: file not found: {}",
                tag, fullPath
            ).c_str());
            return nullptr;
        }

        DebugLog(std::format(
            L"[GlassCompat][Core] {}: LoadLibrary: {}",
            tag, fullPath
        ).c_str());

        HMODULE h = LoadLibraryW(fullPath.c_str());
        if (!h)
        {
            DebugLog(std::format(
                L"[GlassCompat][Core] {}: LoadLibrary FAILED (err={}): {}",
                tag, GetLastError(), fullPath
            ).c_str());
            return nullptr;
        }

        DebugLog(std::format(
            L"[GlassCompat][Core] {}: loaded at 0x{:X}",
            tag, reinterpret_cast<uintptr_t>(h)
        ).c_str());

        return h;
    }

    // Отдельная функция: __try нельзя в ExecuteShutdown/DoInitialize (C++ unwind).
    // H54: AV/сбой внутри GlassCore_Shutdown не должен сорвать FreeLibrary/FreeSelf Compat.
    void CallGlassCoreShutdown(void (*pShutdown)()) noexcept
    {
        if (!pShutdown)
            return;

        __try
        {
            pShutdown();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DebugLog(
                L"[GlassCompat][Core] GlassCore_Shutdown SEH code=0x%08X — continue unload",
                GetExceptionCode());
        }
    }

    // H63: тяжёлый init вне DllMain — после успешного LoadLibrary.
    void CallGlassCoreEnsureEngine(void (*pEnsure)()) noexcept
    {
        if (!pEnsure)
            return;

        __try
        {
            pEnsure();
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            DebugLog(
                L"[GlassCompat][Core] GlassCore_HostUiEnsureEngine SEH code=0x%08X — keep owned g_hCore",
                GetExceptionCode());
        }
    }

    [[nodiscard]] HMODULE FindModuleViaToolhelp(
        const wchar_t* moduleFileName,
        wchar_t* outPath,
        size_t outPathChars) noexcept
    {
        if (!moduleFileName || !*moduleFileName)
            return nullptr;

        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE)
            return nullptr;

        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        HMODULE found = nullptr;
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (_wcsicmp(me.szModule, moduleFileName) == 0)
                {
                    found = me.hModule;
                    if (outPath && outPathChars > 0)
                        wcsncpy_s(outPath, outPathChars, me.szExePath, _TRUNCATE);
                    break;
                }
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
        return found;
    }

    [[nodiscard]] HMODULE FindModuleViaToolhelp(const wchar_t* moduleFileName) noexcept
    {
        return FindModuleViaToolhelp(moduleFileName, nullptr, 0);
    }

    [[nodiscard]] ULONGLONG GlassToolhelpBits() noexcept
    {
        ULONGLONG bits = 0;
        if (FindModuleViaToolhelp(L"GlassCompat.dll"))
            bits |= 1;
        if (FindModuleViaToolhelp(L"GlassCore.dll"))
            bits |= 2;
        if (FindModuleViaToolhelp(L"WndCompat.dll"))
            bits |= 4;
        if (FindModuleViaToolhelp(L"ExplorerCompat.dll"))
            bits |= 8;
        return bits;
    }

    enum class HostLivenessReason : uint8_t
    {
        Alive = 0,
        PidMissing,
        OpenProcessFailed,
        ProcessSignaled,
        WaitFailed,
        ExitCodeReadFailed,
        ExitCodeNotActive,
        // Мьютекс GlassPlatform.HostAlive достался нам — хост его больше не держит.
        HostAliveReleased,
    };

    static const wchar_t* HostLivenessReasonToString(HostLivenessReason reason) noexcept
    {
        switch (reason)
        {
        case HostLivenessReason::Alive: return L"Alive";
        case HostLivenessReason::PidMissing: return L"PidMissing";
        case HostLivenessReason::OpenProcessFailed: return L"OpenProcessFailed";
        case HostLivenessReason::ProcessSignaled: return L"ProcessSignaled";
        case HostLivenessReason::WaitFailed: return L"WaitFailed";
        case HostLivenessReason::ExitCodeReadFailed: return L"ExitCodeReadFailed";
        case HostLivenessReason::ExitCodeNotActive: return L"ExitCodeNotActive";
        case HostLivenessReason::HostAliveReleased: return L"HostAliveReleased";
        default: return L"Unknown";
        }
    }

    // Проверка живости хоста для инжектированного процесса.
    // Возвращает true, если хост гарантированно жив; false — если считаем, что хост умер.
    static bool CheckHostAlive(
        DWORD& cachedPid,
        HANDLE& hostProc,
        HostLivenessReason& reasonOut) noexcept
    {
        reasonOut = HostLivenessReason::Alive;

        const DWORD pid = ReadHostPid();
        if (pid == 0)
        {
            reasonOut = HostLivenessReason::PidMissing;
            return false;
        }

        if (pid != cachedPid || !hostProc)
        {
            if (pid != cachedPid)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCompat][HostWatch] pid changed: old=%lu new=%lu",
                    static_cast<unsigned long>(cachedPid),
                    static_cast<unsigned long>(pid));
            }

            if (hostProc)
            {
                CloseHandle(hostProc);
                hostProc = nullptr;
            }
            hostProc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            cachedPid = pid;
            if (!hostProc)
            {
                reasonOut = HostLivenessReason::OpenProcessFailed;
                return false;
            }
        }

        const DWORD wait = WaitForSingleObject(hostProc, 0);
        if (wait == WAIT_OBJECT_0)
        {
            reasonOut = HostLivenessReason::ProcessSignaled;
            return false;
        }
        if (wait == WAIT_FAILED)
        {
            reasonOut = HostLivenessReason::WaitFailed;
            return false;
        }

        DWORD exitCode = 0;
        if (!GetExitCodeProcess(hostProc, &exitCode))
        {
            reasonOut = HostLivenessReason::ExitCodeReadFailed;
            return false;
        }
        if (exitCode != STILL_ACTIVE)
        {
            reasonOut = HostLivenessReason::ExitCodeNotActive;
            return false;
        }

        return true;
    }

    // ---------------- Ядро shutdown (выполняется в GUI‑потоке) ----------------

    static void ReportUnloadResult(DWORD pid, UnloadResult result)
    {
        (void)Shared::InjectProcessMap::TrySetUnloadResult(
            pid, static_cast<std::uint32_t>(result));

        wchar_t mapName[64], evtName[64];
        swprintf_s(mapName, L"Local\\GlassCompatResult_%u", pid);
        swprintf_s(evtName, L"Local\\GlassCompatDone_%u", pid);

        HANDLE hMap = OpenFileMappingW(FILE_MAP_WRITE, FALSE, mapName);
        if (hMap)
        {
            void* p = MapViewOfFile(hMap, FILE_MAP_WRITE, 0, 0, sizeof(uint32_t));
            if (p)
            {
                *static_cast<uint32_t*>(p) = static_cast<uint32_t>(result);
                UnmapViewOfFile(p);
            }
            CloseHandle(hMap);
        }
        else
        {
            // err=2 (NOT_FOUND) — ручной тест без платформы, нормально.
            // err=5 (ACCESS_DENIED) — несовпадение сессии или DACL.
            DebugLog(DBG_WARNING,
                L"[GlassCompat][Core] ReportUnloadResult: OpenFileMappingW failed (err=%u)",
                GetLastError());
        }

        HANDLE hEvt = OpenEventW(EVENT_MODIFY_STATE, FALSE, evtName);
        if (hEvt)
        {
            SetEvent(hEvt);
            CloseHandle(hEvt);
        }
        else
        {
            DebugLog(DBG_WARNING,
                L"[GlassCompat][Core] ReportUnloadResult: OpenEventW failed (err=%u)",
                GetLastError());
        }
    }

    // S4: зачистка живых окон наших HWND_MESSAGE-классов перед FreeLibrary.
    // Use-after-unload (stale WndProc → CFG fail, explorer.exe.10148.dmp):
    // каждое найденное окно — best-effort destroy.
    // Вызывать ДО FreeLibrary соответствующего модуля, пока его код на месте.
    void AuditGlassMessageWindowsBeforeFree(
        const wchar_t* modTag,
        HMODULE hMod,
        const wchar_t* const* classes,
        size_t classCount) noexcept
    {
        if (!modTag || !classes || classCount == 0)
            return;
        const DWORD curTid = GetCurrentThreadId();
        for (size_t ci = 0; ci < classCount; ++ci)
        {
            const wchar_t* cls = classes[ci];
            if (!cls || !cls[0])
                continue;
            int stuckTotal = 0;
            HWND prev = nullptr;
            for (int i = 0; i < 128; ++i)
            {
                // Итерация через hwndChildAfter — иначе вечный цикл на первом окне.
                const HWND h = FindWindowExW(HWND_MESSAGE, prev, cls, nullptr);
                if (!h || !IsWindow(h))
                    break;
                DWORD wndTid = 0;
                GetWindowThreadProcessId(h, &wndTid);
                if (wndTid != 0 && wndTid == curTid)
                {
                    (void)DestroyWindow(h);
                }
                else
                {
                    // Чужой поток: WM_CLOSE → DefWindowProc → DestroyWindow на владельце.
                    DWORD_PTR result = 0;
                    (void)SendMessageTimeoutW(h, WM_CLOSE, 0, 0,
                        SMTO_ABORTIFHUNG, 2000, &result);
                }
                if (IsWindow(h))
                {
                    // Не сносится (поток висит/игнорирует WM_CLOSE): идём дальше,
                    // после 3 таких — стоп.
                    prev = h; // шаг вперёд мимо несносимого
                    if (++stuckTotal >= 3)
                        break;
                }
                else
                {
                    prev = nullptr; // список сдвинулся — сначала
                }
            }
            // Примечание: Unregister оставшихся классов — владелец DLL Stop, не здесь.
            (void)hMod;
        }
    }

    // #region agent log
    // GLASS-ULTRACE (R2b, H91): перед FreeLibrary ищем окна, чей обработчик указывает внутрь
    // выгружаемого модуля. Важно: один только GWLP_WNDPROC ничего не показывает, если субкласс
    // ставился через SetWindowSubclass — тогда GWLP_WNDPROC = thunk comctl32, а наши проки лежат
    // в цепочке comctl32 (property "UxSubclassInfo"). Дамп explorer 39284: CFG fail-fast,
    // цель = WndCompat+0x194240, стек через comctl32 thunk, но proc_inside=0 — ложный «чисто».
    struct ModuleRangeUl
    {
        std::uintptr_t base{};
        std::uintptr_t end{};
        wchar_t name[MAX_PATH]{};
    };

    constexpr unsigned kMaxModuleRanges = 512;
    ModuleRangeUl g_moduleRangesUl[kMaxModuleRanges]{};
    unsigned g_moduleRangeCountUl = 0;

    void RefreshModuleRangesUl() noexcept
    {
        g_moduleRangeCountUl = 0;
        HANDLE snap = CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE)
            return;
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);
        if (Module32FirstW(snap, &me))
        {
            do
            {
                if (g_moduleRangeCountUl >= kMaxModuleRanges)
                    break;
                auto& r = g_moduleRangesUl[g_moduleRangeCountUl++];
                r.base = reinterpret_cast<std::uintptr_t>(me.hModule);
                r.end = r.base + me.modBaseSize;
                wcsncpy_s(r.name, me.szModule, _TRUNCATE);
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);
    }

    [[nodiscard]] const ModuleRangeUl* FindModuleRangeUl(const void* proc) noexcept
    {
        const auto a = reinterpret_cast<std::uintptr_t>(proc);
        for (unsigned i = 0; i < g_moduleRangeCountUl; ++i)
        {
            if (a >= g_moduleRangesUl[i].base && a < g_moduleRangesUl[i].end)
                return &g_moduleRangesUl[i];
        }
        return nullptr;
    }

    // Безопасное чтение чужой (comctl32) структуры: только committed-память, под SEH.
    bool TryReadBlobUl(const void* p, size_t bytes, unsigned long long* out, unsigned maxOut, unsigned& outCount) noexcept
    {
        outCount = 0;
        if (!p || !bytes || !out || !maxOut)
            return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
            return false;
        if ((mbi.Protect & PAGE_NOACCESS) || (mbi.Protect & PAGE_GUARD))
            return false;
        const auto regionEnd =
            reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const auto start = reinterpret_cast<std::uintptr_t>(p);
        size_t avail = regionEnd > start ? static_cast<size_t>(regionEnd - start) : 0;
        size_t n = (bytes < avail ? bytes : avail) / sizeof(unsigned long long);
        if (n > maxOut)
            n = maxOut;
        if (!n)
            return false;
        __try
        {
            memcpy(out, p, n * sizeof(unsigned long long));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
        outCount = static_cast<unsigned>(n);
        return true;
    }

    struct StaleCallbackScan
    {
        DWORD pid{};
        std::uintptr_t begin{};
        std::uintptr_t end{};
        unsigned windows{};
        unsigned marked{};
        unsigned procInside{};   // GWLP_WNDPROC внутри модуля
        unsigned propInside{};   // проки в цепочке comctl32 внутри модуля
        unsigned markedLogged{};
        unsigned details{};
        const char* tag{""};
    };

    void LogMarkedWindowUl(StaleCallbackScan& s, HWND hwnd, GlassShared::GlassControlKind kind,
        LONG_PTR proc, const HANDLE subclassProp) noexcept
    {
        wchar_t cls[96]{};
        (void)GetClassNameW(hwnd, cls, _countof(cls));
        char clsJson[220]{};
        GlassUlTrace::WideToJson(clsJson, sizeof(clsJson), cls);
        const ModuleRangeUl* procMod = FindModuleRangeUl(reinterpret_cast<const void*>(proc));
        char procModJson[120]{};
        GlassUlTrace::WideToJson(procModJson, sizeof(procModJson), procMod ? procMod->name : L"");
        GlassUlTrace::Logf(
            "H91",
            "GlassCompat.cpp:ScanStaleCallbacks",
            "marked_window",
            "\"module\":\"%s\",\"hwnd\":\"%p\",\"class\":\"%s\",\"kind\":%u,"
            "\"proc_mod\":\"%s\",\"proc_rva\":\"0x%llX\",\"prop\":%d",
            s.tag,
            static_cast<void*>(hwnd),
            clsJson,
            static_cast<unsigned>(kind),
            procModJson,
            procMod ? static_cast<unsigned long long>(static_cast<std::uintptr_t>(proc) - procMod->base) : 0ull,
            subclassProp ? 1 : 0);
    }

    void NoteWindowCandidate(StaleCallbackScan& s, HWND hwnd) noexcept
    {
        DWORD wndPid = 0;
        (void)GetWindowThreadProcessId(hwnd, &wndPid);
        if (wndPid != s.pid)
            return;
        ++s.windows;

        const LONG_PTR procRaw = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
        const auto proc = static_cast<std::uintptr_t>(procRaw);

        // Цепочка субклассов comctl32 (SetWindowSubclass): property = указатель на структуру
        // с {uIdSubclass, pfnSubclass, dwRefData}[]. Точный layout версионный, поэтому
        // ищем не по полям, а по значениям внутри диапазона модуля.
        const HANDLE subclassProp = GetPropW(hwnd, L"UxSubclassInfo");
        unsigned propHits = 0;
        if (subclassProp)
        {
            unsigned long long words[48]{};
            unsigned got = 0;
            if (TryReadBlobUl(subclassProp, sizeof(words), words, _countof(words), got))
            {
                for (unsigned i = 0; i < got; ++i)
                {
                    if (words[i] >= s.begin && words[i] < s.end)
                        ++propHits;
                }
            }
        }

        const auto kind = GlassShared::GlassGetWindowKind(hwnd);
        if (kind != GlassShared::GlassControlKind::None)
        {
            ++s.marked;
            if (s.markedLogged < 16)
            {
                ++s.markedLogged;
                LogMarkedWindowUl(s, hwnd, kind, procRaw, subclassProp);
            }
        }

        const bool gwlpInside = (proc >= s.begin && proc < s.end);
        if (gwlpInside)
            ++s.procInside;
        if (propHits)
            s.propInside += propHits;

        if (!gwlpInside && !propHits)
            return;
        if (s.details >= 12)
            return;
        ++s.details;
        wchar_t cls[96]{};
        (void)GetClassNameW(hwnd, cls, _countof(cls));
        char clsJson[200]{};
        GlassUlTrace::WideToJson(clsJson, sizeof(clsJson), cls);
        GlassUlTrace::Logf(
            "H91",
            "GlassCompat.cpp:ScanStaleCallbacks",
            "window_proc_in_module",
            "\"module\":\"%s\",\"hwnd\":\"%p\",\"class\":\"%s\",\"gwlp_inside\":%d,"
            "\"prop_hits\":%u,\"kind\":%u",
            s.tag,
            static_cast<void*>(hwnd),
            clsJson,
            gwlpInside ? 1 : 0,
            propHits,
            static_cast<unsigned>(kind));
    }

    BOOL CALLBACK ScanWindowProcCb(HWND hwnd, LPARAM lp) noexcept
    {
        auto* s = reinterpret_cast<StaleCallbackScan*>(lp);
        NoteWindowCandidate(*s, hwnd);
        (void)EnumChildWindows(hwnd, ScanWindowProcCb, lp);
        return TRUE;
    }

    BOOL CALLBACK ScanThreadWindowCb(HWND hwnd, LPARAM lp) noexcept
    {
        NoteWindowCandidate(*reinterpret_cast<StaleCallbackScan*>(lp), hwnd);
        return TRUE;
    }

    // Возвращает число найденных «живых» указателей на код модуля (GWLP + comctl32-цепочка).
    unsigned ScanStaleCallbacksBeforeFree(const char* tag, HMODULE hMod) noexcept
    {
        if (!tag || !hMod)
            return 0;
        RefreshModuleRangesUl();

        DWORD imageSize = 0;
        for (unsigned i = 0; i < g_moduleRangeCountUl; ++i)
        {
            if (g_moduleRangesUl[i].base == reinterpret_cast<std::uintptr_t>(hMod))
            {
                imageSize = static_cast<DWORD>(g_moduleRangesUl[i].end - g_moduleRangesUl[i].base);
                break;
            }
        }
        if (!imageSize)
            return 0;

        StaleCallbackScan s;
        s.pid = GetCurrentProcessId();
        s.begin = reinterpret_cast<std::uintptr_t>(hMod);
        s.end = s.begin + imageSize;
        s.tag = tag;

        (void)EnumWindows(ScanWindowProcCb, reinterpret_cast<LPARAM>(&s));

        // Message-only и окна не-GUI потоков EnumWindows не отдаёт.
        HANDLE threads = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (threads != INVALID_HANDLE_VALUE)
        {
            THREADENTRY32 te{};
            te.dwSize = sizeof(te);
            if (Thread32First(threads, &te))
            {
                do
                {
                    if (te.th32OwnerProcessID != s.pid)
                        continue;
                    (void)EnumThreadWindows(
                        te.th32ThreadID, ScanThreadWindowCb, reinterpret_cast<LPARAM>(&s));
                } while (Thread32Next(threads, &te));
            }
            CloseHandle(threads);
        }

        GlassUlTrace::Logf(
            "H91",
            "GlassCompat.cpp:ScanStaleCallbacks",
            "stale_scan",
            "\"module\":\"%s\",\"windows\":%u,\"marked\":%u,\"proc_inside\":%u,"
            "\"prop_inside\":%u,\"size\":\"0x%X\"",
            tag,
            s.windows,
            s.marked,
            s.procInside,
            s.propInside,
            imageSize);
        return s.procInside + s.propInside;
    }
    // #endregion

    // HWND_MESSAGE-классы наших DLL (источники stale WndProc при выгрузке).
    static const wchar_t* const kExplorerStopClasses[] = {
        ExplorerCompatMsg::kExplorerXamlShutdownWndClass,
        ExplorerCompatMsg::kExplorerIxdeComReclaimWndClass,
        ExplorerCompatMsg::kExplorerXamlUwpShutdownWndClass,
    };
    static const wchar_t* const kTaskbarStopClasses[] = {
        TaskbarCompatMsg::kTaskbarXamlShutdownWndClass,
    };
    static const wchar_t* const kUiStopClasses[] = {
        UICompatMsg::kUiXamlShutdownWndClass,
    };
    static const wchar_t* const kWndStopClasses[] = {
        L"WndCompat.ControlWindow",
    };

    void ExecuteShutdown()
    {
        g_initDone.store(false, std::memory_order_release);

        DWORD pid = GetCurrentProcessId();
        const ULONGLONG shutdownT0 = GetTickCount64();
        const char* const shutdownReason = g_ultraceShutdownReason;
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H88",
            "GlassCompat.cpp:ExecuteShutdown",
            "shutdown_begin",
            "\"reason\":\"%s\",\"phase\":\"begin\"",
            shutdownReason);
        // #endregion
        GlassUlTrace::LogTraceInitOnce();
        DebugLog(L"[GlassCompat][Core] ExecuteShutdown: BEGIN");
        (void)Shared::InjectProcessMap::TrySetState(
            pid, Shared::InjectProcessMap::State::Unloading);

        // Loader: начало выгрузки модулей (clear diagFlag → серый через isProcessStopping).
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            pid, Shared::InjectProcessMap::DiagLoader, false);

        std::wstring processName = GetProcessNameLower();
        UnloadResult result = UnloadResult::Success;
        const bool isExplorer = (processName == L"explorer.exe");
        ULONGLONG forcedUnloadMask = 0;

        // STOP S0: passthrough explorerframe ExtendFrame/DWM до XAML CHROME_REDRAW (glass ON, policy ещё true).
        if (isExplorer && g_hCore)
        {
            using BeginChromeRestoreFn = void(WINAPI*)() noexcept;
            const auto pfnBeginRestore = reinterpret_cast<BeginChromeRestoreFn>(
                GetProcAddress(g_hCore, "GlassCore_BeginExplorerShellChromeRestore"));
            if (pfnBeginRestore)
            {
                DebugLog(L"[GlassCompat][Core] [Shutdown] Calling GlassCore_BeginExplorerShellChromeRestore");
                pfnBeginRestore();
            }
        }

        // 0. UICompat (CoreWindow hosts) — Unadvise до остального XAML.
        // Soft/remote Ensure мог загрузить UICompat без удержания g_hUICompat.
        HMODULE hUiShutdown = g_hUICompat ? g_hUICompat : GetModuleHandleW(L"UICompat.dll");
        if (hUiShutdown)
        {
            auto pUiShutdown = reinterpret_cast<UICompatShutdownFn>(
                GetProcAddress(hUiShutdown, "UICompat_Shutdown"));
            auto pUiIsShutdownComplete = reinterpret_cast<UICompatIsShutdownCompleteFn>(
                GetProcAddress(hUiShutdown, "UICompat_IsShutdownComplete"));
            if (pUiShutdown)
            {
                (void)Shared::InjectProcessMap::TrySetShutdownPhase(
                    GetCurrentProcessId(),
                    Shared::InjectProcessMap::ShutdownPhaseUiDetaching);
                DebugLog(L"[GlassCompat][Core] [Shutdown] Calling UICompat_Shutdown");
                (void)pUiShutdown();
                int waitedMs = 0;
                int complete = 0;
                if (pUiIsShutdownComplete)
                {
                    constexpr int kUiShutdownWaitMs = 1500;
                    while (waitedMs < kUiShutdownWaitMs)
                    {
                        if (pUiIsShutdownComplete())
                            break;
                        Sleep(50);
                        waitedMs += 50;
                    }
                    complete = pUiIsShutdownComplete() ? 1 : 0;
                }
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H83",
                    "GlassCompat.cpp:ExecuteShutdown",
                    "module_shutdown",
                    "\"module\":\"UICompat\",\"called\":1,\"complete\":%d,\"waited_ms\":%d",
                    complete,
                    waitedMs);
                // #endregion
                (void)Shared::InjectProcessMap::TrySetShutdownPhase(
                    GetCurrentProcessId(),
                    Shared::InjectProcessMap::ShutdownPhaseUiDone);
            }
        }

        // 1. TaskbarCompat до ExplorerCompat: Unadvise/RestoreAll на STA трея до RedrawWindow(Shell_TrayWnd)
        // в ExplorerCompat_Shutdown (runtime: BgRegistry/Reassert на tid трея + paint после Core-off).
        if (isExplorer && g_hTaskbar)
        {
            auto pTaskbarShutdown = reinterpret_cast<TaskbarCompatShutdownFn>(
                GetProcAddress(g_hTaskbar, "TaskbarCompat_Shutdown"));
            auto pTaskbarIsShutdownComplete = reinterpret_cast<TaskbarCompatIsShutdownCompleteFn>(
                GetProcAddress(g_hTaskbar, "TaskbarCompat_IsShutdownComplete"));
            if (pTaskbarShutdown)
            {
                DebugLog(L"[GlassCompat][Core] [Shutdown] Calling TaskbarCompat_Shutdown (before ExplorerCompat)");
                (void)pTaskbarShutdown();
                int waitedMs = 0;
                int complete = 0;
                if (pTaskbarIsShutdownComplete)
                {
                    constexpr int kTaskbarShutdownWaitMs = 1500;
                    while (waitedMs < kTaskbarShutdownWaitMs)
                    {
                        if (pTaskbarIsShutdownComplete())
                            break;
                        Sleep(50);
                        waitedMs += 50;
                    }
                    complete = pTaskbarIsShutdownComplete() ? 1 : 0;
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[GlassCompat][Core] [Shutdown] TaskbarCompat wait: waitedMs=%d complete=%d",
                        waitedMs,
                        complete);
                }
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H86",
                    "GlassCompat.cpp:ExecuteShutdown",
                    "module_shutdown",
                    "\"module\":\"TaskbarCompat\",\"called\":1,\"complete\":%d,\"waited_ms\":%d",
                    complete,
                    waitedMs);
                // #endregion
            }
        }

        // 2. ExplorerCompat (XAML Restore/Unadvise на STA до снятия Core-хуков).
        if (isExplorer && g_hExplorer)
        {
            auto pExplorerShutdown = reinterpret_cast<ExplorerCompatShutdownFn>(
                GetProcAddress(g_hExplorer, "ExplorerCompat_Shutdown"));

            if (pExplorerShutdown)
            {
                result |= UnloadResult::ExplorerShutdown_Called;
                DebugLog(L"[GlassCompat][Core] [Shutdown] Calling ExplorerCompat_Shutdown (before Core)");
                pExplorerShutdown();
                result |= UnloadResult::ExplorerShutdown_Returned;
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H86",
                    "GlassCompat.cpp:ExecuteShutdown",
                    "module_shutdown",
                    "\"module\":\"ExplorerCompat\",\"called\":1,\"complete\":1,\"waited_ms\":0");
                // #endregion
            }
        }
        else
        {
            result |= UnloadResult::ExplorerShutdown_Skipped;
        }

        // 3. GlassCore_Shutdown — только g_hCore / GetModuleHandle.
        // HWiNFO zombie (LoadLibrary fail + Toolhelp): НЕ брать Toolhelp-base и НЕ FreeLibrary —
        // H60/H61: unmap без полного teardown → AV в <unknown> / hang (доказано runtime).
        {
            HMODULE hCoreForShutdown = g_hCore ? g_hCore : GetModuleHandleW(L"GlassCore.dll");

            if (hCoreForShutdown)
            {
                auto pCoreShutdown = reinterpret_cast<void(*)()>(
                    GetProcAddress(hCoreForShutdown, "GlassCore_Shutdown"));

                if (pCoreShutdown)
                {
                    result |= UnloadResult::CoreShutdown_Called;
                    (void)Shared::InjectProcessMap::TrySetShutdownPhase(
                        GetCurrentProcessId(),
                        Shared::InjectProcessMap::ShutdownPhaseCoreDetaching);
                    DebugLog(L"[GlassCompat][Core] [Shutdown] Calling GlassCore_Shutdown");
                    CallGlassCoreShutdown(pCoreShutdown);
                    (void)Shared::InjectProcessMap::TrySetShutdownPhase(
                        GetCurrentProcessId(),
                        Shared::InjectProcessMap::ShutdownPhaseCoreDone);
                    result |= UnloadResult::CoreShutdown_Returned;
                    // #region agent log
                    // GLASS-ULTRACE
                    GlassUlTrace::Logf(
                        "H88",
                        "GlassCompat.cpp:ExecuteShutdown",
                        "module_shutdown",
                        "\"module\":\"GlassCore\",\"called\":1,\"complete\":1,\"waited_ms\":0");
                    // #endregion
                }
                else
                {
                    result |= UnloadResult::CoreShutdown_NotFound;
                    DebugLog(L"[GlassCompat][Core] [Shutdown] GlassCore_Shutdown not found");
                }
            }
            else
            {
                result |= UnloadResult::CoreShutdown_NotLoaded;
                DebugLog(L"[GlassCompat][Core] [Shutdown] GlassCore not loaded, skipping shutdown");
            }
        }

        // 3. WndCompat_BeginShutdown (синхронный, внутри таймаут 2с)
        // После GlassCore_Shutdown: при Root_DetachAll и RedrawWindow хуки Core уже сняты.
        if (g_hWndCompat)
        {
            auto pWndShutdown = reinterpret_cast<WndCompatBeginShutdownFn>(
                GetProcAddress(g_hWndCompat, "WndCompat_BeginShutdown"));

            if (pWndShutdown)
            {
                result |= UnloadResult::BeginWnd_Called;
                (void)Shared::InjectProcessMap::TrySetShutdownPhase(
                    GetCurrentProcessId(),
                    Shared::InjectProcessMap::ShutdownPhaseWndDetaching);
                DebugLog(L"[GlassCompat][Core] [Shutdown] Calling WndCompat_BeginShutdown");
                pWndShutdown();
                (void)Shared::InjectProcessMap::TrySetShutdownPhase(
                    GetCurrentProcessId(),
                    Shared::InjectProcessMap::ShutdownPhaseWndDone);
                result |= UnloadResult::BeginWnd_Returned;
                DebugLog(L"[GlassCompat][Core] [Shutdown] WndCompat_BeginShutdown returned");
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H88",
                    "GlassCompat.cpp:ExecuteShutdown",
                    "module_shutdown",
                    "\"module\":\"WndCompat\",\"called\":1,\"complete\":1,\"waited_ms\":0");
                // #endregion
            }
            else
            {
                result |= UnloadResult::BeginWnd_NotFound;
                DebugLog(L"[GlassCompat][Core] [Shutdown] WndCompat_BeginShutdown not found");
            }
        }
        else
        {
            result |= UnloadResult::BeginWnd_NotLoaded;
            DebugLog(L"[GlassCompat][Core] [Shutdown] WndCompat not loaded, skipping BeginShutdown");
        }

        // 4. FreeLibrary GlassCore — только owned LoadLibrary / GetModuleHandle (не Toolhelp-zombie).
        {
            HMODULE hCoreFree = g_hCore;
            g_hCore = nullptr;
            if (!hCoreFree)
                hCoreFree = GetModuleHandleW(L"GlassCore.dll");

            const bool engineReady = g_coreEngineReady;
            g_coreEngineReady = false;

            unsigned freed = 0;
            if (hCoreFree && !engineReady)
            {
                // H64/H66: после init-AV FreeLibrary(Core) на Stop зависает / UAF (H65 early free).
                // Residual Core; Compat/Wnd выгружаются. Не worker (loader lock).
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCompat][Core] [Shutdown] skip FreeLibrary GlassCore (init failed / not ready)");
                hCoreFree = nullptr;
            }

            if (hCoreFree)
            {
                result |= UnloadResult::FreeCore_Called;
                constexpr unsigned kMaxCoreFreeAttempts = 4;
                for (; freed < kMaxCoreFreeAttempts;)
                {
                    DebugLog(
                        L"[GlassCompat][Core] [Shutdown] FreeLibrary GlassCore (attempt=%u)",
                        freed + 1);
                    const unsigned rcCoreBefore = GlassUlTrace::QueryLoadCount(hCoreFree);
                    const BOOL coreFreed = FreeLibrary(hCoreFree);
                    ++freed;
                    // #region agent log
                    // GLASS-ULTRACE
                    GlassUlTrace::Logf(
                        "H88",
                        "GlassCompat.cpp:ExecuteShutdown",
                        "module_free",
                        "\"module\":\"GlassCore\",\"base\":\"%p\",\"ok\":%d,\"owned\":1,\"attempt\":%u,\"rc_before\":%u,\"rc_after\":%u,\"gmh\":%d,\"toolhelp\":%d",
                        static_cast<void*>(hCoreFree),
                        coreFreed ? 1 : 0,
                        freed,
                        rcCoreBefore,
                        GlassUlTrace::QueryLoadCountByName(L"GlassCore.dll"),
                        GlassUlTrace::ModulePresentGmh(L"GlassCore.dll") ? 1 : 0,
                        GlassUlTrace::ModulePresentToolhelp(L"GlassCore.dll") ? 1 : 0);
                    // #endregion
                    result |= UnloadResult::FreeCore_Returned;

                    hCoreFree = GetModuleHandleW(L"GlassCore.dll");
                    if (!hCoreFree)
                        break;
                    Sleep(20);
                }
            }

            const bool stillGmh = GetModuleHandleW(L"GlassCore.dll") != nullptr;
            const bool stillToolhelp = FindModuleViaToolhelp(L"GlassCore.dll") != nullptr;
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCompat][Core] [Shutdown] GlassCore after FreeLibrary: gmh=%s toolhelp=%s "
                L"(freed=%u ready=%d)",
                stillGmh ? L"LOADED" : L"gone",
                stillToolhelp ? L"LOADED" : L"gone",
                freed,
                engineReady ? 1 : 0);
        }

        // ExplorerCompat + WndCompat: одна exclusive-секция — иначе WinEvent может вызвать
        // ExplorerCompat_HandleWinEvent после FreeLibrary(ExplorerCompat).
        if (g_hExplorer || g_hTaskbar || g_hUICompat || g_hWndCompat)
        {
            if (g_hExplorer)
                result |= UnloadResult::FreeExplorer_Called;
            if (g_hWndCompat)
                result |= UnloadResult::FreeWnd_Called;

            DebugLog(L"[GlassCompat][Core] [Shutdown] FreeLibrary Explorer/Taskbar/UI/Wnd (under SRWLOCK)");

            HMODULE savedExplorer = nullptr;
            HMODULE savedTaskbar = nullptr;
            HMODULE savedUICompat = nullptr;
            HMODULE savedWnd = nullptr;
            AcquireSRWLockExclusive(&g_wndCompatLock);
            savedExplorer = g_hExplorer;
            g_hExplorer = nullptr;
            g_explorerEnsurePosted.store(false, std::memory_order_release);
            g_pfnExplorerHandleWinEvent.store(nullptr, std::memory_order_release);
            savedTaskbar = g_hTaskbar;
            g_hTaskbar = nullptr;
            g_taskbarEnsurePosted.store(false, std::memory_order_release);
            g_pfnTaskbarHandleWinEvent.store(nullptr, std::memory_order_release);
            savedUICompat = g_hUICompat;
            g_hUICompat = nullptr;
            g_uiCompatEnsurePosted.store(false, std::memory_order_release);
            savedWnd = g_hWndCompat;
            g_hWndCompat = nullptr;
            g_pfnHandleWinEvent.store(nullptr, std::memory_order_release);
            ReleaseSRWLockExclusive(&g_wndCompatLock);

            // HWiNFO: FreeLibrary(WndCompat) при живом in-flight WinEvent → UAF на другом tid
            // (fault IP в бывшем образе WndCompat, module=<unknown>).
            // H-R2 SEH: MsgWait+DispatchMessage на GUI после XAML Unadvise зависает
            // (лог: ui_shutdown_marshalled → нет uicompat_free_result). Только Sleep.
            if (savedWnd)
            {
                const bool cwHost = Shared::RootBackdropPolicy::IsCoreWindowHostProcess();
                for (int i = 0; i < 50; ++i)
                {
                    if (g_winEventCallbacksInFlight.load(std::memory_order_acquire) == 0)
                        break;
                    (void)WakeWinEventCallbackThreads();
                    if (cwHost)
                    {
                        Sleep(10);
                        continue;
                    }
                    MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
                    MSG msg{};
                    int pumped = 0;
                    while (pumped < 32 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                    {
                        ++pumped;
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                }
            }

            if (savedExplorer)
            {
                AuditGlassMessageWindowsBeforeFree(L"ExplorerCompat", savedExplorer,
                    kExplorerStopClasses, _countof(kExplorerStopClasses));
                // #region agent log
                // GLASS-ULTRACE (H91)
                ScanStaleCallbacksBeforeFree("ExplorerCompat", savedExplorer);
                // #endregion
                const unsigned rcExBefore = GlassUlTrace::QueryLoadCount(savedExplorer);
                const BOOL exFreed = FreeLibrary(savedExplorer);
                result |= UnloadResult::FreeExplorer_Returned;
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H86",
                    "GlassCompat.cpp:ExecuteShutdown",
                    "module_free",
                    "\"module\":\"ExplorerCompat\",\"base\":\"%p\",\"ok\":%d,\"owned\":%d,\"rc_before\":%u,\"rc_after\":%u,\"gmh\":%d,\"toolhelp\":%d",
                    static_cast<void*>(savedExplorer),
                    exFreed ? 1 : 0,
                    g_explorerOwnedPin ? 1 : 0,
                    rcExBefore,
                    GlassUlTrace::QueryLoadCountByName(L"ExplorerCompat.dll"),
                    GlassUlTrace::ModulePresentGmh(L"ExplorerCompat.dll") ? 1 : 0,
                    GlassUlTrace::ModulePresentToolhelp(L"ExplorerCompat.dll") ? 1 : 0);
                // #endregion
                HMODULE hCheckEx = GetModuleHandleW(L"ExplorerCompat.dll");
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCompat][Core] [Shutdown] ExplorerCompat after FreeLibrary: %s",
                    hCheckEx ? L"STILL LOADED (refcount/COM)" : L"unloaded OK");

                // IXDE держит LoadLibrary-pin вне COM free-list.
                // Residual FreeLibrary — только при objectCount==0 и shutdownDone (эталон 1017).
                if (hCheckEx)
                {
                    forcedUnloadMask |= 0x1ULL;
                    CoFreeUnusedLibraries();
                    HMODULE hCheckEx2 = GetModuleHandleW(L"ExplorerCompat.dll");
                    unsigned long long comState = 0xFFFFFFFFFFFFFFFFULL;
                    bool residualDropped = false;
                    if (hCheckEx2)
                    {
                        residualDropped = TryDropIxdeResidualModule(
                            hCheckEx2, "ExplorerCompat_DebugComState", comState);
                        if (residualDropped)
                            forcedUnloadMask |= 0x10ULL;
                        hCheckEx2 = GetModuleHandleW(L"ExplorerCompat.dll");
                    }
                    if (hCheckEx2)
                        forcedUnloadMask |= 0x2ULL;
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[GlassCompat][Core] [Shutdown] ExplorerCompat after residual reclaim: %s (dropped=%d)",
                        hCheckEx2 ? L"STILL LOADED" : L"unloaded OK",
                        residualDropped ? 1 : 0);
                }
            }
            if (savedTaskbar)
            {
                DebugLog(L"[GlassCompat][Core] [Shutdown] FreeLibrary TaskbarCompat");
                AuditGlassMessageWindowsBeforeFree(L"TaskbarCompat", savedTaskbar,
                    kTaskbarStopClasses, _countof(kTaskbarStopClasses));
                const unsigned rcTbBefore = GlassUlTrace::QueryLoadCount(savedTaskbar);
                const BOOL tbFreed = FreeLibrary(savedTaskbar);
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H86",
                    "GlassCompat.cpp:ExecuteShutdown",
                    "module_free",
                    "\"module\":\"TaskbarCompat\",\"base\":\"%p\",\"ok\":%d,\"owned\":%d,\"rc_before\":%u,\"rc_after\":%u,\"gmh\":%d,\"toolhelp\":%d",
                    static_cast<void*>(savedTaskbar),
                    tbFreed ? 1 : 0,
                    g_taskbarOwnedPin ? 1 : 0,
                    rcTbBefore,
                    GlassUlTrace::QueryLoadCountByName(L"TaskbarCompat.dll"),
                    GlassUlTrace::ModulePresentGmh(L"TaskbarCompat.dll") ? 1 : 0,
                    GlassUlTrace::ModulePresentToolhelp(L"TaskbarCompat.dll") ? 1 : 0);
                // #endregion
                CoFreeUnusedLibraries();
                constexpr int kTaskbarReclaimWaitMs = 2000;
                int reclaimWaitedMs = 0;
                HMODULE hCheckTaskbar = GetModuleHandleW(L"TaskbarCompat.dll");
                while (hCheckTaskbar && reclaimWaitedMs < kTaskbarReclaimWaitMs)
                {
                    Sleep(50);
                    reclaimWaitedMs += 50;
                    CoFreeUnusedLibraries();
                    hCheckTaskbar = GetModuleHandleW(L"TaskbarCompat.dll");
                }

                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCompat][Core] [Shutdown] TaskbarCompat after FreeLibrary: %s (waitedMs=%d)",
                    hCheckTaskbar ? L"STILL LOADED (refcount/COM)" : L"unloaded OK",
                    reclaimWaitedMs);

                if (hCheckTaskbar)
                {
                    forcedUnloadMask |= 0x4ULL;
                    CoFreeUnusedLibraries();
                    HMODULE hCheckTaskbar2 = GetModuleHandleW(L"TaskbarCompat.dll");
                    unsigned long long comState = 0xFFFFFFFFFFFFFFFFULL;
                    bool residualDropped = false;
                    if (hCheckTaskbar2)
                    {
                        residualDropped = TryDropIxdeResidualModule(
                            hCheckTaskbar2, "TaskbarCompat_DebugComState", comState);
                        if (residualDropped)
                            forcedUnloadMask |= 0x20ULL;
                        hCheckTaskbar2 = GetModuleHandleW(L"TaskbarCompat.dll");
                    }
                    if (hCheckTaskbar2)
                        forcedUnloadMask |= 0x8ULL;
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[GlassCompat][Core] [Shutdown] TaskbarCompat after residual reclaim: %s (dropped=%d)",
                        hCheckTaskbar2 ? L"STILL LOADED" : L"unloaded OK",
                        residualDropped ? 1 : 0);
                }
                else
                {
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[GlassCompat][Core] [Shutdown] TaskbarCompat final unload state: unloaded OK");
                }
            }
            if (savedUICompat)
            {
                // Как TaskbarCompat: FreeLibrary → CoFree wait ≤2s → TryDropIxdeResidualModule.
                DebugLog(L"[GlassCompat][Core] [Shutdown] FreeLibrary UICompat");
                AuditGlassMessageWindowsBeforeFree(L"UICompat", savedUICompat,
                    kUiStopClasses, _countof(kUiStopClasses));
                const unsigned rcUiBefore = GlassUlTrace::QueryLoadCount(savedUICompat);
                const BOOL uiFreed = FreeLibrary(savedUICompat);
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H83",
                    "GlassCompat.cpp:ExecuteShutdown",
                    "module_free",
                    "\"module\":\"UICompat\",\"base\":\"%p\",\"ok\":%d,\"owned\":%d,\"rc_before\":%u,\"rc_after\":%u,\"gmh\":%d,\"toolhelp\":%d",
                    static_cast<void*>(savedUICompat),
                    uiFreed ? 1 : 0,
                    g_uiCompatOwnedPin ? 1 : 0,
                    rcUiBefore,
                    GlassUlTrace::QueryLoadCountByName(L"UICompat.dll"),
                    GlassUlTrace::ModulePresentGmh(L"UICompat.dll") ? 1 : 0,
                    GlassUlTrace::ModulePresentToolhelp(L"UICompat.dll") ? 1 : 0);
                // #endregion
                const bool cwUiHost = Shared::RootBackdropPolicy::IsCoreWindowHostProcess();

                // HR7 (лог 561350 @81939): SEH FreeLibrary → still:true → CoFreeUnusedLibraries
                // не возвращается (нет uicompat_free_result). IXDE pin не через CoLoad —
                // CoFree бесполезен и блокирует FreeSelf. Сразу TryDrop.
                HMODULE hCheckUi = GetModuleHandleW(L"UICompat.dll");
                int reclaimWaitedMs = 0;
                if (!cwUiHost)
                {
                    CoFreeUnusedLibraries();
                    constexpr int kUiReclaimWaitMs = 2000;
                    while (hCheckUi && reclaimWaitedMs < kUiReclaimWaitMs)
                    {
                        Sleep(50);
                        reclaimWaitedMs += 50;
                        CoFreeUnusedLibraries();
                        hCheckUi = GetModuleHandleW(L"UICompat.dll");
                    }
                }
                else if (hCheckUi)
                {
                    // CW host: UICompat ещё загружена — ShellHost держит COM proxies.
                    // Агрессивный drain с msg pump + CoFreeUnusedLibraries.
                    forcedUnloadMask |= 0x40ULL;
                    for (int round = 0; round < 8; ++round)
                    {
                        CoFreeUnusedLibraries();
                        for (int m = 0; m < 300; m += 10)
                        {
                            MSG msg;
                            PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE | PM_NOYIELD);
                            Sleep(10);
                        }
                        CoFreeUnusedLibraries();
                        hCheckUi = GetModuleHandleW(L"UICompat.dll");
                        if (!hCheckUi)
                            break;
                    }
                }

                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCompat][Core] [Shutdown] UICompat after FreeLibrary: %s (waitedMs=%d)",
                    hCheckUi ? L"STILL LOADED (refcount/COM)" : L"unloaded OK",
                    reclaimWaitedMs);

                unsigned long long comState = 0xFFFFFFFFFFFFFFFFULL;
                bool residualDropped = false;
                if (hCheckUi)
                {
                    forcedUnloadMask |= 0x40ULL;
                    if (!cwUiHost)
                        CoFreeUnusedLibraries();
                    HMODULE hCheckUi2 = GetModuleHandleW(L"UICompat.dll");
                    if (hCheckUi2)
                    {
                        residualDropped = TryDropIxdeResidualModule(
                            hCheckUi2, "UICompat_DebugComState", comState);
                        if (residualDropped)
                            forcedUnloadMask |= 0x80ULL;
                        hCheckUi2 = GetModuleHandleW(L"UICompat.dll");
                    }
                    hCheckUi = hCheckUi2;
                }
                DebugLog(
                    DBG_IMPORTANT,
                    L"[GlassCompat][Core] [Shutdown] UICompat after residual reclaim: %s (dropped=%d)",
                    hCheckUi ? L"STILL LOADED" : L"unloaded OK",
                    residualDropped ? 1 : 0);
            }
            if (savedWnd)
            {
                AuditGlassMessageWindowsBeforeFree(L"WndCompat", savedWnd,
                    kWndStopClasses, _countof(kWndStopClasses));
                // #region agent log
                // GLASS-ULTRACE (H91): эксперимент «не выгружать под живым указателем».
                // GLASS_ULTRACE_WND_SAFE=1 → WndCompat ВООБЩЕ не выгружается в этом процессе
                // (модуль остаётся; крэша по stale-указателю быть не должно).
                const unsigned staleWndProcs = ScanStaleCallbacksBeforeFree("WndCompat", savedWnd);
                wchar_t safeFlag[16]{};
                const bool safeEnv =
                    GetEnvironmentVariableW(L"GLASS_ULTRACE_WND_SAFE", safeFlag, _countof(safeFlag)) > 0;
                // Эксперимент: флаг = «не выгружать WndCompat вообще» (не только при найденных
                // указателях). Нужен, чтобы отделить «наш unmap» от «чужие ссылки/COM».
                const bool skipWndFree = safeEnv;
                if (skipWndFree)
                {
                    GlassUlTrace::Logf(
                        "H91",
                        "GlassCompat.cpp:ExecuteShutdown",
                        "wnd_free_skipped",
                        "\"stale_procs\":%u,\"env\":1",
                        staleWndProcs);
                }
                else
                {
                    GlassUlTrace::Logf(
                        "H91",
                        "GlassCompat.cpp:ExecuteShutdown",
                        "wnd_free_proceed",
                        "\"stale_procs\":%u",
                        staleWndProcs);
                }
                // #endregion
                // GLASS-ULTRACE (H91): при skipWndFree модуль НЕ выгружается — логируем отдельно,
                // чтобы rc_after в module_free не путался с «не выгрузилось из-за ссылки».
                if (!skipWndFree)
                {
                    const unsigned rcWndBefore = GlassUlTrace::QueryLoadCount(savedWnd);
                    const BOOL wndFreed = FreeLibrary(savedWnd);
                    result |= UnloadResult::FreeWnd_Returned;
                    // #region agent log
                    // GLASS-ULTRACE
                    GlassUlTrace::Logf(
                        "H88",
                        "GlassCompat.cpp:ExecuteShutdown",
                        "module_free",
                        "\"module\":\"WndCompat\",\"base\":\"%p\",\"ok\":%d,\"owned\":1,\"rc_before\":%u,\"rc_after\":%u,\"gmh\":%d,\"toolhelp\":%d",
                        static_cast<void*>(savedWnd),
                        wndFreed ? 1 : 0,
                        rcWndBefore,
                        GlassUlTrace::QueryLoadCountByName(L"WndCompat.dll"),
                        GlassUlTrace::ModulePresentGmh(L"WndCompat.dll") ? 1 : 0,
                        GlassUlTrace::ModulePresentToolhelp(L"WndCompat.dll") ? 1 : 0);
                    // #endregion
                }
            }
        }

        // 5. Сообщаем результат в GlassPlatform.
        // Физическую верификацию выгрузки делает GlassPlatform снаружи через Toolhelp —
        // GetModuleHandleW здесь ненадёжен из-за async системного cleanup.
        DebugLog(L"[GlassCompat][Core] ExecuteShutdown: COMPLETE, reporting result=0x%08X",
            static_cast<uint32_t>(result));
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H88",
            "GlassCompat.cpp:ExecuteShutdown",
            "shutdown_end",
            "\"reason\":\"%s\",\"result\":%u,\"mask\":\"0x%X\",\"ms\":%llu,\"phase\":\"end\"",
            shutdownReason,
            static_cast<unsigned>(result),
            static_cast<unsigned>(result),
            GetTickCount64() - shutdownT0);
        // #endregion

        ReportUnloadResult(pid, result);
    }

    // ---------------- TaskbarCompat / ExplorerCompat: lazy load (Win11) ----------------

    void TryLoadAndInitTaskbarCompat() noexcept
    {
        if (g_hTaskbar)
            return;

        if (!IsWin11OrNewerBuild())
        {
            DebugLog(L"[GlassCompat][Core] TryLoadAndInitTaskbarCompat: skip (Win10)");
            g_taskbarEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        if (g_mode != Shared::InjectionMode::Full || !g_hWndCompat)
        {
            DebugLog(
                L"[GlassCompat][Core] TryLoadAndInitTaskbarCompat: skip (mode=%u wnd=%d)",
                static_cast<DWORD>(g_mode),
                g_hWndCompat ? 1 : 0);
            g_taskbarEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        const std::wstring processName = GetProcessNameLower();
        if (processName != L"explorer.exe")
        {
            DebugLog(L"[GlassCompat][Core] TryLoadAndInitTaskbarCompat: skip (not explorer)");
            g_taskbarEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        HMODULE hSelf = g_hSelf ? g_hSelf : GetModuleHandleW(L"GlassCompat.dll");
        const std::wstring taskbarPath = ResolvePeerDllPath(hSelf, L"TaskbarCompat.dll");

        g_hTaskbar = GetModuleHandleW(L"TaskbarCompat.dll");
        if (g_hTaskbar)
        {
            g_taskbarOwnedPin = false;
            DebugLog(L"[GlassCompat][Core] TaskbarCompat already loaded — skipping LoadLibrary");
        }
        else if (!taskbarPath.empty())
        {
            g_hTaskbar = LoadOptionalModule(taskbarPath, L"TaskbarCompat");
            g_taskbarOwnedPin = g_hTaskbar != nullptr;
        }

        if (!g_hTaskbar)
        {
            // Нет файла (типично Win10 layout) — soft skip; posted сброшен для редкого retry.
            g_taskbarEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        auto pInitTaskbar = reinterpret_cast<TaskbarCompatInitFn>(
            GetProcAddress(g_hTaskbar, "TaskbarCompat_Initialize"));
        if (!pInitTaskbar)
        {
            DebugLog(L"[GlassCompat][Core] TaskbarCompat_Initialize not found, unloading");
            FreeLibrary(g_hTaskbar);
            g_hTaskbar = nullptr;
            g_taskbarEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        DebugLog(L"[GlassCompat][Core] Calling TaskbarCompat_Initialize (Tray-attach path)");
        const BOOL ok = pInitTaskbar(hSelf);
        if (!ok)
        {
            DebugLog(L"[GlassCompat][Core] TaskbarCompat_Initialize returned FALSE, unloading");
            FreeLibrary(g_hTaskbar);
            g_hTaskbar = nullptr;
            g_taskbarEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        InjectMapUpsert(
            Shared::InjectProcessMap::Kind::ActiveGui,
            1,
            Shared::InjectProcessMap::State::Alive,
            CurrentModuleBits());
        RepublishDiagFlagsFromLoadedModules();
    }

    void EnsureTaskbarCompatLoadedImpl() noexcept
    {
        if (g_hTaskbar)
            return;

        if (!IsWin11OrNewerBuild())
            return;

        if (g_shutdownStarted.load(std::memory_order_acquire)
            || g_shutdownComplete.load(std::memory_order_acquire))
            return;

        const DWORD tid = g_guiThreadId;
        if (tid == 0)
        {
            DebugLog(L"[GlassCompat][Core] EnsureTaskbarCompatLoaded: no GlassGuiThread yet");
            return;
        }

        bool expected = false;
        if (!g_taskbarEnsurePosted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;

        if (!PostThreadMessageW(tid, GlassCompatMsg::WM_GLASS_ENSURE_TASKBAR_COMPAT, 0, 0))
        {
            g_taskbarEnsurePosted.store(false, std::memory_order_release);
            DebugLog(
                DBG_WARNING,
                L"[GlassCompat][Core] EnsureTaskbarCompatLoaded: PostThreadMessage failed GLE=%u",
                GetLastError());
        }
    }

    void TryLoadAndInitExplorerCompat() noexcept
    {
        if (g_hExplorer)
            return;

        if (!IsWin11OrNewerBuild())
        {
            DebugLog(L"[GlassCompat][Core] TryLoadAndInitExplorerCompat: skip (Win10)");
            g_explorerEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        if (g_mode != Shared::InjectionMode::Full || !g_hWndCompat)
        {
            DebugLog(
                L"[GlassCompat][Core] TryLoadAndInitExplorerCompat: skip (mode=%u wnd=%d)",
                static_cast<DWORD>(g_mode),
                g_hWndCompat ? 1 : 0);
            g_explorerEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        const std::wstring processName = GetProcessNameLower();
        if (processName != L"explorer.exe")
        {
            DebugLog(L"[GlassCompat][Core] TryLoadAndInitExplorerCompat: skip (not explorer)");
            g_explorerEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        HMODULE hSelf = g_hSelf ? g_hSelf : GetModuleHandleW(L"GlassCompat.dll");
        const std::wstring explorerPath = ResolvePeerDllPath(hSelf, L"ExplorerCompat.dll");

        g_hExplorer = GetModuleHandleW(L"ExplorerCompat.dll");
        if (g_hExplorer)
        {
            g_explorerOwnedPin = false;
            DebugLog(L"[GlassCompat][Core] ExplorerCompat already loaded — skipping LoadLibrary");
        }
        else if (!explorerPath.empty())
        {
            g_hExplorer = LoadOptionalModule(explorerPath, L"ExplorerCompat");
            g_explorerOwnedPin = g_hExplorer != nullptr;
        }

        if (!g_hExplorer)
        {
            g_explorerEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        auto pInit = reinterpret_cast<ExplorerCompatInitFn>(
            GetProcAddress(g_hExplorer, "ExplorerCompat_Initialize"));
        if (!pInit)
        {
            DebugLog(L"[GlassCompat][Core] ExplorerCompat_Initialize not found, unloading");
            FreeLibrary(g_hExplorer);
            g_hExplorer = nullptr;
            g_explorerEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        DebugLog(L"[GlassCompat][Core] Calling ExplorerCompat_Initialize (FileExplorerShellRoot path)");
        bool ok = false;
        try
        {
            ok = pInit(hSelf);
        }
        catch (...)
        {
            DebugLog(L"[GlassCompat][Core] EXCEPTION in ExplorerCompat_Initialize");
            ok = false;
        }

        if (!ok)
        {
            DebugLog(L"[GlassCompat][Core] ExplorerCompat_Initialize returned FALSE, unloading");
            FreeLibrary(g_hExplorer);
            g_hExplorer = nullptr;
            g_explorerEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        InjectMapUpsert(
            Shared::InjectProcessMap::Kind::ActiveGui,
            1,
            Shared::InjectProcessMap::State::Alive,
            CurrentModuleBits());
        RepublishDiagFlagsFromLoadedModules();
    }

    void EnsureExplorerCompatLoadedImpl() noexcept
    {
        if (g_hExplorer)
            return;

        if (!IsWin11OrNewerBuild())
            return;

        if (g_shutdownStarted.load(std::memory_order_acquire)
            || g_shutdownComplete.load(std::memory_order_acquire))
            return;

        const DWORD tid = g_guiThreadId;
        if (tid == 0)
        {
            DebugLog(L"[GlassCompat][Core] EnsureExplorerCompatLoaded: no GlassGuiThread yet");
            return;
        }

        bool expected = false;
        if (!g_explorerEnsurePosted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;

        if (!PostThreadMessageW(tid, GlassCompatMsg::WM_GLASS_ENSURE_EXPLORER_COMPAT, 0, 0))
        {
            g_explorerEnsurePosted.store(false, std::memory_order_release);
            DebugLog(
                DBG_WARNING,
                L"[GlassCompat][Core] EnsureExplorerCompatLoaded: PostThreadMessage failed GLE=%u",
                GetLastError());
        }
    }

    void TryLoadAndInitUICompat() noexcept
    {
        if (g_hUICompat)
            return;

        if (!IsWin11OrNewerBuild())
        {
            DebugLog(L"[GlassCompat][Core] TryLoadAndInitUICompat: skip (Win10)");
            g_uiCompatEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        if (!Shared::RootBackdropPolicy::IsCoreWindowHostProcess())
        {
            DebugLog(L"[GlassCompat][Core] TryLoadAndInitUICompat: skip (not CoreWindow host)");
            g_uiCompatEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        // SEH/ShellHost: UICompat обязателен даже без WndCompat (IXDE TAP-only иначе
        // Shutdown no-op → NC без restore). Full+Wnd — как раньше для прочих.
        if (g_mode != Shared::InjectionMode::Full)
        {
            DebugLog(
                L"[GlassCompat][Core] TryLoadAndInitUICompat: skip (mode=%u)",
                static_cast<DWORD>(g_mode));
            g_uiCompatEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        HMODULE hSelf = g_hSelf ? g_hSelf : GetModuleHandleW(L"GlassCompat.dll");
        const std::wstring uiPath = ResolvePeerDllPath(hSelf, L"UICompat.dll");

        // Remote inject мог LoadLibrary UICompat до Ensure. GetModuleHandle НЕ
        // держит ref → CoFree/soft FreeLibrary снимает единственный pin без
        // UICompat_Shutdown → NC кастом остаётся (лог Stop: no_uicompat, нет restore_all).
        // GetModuleHandleEx(0) инкрементирует refcount (как наш LoadLibrary).
        {
            HMODULE pinned = nullptr;
            if (GetModuleHandleExW(0, L"UICompat.dll", &pinned) && pinned)
            {
                g_hUICompat = pinned;
                g_uiCompatOwnedPin = true;
                DebugLog(
                    L"[GlassCompat][Core] UICompat already loaded — pinned via GetModuleHandleEx");
            }
            else if (!uiPath.empty())
            {
                const DWORD attr = GetFileAttributesW(uiPath.c_str());
                (void)attr;
                SetLastError(0);
                g_hUICompat = LoadOptionalModule(uiPath, L"UICompat");
                g_uiCompatOwnedPin = g_hUICompat != nullptr;
            }
        }

        if (!g_hUICompat)
        {
            g_uiCompatEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        auto pInit = reinterpret_cast<UICompatInitFn>(
            GetProcAddress(g_hUICompat, "UICompat_Initialize"));
        if (!pInit)
        {
            DebugLog(L"[GlassCompat][Core] UICompat_Initialize not found, unloading");
            FreeLibrary(g_hUICompat);
            g_hUICompat = nullptr;
            g_uiCompatEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        DebugLog(L"[GlassCompat][Core] Calling UICompat_Initialize (CoreWindow host path)");
        if (!pInit(hSelf))
        {
            DebugLog(L"[GlassCompat][Core] UICompat_Initialize returned FALSE, unloading");
            FreeLibrary(g_hUICompat);
            g_hUICompat = nullptr;
            g_uiCompatEnsurePosted.store(false, std::memory_order_release);
            return;
        }

        const std::uint32_t bits = CurrentModuleBits();
        const std::wstring exe = GetProcessBaseName();
        const bool upsertOk = Shared::InjectProcessMap::TryUpsert(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::Kind::ActiveGui,
            1,
            Shared::InjectProcessMap::State::Alive,
            bits,
            exe.c_str());
        RepublishDiagFlagsFromLoadedModules();

    }

    void EnsureUICompatLoadedImpl() noexcept
    {
        if (g_hUICompat)
            return;

        if (!IsWin11OrNewerBuild())
            return;

        if (g_shutdownStarted.load(std::memory_order_acquire)
            || g_shutdownComplete.load(std::memory_order_acquire))
            return;

        const DWORD tid = g_guiThreadId;
        if (tid == 0)
        {
            DebugLog(L"[GlassCompat][Core] EnsureUICompatLoaded: no GlassGuiThread yet");
            return;
        }

        bool expected = false;
        if (!g_uiCompatEnsurePosted.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel))
            return;

        if (!PostThreadMessageW(tid, GlassCompatMsg::WM_GLASS_ENSURE_UI_COMPAT, 0, 0))
        {
            g_uiCompatEnsurePosted.store(false, std::memory_order_release);
            DebugLog(
                DBG_WARNING,
                L"[GlassCompat][Core] EnsureUICompatLoaded: PostThreadMessage failed GLE=%u",
                GetLastError());
        }
    }

    // ---------------- Инициализация (в GUI‑потоке) ----------------

    void DoInitialize(HMODULE hSelf)
    {
        DebugLog(std::format(
            L"[GlassCompat][Core] DoInitialize: BEGIN (hSelf=0x{:X})",
            reinterpret_cast<uintptr_t>(hSelf)
        ).c_str());

        // читаем режим до загрузки модулей

        DebugLog(L"[GlassCompat][Core] InjectionMode=%u", static_cast<DWORD>(g_mode));

        Shared::ThemeHelper::TryEnableProcessDarkMode();

        std::wstring dir = GetModuleDirectory(hSelf);
        std::wstring processName = GetProcessNameLower();
        ProcessBlockList::Initialize();
        if (ProcessBlockList::IsBlocked(processName))
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCompat][Core] DoInitialize: blocked process — skip peer modules");
            return;
        }
        const std::wstring corePath = ResolvePeerDllPath(hSelf, L"GlassCore.dll");
        const std::wstring wndPath = ResolvePeerDllPath(hSelf, L"WndCompat.dll");
        DebugLog(std::format(L"[GlassCompat][Core] Process name: {}", processName).c_str());
        DebugLog(std::format(L"[GlassCompat][Core] Module directory: {}", dir).c_str());
        DebugLog(std::format(L"[GlassCompat][Core] GlassCore path: {}", corePath).c_str());

        // Слот до LoadLibrary(Core): иначе TrySetDiagFlag в Startup → no_slot (H3).
        InjectMapUpsert(
            Shared::InjectProcessMap::Kind::ActiveGui,
            1,
            Shared::InjectProcessMap::State::Alive,
            Shared::InjectProcessMap::ModuleCompat);

        // Loader: начало загрузки модулей (Starting → жёлтый).
        (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagLifecycleLoader,
            true);

        // ---------------------------------------------------------
        // 1. GlassCore — всегда LoadLibrary (свой ref). H64: init в DllMain под SEH + EnsureEngine.
        // HWiNFO: AV → LoadLibrary OK, engine not ready → FreeLibrary с таймаутом на Stop.
        // H32: SEH/ShellHost/SMEH (AppContainer) — НЕ грузим GlassCore: IAT/Immersive ломают
        // CoreWindow (SHOW→HIDE в логе; SMEH PID-churn). UICompat работает без Core ModuleHandler.
        // ---------------------------------------------------------
        g_coreEngineReady = false;
        const bool skipCoreForCwHost = Shared::RootBackdropPolicy::IsCoreWindowHostProcess();
        if (skipCoreForCwHost)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCompat][Core] CoreWindow host — GlassCore skipped (UICompat-only path)");
        }
        else
        {
            g_hCore = corePath.empty() ? nullptr : LoadOptionalModule(corePath, L"GlassCore");
            const DWORD coreLoadGle = g_hCore ? 0 : GetLastError();
            (void)coreLoadGle;

            wchar_t thPath[MAX_PATH]{};
            const HMODULE thCore = FindModuleViaToolhelp(L"GlassCore.dll", thPath, MAX_PATH);
            const bool zombieToolhelp =
                !g_hCore && thCore && thPath[0] && _wcsicmp(thPath, corePath.c_str()) == 0;

            if (zombieToolhelp)
            {
                DebugLog(
                    L"[GlassCompat][Core] HWiNFO-style zombie GlassCore in Toolhelp — leave mapped "
                    L"(no orphan FreeLibrary; see H60/H61 crash evidence)");
            }

            if (g_hCore)
            {
                auto pEnsure = reinterpret_cast<void (*)()>(
                    GetProcAddress(g_hCore, "GlassCore_HostUiEnsureEngine"));
                CallGlassCoreEnsureEngine(pEnsure);

                // GPA по короткому имени: GlassCore.def (x86 WINAPI иначе _Name@N → null).
                using IsReadyFn = BOOL(WINAPI*)();
                auto pReady = reinterpret_cast<IsReadyFn>(
                    GetProcAddress(g_hCore, "GlassCore_IsEngineReady"));
                g_coreEngineReady = pReady && pReady();
                if (!pReady)
                {
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[GlassCompat][Core] GetProcAddress(GlassCore_IsEngineReady) failed — "
                        L"rebuild GlassCore with GlassCore.def");
                }

                // H65 early FreeLibrary ОТКАЧЕН (H66): step=4 Install уже был, Free не снял модуль
                // (value1=3) → живые IAT без образа → UAF HookHelper + hang Stop. Держим Core mapped.
                if (g_coreEngineReady)
                {
                    using SetModeFn = void(WINAPI*)(DWORD);
                    auto pSetMode = reinterpret_cast<SetModeFn>(
                        GetProcAddress(g_hCore, "GlassCore_SetInjectionMode"));
                    if (pSetMode)
                        pSetMode(static_cast<DWORD>(g_mode));
                    else
                    {
                        DebugLog(
                            DBG_IMPORTANT,
                            L"[GlassCompat][Core] GetProcAddress(GlassCore_SetInjectionMode) failed — "
                            L"rebuild GlassCore with GlassCore.def");
                    }
                }
                else
                {
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[GlassCompat][Core] Engine not ready after init AV — keep owned GlassCore "
                        L"(no early FreeLibrary; H65/H60 evidence)");
                }
            }
        }

        const bool coreShutdownExport =
            g_hCore && GetProcAddress(g_hCore, "GlassCore_Shutdown") != nullptr;
        (void)coreShutdownExport;

        // ---------------------------------------------------------
        // 2. WndCompat — всегда LoadLibrary (свой ref), затем Initialize
        // ---------------------------------------------------------
        g_hWndCompat = wndPath.empty() ? nullptr : LoadOptionalModule(wndPath, L"WndCompat");

        // Если WndCompat загружен — вызываем Initialize (идемпотентно внутри DLL)
        if (g_hWndCompat)
        {
            auto pInitWnd = reinterpret_cast<WndCompatInitFn>(
                GetProcAddress(g_hWndCompat, "WndCompat_Initialize"));

            if (pInitWnd)
            {
                DebugLog(L"[GlassCompat][Core] Calling WndCompat_Initialize(mode=%u)",
                    static_cast<DWORD>(g_mode));

                BOOL ok = FALSE;
                try
                {
                    // передаём mode как DWORD
                    ok = pInitWnd(static_cast<DWORD>(g_mode));
                }
                catch (...)
                {
                    DebugLog(L"[GlassCompat][Core] EXCEPTION in WndCompat_Initialize");
                    ok = FALSE;
                }

                if (!ok)
                {
                    DebugLog(L"[GlassCompat][Core] WndCompat_Initialize returned FALSE, unloading");
                    FreeLibrary(g_hWndCompat);
                    g_hWndCompat = nullptr;
                }
            }
            else
            {
                DebugLog(L"[GlassCompat][Core] WndCompat_Initialize not found, unloading");
                FreeLibrary(g_hWndCompat);
                g_hWndCompat = nullptr;
            }
        }

        // ---------------------------------------------------------
        // 3. ExplorerCompat — НЕ грузим здесь: lazy по FileExplorerShellRoot (Win11)
        //    (CabinetWClass/ExploreWClass → ExplorerRoot::Attach → EnsureExplorerCompatLoaded).
        // 4. TaskbarCompat — lazy по Tray-аттачу (Win11)
        //    (Shell_*TrayWnd → TaskbarHost_Attach → EnsureTaskbarCompatLoaded).
        // ---------------------------------------------------------

        // ---------------------------------------------------------
        // Итог
        // ---------------------------------------------------------
        DebugLog(L"[GlassCompat][Core] === INITIALIZATION SUMMARY ===");
        DebugLog(std::format(
            L"[GlassCompat][Core] InjectionMode: {}",
            g_mode == Shared::InjectionMode::Full ? L"Full" : L"PopupOnly"
        ).c_str());
        DebugLog(std::format(
            L"[GlassCompat][Core] GlassCore: {}",
            g_hCore ? L"LOADED" : L"NOT LOADED"
        ).c_str());
        DebugLog(std::format(
            L"[GlassCompat][Core] WndCompat: {}",
            g_hWndCompat ? L"LOADED" : L"NOT LOADED"
        ).c_str());
        DebugLog(std::format(
            L"[GlassCompat][Core] ExplorerCompat: {} (lazy: FileExplorerShellRoot, Win11)",
            g_hExplorer ? L"LOADED" : L"PENDING/NOT LOADED"
        ).c_str());
        DebugLog(std::format(
            L"[GlassCompat][Core] TaskbarCompat: {} (lazy: Tray-attach, Win11)",
            g_hTaskbar ? L"LOADED" : L"PENDING/NOT LOADED"
        ).c_str());
        DebugLog(L"[GlassCompat][Core] === END SUMMARY ===");

        GlassCompat::ReassertProcessCrashGuard();

        // Loader: загрузка модулей завершена (Running → зелёный).
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagLoader,
            true);
        (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagLifecycleLoader,
            false);

        // H35: в SEH/ShellHost/SMEH UICompat иначе никогда не грузится (Ensure ждёт Attach,
        // Attach ждёт remote SHOW, remote SHOW ждал initDone на чужом потоке → not_ready).
        if (skipCoreForCwHost && g_hWndCompat)
        {
            TryLoadAndInitUICompat();
            // S3/H49: НЕ AttachExisting синхронно в DoInitialize (WerFault).
            // HR16: post на GUI после COMPLETE — патч cloaked CW до анимации NC.
            if (g_guiThreadId != 0)
            {
                (void)PostThreadMessageW(
                    g_guiThreadId,
                    GlassCompatMsg::WM_GLASS_COREWINDOW_ATTACH_EXISTING,
                    0,
                    0);
            }
        }

        DebugLog(L"[GlassCompat][Core] DoInitialize: COMPLETE");
        {
            const std::uint32_t bits = CurrentModuleBits();
            const std::wstring exe = GetProcessBaseName();
            const bool upsertOk = Shared::InjectProcessMap::TryUpsert(
                GetCurrentProcessId(),
                Shared::InjectProcessMap::Kind::ActiveGui,
                1,
                Shared::InjectProcessMap::State::Alive,
                bits,
                exe.c_str());
            RepublishDiagFlagsFromLoadedModules();
        }
    }

    // ---------------- GUI‑поток GlassCompat ----------------

    void NotifyExplorerRootBackdropPolicyImpl(BOOL enabled) noexcept
    {
        if (!g_hExplorer)
        {
            DebugLog(
                DBG_WARNING,
                L"[GlassCompat][Core] Explorer backdrop policy: ExplorerCompat not loaded (enabled=%d)",
                enabled ? 1 : 0);
            return;
        }

        using Fn = void(WINAPI*)(BOOL);
        const auto pfn = reinterpret_cast<Fn>(
            GetProcAddress(g_hExplorer, "ExplorerCompat_OnRootBackdropPolicyChanged"));
        if (pfn)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCompat][Core] Explorer backdrop policy → ExplorerCompat (enabled=%d)",
                enabled ? 1 : 0);
            pfn(enabled);
            return;
        }

        DebugLog(
            DBG_WARNING,
            L"[GlassCompat][Core] ExplorerCompat_OnRootBackdropPolicyChanged not found (enabled=%d)",
            enabled ? 1 : 0);
    }

    // Ленивое переоткрытие reload-объектов: при старте GUI-потока платформа может ещё не создать семафор/mapping.
    static void TryOpenReloadRenderSync(
        HANDLE& hReloadSemaphore,
        HANDLE& hReloadGenMap,
        HANDLE& hReloadFlagsMap,
        void*& pReloadGenView,
        void*& pReloadFlagsView) noexcept
    {
        if (!hReloadSemaphore)
        {
            HANDLE h = OpenSemaphoreW(SYNCHRONIZE, FALSE, kReloadRenderSemaphoreName);
            if (h)
            {
                hReloadSemaphore = h;
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCompat][Core] GlassGuiThread: reload semaphore opened (lazy)");
            }
        }

        if (!hReloadGenMap || !pReloadGenView)
        {
            if (pReloadGenView)
            {
                UnmapViewOfFile(pReloadGenView);
                pReloadGenView = nullptr;
            }
            if (hReloadGenMap)
            {
                CloseHandle(hReloadGenMap);
                hReloadGenMap = nullptr;
            }
            hReloadGenMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kReloadGenerationMappingName);
            if (hReloadGenMap)
            {
                pReloadGenView = MapViewOfFile(hReloadGenMap, FILE_MAP_READ, 0, 0, sizeof(DWORD));
                if (pReloadGenView)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCompat][Core] GlassGuiThread: reload gen mapping mapped (lazy)");
                }
            }
        }

        if (!hReloadFlagsMap || !pReloadFlagsView)
        {
            if (pReloadFlagsView)
            {
                UnmapViewOfFile(pReloadFlagsView);
                pReloadFlagsView = nullptr;
            }
            if (hReloadFlagsMap)
            {
                CloseHandle(hReloadFlagsMap);
                hReloadFlagsMap = nullptr;
            }
            hReloadFlagsMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kReloadRenderFlagsMappingName);
            if (hReloadFlagsMap)
            {
                pReloadFlagsView = MapViewOfFile(hReloadFlagsMap, FILE_MAP_READ, 0, 0, sizeof(DWORD));
                if (pReloadFlagsView)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCompat][Core] GlassGuiThread: reload flags mapping mapped (lazy)");
                }
            }
        }
    }

    // Как backup 1510 / 1017-glasscore-residual (рабочая выгрузка): не ждём HostShutdown.
    // pin=2 от GetModuleHandleEx держит DLL на время ExecuteShutdown;
    // UnhookWinEvent снимет OS-pin отдельно (deferred FreeLibrary).
    // H26 EnumWindows+SendMessageTimeout ОТКАТАН: для Taskmgr synced=0 (не помогает);
    // в explorer давал sync WM_NULL на 80+ top-level после S0/S1 — риск для UI restore,
    // но корневой баг шапки при ShellExt-stop — VTC-gate (g_hostBarRestoreOnIslandUiQueue).
    [[noreturn]] void FreeSelfAndExitThread(
        HMODULE hSelfRef,
        ULONGLONG path) noexcept
    {
        (void)path;
        (void)Shared::InjectProcessMap::TryClearPid(GetCurrentProcessId());

        // H69: без DispatchMessage (explorer). H72: wake callback-потоков в GUI до вызова;
        // здесь последний барьер — in-flight WinEventProc после deferred Unhook.
        // path=1 раньше сразу шёл в FLAET → AV в <Unloaded_GlassCompat> (ShellHost/Adobe, 18.08).
        // Remote LoadLibrary (SEH/SMEH/ShellHost): pin LoadLibrary + pin GetModuleHandleEx — после Unhook
        // нужен лишний FreeLibrary, иначе модуль остаётся (скриншот InjectMap).
        const bool remoteFlagBefore =
            g_remoteLoadLibraryPin.load(std::memory_order_acquire);
        const unsigned rcBegin = GlassUlTrace::QueryLoadCount(hSelfRef);
        const ULONG inflightBegin = g_winEventCallbacksInFlight.load(std::memory_order_acquire);
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H80",
            "GlassCompat.cpp:FreeSelfAndExitThread",
            "freeself_begin",
            "\"path\":%llu,\"handle\":\"%p\",\"remote_flag\":%d,\"rc\":%u,\"inflight\":%lu,\"gui_gen\":%u",
            path,
            static_cast<void*>(hSelfRef),
            remoteFlagBefore ? 1 : 0,
            rcBegin,
            static_cast<unsigned long>(inflightBegin),
            g_guiGeneration.load(std::memory_order_acquire));
        // #endregion
        const bool hadRemotePin =
            g_remoteLoadLibraryPin.exchange(false, std::memory_order_acq_rel);
        BOOL remoteCompOk = FALSE;
        unsigned rcAfterRemote = rcBegin;
        if (hadRemotePin && hSelfRef)
        {
            const unsigned rcCompBefore = GlassUlTrace::QueryLoadCount(hSelfRef);
            remoteCompOk = FreeLibrary(hSelfRef);
            rcAfterRemote = GlassUlTrace::QueryLoadCount(hSelfRef);
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H80",
                "GlassCompat.cpp:FreeSelfAndExitThread",
                "freeself_remote_comp",
                "\"called\":1,\"ok\":%d,\"rc_before\":%u,\"rc_after\":%u",
                remoteCompOk ? 1 : 0,
                rcCompBefore,
                rcAfterRemote);
            // #endregion
        }
        else
        {
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H80",
                "GlassCompat.cpp:FreeSelfAndExitThread",
                "freeself_remote_comp",
                "\"called\":0,\"ok\":0,\"remote_flag\":%d",
                hadRemotePin ? 1 : 0);
            // #endregion
        }

        DrainBeforeFreeSelf();

        // #region agent log
        // GLASS-ULTRACE (H82): expected_rc_after = сколько ссылок ОСТАНЕТСЯ после FLAET.
        // >0 и при этом dll_detach не пришёл = deferred free ОС не доставлен (idle/PLM-процесс).
        const unsigned rcBeforeFlaet = GlassUlTrace::QueryLoadCount(hSelfRef);
        const unsigned ownRefs = 1u + (hadRemotePin ? 1u : 0u);
        const unsigned expectedRcAfter = rcBeforeFlaet > ownRefs ? rcBeforeFlaet - ownRefs : 0u;
        const unsigned osRefs = expectedRcAfter;
        GlassUlTrace::Logf(
            "H82",
            "GlassCompat.cpp:FreeSelfAndExitThread",
            "freeself_flaet",
            "\"path\":%llu,\"handle\":\"%p\",\"rc\":%u,\"our_pins\":1,\"remote_comp\":%d,"
            "\"expected_rc_after\":%u,\"os_refs\":%u,\"inflight\":%lu",
            path,
            static_cast<void*>(hSelfRef),
            rcBeforeFlaet,
            hadRemotePin ? 1 : 0,
            expectedRcAfter,
            osRefs,
            static_cast<unsigned long>(g_winEventCallbacksInFlight.load(std::memory_order_acquire)));
        // #endregion
        FreeLibraryAndExitThread(hSelfRef, 0);
    }

    // H54: SEH GUI игнорирует WM_QUIT/Stop (лог: host_post posted:true, нет loop_exit).
    // Отдельный waiter на Stop → force ExecuteShutdown; GUI сам FreeSelf. Без TerminateThread.
    void StartCwUnloadWatcher() noexcept;

    DWORD WINAPI GlassGuiThreadProc(LPVOID)
    {
        // Host ForceUnload читает tid через WPM (explorer без bootstrap guiTid).
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&GlassCompat_GuiThreadId),
            static_cast<LONG>(GetCurrentThreadId()));

        // Добавляем явную LoadLibrary-ссылку на GlassCompat.dll.
        //
        // Проблема: ОС держит рефкаунт GlassCompat.dll = 1 (через SetWinEventHook/WINEVENT_INCONTEXT).
        // Когда хост вызывает UnhookWinEvent, ОС планирует FreeLibrary в целевом процессе —
        // это происходит практически немедленно при следующем диспатче событий UI-потоком.
        // Если GlassGuiThreadProc в это время ещё выполняет ExecuteShutdown(),
        // DLL размапируется пока мы в ней исполняемся → abort()/крэш во всех процессах.
        //
        // Решение: увеличиваем рефкаунт до 2. Тогда UnhookWinEvent опускает его до 1 (DLL жива),
        // а FreeLibraryAndExitThread в конце атомарно опускает до 0 и завершает поток.
        HMODULE hSelfRef = nullptr;
        const unsigned rcPinBefore = GlassUlTrace::QueryLoadCountByName(L"GlassCompat.dll");
        const BOOL pinOk = GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&GlassGuiThreadProc),
            &hSelfRef);
        const DWORD pinGle = pinOk ? 0u : GetLastError();
        // #region agent log
        // GLASS-ULTRACE
        GlassUlTrace::Logf(
            "H89",
            "GlassCompat.cpp:GlassGuiThreadProc",
            "pin_acq",
            "\"gen\":%u,\"handle\":\"%p\",\"ok\":%d,\"gle\":%lu,\"rc_before\":%u,\"rc_after\":%u",
            g_guiGeneration.load(std::memory_order_acquire),
            static_cast<void*>(hSelfRef),
            pinOk ? 1 : 0,
            static_cast<unsigned long>(pinGle),
            rcPinBefore,
            GlassUlTrace::QueryLoadCount(hSelfRef ? hSelfRef : GetModuleHandleW(L"GlassCompat.dll")));
        // #endregion

        DoInitialize(g_hSelf);

        // GlassCore + WndCompat + провайдеры готовы: разрешаем WinEvent-делегирование.
        g_initDone.store(true, std::memory_order_release);

        // Подписываемся на stop-событие платформы: когда хост получит команду /stop,
        // он просигнализирует kStopEventName и все инжектированные GUI-потоки
        // выйдут из цикла и вызовут ExecuteShutdown (снятие blur, FreeLibrary).
        //
        // Если открыть событие не удалось (платформа ещё не создала его или уже
        // удалила) — Fallback: периодически проверяем ReadHostPid(). Когда хост
        // завершился и PID исчез, инициируем shutdown самостоятельно.
        HANDLE hPlatformStop = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
        const DWORD gleStop = hPlatformStop ? 0u : GetLastError();
        HANDLE hUnloadRequest = Shared::InjectProcessMap::CreateUnloadRequestEvent(nullptr);
        // H71: HostShutdown открываем СРАЗУ (как BlockList watcher), не после ExecuteShutdown.
        // H70 OpenEvent после COMPLETE → часто WAIT_FAILED: хост уже CloseHostShutdownEvent
        // (лог: value1=0xFFFFFFFF у conhost/ollama/RtkAud/NhNotif; FreeSelf без DETACH).
        HANDLE hHostShutdownEarly = OpenEventW(
            SYNCHRONIZE,
            FALSE,
            GlassShared::GlassPlatformTask::kHostShutdownEventName);
        const DWORD gleHostShutdown = hHostShutdownEarly ? 0u : GetLastError();
        // Dead-man switch хоста: живой хост владеет мьютексом всегда, поэтому любой
        // успешный захват (WAIT_OBJECT_0 или WAIT_ABANDONED) означает его смерть.
        // Закрывает дыру, из-за которой CoreWindow-процессы не выгружались после
        // TerminateProcess от Планировщика: StopEvent в этом случае не приходит никогда,
        // а cwDeferToStop бесконечно сбрасывал PID-счётчик.
        HANDLE hHostAlive = OpenMutexW(
            SYNCHRONIZE, FALSE, GlassShared::GlassPlatformTask::kHostAliveMutexName);
        HANDLE hReloadSemaphore = OpenSemaphoreW(SYNCHRONIZE, FALSE, kReloadRenderSemaphoreName);
        HANDLE hReloadGenMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kReloadGenerationMappingName);
        HANDLE hReloadFlagsMap = OpenFileMappingW(FILE_MAP_READ, FALSE, kReloadRenderFlagsMappingName);
        void* pReloadGenView = nullptr;
        void* pReloadFlagsView = nullptr;
        if (hReloadGenMap)
            pReloadGenView = MapViewOfFile(hReloadGenMap, FILE_MAP_READ, 0, 0, sizeof(DWORD));
        if (hReloadFlagsMap)
            pReloadFlagsView = MapViewOfFile(hReloadFlagsMap, FILE_MAP_READ, 0, 0, sizeof(DWORD));

        DebugLog(L"[GlassCompat][Core] GlassGuiThread: stop event %s, unload request %s",
            hPlatformStop ? L"opened" : L"not found (fallback to PID poll)",
            hUnloadRequest ? L"opened" : L"not created");
        DebugLog(L"[GlassCompat][Core] GlassGuiThread: reload semaphore %s, gen mapping %s",
            hReloadSemaphore ? L"opened" : L"not found",
            pReloadGenView ? L"mapped" : L"not available");

        // Отдельный waiter: MsgWait при непустой QS может никогда не отдать StopEvent.
        // WaitForSingleObject + WM_QUIT гарантирует выход из GUI-цикла (H25).
        // Dup + cancel: нельзя делить hPlatformStop — GUI закрывает его до FreeSelf.
        const DWORD guiTid = GetCurrentThreadId();

        HANDLE hStopQuitCancel = nullptr;
        HANDLE hStopQuitThread = nullptr;
        // H54: для cw — свой OpenEvent(Stop) в waiter (не только Dup GUI-handle).
        if (hPlatformStop || Shared::RootBackdropPolicy::IsCoreWindowHostProcess())
        {
            struct StopQuitParam
            {
                HANDLE stopDup{};
                HANDLE cancel{};
                DWORD tid{};
            };
            HANDLE stopDup = nullptr;
            if (hPlatformStop)
            {
                (void)DuplicateHandle(
                    GetCurrentProcess(),
                    hPlatformStop,
                    GetCurrentProcess(),
                    &stopDup,
                    SYNCHRONIZE,
                    FALSE,
                    0);
            }
            if (!stopDup)
                stopDup = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
            if (stopDup)
            {
                hStopQuitCancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                auto* param = new (std::nothrow) StopQuitParam{ stopDup, hStopQuitCancel, guiTid };
                if (param && hStopQuitCancel)
                {
                    hStopQuitThread = CreateThread(
                        nullptr,
                        0,
                        [](LPVOID pv) -> DWORD
                        {
                            auto* p = static_cast<StopQuitParam*>(pv);
                            HANDLE waits[2] = { p->stopDup, p->cancel };
                            const DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                            if (wr == WAIT_OBJECT_0)
                            {
                                const BOOL posted = PostThreadMessageW(p->tid, WM_QUIT, 0, 0);
                            }
                            if (p->stopDup)
                                CloseHandle(p->stopDup);
                            delete p;
                            return 0;
                        },
                        param,
                        0,
                        nullptr);
                    if (!hStopQuitThread)
                    {
                        delete param;
                        CloseHandle(stopDup);
                    }
                }
                else
                {
                    delete param;
                    CloseHandle(stopDup);
                    if (hStopQuitCancel)
                    {
                        CloseHandle(hStopQuitCancel);
                        hStopQuitCancel = nullptr;
                    }
                }
            }
        }

        // Состояние наблюдения за хост-процессом.
        DWORD hostWatchPid = 0;
        HANDLE hostWatchProcess = nullptr;
        int hostDeadConfirmTicks = 0;
        constexpr int kHostDeadConfirmNeed = 2; // debounce: 2 последовательных тика

        bool shutdownByHostLiveness = false;
        HostLivenessReason hostDeadReason = HostLivenessReason::Alive;
        bool softModuleUnloadDone = false;
        const char* guiStopTrigger = "unknown";

        MSG msg{};
        const bool cwHostGui = Shared::RootBackdropPolicy::IsCoreWindowHostProcess();
        while (true)
        {
            // Ранний poll Stop — до MsgWait/reload (обход starvation QS_ALLINPUT).
            if (hPlatformStop)
            {
                const DWORD stopWr = WaitForSingleObject(hPlatformStop, 0);
                if (stopWr == WAIT_OBJECT_0)
                {
                    guiStopTrigger = "stopEvent";
                    break;
                }
                // H54: битый handle в AppContainer — переоткрыть named Stop.
                if (stopWr == WAIT_FAILED)
                {
                    CloseHandle(hPlatformStop);
                    hPlatformStop = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
                    if (hPlatformStop &&
                        WaitForSingleObject(hPlatformStop, 0) == WAIT_OBJECT_0)
                    {
                        guiStopTrigger = "stopEvent";
                        break;
                    }
                }
            }

            // H-R2 (log 561350 @73028→73050): в SEH named Wait(Stop) мёртв, StopQuit
            // не стреляет; host WPM force ok, но GUI не поллил флаг → 22s до loop_exit.
            if (cwHostGui && InterlockedCompareExchange(&GlassCompat_CwForceUnloadFlag, 0, 0) != 0)
            {
                guiStopTrigger = "cwForceFlag";
                break;
            }

            // Heartbeat NDJSON убран с GUI-потока: CreateFile в AppContainer мог
            // блокировать poll Stop (H54: последний hb SEH, затем нет loop_exit).

            // Per-PID unload: MsgWait на auto-reset УЖЕ снимает сигнал — нельзя откладывать на poll (H1).
            auto handlePerPidUnload = [&](bool alreadySignaled) -> bool
            {
                if (!hUnloadRequest)
                    return false;
                if (!alreadySignaled &&
                    WaitForSingleObject(hUnloadRequest, 0) != WAIT_OBJECT_0)
                {
                    return false;
                }

                const bool hostShutdownAlready =
                    hHostShutdownEarly &&
                    WaitForSingleObject(hHostShutdownEarly, 0) == WAIT_OBJECT_0;
                const bool hostGone = !GlassShared::GlassPlatformTask::IsGlassPlatformRunning();

                if (!softModuleUnloadDone)
                {
                    // Не трогаем g_shutdownStarted: soft unload ≠ global Stop (reinject + полный Stop).
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCompat][Core] GlassGuiThread: per-PID soft unload — ExecuteShutdown");
                    // #region agent log
                    // GLASS-ULTRACE
                    GlassUlTrace::Logf(
                        "H90",
                        "GlassCompat.cpp:GlassGuiThreadProc",
                        "stop_trigger",
                        "\"what\":\"unloadRequest\",\"soft\":1,\"host_gone\":%d,\"host_shutdown\":%d",
                        hostGone ? 1 : 0,
                        hostShutdownAlready ? 1 : 0);
                    // #endregion
                    g_ultraceShutdownReason = "unloadRequest";
                    ExecuteShutdown();
                    softModuleUnloadDone = true;
                    g_softModulesUnloaded.store(true, std::memory_order_release);
                    g_initDone.store(false, std::memory_order_release);
                    ClearDiagFlagsInMap();
                    // Soft unload не снимает GlassCompat: CrashGuard и WinEventProc остаются.
                    if (GlassCompat::IsProcessCrashGuardInstalled())
                    {
                        (void)Shared::InjectProcessMap::TrySetDiagFlag(
                            GetCurrentProcessId(),
                            Shared::InjectProcessMap::DiagCrashGuard,
                            true);
                    }
                    (void)Shared::InjectProcessMap::TrySetDiagFlag(
                        GetCurrentProcessId(),
                        Shared::InjectProcessMap::DiagWinEventProc,
                        true);
                    (void)Shared::InjectProcessMap::TrySetModuleBits(
                        GetCurrentProcessId(),
                        Shared::InjectProcessMap::ModuleCompat);
                    (void)Shared::InjectProcessMap::TrySetState(
                        GetCurrentProcessId(),
                        Shared::InjectProcessMap::State::Alive);
                }

                if (hostShutdownAlready || hostGone)
                {
                    if (hostGone)
                    {
                        shutdownByHostLiveness = true;
                        hostDeadReason = HostLivenessReason::PidMissing;
                    }
                    return true; // caller breaks
                }
                return false;
            };

            if (handlePerPidUnload(false))
            {
                guiStopTrigger = "unloadRequest";
                break;
            }

            TryOpenReloadRenderSync(
                hReloadSemaphore, hReloadGenMap, hReloadFlagsMap,
                pReloadGenView, pReloadFlagsView);

            HANDLE waitArr[4]{};
            unsigned stopIdx = UINT_MAX;
            unsigned reloadIdx = UINT_MAX;
            unsigned unloadIdx = UINT_MAX;
            unsigned hostAliveIdx = UINT_MAX;
            unsigned nH = 0;
            if (hPlatformStop)
            {
                stopIdx = nH;
                waitArr[nH++] = hPlatformStop;
            }
            if (hReloadSemaphore)
            {
                reloadIdx = nH;
                waitArr[nH++] = hReloadSemaphore;
            }
            if (hUnloadRequest)
            {
                unloadIdx = nH;
                waitArr[nH++] = hUnloadRequest;
            }
            if (hHostAlive)
            {
                hostAliveIdx = nH;
                waitArr[nH++] = hHostAlive;
            }

            // H54 SEH: MsgWait+QS при XAML/COM может долго не отдавать Stop; Wait без QS.
            const DWORD r = cwHostGui
                ? ((nH > 0)
                    ? WaitForMultipleObjects(nH, waitArr, FALSE, 200)
                    : WAIT_TIMEOUT)
                : MsgWaitForMultipleObjects(
                    nH, nH ? waitArr : nullptr,
                    FALSE, 1000, QS_ALLINPUT);

            // WAIT_ABANDONED_0 — мьютекс HostAlive от умершего хоста.
            unsigned signaledIdx = UINT_MAX;
            if (nH > 0)
            {
                if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + nH)
                    signaledIdx = r - WAIT_OBJECT_0;
                else if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + nH)
                    signaledIdx = r - WAIT_ABANDONED_0;
            }

            if (signaledIdx != UINT_MAX)
            {
                const unsigned i = signaledIdx;
                if (i == hostAliveIdx)
                {
                    // Отдаём мьютекс дальше: остальные инжектированные процессы
                    // ждут на нём же и должны получить тот же сигнал.
                    (void)ReleaseMutex(hHostAlive);
                    shutdownByHostLiveness = true;
                    hostDeadReason = HostLivenessReason::HostAliveReleased;
                    guiStopTrigger = "hostAlive";
                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCompat][HostWatch] host dead detected: HostAlive mutex released");
                    break;
                }
                if (i == stopIdx)
                {
                    guiStopTrigger = "stopEvent";
                    DebugLog(L"[GlassCompat][Core] GlassGuiThread: stop event signaled, initiating shutdown");
                    break;
                }
                if (i == unloadIdx)
                {
                    if (handlePerPidUnload(true))
                    {
                        guiStopTrigger = "unloadRequest";
                        break;
                    }
                    continue;
                }
                if (i == reloadIdx)
                {
                    DWORD gen = 0;
                    if (pReloadGenView)
                    {
                        gen = *static_cast<const DWORD*>(pReloadGenView);
                        const DWORD last = s_lastHandledReloadGen.load(std::memory_order_relaxed);
                        if (gen == last)
                            continue;
                        s_lastHandledReloadGen.store(gen, std::memory_order_relaxed);
                    }

                    DWORD reloadFlags = kReloadRenderFlagAll;
                    if (pReloadFlagsView)
                        reloadFlags = *static_cast<const DWORD*>(pReloadFlagsView);

                    DebugLog(DBG_IMPORTANT,
                        L"[GlassCompat][Core] GlassGuiThread: reload gen=%lu flags=0x%X",
                        static_cast<unsigned long>(gen),
                        reloadFlags);

                    if (g_shutdownStarted.load(std::memory_order_acquire))
                        continue;

                    if ((reloadFlags & kReloadRenderFlagGlassCore) != 0 && g_hCore)
                    {
                        auto fn = reinterpret_cast<GlassCoreOnThemePaintChangedFn>(GetProcAddress(
                            g_hCore, "GlassCore_OnUserThemePaintSettingsChanged"));
                        if (fn)
                        {
                            DebugLog(L"[GlassCompat][Core] GlassGuiThread: reload (gen=%lu) → GlassCore",
                                static_cast<unsigned long>(gen));
                            fn();
                        }
                        using GlassCoreVoidFn = void(WINAPI*)();
                        if ((reloadFlags & kReloadRenderFlagRootBackdropReapply) != 0)
                        {
                            if (auto reapply = reinterpret_cast<GlassCoreVoidFn>(
                                    GetProcAddress(g_hCore, "GlassCore_ReapplyOpenRootBackdrops")))
                            {
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: reload (gen=%lu) → ReapplyOpenRootBackdrops",
                                    static_cast<unsigned long>(gen));
                                reapply();
                            }
                            // ExplorerCompat notify — только из WndCompat sweep (без дубля belt).
                        }
                        if (auto syncEx = reinterpret_cast<GlassCoreVoidFn>(
                                GetProcAddress(g_hCore, "GlassCore_SyncExplorerFrameBackdropHookFromRegistry")))
                            syncEx();
                    }
                    if (g_hCore)
                    {
                        using GlassCoreVoidFn = void(WINAPI*)() noexcept;
                        using GlassCoreScopeFn = void(WINAPI*)(DWORD) noexcept;
                        using GlassCoreScopeLPARAMFn = void(WINAPI*)(LPARAM) noexcept;
                        if ((reloadFlags & kReloadRenderFlagRootLiveBackdropAll) != 0)
                        {
                            if (const auto pfn = reinterpret_cast<GlassCoreScopeFn>(
                                    GetProcAddress(g_hCore, "GlassCore_PostLiveRootBackdropReapply")))
                            {
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → PostLiveRootBackdropReapply(AllTracked)");
                                pfn(0);
                            }
                        }
                        if ((reloadFlags & kReloadRenderFlagRootLiveBackdropApplied) != 0)
                        {
                            if (const auto pfn = reinterpret_cast<GlassCoreScopeFn>(
                                    GetProcAddress(g_hCore, "GlassCore_PostLiveRootBackdropReapply")))
                            {
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → PostLiveRootBackdropReapply(AppliedOnly)");
                                pfn(1);
                            }
                        }
                        if ((reloadFlags & kReloadRenderFlagRenderText) != 0)
                        {
                            if (const auto pfn = reinterpret_cast<GlassCoreVoidFn>(
                                    GetProcAddress(g_hCore, "GlassCore_ReloadRenderTextInkFromRegistry")))
                            {
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → ReloadRenderTextInkFromRegistry");
                                pfn();
                            }
                        }
                        if ((reloadFlags & kReloadRenderFlagRootLiveColorizationApplied) != 0)
                        {
                            if (const auto pfn = reinterpret_cast<GlassCoreScopeLPARAMFn>(
                                    GetProcAddress(g_hCore, "GlassCore_PostLiveRootColorizationToAppliedRoots")))
                            {
                                const LPARAM liveRootScope =
                                    GlassShared::GlassPlatformTask::ReadReloadRootLiveScope();
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → PostLiveRootColorizationToAppliedRoots scope=%ld",
                                    liveRootScope);
                                pfn(liveRootScope);
                            }
                        }
                        if ((reloadFlags & kReloadRenderFlagRootLiveThemePaint) != 0)
                        {
                            if (const auto pfn = reinterpret_cast<GlassCoreVoidFn>(
                                    GetProcAddress(g_hCore, "GlassCore_InvalidateThemedChromeOnTrackedRoots")))
                            {
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → InvalidateThemedChromeOnTrackedRoots");
                                pfn();
                            }
                        }
                    }
                    if ((reloadFlags & kReloadRenderFlagTaskbarSwca) != 0)
                    {
                        if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
                        {
                            using WndCompatVoidFn = void(WINAPI*)() noexcept;
                            if (const auto pfnTray = reinterpret_cast<WndCompatVoidFn>(
                                    GetProcAddress(hWndCompat, "WndCompat_ReapplyTaskbarBackdrops")))
                            {
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → WndCompat_ReapplyTaskbarBackdrops");
                                pfnTray();
                            }
                        }
                    }
                    if ((reloadFlags & kReloadRenderFlagTaskbarXaml) != 0)
                    {
                        if (const HMODULE hTaskbar = GetModuleHandleW(L"TaskbarCompat.dll"))
                        {
                            using TaskbarVoidFn = void(WINAPI*)() noexcept;
                            if (const auto pfnTb = reinterpret_cast<TaskbarVoidFn>(
                                    GetProcAddress(hTaskbar, "TaskbarCompat_ReloadSettingsAndReapply")))
                            {
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → TaskbarCompat_ReloadSettingsAndReapply");
                                pfnTb();
                            }
                        }
                        // CoreWindow (SEH/NC): окно не уничтожается при Hide — нужен reapply.
                        // paintPathXaml=false (DWM) → Restore как при Stop.
                        if (const HMODULE hUi = GetModuleHandleW(L"UICompat.dll"))
                        {
                            using UiVoidFn = void(WINAPI*)() noexcept;
                            if (const auto pfnUi = reinterpret_cast<UiVoidFn>(
                                    GetProcAddress(hUi, "UICompat_ReloadSettingsAndReapply")))
                            {
                                DebugLog(DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → UICompat_ReloadSettingsAndReapply");
                                pfnUi();
                            }
                        }
                    }
                    if ((reloadFlags & kReloadRenderFlagUiThemeBrushRefresh) != 0)
                    {
                        if (const HMODULE hUi = GetModuleHandleW(L"UICompat.dll"))
                        {
                            using UiVoidFn = void(WINAPI*)() noexcept;
                            if (const auto pfnUi = reinterpret_cast<UiVoidFn>(GetProcAddress(
                                    hUi, "UICompat_RefreshBrushesAfterThemeChange")))
                            {
                                DebugLog(
                                    DBG_IMPORTANT,
                                    L"[GlassCompat][Core] GlassGuiThread: → "
                                    L"UICompat_RefreshBrushesAfterThemeChange");
                                pfnUi();
                            }
                        }
                    }
                    if ((reloadFlags & kReloadRenderFlagAttachList) != 0)
                    {
                        DebugLog(DBG_IMPORTANT,
                            L"[GlassCompat][Core] GlassGuiThread: reload (gen=%lu) → AttachList",
                            static_cast<unsigned long>(gen));
                        ReloadAttachListFromRegistry();
                    }
                    // Не continue в обход post-drain StopEvent poll.
                    if (hPlatformStop && WaitForSingleObject(hPlatformStop, 0) == WAIT_OBJECT_0)
                    {
                        break;
                    }
                    continue;
                }
            }

            // Ограниченный drain: при лавине сообщений (Taskmgr) бесконечный PeekMessage
            // никогда не доходит до WM_QUIT/Stop poll (H28).
            // H-R2: в SEH сначала снимаем WM_QUIT (host_post), иначе XAML Dispatch
            // держит GUI секунды после stop.
            bool quit = false;
            MSG quitMsg{};
            if (PeekMessageW(&quitMsg, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE))
            {
                quit = true;
            }
            int pumped = 0;
            constexpr int kMaxPumpPerTurn = 32;
            while (!quit && pumped < kMaxPumpPerTurn && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                ++pumped;
                if (msg.message == WM_QUIT) { quit = true; break; }
                if (msg.message == GlassCompatMsg::WM_GLASS_MODULES_REINJECT)
                {
                    const bool isExplorer =
                        _wcsicmp(GetProcessNameLower().c_str(), L"explorer.exe") == 0;
                    if (isExplorer)
                    {
                        continue;
                    }
                    if (softModuleUnloadDone &&
                        !g_initDone.load(std::memory_order_acquire) &&
                        GlassShared::GlassPlatformTask::IsGlassPlatformRunning())
                    {
                        DoInitialize(g_hSelf);
                        softModuleUnloadDone = false;
                        g_softModulesUnloaded.store(false, std::memory_order_release);
                        g_initDone.store(true, std::memory_order_release);
                    }
                    continue;
                }
                if (msg.message == GlassCompatMsg::WM_GLASS_ENSURE_TASKBAR_COMPAT)
                {
                    TryLoadAndInitTaskbarCompat();
                    continue;
                }
                if (msg.message == GlassCompatMsg::WM_GLASS_ENSURE_EXPLORER_COMPAT)
                {
                    TryLoadAndInitExplorerCompat();
                    continue;
                }
                if (msg.message == GlassCompatMsg::WM_GLASS_ENSURE_UI_COMPAT)
                {
                    TryLoadAndInitUICompat();
                    continue;
                }
                if (msg.message == GlassCompatMsg::WM_GLASS_COREWINDOW_ATTACH_EXISTING)
                {
                    // HR16: после initDone / eager Refresh — патч уже существующих cloaked CW.
                    // H49: не из DoInitialize синхронно; только post на GUI.
                    TryLoadAndInitUICompat();
                    if (g_hWndCompat)
                    {
                        using AttachExistingFn = void(WINAPI*)();
                        static AttachExistingFn s_pfn = nullptr;
                        if (!s_pfn)
                        {
                            s_pfn = reinterpret_cast<AttachExistingFn>(
                                GetProcAddress(g_hWndCompat, "WndCompat_AttachExistingCoreWindows"));
                        }
                        if (s_pfn)
                            s_pfn();
                    }
                    continue;
                }
                if (msg.message == GlassCompatMsg::WM_GLASS_COREWINDOW_REMOTE_SHOW)
                {
                    const HWND cwHwnd = reinterpret_cast<HWND>(msg.wParam);
                    BOOL cloaked = FALSE;
                    const bool isCloaked =
                        cwHwnd
                        && SUCCEEDED(DwmGetWindowAttribute(
                            cwHwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))
                        && cloaked != FALSE;
                    // HR16: cloaked OK (системная анимация). Destroyed / невидимый без cloak — skip.
                    if (!cwHwnd || !IsWindow(cwHwnd))
                        continue;
                    if (!isCloaked && !IsWindowVisible(cwHwnd))
                        continue;
                    TryLoadAndInitUICompat();
                    if (g_hWndCompat)
                    {
                        HandleWinEventFn pfn = g_pfnHandleWinEvent.load(std::memory_order_acquire);
                        if (!pfn)
                        {
                            pfn = reinterpret_cast<HandleWinEventFn>(
                                GetProcAddress(g_hWndCompat, "WndCompat_HandleWinEvent"));
                            if (pfn)
                                g_pfnHandleWinEvent.store(pfn, std::memory_order_release);
                        }
                        if (pfn)
                        {
                            pfn(
                                EVENT_OBJECT_SHOW,
                                cwHwnd,
                                OBJID_WINDOW,
                                CHILDID_SELF,
                                GetCurrentThreadId(),
                                GetTickCount());
                        }
                    }
                    continue;
                }
                // HR3 SEH: DispatchMessage в XAML/COM после NC держит GUI —
                // нет poll Stop/force-flag (лог: host_post ok, нет loop_exit).
                if (cwHostGui)
                    continue;
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (quit)
            {
                guiStopTrigger = "wm_quit";
                break;
            }

            // MsgWait при непустой очереди возвращает "есть сообщения" и НЕ сообщает
            // о сигнале wait-объектов. После drain обязательно проверяем StopEvent.
            if (hPlatformStop && WaitForSingleObject(hPlatformStop, 0) == WAIT_OBJECT_0)
            {
                guiStopTrigger = "stopEvent";
                DebugLog(L"[GlassCompat][Core] GlassGuiThread: stop event (post-drain poll), initiating shutdown");
                break;
            }

            // Наблюдение за хостом выполняется всегда:
            // это покрывает ручной stop задачи в Планировщике, когда /stop может не вызываться.
            HostLivenessReason lr = HostLivenessReason::Alive;
            if (!CheckHostAlive(hostWatchPid, hostWatchProcess, lr))
            {
                // H51/H52: AppContainer SEH — OpenProcess/PID watch ложно «убивает» хост
                // (лог: hostLiveness:true @55855, host_stop @60016). Пока StopEvent жив и
                // не сигнален — только Stop/WM_QUIT; иначе ранний ExecuteShutdown + UI residual.
                //
                // Бесконечный сброс счётчика больше не запирает процесс: настоящая смерть
                // хоста приходит отдельным сигналом через мьютекс HostAlive выше.
                const bool cwDeferToStop =
                    Shared::RootBackdropPolicy::IsCoreWindowHostProcess()
                    && hPlatformStop
                    && WaitForSingleObject(hPlatformStop, 0) != WAIT_OBJECT_0;
                if (cwDeferToStop)
                {
                    hostDeadConfirmTicks = 0;
                }
                else
                {
                    ++hostDeadConfirmTicks;
                    if (hostDeadConfirmTicks >= kHostDeadConfirmNeed)
                    {
                        shutdownByHostLiveness = true;
                        hostDeadReason = lr;
                        guiStopTrigger = "hostLiveness";
                        DebugLog(DBG_IMPORTANT,
                            L"[GlassCompat][HostWatch] host dead detected: reason=%s pid=%lu",
                            HostLivenessReasonToString(lr),
                            static_cast<unsigned long>(hostWatchPid));
                        break;
                    }
                }
            }
            else
            {
                hostDeadConfirmTicks = 0;
            }
        }

        if (pReloadGenView)
            UnmapViewOfFile(pReloadGenView);
        if (pReloadFlagsView)
            UnmapViewOfFile(pReloadFlagsView);
        if (hReloadGenMap)
            CloseHandle(hReloadGenMap);
        if (hReloadFlagsMap)
            CloseHandle(hReloadFlagsMap);
        if (hStopQuitCancel)
            SetEvent(hStopQuitCancel);
        if (hStopQuitThread)
        {
            WaitForSingleObject(hStopQuitThread, 2000);
            CloseHandle(hStopQuitThread);
            hStopQuitThread = nullptr;
        }
        if (hStopQuitCancel)
        {
            CloseHandle(hStopQuitCancel);
            hStopQuitCancel = nullptr;
        }
        if (hPlatformStop) CloseHandle(hPlatformStop);
        if (hHostAlive) CloseHandle(hHostAlive);
        if (hUnloadRequest) CloseHandle(hUnloadRequest);
        if (hReloadSemaphore) CloseHandle(hReloadSemaphore);
        if (hostWatchProcess) CloseHandle(hostWatchProcess);
        // hHostShutdownEarly держим до Wait после COMPLETE (H71).

        bool expectedShutdownStart = false;
        if (g_shutdownStarted.compare_exchange_strong(
                expectedShutdownStart, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            if (shutdownByHostLiveness)
            {
                DebugLog(DBG_IMPORTANT,
                    L"[GlassCompat][Core] GlassGuiThread: stopping by HostLivenessLost (reason=%s)",
                    HostLivenessReasonToString(hostDeadReason));
            }
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H90",
                "GlassCompat.cpp:GlassGuiThreadProc",
                "stop_trigger",
                "\"what\":\"%s\",\"host_liveness\":%d,\"soft_done\":%d",
                guiStopTrigger,
                shutdownByHostLiveness ? 1 : 0,
                softModuleUnloadDone ? 1 : 0);
            // #endregion
            // Soft unload уже снимал peer-модули — полный ExecuteShutdown не нужен.
            // UICompat мог вернуться через Ensure после soft → добить Restore/Unadvise.
            if (!softModuleUnloadDone)
            {
                g_ultraceShutdownReason = guiStopTrigger;
                ExecuteShutdown();
            }
            else if (GetModuleHandleW(L"UICompat.dll"))
            {
                DebugLog(L"[GlassCompat][Core] GlassGuiThread: soft-done — UICompat_Shutdown residual");
                if (const HMODULE hUi = GetModuleHandleW(L"UICompat.dll"))
                {
                    using UiShutdownFn = BOOL(WINAPI*)();
                    if (const auto pfn = reinterpret_cast<UiShutdownFn>(
                            GetProcAddress(hUi, "UICompat_Shutdown")))
                        (void)pfn();
                }
            }
        }
        else
        {
            DebugLog(DBG_WARNING,
                L"[GlassCompat][Core] GlassGuiThread: secondary stop trigger ignored (shutdown already started)");
        }

        g_shutdownComplete.store(true, std::memory_order_release);
        if (g_shutdownEvent)
            SetEvent(g_shutdownEvent);

        DebugLog(L"[GlassCompat][Core] GlassGuiThread: shutdown complete");

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER) && defined(_DEBUG)
        Shared::Log::GlassLogClient::Instance().Stop();
#endif

        // H71: path=1 ждёт HostShutdown на handle, открытом при старте GUI-потока.
        // Не OpenEvent после COMPLETE (H70 → WAIT_FAILED, гонка с Close на хосте).
        // H51: даже при HostLivenessLost ждём, если HostShutdown ещё не сигнален —
        // иначе ложный OpenProcessFailed в AppContainer даёт FreeSelf до Unhook.
        // Не ReadHostPid()==0 как успех (H55/H56). Без DispatchMessage (H68/H69).
        {
            if (hHostShutdownEarly)
            {
                // #region agent log
                // GLASS-ULTRACE (R2, H82): после смерти хоста HostShutdown не приходит никогда —
                // здесь процесс висит 30 с, держа GlassCompat (наблюдение «модуль остался»).
                const BOOL already = (WaitForSingleObject(hHostShutdownEarly, 0) == WAIT_OBJECT_0);
                const ULONGLONG hsT0 = GetTickCount64();
                GlassUlTrace::Logf(
                    "H82",
                    "GlassCompat.cpp:GlassGuiThreadProc",
                    "hostshutdown_wait",
                    "\"already_signaled\":%d,\"by_host_liveness\":%d",
                    already ? 1 : 0,
                    shutdownByHostLiveness ? 1 : 0);
                // #endregion
                if (!already)
                    (void)WaitForSingleObject(hHostShutdownEarly, 30000);
                // #region agent log
                // GLASS-ULTRACE (H82)
                GlassUlTrace::Logf(
                    "H82",
                    "GlassCompat.cpp:GlassGuiThreadProc",
                    "hostshutdown_done",
                    "\"signaled\":%d,\"ms\":%llu,\"rc\":%u",
                    (WaitForSingleObject(hHostShutdownEarly, 0) == WAIT_OBJECT_0) ? 1 : 0,
                    GetTickCount64() - hsT0,
                    GlassUlTrace::QueryLoadCountByName(L"GlassCompat.dll"));
                // #endregion
                CloseHandle(hHostShutdownEarly);
                hHostShutdownEarly = nullptr;
            }

            // H72: после HostShutdown OS-pin ещё может быть — будим ВСЕ потоки процесса
            // (WM_NULL) + WinEvent-callback targets, плюс alertable SleepEx (как 23.07: 8 раундов).
            constexpr int drainRounds = 8;
            for (int round = 0; round < drainRounds; ++round)
            {
                (void)WakeAllProcessThreadsWmNull();
                (void)WakeWinEventCallbackThreads();
                SleepEx(50, TRUE);
            }
        }

        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&GlassCompat_GuiThreadId), 0);

        // FreeLibraryAndExitThread — атомарно освобождает нашу явную ссылку на GlassCompat.dll
        // и завершает поток. Это единственный безопасный способ: нельзя вызвать FreeLibrary
        // и затем return, так как после FreeLibrary DLL может быть немедленно размапирована
        // и код ниже окажется недоступен.
        //
        // К моменту вызова ExecuteShutdown() уже завершён, все in-flight вызовы
        // GlassCompat_WinEventProc закончились (UnhookWinEvent снял хук до или во время
        // нашего shutdown). Поэтому падение рефкаунта до 0 здесь безопасно.
        if (hSelfRef)
            FreeSelfAndExitThread(hSelfRef, 1);

        return 0;
    }

    void StartCwUnloadWatcher() noexcept
    {
        static std::once_flag s_cwUnloadOnce;
        std::call_once(s_cwUnloadOnce, []() noexcept
        {
            if (!Shared::RootBackdropPolicy::IsCoreWindowHostProcess())
                return;

            HMODULE hRef = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(&StartCwUnloadWatcher),
                    &hRef))
            {
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H87",
                    "GlassCompat.cpp:StartCwUnloadWatcher",
                    "watcher_exit",
                    "\"kind\":\"cw\",\"branch\":\"pin_fail\",\"pin_released\":0");
                // #endregion
                return;
            }
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H87",
                "GlassCompat.cpp:StartCwUnloadWatcher",
                "watcher_start",
                "\"kind\":\"cw\",\"handle\":\"%p\",\"rc\":%u",
                static_cast<void*>(hRef),
                GlassUlTrace::QueryLoadCount(hRef));
            // #endregion

            HANDLE hThread = CreateThread(
                nullptr,
                0,
                [](LPVOID pv) -> DWORD
                {
                    const HMODULE hPin = static_cast<HMODULE>(pv);

                    HANDLE hStop = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
                    // H54: Stop Wait в SEH не будит — ждём ещё Local\GlassCompat.CwForce.<pid>.
                    wchar_t forceName[80]{};
                    swprintf_s(
                        forceName,
                        L"Local\\GlassCompat.CwForce.%u",
                        GetCurrentProcessId());
                    HANDLE hForce = OpenEventW(SYNCHRONIZE, FALSE, forceName);

                    // Named Wait в SEH ненадёжен (force open ok, SetEvent не будит).
                    // HR5: только poll Stop/Force/flag — без WaitForMultipleObjects
                    // (лог: flag=1 WPM, wake через ~10s только по stop).
                    DWORD wake = WAIT_TIMEOUT;
                    const char* wakeSrc = "complete";
                    for (;;)
                    {
                        if (InterlockedCompareExchange(&GlassCompat_CwForceUnloadFlag, 0, 0) != 0)
                        {
                            wakeSrc = "wpm_flag";
                            wake = 0;
                            break;
                        }
                        if (!hForce)
                            hForce = OpenEventW(SYNCHRONIZE, FALSE, forceName);
                        if (hStop && WaitForSingleObject(hStop, 0) == WAIT_OBJECT_0)
                        {
                            wakeSrc = "stop";
                            wake = 0;
                            break;
                        }
                        if (hForce && WaitForSingleObject(hForce, 0) == WAIT_OBJECT_0)
                        {
                            wakeSrc = "force";
                            wake = 0;
                            break;
                        }
                        if (g_shutdownComplete.load(std::memory_order_acquire))
                        {
                            wakeSrc = "complete";
                            break;
                        }
                        if (ReadHostPid() == 0)
                        {
                            wakeSrc = "pid_gone";
                            break;
                        }
                        Sleep(50);
                    }
                    if (hStop)
                        CloseHandle(hStop);
                    if (hForce)
                        CloseHandle(hForce);

                    // #region agent log
                    // GLASS-ULTRACE
                    GlassUlTrace::Logf(
                        "H87",
                        "GlassCompat.cpp:StartCwUnloadWatcher",
                        "watcher_wake",
                        "\"kind\":\"cw\",\"trigger\":\"%s\",\"wake\":%lu",
                        wakeSrc,
                        static_cast<unsigned long>(wake));
                    // #endregion

                    // HR3: сначала WM_QUIT, потом лог — CreateFile в AC раньше
                    // блокировал PostThreadMessage (нет stop_seen / residual SEH).
                    if (g_guiThreadId != 0)
                        (void)PostThreadMessageW(g_guiThreadId, WM_QUIT, 0, 0);

                    // H54: SEH теперь видит Stop (wake:stop) и доходит до ExecuteShutdown,
                    // но FreeSelf ещё ждёт HostShutdown+H72 (~секунды). Watcher не
                    // TerminateThread(GUI) и не FLAET, пока GUI жив — иначе AV в
                    // <Unloaded_GlassCompat> (ShellHost dump 18.08).
                    const bool shutdownAlready =
                        g_shutdownStarted.load(std::memory_order_acquire)
                        || g_shutdownComplete.load(std::memory_order_acquire);
                    const int waitIters = shutdownAlready ? 600 : 120; // 30s / 6s
                    for (int i = 0; i < waitIters; ++i)
                    {
                        if (g_shutdownComplete.load(std::memory_order_acquire))
                        {
                            // #region agent log
                            // GLASS-ULTRACE
                            GlassUlTrace::Logf(
                                "H87",
                                "GlassCompat.cpp:StartCwUnloadWatcher",
                                "watcher_exit",
                                "\"kind\":\"cw\",\"branch\":\"wait_complete\",\"pin_released\":1,\"shutdownStarted\":1,\"shutdownComplete\":1,\"guiTid\":%lu,\"loaded\":1",
                                static_cast<unsigned long>(GlassCompat_GuiThreadId));
                            // #endregion
                            FreeLibrary(hPin);
                            return 0;
                        }
                        if (GetModuleHandleW(L"GlassCompat.dll") == nullptr)
                        {
                            // #region agent log
                            // GLASS-ULTRACE
                            GlassUlTrace::Logf(
                                "H87",
                                "GlassCompat.cpp:StartCwUnloadWatcher",
                                "watcher_exit",
                                "\"kind\":\"cw\",\"branch\":\"wait_unloaded\",\"pin_released\":1,\"guiTid\":%lu,\"loaded\":0",
                                static_cast<unsigned long>(GlassCompat_GuiThreadId));
                            // #endregion
                            FreeLibrary(hPin);
                            return 0;
                        }
                        // GUI уже в path=1 — не форсим.
                        if (g_shutdownStarted.load(std::memory_order_acquire) && i + 1 == waitIters)
                            break;
                        Sleep(50);
                    }

                    if (g_shutdownStarted.load(std::memory_order_acquire)
                        || g_shutdownComplete.load(std::memory_order_acquire))
                    {
                        for (int i = 0; i < 600; ++i)
                        {
                            if (g_shutdownComplete.load(std::memory_order_acquire)
                                || GetModuleHandleW(L"GlassCompat.dll") == nullptr)
                                break;
                            Sleep(50);
                        }
                        // #region agent log
                        // GLASS-ULTRACE
                        GlassUlTrace::Logf(
                            "H87",
                            "GlassCompat.cpp:StartCwUnloadWatcher",
                            "watcher_exit",
                            "\"kind\":\"cw\",\"branch\":\"already_started\",\"pin_released\":1,\"shutdownStarted\":1,\"guiTid\":%lu,\"loaded\":%d",
                            static_cast<unsigned long>(GlassCompat_GuiThreadId),
                            GetModuleHandleW(L"GlassCompat.dll") ? 1 : 0);
                        // #endregion
                        FreeLibrary(hPin);
                        return 0;
                    }

                    bool expected = false;
                    if (g_shutdownStarted.compare_exchange_strong(
                            expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
                    {
                        // #region agent log
                        // GLASS-ULTRACE
                        GlassUlTrace::Logf(
                            "H90",
                            "GlassCompat.cpp:StartCwUnloadWatcher",
                            "stop_trigger",
                            "\"what\":\"%s\",\"kind\":\"cw\"",
                            wakeSrc);
                        // #endregion
                        g_ultraceShutdownReason = wakeSrc;
                        ExecuteShutdown();
                    }

                    // HostShutdown обычно уже signaled (ForceUnload после него на хосте).
                    if (HANDLE hHs = OpenEventW(
                            SYNCHRONIZE,
                            FALSE,
                            GlassShared::GlassPlatformTask::kHostShutdownEventName))
                    {
                        (void)WaitForSingleObject(hHs, 5000);
                        CloseHandle(hHs);
                    }

                    const DWORD guiTid = g_guiThreadId;
                    if (guiTid != 0)
                    {
                        (void)PostThreadMessageW(guiTid, WM_QUIT, 0, 0);
                        (void)WakeAllProcessThreadsWmNull();
                        (void)WakeWinEventCallbackThreads();
                        if (HANDLE hGui = OpenThread(
                                THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, guiTid))
                        {
                            for (int i = 0; i < 40; ++i)
                            {
                                DWORD ec = 0;
                                if (!GetExitCodeThread(hGui, &ec) || ec != STILL_ACTIVE)
                                    break;
                                if (g_shutdownComplete.load(std::memory_order_acquire)
                                    || GetModuleHandleW(L"GlassCompat.dll") == nullptr)
                                    break;
                                (void)PostThreadMessageW(guiTid, WM_QUIT, 0, 0);
                                (void)WakeWinEventCallbackThreads();
                                Sleep(50);
                            }
                            CloseHandle(hGui);
                        }
                    }

                    // GUI сам path=1 FreeSelf, либо ещё жив — FLAET из watcher запрещён.
                    if (g_shutdownComplete.load(std::memory_order_acquire)
                        || GetModuleHandleW(L"GlassCompat.dll") == nullptr
                        || GlassCompat_GuiThreadId != 0)
                    {
                        DebugLog(
                            DBG_IMPORTANT,
                            L"[GlassCompat][CW] watcher skip FreeSelf (complete=%d loaded=%d guiTid=%u)",
                            g_shutdownComplete.load(std::memory_order_acquire) ? 1 : 0,
                            GetModuleHandleW(L"GlassCompat.dll") ? 1 : 0,
                            static_cast<unsigned>(GlassCompat_GuiThreadId));
                        // #region agent log
                        // GLASS-ULTRACE
                        GlassUlTrace::Logf(
                            "H87",
                            "GlassCompat.cpp:StartCwUnloadWatcher",
                            "watcher_exit",
                            "\"kind\":\"cw\",\"branch\":\"skip_freeself\",\"pin_released\":1,\"shutdownComplete\":%d,\"guiTid\":%lu,\"loaded\":%d",
                            g_shutdownComplete.load(std::memory_order_acquire) ? 1 : 0,
                            static_cast<unsigned long>(GlassCompat_GuiThreadId),
                            GetModuleHandleW(L"GlassCompat.dll") ? 1 : 0);
                        // #endregion
                        FreeLibrary(hPin);
                        return 0;
                    }

                    g_shutdownComplete.store(true, std::memory_order_release);
                    if (g_shutdownEvent)
                        SetEvent(g_shutdownEvent);

                    // #region agent log
                    // GLASS-ULTRACE
                    GlassUlTrace::Logf(
                        "H87",
                        "GlassCompat.cpp:StartCwUnloadWatcher",
                        "watcher_exit",
                        "\"kind\":\"cw\",\"branch\":\"freeself\",\"pin_released\":0,\"guiTid\":%lu,\"loaded\":%d",
                        static_cast<unsigned long>(GlassCompat_GuiThreadId),
                        GetModuleHandleW(L"GlassCompat.dll") ? 1 : 0);
                    // #endregion
                    FreeSelfAndExitThread(hPin, 1);
                    return 0;
                },
                hRef,
                0,
                nullptr);

            if (!hThread)
            {
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H87",
                    "GlassCompat.cpp:StartCwUnloadWatcher",
                    "watcher_exit",
                    "\"kind\":\"cw\",\"branch\":\"create_fail\",\"pin_released\":1");
                // #endregion
                FreeLibrary(hRef);
                return;
            }
            CloseHandle(hThread);
        });
    }

    // INCONTEXT грузит DLL даже в Forbidden/BlockList процессы без GlassGuiThread.
    // Без явного pin+Stop-watcher OS-pin часто остаётся (Taskmgr: mode=0, 7k WinEvent без FreeSelf).
    void LogMinimalUnload(const wchar_t* fmt, ...) noexcept
    {
        wchar_t buf[384]{};
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
        va_end(args);
        OutputDebugStringW(buf);
        OutputDebugStringW(L"\n");
        // Без DebugLog: иначе path=2 поднимает pipe-воркер в BlockList-процессах
        // (msedgewebview2/taskhostw), и при reboot joinable std::thread даёт abort().
    }

    struct MinimalUnloadPack
    {
        HMODULE module{};
        ULONGLONG path{};
    };

    void StartMinimalUnloadWatcher(ULONGLONG path) noexcept
    {
        static std::once_flag s_minimalUnloadOnce;
        std::call_once(s_minimalUnloadOnce, [path]() noexcept
        {
            const auto kind = (path == 3)
                ? Shared::InjectProcessMap::Kind::RejectedForbidden
                : Shared::InjectProcessMap::Kind::RejectedBlockList;
            InjectMapUpsert(
                kind,
                static_cast<std::uint8_t>(path),
                Shared::InjectProcessMap::State::Alive,
                Shared::InjectProcessMap::ModuleCompat);

            HMODULE hRef = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                    reinterpret_cast<LPCWSTR>(&StartMinimalUnloadWatcher),
                    &hRef))
            {
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H87",
                    "GlassCompat.cpp:StartMinimalUnloadWatcher",
                    "watcher_exit",
                    "\"kind\":\"minimal\",\"path\":%llu,\"branch\":\"pin_fail\",\"pin_released\":0",
                    path);
                // #endregion
                return;
            }
            // #region agent log
            // GLASS-ULTRACE
            GlassUlTrace::Logf(
                "H87",
                "GlassCompat.cpp:StartMinimalUnloadWatcher",
                "watcher_start",
                "\"kind\":\"minimal\",\"path\":%llu,\"handle\":\"%p\",\"rc\":%u",
                path,
                static_cast<void*>(hRef),
                GlassUlTrace::QueryLoadCount(hRef));
            // #endregion

            auto* pack = new (std::nothrow) MinimalUnloadPack{ hRef, path };
            if (!pack)
            {
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H87",
                    "GlassCompat.cpp:StartMinimalUnloadWatcher",
                    "watcher_exit",
                    "\"kind\":\"minimal\",\"path\":%llu,\"branch\":\"pack_fail\",\"pin_released\":1",
                    path);
                // #endregion
                FreeLibrary(hRef);
                return;
            }

            HANDLE hThread = CreateThread(
                nullptr,
                0,
                [](LPVOID pv) -> DWORD
                {
                    auto* p = static_cast<MinimalUnloadPack*>(pv);
                    const HMODULE hMod = p->module;
                    const ULONGLONG watcherPath = p->path;
                    delete p;

                    const std::wstring exe = GetProcessNameLower();
                    LogMinimalUnload(
                        L"[GlassCompat][MinimalUnload] start pid=%u exe=%s path=%llu",
                        GetCurrentProcessId(),
                        exe.c_str(),
                        watcherPath);

                    HANDLE hStop = OpenEventW(SYNCHRONIZE, FALSE, kStopEventName);
                    HANDLE hUnload = Shared::InjectProcessMap::CreateUnloadRequestEvent(nullptr);
                    HANDLE hHostShutdown = OpenEventW(
                        SYNCHRONIZE,
                        FALSE,
                        GlassShared::GlassPlatformTask::kHostShutdownEventName);
                    // Dead-man switch: при убитом хосте StopEvent и HostShutdown не придут
                    // никогда, и watcher висел до таймаута в каждом процессе.
                    HANDLE hHostAlive = OpenMutexW(
                        SYNCHRONIZE,
                        FALSE,
                        GlassShared::GlassPlatformTask::kHostAliveMutexName);
                    if (hStop || hUnload || hHostAlive)
                    {
                        HANDLE waits[3]{};
                        DWORD n = 0;
                        unsigned stopIdx = UINT_MAX;
                        unsigned unloadIdx = UINT_MAX;
                        unsigned hostAliveIdx = UINT_MAX;
                        if (hStop)
                        {
                            stopIdx = n;
                            waits[n++] = hStop;
                        }
                        if (hUnload)
                        {
                            unloadIdx = n;
                            waits[n++] = hUnload;
                        }
                        if (hHostAlive)
                        {
                            hostAliveIdx = n;
                            waits[n++] = hHostAlive;
                        }

                        const DWORD wr = WaitForMultipleObjects(n, waits, FALSE, INFINITE);
                        const unsigned signaled = (wr >= WAIT_ABANDONED_0 && wr < WAIT_ABANDONED_0 + n)
                            ? (wr - WAIT_ABANDONED_0)
                            : (wr - WAIT_OBJECT_0);
                        const bool fromUnload =
                            (unloadIdx != UINT_MAX && signaled == unloadIdx);
                        const bool hostDead =
                            (hostAliveIdx != UINT_MAX && signaled == hostAliveIdx);
                        if (hostDead)
                        {
                            // Передать сигнал следующему ожидающему процессу.
                            (void)ReleaseMutex(hHostAlive);
                        }

                        const wchar_t* trigger = L"stop";
                        if (wr == WAIT_FAILED)
                            trigger = L"wait_failed";
                        else if (hostDead)
                            trigger = L"hostAlive";
                        else if (fromUnload)
                            trigger = L"unload";
                        LogMinimalUnload(
                            L"[GlassCompat][MinimalUnload] wake trigger=%s hostDead=%d fromUnload=%d wr=0x%08X",
                            trigger,
                            hostDead ? 1 : 0,
                            fromUnload ? 1 : 0,
                            wr);
                        // #region agent log
                        // GLASS-ULTRACE
                        GlassUlTrace::Logf(
                            "H87",
                            "GlassCompat.cpp:StartMinimalUnloadWatcher",
                            "watcher_wake",
                            "\"kind\":\"minimal\",\"path\":%llu,\"trigger\":\"%s\",\"hostDead\":%d,\"fromUnload\":%d,\"wr\":%lu",
                            watcherPath,
                            (wr == WAIT_FAILED) ? "wait_failed" : (hostDead ? "hostAlive" : (fromUnload ? "unloadRequest" : "stopEvent")),
                            hostDead ? 1 : 0,
                            fromUnload ? 1 : 0,
                            static_cast<unsigned long>(wr));
                        // #endregion

                        if (hStop)
                            CloseHandle(hStop);
                        if (hUnload)
                            CloseHandle(hUnload);

                        (void)Shared::InjectProcessMap::TrySetState(
                            GetCurrentProcessId(),
                            Shared::InjectProcessMap::State::Unloading);

                        // H58: на глобальном Stop ждём HostShutdown; per-PID unload — FreeSelf сразу.
                        // Хост уже мёртв — HostShutdown не придёт, ждать нечего.
                        if (!fromUnload && !hostDead && hHostShutdown)
                        {
                            HANDLE shutdownWaits[2]{ hHostShutdown, hHostAlive };
                            const DWORD n2 = hHostAlive ? 2u : 1u;
                            const DWORD sr =
                                WaitForMultipleObjects(n2, shutdownWaits, FALSE, 30000);
                            if (hHostAlive
                                && (sr == WAIT_OBJECT_0 + 1 || sr == WAIT_ABANDONED_0 + 1))
                            {
                                (void)ReleaseMutex(hHostAlive);
                            }
                            LogMinimalUnload(
                                L"[GlassCompat][MinimalUnload] HostShutdown wait result=0x%08X",
                                sr);
                            // #region agent log
                            // GLASS-ULTRACE
                            GlassUlTrace::Logf(
                                "H87",
                                "GlassCompat.cpp:StartMinimalUnloadWatcher",
                                "watcher_wake",
                                "\"kind\":\"minimal\",\"path\":%llu,\"h58\":\"waited\",\"sr\":%lu",
                                watcherPath,
                                static_cast<unsigned long>(sr));
                            // #endregion
                        }
                        else
                        {
                            LogMinimalUnload(
                                L"[GlassCompat][MinimalUnload] HostShutdown wait skipped hostDead=%d fromUnload=%d hasEvent=%d",
                                hostDead ? 1 : 0,
                                fromUnload ? 1 : 0,
                                hHostShutdown ? 1 : 0);
                        }

                        (void)WakeWinEventCallbackThreads();
                        DrainBeforeFreeSelf();
                    }
                    else
                    {
                        LogMinimalUnload(
                            L"[GlassCompat][MinimalUnload] fallback PID poll (no Stop/Unload/HostAlive handles)");
                        while (ReadHostPid() != 0)
                            Sleep(500);
                        LogMinimalUnload(
                            L"[GlassCompat][MinimalUnload] wake trigger=pid_gone");
                    }
                    if (hHostShutdown)
                        CloseHandle(hHostShutdown);
                    if (hHostAlive)
                        CloseHandle(hHostAlive);

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
                    Shared::Log::GlassLogClient::Instance().Stop();
#endif
                    LogMinimalUnload(
                        L"[GlassCompat][MinimalUnload] FreeSelf path=%llu pid=%u",
                        watcherPath,
                        GetCurrentProcessId());
                    // #region agent log
                    // GLASS-ULTRACE
                    GlassUlTrace::Logf(
                        "H87",
                        "GlassCompat.cpp:StartMinimalUnloadWatcher",
                        "watcher_exit",
                        "\"kind\":\"minimal\",\"path\":%llu,\"branch\":\"freeself\",\"pin_released\":0",
                        watcherPath);
                    // #endregion
                    FreeSelfAndExitThread(hMod, watcherPath);
                },
                pack,
                0,
                nullptr);

            if (!hThread)
            {
                // #region agent log
                // GLASS-ULTRACE
                GlassUlTrace::Logf(
                    "H87",
                    "GlassCompat.cpp:StartMinimalUnloadWatcher",
                    "watcher_exit",
                    "\"kind\":\"minimal\",\"path\":%llu,\"branch\":\"create_fail\",\"pin_released\":1",
                    path);
                // #endregion
                FreeLibrary(hRef);
                delete pack;
                return;
            }
            CloseHandle(hThread);
        });
    }

    void StartGuiThreadIfNeeded()
    {
        // Если FreeLibraryAndExitThread снял наш pin, но OS-pin остался (Taskmgr),
        // DLL жива со static-состоянием и мёртвым GUI-потоком — без resurrect
        // второй Stop никогда не получит watcher (H27).
        if (g_guiThreadHandle)
        {
            const DWORD wr = WaitForSingleObject(g_guiThreadHandle, 0);
            if (wr == WAIT_TIMEOUT)
                return;

            CloseHandle(g_guiThreadHandle);
            g_guiThreadHandle = nullptr;
            g_guiThreadId = 0;
        }

        g_guiGeneration.fetch_add(1, std::memory_order_acq_rel);
        g_shutdownStarted.store(false, std::memory_order_release);
        g_shutdownComplete.store(false, std::memory_order_release);
        if (!g_shutdownEvent)
            g_shutdownEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        else
            ResetEvent(g_shutdownEvent);

        g_guiThreadHandle = CreateThread(
            nullptr,
            0,
            GlassGuiThreadProc,
            nullptr,
            0,
            &g_guiThreadId
        );

        const DWORD createThreadError = g_guiThreadHandle ? ERROR_SUCCESS : GetLastError();

        if (!g_guiThreadHandle)
        {
            DebugLog(L"[GlassCompat][Core] FAILED to create GlassGuiThread");
        }
        else
        {
            DebugLog(L"[GlassCompat][Core] GlassGuiThread created");
        }
    }
} // anonymous namespace

// ---------------- Публичный namespace ----------------

namespace GlassCompat
{
    bool Initialize(HMODULE hSelf)
    {
        bool expected = false;
        if (!g_initialized.compare_exchange_strong(expected, true))
        {
            ProcessBlockList::Initialize();
            const bool blocked = ProcessBlockList::IsBlocked(GetProcessNameLower());
            if (blocked)
                return false;
            // Уже пытались: Forbidden держит g_initialized=true без GUI (без retry-спама).
            return g_mode != Shared::InjectionMode::Forbidden;
        }

        g_hSelf = hSelf ? hSelf : GetModuleHandleW(L"GlassCompat.dll");
        g_mode = ReadModeFromRegistry();

        ProcessBlockList::Initialize();
        if (ProcessBlockList::IsBlocked(GetProcessNameLower()))
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[GlassCompat][Core] Initialize: process BLOCKLISTED — minimal watcher only");
            StartMinimalUnloadWatcher(2);
            return false;
        }

        if (g_mode == Shared::InjectionMode::Forbidden)
        {
            DebugLog(L"[GlassCompat][Core] Initialize: process FORBIDDEN — lifetime watcher only");
            // Не сбрасываем g_initialized: иначе каждый WinEvent снова читает AttachList
            // (Taskmgr: 7000+ mode resolved за сессию) и так и не получает unload-watcher.
            StartMinimalUnloadWatcher(3);
            return false;
        }

        DebugLog(std::format(
            L"[GlassCompat][Core] Initialize: BEGIN (hSelf=0x{:X})",
            reinterpret_cast<uintptr_t>(g_hSelf)
        ).c_str());

        // Normal path=1: UEF/VEH только здесь. BlockList не вызывает Initialize.
        GlassCompat::InstallProcessCrashGuard();

        StartGuiThreadIfNeeded();

        DebugLog(L"[GlassCompat][Core] Initialize: END");
        return true;
    }

    bool RequestShutdown()
    {
        // H-R2/HR3: флаг + WM_QUIT до любого лога (CreateFile в AC блокировал thrExit:259).
        InterlockedExchange(&GlassCompat_CwForceUnloadFlag, 1);
        if (g_guiThreadId != 0)
            (void)PostThreadMessageW(g_guiThreadId, WM_QUIT, 0, 0);

        bool expected = false;
        if (!g_shutdownRequested.compare_exchange_strong(expected, true))
        {
            return true;
        }

        return true;
    }
}

// ---------------- EXPORTS ----------------

// H54: SEH не будит Wait на Stop/CwForce named events (лог: force:true, нет stop_seen).
// Хост пишет 1 через WriteProcessMemory — watcher polling, без CreateRemoteThread.
extern "C" __declspec(dllexport) volatile LONG GlassCompat_CwForceUnloadFlag = 0;
extern "C" __declspec(dllexport) volatile DWORD GlassCompat_GuiThreadId = 0;

extern "C"
__declspec(dllexport) BOOL WINAPI GlassCompat_Initialize(HMODULE hModule)
{
    GlassCompat::Initialize(hModule);
    return TRUE;
}

extern "C"
__declspec(dllexport) DWORD WINAPI GlassCompat_RemoteBootstrap(LPVOID)
{
    const bool remoteBefore = g_remoteLoadLibraryPin.load(std::memory_order_acquire);
    const unsigned rcBefore = GlassUlTrace::QueryLoadCountByName(L"GlassCompat.dll");
    g_remoteLoadLibraryPin.store(true, std::memory_order_release);

    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&GlassCompat_RemoteBootstrap),
        &hSelf);

    const bool ok = GlassCompat::Initialize(hSelf);
    const DWORD exitCode = !ok ? 0u : (g_guiThreadId != 0 ? g_guiThreadId : 1u);
    // #region agent log
    // GLASS-ULTRACE
    GlassUlTrace::Logf(
        "H80",
        "GlassCompat.cpp:GlassCompat_RemoteBootstrap",
        "bootstrap",
        "\"remote_flag_before\":%d,\"remote_flag_after\":%d,\"rc_before\":%u,\"rc_after\":%u,\"ok\":%d,\"exit\":%lu",
        remoteBefore ? 1 : 0,
        g_remoteLoadLibraryPin.load(std::memory_order_acquire) ? 1 : 0,
        rcBefore,
        GlassUlTrace::QueryLoadCount(hSelf),
        ok ? 1 : 0,
        static_cast<unsigned long>(exitCode));
    // #endregion

    // 1 = init принят (GUI/watcher стартовал или уже был).
    // H54: хост кэширует GUI tid из exit code (AppContainer mapping Local\ ему не виден).
    return exitCode;
}

extern "C"
__declspec(dllexport) DWORD WINAPI GlassCompat_RemoteCoreWindowShow(LPVOID hwndAsPtr)
{
    const HWND hWnd = reinterpret_cast<HWND>(hwndAsPtr);

    if (!hWnd || !IsWindow(hWnd))
        return 0;

    if (!Shared::RootBackdropPolicy::IsCoreWindowHostProcess())
        return 0;

    // HR8: не вызывать Initialize с CRT — гонка с DoInitialize (лог @82458).
    // Только Post; если GUI ещё нет — хост повторит через post_gui после bootstrap.
    const DWORD tid = g_guiThreadId;
    if (tid == 0)
    {
        return 2;
    }

    if (!PostThreadMessageW(
            tid,
            GlassCompatMsg::WM_GLASS_COREWINDOW_REMOTE_SHOW,
            reinterpret_cast<WPARAM>(hWnd),
            0))
    {
        return 3;
    }

    return 1;
}

extern "C"
__declspec(dllexport) BOOL WINAPI GlassCompat_RequestShutdown(HMODULE hSelf)
{
    GlassCompat::RequestShutdown();
    return TRUE;
}

extern "C"
__declspec(dllexport) BOOL WINAPI GlassCompat_ReloadAttachList() noexcept
{
    ReloadAttachListFromRegistry();
    return TRUE;
}

extern "C"
__declspec(dllexport) void WINAPI GlassCompat_NotifyExplorerRootBackdropPolicyChanged(BOOL enabled) noexcept
{
    NotifyExplorerRootBackdropPolicyImpl(enabled);
}

extern "C"
__declspec(dllexport) void WINAPI GlassCompat_EnsureTaskbarCompatLoaded() noexcept
{
    EnsureTaskbarCompatLoadedImpl();
}

extern "C"
__declspec(dllexport) void WINAPI GlassCompat_EnsureExplorerCompatLoaded() noexcept
{
    EnsureExplorerCompatLoadedImpl();
}

extern "C"
__declspec(dllexport) void WINAPI GlassCompat_EnsureUICompatLoaded() noexcept
{
    EnsureUICompatLoadedImpl();
}

extern "C"
__declspec(dllexport) DWORD WINAPI GlassCompat_RemoteEnsureUICompat(LPVOID)
{
    TryLoadAndInitUICompat();
    EnsureUICompatLoadedImpl();
    return g_hUICompat ? 1u : 0u;
}

// ── Глобальный WinEvent-колбэк (WINEVENT_INCONTEXT, все процессы) ────────────
//
// Вызывается ОС в контексте потока-источника события:
//   – в хост-процессе (Rundll32): немедленно возвращаемся
//   – в целевом процессе: однократная инициализация → делегируем в WndCompat
//
// Не вызывает LoadLibrary напрямую — инициализация происходит в GUI-потоке
// GlassCompat (StartGuiThreadIfNeeded / DoInitialize).
extern "C" __declspec(dllexport)
void CALLBACK GlassCompat_WinEventProc(
    HWINEVENTHOOK,
    DWORD  event,
    HWND   hWnd,
    LONG   idObject,
    LONG   idChild,
    DWORD  dwEventThread,
    DWORD  dwmsEventTime)
{
    WinEventCallbackProbeGuard callbackProbeGuard;
    RegisterWinEventCallbackTarget(hWnd, GetCurrentThreadId());

#if defined(GLASS_ENABLE_LOGGING) && !defined(GLASS_LOG_SERVER)
    // Диагностика INCONTEXT: в каком процессе реально вызывается колбэк (см. план host ping).
    // Первые N вызовов на процесс — без спама в долгой работе.
    {
        static std::atomic<unsigned> s_winEventDiagCount{ 0 };
        const unsigned n = s_winEventDiagCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 64)
        {
            wchar_t mod[MAX_PATH]{};
            (void)GetModuleFileNameW(nullptr, mod, MAX_PATH);
            const wchar_t* base = wcsrchr(mod, L'\\');
            if (base)
                ++base;
            else
                base = mod;
            DebugLog(DBG_NORMAL,
                L"[GlassCompat][WinEventDiag] event=%u hwnd=0x%p evTid=%u pid=%u mod=%ls",
                event, hWnd, dwEventThread, GetCurrentProcessId(), base);
        }
    }
#endif

    // Классификация процесса (один раз на процесс). ReadHostPid() дорог — не вызывать на каждый WinEvent.
    //
    // HostProcess / ShortLivedAgent: только return — shutdown и CloseHandle(mutex) в EntryRundll::Main.
    //   Watcher с FreeLibraryAndExitThread здесь гоняется с done: хоста → Abort и «висящий» mutex.
    // BlockedProcess: watcher для явной выгрузки после StopEvent (нет GlassGuiThread).
    enum class WinEventInjectKind : uint8_t
    {
        Normal = 0,
        HostProcess,
        ShortLivedAgent,
        BlockedProcess,
    };

    static const WinEventInjectKind s_injectKind = []() noexcept -> WinEventInjectKind
    {
        const DWORD hostPid = ReadHostPid();
        if (hostPid && hostPid == GetCurrentProcessId())
            return WinEventInjectKind::HostProcess;

        if (GlassCompat::IsShortLivedGlassCompatProcess())
            return WinEventInjectKind::ShortLivedAgent;

        ProcessBlockList::Initialize();
        if (ProcessBlockList::IsBlocked(GetProcessNameLower()))
            return WinEventInjectKind::BlockedProcess;

        return WinEventInjectKind::Normal;
    }();

    if (s_injectKind == WinEventInjectKind::HostProcess ||
        s_injectKind == WinEventInjectKind::ShortLivedAgent)
    {
        // AppContainer: глобальный INCONTEXT часто доставляет событие в хост (OOC).
        // Forward remote SHOW/UNCLOAKED → GUI Attach+Reapply в SEH/ShellHost.
        if (s_injectKind == WinEventInjectKind::HostProcess
            && hWnd
            && idObject == OBJID_WINDOW
            && idChild == CHILDID_SELF)
        {
            // LOCATIONCHANGE: SEH NC часто CREATE как 1×1, размер позже (лог H12 defer).
            const bool wakeEvent =
                event == EVENT_OBJECT_CREATE
                || event == EVENT_OBJECT_SHOW
                || event == EVENT_SYSTEM_FOREGROUND
                || event == EVENT_OBJECT_UNCLOAKED
                || event == EVENT_OBJECT_LOCATIONCHANGE;
            if (wakeEvent)
            {
                wchar_t cls[96]{};
                if (GetClassNameW(hWnd, cls, 96) > 0
                    && (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") == 0
                        || _wcsicmp(cls, L"ControlCenterWindow") == 0))
                {
                    // LOCATIONCHANGE шумный — только при ненулевом client (sized).
                    if (event == EVENT_OBJECT_LOCATIONCHANGE)
                    {
                        RECT rc{};
                        if (!GetClientRect(hWnd, &rc))
                            return;
                        const int cx = rc.right - rc.left;
                        const int cy = rc.bottom - rc.top;
                        if (cx <= 0 || cy <= 0)
                            return;
                    }

                    DWORD wndPid = 0;
                    GetWindowThreadProcessId(hWnd, &wndPid);
                    if (wndPid != 0 && wndPid != GetCurrentProcessId())
                    {
                        GlassCompat::CoreWindowHostInject::NotifyRemoteCoreWindowShow(
                            wndPid, hWnd);
                    }
                }
            }
        }
        return;
    }

    if (s_injectKind == WinEventInjectKind::BlockedProcess)
    {
        // ProcessBlockList: без GUI, но DLL уже в процессе (INCONTEXT) — тот же minimal watcher.
        StartMinimalUnloadWatcher(2);
        return;
    }

    // Однократная инициализация в целевом процессе
    ProcessBlockList::Initialize();
    if (ProcessBlockList::IsBlocked(GetProcessNameLower()))
    {
        StartMinimalUnloadWatcher(2);
        return;
    }

    if (!GlassCompat::Initialize(nullptr))
        return;

    // DoInitialize ещё не завершён: GlassCore/WndCompat/провайдеры не готовы.
    // Если делегировать сейчас, субкласс встанет, но GetMenuEventProvider() → nullptr
    // и MN_SIZEWINDOW / политика тултипов пропустит хуки → первый кадр без кастома.
    if (!g_initDone.load(std::memory_order_acquire))
    {
        // Soft unload: reinject на GUI-потоке. Explorer — НЕ reinject (H6: Stop AV после soft+reinject).
        if (g_softModulesUnloaded.load(std::memory_order_acquire) &&
            g_guiThreadId != 0 &&
            GlassShared::GlassPlatformTask::IsGlassPlatformRunning())
        {
            const bool isExplorer =
                _wcsicmp(GetProcessNameLower().c_str(), L"explorer.exe") == 0;
            if (!isExplorer)
            {
                (void)PostThreadMessageW(
                    g_guiThreadId,
                    GlassCompatMsg::WM_GLASS_MODULES_REINJECT,
                    0,
                    0);
            }
        }
        return;
    }

    // SRWLOCK (shared): g_hWndCompat / g_hExplorer не обнулятся под exclusive FreeLibrary
    // пока мы внутри колбэков WinEvent.
    if (!g_hWndCompat && !g_hExplorer)
        return;

    AcquireSRWLockShared(&g_wndCompatLock);

    if (g_hWndCompat)
    {
        HandleWinEventFn pfn = g_pfnHandleWinEvent.load(std::memory_order_acquire);
        if (!pfn)
        {
            pfn = reinterpret_cast<HandleWinEventFn>(
                GetProcAddress(g_hWndCompat, "WndCompat_HandleWinEvent"));
            if (pfn)
                g_pfnHandleWinEvent.store(pfn, std::memory_order_release);
        }
        if (pfn)
            pfn(event, hWnd, idObject, idChild, dwEventThread, dwmsEventTime);
    }

    if (g_hExplorer)
    {
        HandleWinEventFn epfn = g_pfnExplorerHandleWinEvent.load(std::memory_order_acquire);
        if (!epfn)
        {
            epfn = reinterpret_cast<HandleWinEventFn>(
                GetProcAddress(g_hExplorer, "ExplorerCompat_HandleWinEvent"));
            if (epfn)
                g_pfnExplorerHandleWinEvent.store(epfn, std::memory_order_release);
        }
        if (epfn)
            epfn(event, hWnd, idObject, idChild, dwEventThread, dwmsEventTime);
    }

    if (g_hTaskbar)
    {
        HandleWinEventFn tpfn = g_pfnTaskbarHandleWinEvent.load(std::memory_order_acquire);
        if (!tpfn)
        {
            tpfn = reinterpret_cast<HandleWinEventFn>(
                GetProcAddress(g_hTaskbar, "TaskbarCompat_HandleWinEvent"));
            if (tpfn)
                g_pfnTaskbarHandleWinEvent.store(tpfn, std::memory_order_release);
        }
        if (tpfn)
            tpfn(event, hWnd, idObject, idChild, dwEventThread, dwmsEventTime);
    }

    ReleaseSRWLockShared(&g_wndCompatLock);
}