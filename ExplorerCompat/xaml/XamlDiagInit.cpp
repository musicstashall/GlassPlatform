#include "ExplorerPch.h"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/XamlGlassIds.hpp"
#include "xaml/XamlShutdownMarshal.hpp"
#include "xaml/XamlTapSite.hpp"
#include "xaml/XamlComLifetime.hpp"

#include <DebugLog.h>
#include <ExplorerCompatHostPolicy.h>
#include <GlassMessages.h>
#include <objbase.h>

#include <atomic>
#include <mutex>

extern "C" void WINAPI ExplorerCompat_NotifyDesktopBridgeWindow(HWND hwnd) noexcept;

namespace
{
    #if 0
    constexpr const wchar_t* kDiagModuleTag = L"TaskbarCompat";
    #else
    constexpr const wchar_t* kDiagModuleTag = L"ExplorerCompat";
    #endif

    DWORD WINAPI UdkInstallThreadProc(LPVOID param) noexcept;

    using GlassCore_SetXamlHostDllNotifyFn = void(WINAPI*)(void(WINAPI*)(HMODULE, const wchar_t*));
    using GlassCore_NotifyExistingXamlHostDllsFn = void(WINAPI*)();

    static GlassCore_SetXamlHostDllNotifyFn s_pGlassCore_SetXamlHostDllNotify = nullptr;
    static GlassCore_NotifyExistingXamlHostDllsFn s_pGlassCore_NotifyExistingXamlHostDlls = nullptr;

    using InitializeXamlDiagnosticsExFn = HRESULT(WINAPI*)(
        LPCWSTR endPointName,
        DWORD pid,
        LPCWSTR wszDllXamlDiagnostics,
        LPCWSTR wszTAPDllName,
        CLSID tapClsid,
        LPCWSTR wszInitializationData);

    // Как FileBrowsWindowPatch TAPSite::InstallUdk: полный путь к диагностической DLL + короткое имя TAP.
    #if 0
    constexpr const wchar_t* kTapDllFileName = L"TaskbarCompat.dll";
    #else
    constexpr const wchar_t* kTapDllFileName = L"ExplorerCompat.dll";
    #endif

    HMODULE g_frameworkUdkDll = nullptr;
    bool g_frameworkUdkOwnsLoad = false;
    std::atomic<bool> g_diagRunning{ false };
    std::atomic<bool> g_xamlStopRequested{ false };
    std::atomic<bool> g_globalUdkThreadPosted{ false };
    std::atomic<int> g_connectionCounter{ 1 };
    std::atomic<int> g_ukdInstallThreadsActive{ 0 };
    HWND g_ixdeReclaimHwnd = nullptr;
    // Повторные параллельные InitializeXamlDiagnosticsEx после успешной сессии дают hr=0x80070490 и зависание explorer.
    SRWLOCK g_udkIxdeLock = SRWLOCK_INIT;
    std::atomic<bool> g_udkIxdeSucceeded{ false };
    std::atomic<bool> g_udkIxdeTerminalResolved{ false };
    std::atomic<long> g_udkIxdeLastHr{ static_cast<long>(E_PENDING) };

    wchar_t g_tapDllFullPath[MAX_PATH]{};
    std::atomic<bool> g_tapPathReady{ false };

    [[nodiscard]] bool ShouldAbortInstall() noexcept
    {
        return g_xamlStopRequested.load(std::memory_order_acquire);
    }

    void WINAPI OnXamlHostDllFromGlassCoreModuleHandler(HMODULE, const wchar_t* baseName) noexcept
    {
        (void)baseName;
        // Инициализация XAML Diagnostics теперь происходит в ExplorerCompat_NotifyDesktopBridgeWindow
    }

    static void RegisterGlassCoreXamlModuleNotify() noexcept
    {
#if 0
        return;
#else
        const HMODULE hCore = GetModuleHandleW(L"GlassCore.dll");
        if (!hCore)
            return;
        s_pGlassCore_SetXamlHostDllNotify = reinterpret_cast<GlassCore_SetXamlHostDllNotifyFn>(
            GetProcAddress(hCore, "GlassCore_SetXamlHostDllNotify"));
        s_pGlassCore_NotifyExistingXamlHostDlls = reinterpret_cast<GlassCore_NotifyExistingXamlHostDllsFn>(
            GetProcAddress(hCore, "GlassCore_NotifyExistingXamlHostDlls"));
        if (s_pGlassCore_SetXamlHostDllNotify)
            s_pGlassCore_SetXamlHostDllNotify(&OnXamlHostDllFromGlassCoreModuleHandler);
        if (s_pGlassCore_NotifyExistingXamlHostDlls)
            s_pGlassCore_NotifyExistingXamlHostDlls();
#endif
    }

    static void UnregisterGlassCoreXamlModuleNotify() noexcept
    {
#if 0
        return;
#else
        if (s_pGlassCore_SetXamlHostDllNotify)
        {
            s_pGlassCore_SetXamlHostDllNotify(nullptr);
            s_pGlassCore_SetXamlHostDllNotify = nullptr;
        }
        s_pGlassCore_NotifyExistingXamlHostDlls = nullptr;
#endif
    }

    void InterruptibleSleepMs(int totalMs) noexcept
    {
        int left = totalMs;
        while (left > 0 && !ShouldAbortInstall())
        {
            const int chunk = left > 50 ? 50 : left;
            Sleep(static_cast<DWORD>(chunk));
            left -= chunk;
        }
    }

    struct ResolvedFrameworkUdk
    {
        HMODULE module{};
        bool ownsLoad{};
    };
    enum class UdkResolveSource : int
    {
        None = 0,
        AlreadyLoaded,
        ShortName,
        System32FullPath,
        System32SearchFlag,
        DefaultDirsSearchFlag,
        MuxNeighbor,
        WuxNeighbor,
        SystemAppsNeighbor,
        MuxIxdeFallback,
        WuxIxdeFallback
    };
    std::atomic<int> g_lastUdkResolveSource{ static_cast<int>(UdkResolveSource::None) };
    std::atomic<bool> g_loggedNoSystemAppsClientDirs{ false };
    std::atomic<bool> g_loggedNoSystemAppsUdkCandidates{ false };
    std::atomic<bool> g_noSystemAppsUdkCandidatesConfirmed{ false };
    std::atomic<int> g_udkPathRejectLogBudget{ 64 };
    std::mutex g_collectSystemAppsMx;

    [[nodiscard]] bool CollectSystemAppsClientDirs(
        std::vector<std::wstring>& clientDirs,
        std::wstring* systemAppsRoot = nullptr) noexcept
    {
        // Сериализация: BootstrapScan + NotifyBridge + MUX warmup могут звать одновременно.
        std::lock_guard lock(g_collectSystemAppsMx);

        clientDirs.clear();
        if (systemAppsRoot)
            systemAppsRoot->clear();

        if (g_xamlStopRequested.load(std::memory_order_acquire))
            return false;

        wchar_t winDir[MAX_PATH]{};
        if (GetWindowsDirectoryW(winDir, static_cast<DWORD>(std::size(winDir))) == 0)
            return false;

        const std::wstring systemApps = std::wstring(winDir) + L"\\SystemApps";
        if (systemAppsRoot)
            *systemAppsRoot = systemApps;
        const std::wstring searchGlob = systemApps + L"\\*";

        WIN32_FIND_DATAW fd{};
        const HANDLE hFind = FindFirstFileW(searchGlob.c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE)
            return false;

        constexpr size_t kMaxClientDirs = 64;
        clientDirs.reserve(16);
        do
        {
            if (g_xamlStopRequested.load(std::memory_order_acquire))
                break;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                continue;
            if (fd.cFileName[0] == L'.')
                continue;
            if (wcsncmp(fd.cFileName, L"MicrosoftWindows.Client.", 24) != 0)
                continue;
            if (clientDirs.size() >= kMaxClientDirs)
                break;

            // Локальная копия имени: не передавать fd.cFileName напрямую в emplace
            // (AV write @0x30 в dump указывал на emplace_back на этом месте).
            wchar_t nameCopy[MAX_PATH]{};
            wcsncpy_s(nameCopy, fd.cFileName, _TRUNCATE);
            if (nameCopy[0] == L'\0')
                continue;

            try
            {
                clientDirs.emplace_back(nameCopy);
            }
            catch (...)
            {
                FindClose(hFind);
                clientDirs.clear();
                return false;
            }
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);

        try
        {
            std::sort(
                clientDirs.begin(),
                clientDirs.end(),
                [](const std::wstring& a, const std::wstring& b) noexcept {
                    auto rank = [](const std::wstring& s) noexcept -> int {
                        if (s.find(L"FileExp_") != std::wstring::npos)
                            return 0;
                        if (s.find(L"CBS_") != std::wstring::npos)
                            return 1;
                        if (s.find(L"ShellExperience") != std::wstring::npos)
                            return 2;
                        return 3;
                    };
                    const int ra = rank(a);
                    const int rb = rank(b);
                    if (ra != rb)
                        return ra < rb;
                    return a < b;
                });
        }
        catch (...)
        {
            clientDirs.clear();
            return false;
        }
        return true;
    }

    void LogUdkPathRejected(const wchar_t* reason, const wchar_t* path, DWORD gle = 0) noexcept
    {
        const int budget = g_udkPathRejectLogBudget.fetch_sub(1, std::memory_order_acq_rel);
        if (budget <= 0)
            return;
        if (gle != 0)
        {
            DebugLog(
                DBG_NORMAL,
                L"[ExplorerCompat][XamlDiag] FrameworkUdk: path reject reason=%s path=%s GLE=%u",
                reason ? reason : L"n/a",
                path ? path : L"(null)",
                static_cast<unsigned>(gle));
            return;
        }
        DebugLog(
            DBG_NORMAL,
            L"[ExplorerCompat][XamlDiag] FrameworkUdk: path reject reason=%s path=%s",
            reason ? reason : L"n/a",
            path ? path : L"(null)");
    }

    [[nodiscard]] const wchar_t* UdkResolveSourceToString(UdkResolveSource s) noexcept
    {
        switch (s)
        {
        case UdkResolveSource::AlreadyLoaded: return L"AlreadyLoaded";
        case UdkResolveSource::ShortName: return L"ShortName";
        case UdkResolveSource::System32FullPath: return L"System32FullPath";
        case UdkResolveSource::System32SearchFlag: return L"System32SearchFlag";
        case UdkResolveSource::DefaultDirsSearchFlag: return L"DefaultDirsSearchFlag";
        case UdkResolveSource::MuxNeighbor: return L"MuxNeighbor";
        case UdkResolveSource::WuxNeighbor: return L"WuxNeighbor";
        case UdkResolveSource::SystemAppsNeighbor: return L"SystemAppsNeighbor";
        case UdkResolveSource::MuxIxdeFallback: return L"MuxIxdeFallback";
        case UdkResolveSource::WuxIxdeFallback: return L"WuxIxdeFallback";
        default: return L"None";
        }
    }

    [[nodiscard]] ResolvedFrameworkUdk ResolveFrameworkUdkModule() noexcept
    {
        ResolvedFrameworkUdk r{};
        g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::None), std::memory_order_release);

        r.module = GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
        if (r.module)
        {
            g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::AlreadyLoaded), std::memory_order_release);
            DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] FrameworkUdk: уже загружен в процессе");
            return r;
        }

        r.module = LoadLibraryW(L"Microsoft.Internal.FrameworkUdk.dll");
        if (r.module)
        {
            r.ownsLoad = true;
            g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::ShortName), std::memory_order_release);
            DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary(short-name) OK");
            return r;
        }
        DebugLog(
            DBG_NORMAL,
            L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary(short-name) fail GLE=%u",
            static_cast<unsigned>(GetLastError()));

        wchar_t sysPath[MAX_PATH]{};
        if (GetSystemDirectoryW(sysPath, MAX_PATH) > 0)
        {
            wchar_t full[MAX_PATH]{};
            swprintf_s(full, L"%s\\Microsoft.Internal.FrameworkUdk.dll", sysPath);
            DebugLog(
                DBG_NORMAL,
                L"[ExplorerCompat][XamlDiag] FrameworkUdk: probe system32 path=%s attrs=0x%08X",
                full,
                static_cast<unsigned>(GetFileAttributesW(full)));
            r.module = LoadLibraryW(full);
            if (r.module)
            {
                r.ownsLoad = true;
                g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::System32FullPath), std::memory_order_release);
                DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary %s", full);
                return r;
            }
            DebugLog(
                DBG_NORMAL,
                L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary(system32-full) fail GLE=%u",
                static_cast<unsigned>(GetLastError()));
        }

        r.module = LoadLibraryExW(
            L"Microsoft.Internal.FrameworkUdk.dll",
            nullptr,
            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (r.module)
        {
            r.ownsLoad = true;
            g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::System32SearchFlag), std::memory_order_release);
            DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibraryEx(system32) OK");
            return r;
        }
        DebugLog(
            DBG_NORMAL,
            L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibraryEx(system32) fail GLE=%u",
            static_cast<unsigned>(GetLastError()));

        r.module = LoadLibraryExW(
            L"Microsoft.Internal.FrameworkUdk.dll",
            nullptr,
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (r.module)
        {
            r.ownsLoad = true;
            g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::DefaultDirsSearchFlag), std::memory_order_release);
            DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibraryEx(default-dirs) OK");
            return r;
        }
        DebugLog(
            DBG_NORMAL,
            L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibraryEx(default-dirs) fail GLE=%u",
            static_cast<unsigned>(GetLastError()));

        HMODULE mux = GetModuleHandleW(L"Microsoft.UI.Xaml.dll");
        if (mux)
        {
            wchar_t muxDir[MAX_PATH]{};
            if (GetModuleFileNameW(mux, muxDir, MAX_PATH) != 0)
            {
                DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] MUX module path=%s", muxDir);
                if (wchar_t* slash = wcsrchr(muxDir, L'\\'))
                    *slash = L'\0';
                wchar_t udkPath[MAX_PATH]{};
                swprintf_s(udkPath, L"%s\\Microsoft.Internal.FrameworkUdk.dll", muxDir);
                DebugLog(
                    DBG_NORMAL,
                    L"[ExplorerCompat][XamlDiag] FrameworkUdk: probe рядом с MUX path=%s attrs=0x%08X",
                    udkPath,
                    static_cast<unsigned>(GetFileAttributesW(udkPath)));
                r.module = LoadLibraryW(udkPath);
                if (r.module)
                {
                    r.ownsLoad = true;
                    g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::MuxNeighbor), std::memory_order_release);
                    DebugLog(DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary рядом с Microsoft.UI.Xaml: %s",
                        udkPath);
                    return r;
                }
                DebugLog(
                    DBG_NORMAL,
                    L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary(рядом с MUX) fail GLE=%u",
                    static_cast<unsigned>(GetLastError()));
            }
        }

        // UWP-остров трея (Windows.UI.Xaml без WinUI в процессе): FrameworkUdk лежит рядом с Windows.UI.Xaml.dll.
        HMODULE wux = GetModuleHandleW(L"Windows.UI.Xaml.dll");
        if (wux)
        {
            wchar_t wuxDir[MAX_PATH]{};
            if (GetModuleFileNameW(wux, wuxDir, MAX_PATH) != 0)
            {
                DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] WUX module path=%s", wuxDir);
                if (wchar_t* slash = wcsrchr(wuxDir, L'\\'))
                    *slash = L'\0';
                wchar_t udkPath[MAX_PATH]{};
                swprintf_s(udkPath, L"%s\\Microsoft.Internal.FrameworkUdk.dll", wuxDir);
                DebugLog(
                    DBG_NORMAL,
                    L"[ExplorerCompat][XamlDiag] FrameworkUdk: probe рядом с WUX path=%s attrs=0x%08X",
                    udkPath,
                    static_cast<unsigned>(GetFileAttributesW(udkPath)));
                r.module = LoadLibraryW(udkPath);
                if (r.module)
                {
                    r.ownsLoad = true;
                    g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::WuxNeighbor), std::memory_order_release);
                    DebugLog(DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary рядом с Windows.UI.Xaml: %s",
                        udkPath);
                    return r;
                }
                DebugLog(
                    DBG_NORMAL,
                    L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary(рядом с WUX) fail GLE=%u",
                    static_cast<unsigned>(GetLastError()));
            }
        }

        // Deterministic fallback: явный перебор SystemApps\\MicrosoftWindows.Client.*\\Microsoft.Internal.FrameworkUdk.dll
        std::vector<std::wstring> clientDirs;
        std::wstring systemApps;
        if (CollectSystemAppsClientDirs(clientDirs, &systemApps))
        {
            for (const std::wstring& dir : clientDirs)
            {
                const std::wstring udkPath = systemApps + L"\\" + dir + L"\\Microsoft.Internal.FrameworkUdk.dll";
                const DWORD attrs = GetFileAttributesW(udkPath.c_str());
                if (attrs == INVALID_FILE_ATTRIBUTES)
                {
                    LogUdkPathRejected(L"MissingFile", udkPath.c_str());
                    continue;
                }
                g_loggedNoSystemAppsUdkCandidates.store(false, std::memory_order_release);
                DebugLog(
                    DBG_NORMAL,
                    L"[ExplorerCompat][XamlDiag] FrameworkUdk: probe SystemApps path=%ls attrs=0x%08X",
                    udkPath.c_str(),
                    static_cast<unsigned>(attrs));
                r.module = LoadLibraryW(udkPath.c_str());
                if (r.module)
                {
                    r.ownsLoad = true;
                    g_noSystemAppsUdkCandidatesConfirmed.store(false, std::memory_order_release);
                    g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::SystemAppsNeighbor), std::memory_order_release);
                    DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary SystemApps: %ls", udkPath.c_str());
                    return r;
                }
                LogUdkPathRejected(L"LoadLibraryFail", udkPath.c_str(), GetLastError());
            }

            // Если MUX лежит в SystemApps\\MicrosoftWindows.Client.*, пробуем соседние client-пакеты того же корня.
            if (mux)
            {
                wchar_t muxPath[MAX_PATH]{};
                if (GetModuleFileNameW(mux, muxPath, static_cast<DWORD>(std::size(muxPath))) != 0)
                {
                    const std::wstring muxPathStr = muxPath;
                    const std::wstring marker = L"\\SystemApps\\MicrosoftWindows.Client.";
                    const size_t markerPos = muxPathStr.find(marker);
                    if (markerPos != std::wstring::npos)
                    {
                        const std::wstring baseRoot = muxPathStr.substr(0, markerPos) + L"\\SystemApps";
                        for (const std::wstring& dir : clientDirs)
                        {
                            const std::wstring udkPath = baseRoot + L"\\" + dir + L"\\Microsoft.Internal.FrameworkUdk.dll";
                            if (GetFileAttributesW(udkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                            {
                                LogUdkPathRejected(L"MuxSiblingMissing", udkPath.c_str());
                                continue;
                            }
                            r.module = LoadLibraryW(udkPath.c_str());
                            if (r.module)
                            {
                                r.ownsLoad = true;
                                g_noSystemAppsUdkCandidatesConfirmed.store(false, std::memory_order_release);
                                g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::SystemAppsNeighbor), std::memory_order_release);
                                DebugLog(
                                    DBG_IMPORTANT,
                                    L"[ExplorerCompat][XamlDiag] FrameworkUdk: LoadLibrary MUX-sibling SystemApps: %ls",
                                    udkPath.c_str());
                                return r;
                            }
                            LogUdkPathRejected(L"MuxSiblingLoadFail", udkPath.c_str(), GetLastError());
                        }
                    }
                }
            }

            if (clientDirs.empty())
            {
                g_noSystemAppsUdkCandidatesConfirmed.store(true, std::memory_order_release);
                if (!g_loggedNoSystemAppsClientDirs.exchange(true, std::memory_order_acq_rel))
                {
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlDiag] FrameworkUdk: SystemApps scan не нашёл MicrosoftWindows.Client.*");
                }
            }
            else
            {
                bool hasAnyUdkCandidate = false;
                for (const std::wstring& dir : clientDirs)
                {
                    const std::wstring udkPath = systemApps + L"\\" + dir + L"\\Microsoft.Internal.FrameworkUdk.dll";
                    if (GetFileAttributesW(udkPath.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        hasAnyUdkCandidate = true;
                        break;
                    }
                }
                g_noSystemAppsUdkCandidatesConfirmed.store(!hasAnyUdkCandidate, std::memory_order_release);
                if (!hasAnyUdkCandidate && !g_loggedNoSystemAppsUdkCandidates.exchange(true, std::memory_order_acq_rel))
                {
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlDiag] FrameworkUdk: в найденных MicrosoftWindows.Client.* нет Microsoft.Internal.FrameworkUdk.dll");
                }
            }
        }

        // Fallback: на части сборок InitializeXamlDiagnosticsEx экспортируется из XAML-движка, без отдельной
        // Microsoft.Internal.FrameworkUdk.dll в адресном пространстве (см. Acrylic ExplorerTAP / UWP dual-stack).
        mux = GetModuleHandleW(L"Microsoft.UI.Xaml.dll");
        if (mux && GetProcAddress(mux, "InitializeXamlDiagnosticsEx"))
        {
            r.module = mux;
            r.ownsLoad = false;
            g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::MuxIxdeFallback), std::memory_order_release);
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlDiag] IXDE: GetProcAddress на Microsoft.UI.Xaml.dll (без FrameworkUdk.dll)");
            return r;
        }

        // Только чистый UWP-XAML: при уже загруженном Microsoft.UI.Xaml (WinUI) вызов IXDE из Windows.UI.Xaml
        // стабильно даёт 0x80070490 — оставляем ожидание реального FrameworkUdk рядом с MUX / поллинг.
        wux = GetModuleHandleW(L"Windows.UI.Xaml.dll");
        if (wux && !mux && GetProcAddress(wux, "InitializeXamlDiagnosticsEx"))
        {
            r.module = wux;
            r.ownsLoad = false;
            g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::WuxIxdeFallback), std::memory_order_release);
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlDiag] IXDE: GetProcAddress на Windows.UI.Xaml.dll (без FrameworkUdk.dll, WinUI "
                L"не загружен)");
            return r;
        }

        return r;
    }

    // Холодный старт explorer: LoadLibrary(L"Microsoft.UI.Xaml.dll") без полного пути часто даёт GLE=126/1158 —
    // MUX лежит только в подпапке AppX (%WINDIR%\SystemApps\MicrosoftWindows.Client.*). Без MUX
    // ResolveFrameworkUdkModule не может загрузить FrameworkUdk рядом и не получает IXDE (лог: FrameworkUdk.dll=0).
    [[nodiscard]] bool TryLoadMicrosoftUiXamlFromSystemAppsPackages(const wchar_t* reasonTag = nullptr) noexcept
    {
        if (GetModuleHandleW(L"Microsoft.UI.Xaml.dll"))
            return true;

        std::vector<std::wstring> clientDirs;
        std::wstring systemApps;
        if (!CollectSystemAppsClientDirs(clientDirs, &systemApps))
            return false;

        if (clientDirs.empty())
        {
            DebugLog(
                DBG_NORMAL,
                L"[ExplorerCompat][XamlDiag] MUX warmup(%s): MicrosoftWindows.Client.* не найден",
                reasonTag ? reasonTag : L"n/a");
            return false;
        }

        for (const std::wstring& dir : clientDirs)
        {
            const std::wstring dllPath = systemApps + L"\\" + dir + L"\\Microsoft.UI.Xaml.dll";
            if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                continue;
            const HMODULE m = LoadLibraryW(dllPath.c_str());
            if (m)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlDiag] предзагрузка MUX из SystemApps(%s): %ls",
                    reasonTag ? reasonTag : L"n/a",
                    dllPath.c_str());
                return true;
            }
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlDiag] LoadLibrary MUX не удалась (%s): %ls GLE=%u",
                reasonTag ? reasonTag : L"n/a",
                dllPath.c_str(),
                static_cast<unsigned>(GetLastError()));
        }
        DebugLog(
            DBG_NORMAL,
            L"[ExplorerCompat][XamlDiag] MUX warmup(%s): валидных loadable кандидатов не найдено",
            reasonTag ? reasonTag : L"n/a");
        return false;
    }

    void AdoptFrameworkUdkAfterSuccessfulInit(ResolvedFrameworkUdk udk) noexcept
    {
        if (!udk.module)
            return;

        void* prev = InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID*>(&g_frameworkUdkDll),
            udk.module,
            nullptr);
        if (prev == nullptr)
        {
            g_frameworkUdkOwnsLoad = udk.ownsLoad;
        }
        else if (udk.ownsLoad && udk.module != g_frameworkUdkDll)
        {
            FreeLibrary(udk.module);
        }
    }

    void ReleaseOwnedUdkIfUnused(ResolvedFrameworkUdk udk) noexcept
    {
        if (!udk.ownsLoad || !udk.module)
            return;
        if (udk.module != g_frameworkUdkDll)
            FreeLibrary(udk.module);
    }

    struct UdkInstallThreadActiveGuard
    {
        UdkInstallThreadActiveGuard() noexcept
        {
            g_ukdInstallThreadsActive.fetch_add(1, std::memory_order_relaxed);
        }
        ~UdkInstallThreadActiveGuard() noexcept
        {
            g_ukdInstallThreadsActive.fetch_sub(1, std::memory_order_relaxed);
        }
    };

    LRESULT CALLBACK IxdeReclaimWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (msg == ExplorerCompatMsg::WM_GLASS_EXPLORER_IXDE_COM_RECLAIM)
        {
            CoFreeUnusedLibraries();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void EnsureIxdeReclaimWindowOnCurrentThread() noexcept
    {
        if (g_ixdeReclaimHwnd && IsWindow(g_ixdeReclaimHwnd))
            return;

        HMODULE hMod = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&EnsureIxdeReclaimWindowOnCurrentThread),
                &hMod))
        {
            hMod = GetModuleHandleW(nullptr);
        }

        // Как XamlShutdownMarshal: после FreeLibrary+LoadLibrary класс может остаться
        // со stale lpfnWndProc → CreateWindowExW (WM_NCCREATE) крашит explorer.
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        const bool classExists =
            GetClassInfoExW(
                hMod, ExplorerCompatMsg::kExplorerIxdeComReclaimWndClass, &existing)
            != 0;
        const bool procStale = classExists && existing.lpfnWndProc != IxdeReclaimWndProc;
        if (procStale)
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlDiag] IXDE reclaim-класс от прошлой загрузки — re-register");
            (void)UnregisterClassW(
                ExplorerCompatMsg::kExplorerIxdeComReclaimWndClass, hMod);

            WNDCLASSEXW recheck{};
            recheck.cbSize = sizeof(recheck);
            if (GetClassInfoExW(
                    hMod, ExplorerCompatMsg::kExplorerIxdeComReclaimWndClass, &recheck)
                && recheck.lpfnWndProc != IxdeReclaimWndProc)
            {
                DebugLog(
                    DBG_ERROR,
                    L"[ExplorerCompat][XamlDiag] stale IXDE reclaim-класс не снят — окно не создаём");
                g_ixdeReclaimHwnd = nullptr;
                return;
            }
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = IxdeReclaimWndProc;
        wc.hInstance = hMod;
        wc.lpszClassName = ExplorerCompatMsg::kExplorerIxdeComReclaimWndClass;
        if (RegisterClassExW(&wc) == 0)
        {
            const DWORD gle = GetLastError();
            if (gle != ERROR_CLASS_ALREADY_EXISTS)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[ExplorerCompat][XamlDiag] IXDE reclaim RegisterClassEx failed GLE=%u",
                    static_cast<unsigned>(gle));
                return;
            }
        }

        const HWND created = CreateWindowExW(
            0,
            ExplorerCompatMsg::kExplorerIxdeComReclaimWndClass,
            L"",
            0,
            0,
            0,
            0,
            0,
            HWND_MESSAGE,
            nullptr,
            hMod,
            nullptr);
        if (!created)
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlDiag] IXDE reclaim CreateWindowEx failed GLE=%u",
                static_cast<unsigned>(GetLastError()));
            return;
        }

        g_ixdeReclaimHwnd = created;
        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlDiag] IXDE reclaim HWND_MESSAGE hwnd=0x%p tid=%u",
            created,
            GetCurrentThreadId());
    }

    void RunCoFreeUnusedLibrariesOnIxdeThread() noexcept
    {
        const HWND h = g_ixdeReclaimHwnd;
        if (!h || !IsWindow(h))
            return;

        const DWORD wndTid = GetWindowThreadProcessId(h, nullptr);
        if (wndTid != 0 && wndTid == GetCurrentThreadId())
        {
            CoFreeUnusedLibraries();
            return;
        }

        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(
                h,
                ExplorerCompatMsg::WM_GLASS_EXPLORER_IXDE_COM_RECLAIM,
                0,
                0,
                SMTO_ABORTIFHUNG,
                5'000,
                &result) != 0)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlDiag] CoFreeUnusedLibraries on IXDE tid=%u",
                static_cast<unsigned>(wndTid));
            return;
        }

        DebugLog(
            DBG_WARNING,
            L"[ExplorerCompat][XamlDiag] IXDE CoFree SendMessageTimeout failed GLE=%u",
            static_cast<unsigned>(GetLastError()));
    }

    void DestroyIxdeReclaimWindow() noexcept
    {
        const HWND h = g_ixdeReclaimHwnd;
        HMODULE hMod = nullptr;
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&DestroyIxdeReclaimWindow),
                &hMod))
        {
            hMod = GetModuleHandleW(nullptr);
        }

        if (h && IsWindow(h))
            DestroyWindow(h);
        g_ixdeReclaimHwnd = nullptr;

        // Снять класс до FreeLibrary — иначе следующий LoadLibrary + CreateWindowEx
        // попадёт в stale WndProc (crash explorer @ CreateWindowExW).
        if (hMod)
        {
            if (!UnregisterClassW(ExplorerCompatMsg::kExplorerIxdeComReclaimWndClass, hMod))
            {
                const DWORD gle = GetLastError();
                if (gle != ERROR_CLASS_DOES_NOT_EXIST)
                {
                    DebugLog(
                        DBG_WARNING,
                        L"[ExplorerCompat][XamlDiag] IXDE reclaim UnregisterClass GLE=%u",
                        static_cast<unsigned>(gle));
                }
            }
        }

        DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] IXDE reclaim HWND_MESSAGE destroyed");
    }

    [[nodiscard]] HRESULT CallFrameworkUdkInit(
        InitializeXamlDiagnosticsExFn pIxde,
        DWORD pid,
        HWND hwndForLog,
        wchar_t* connBuf,
        size_t connCch) noexcept
    {
        if (hwndForLog)
        {
            swprintf_s(
                connBuf,
                connCch,
                L"WinUIVisualDiag_h%p_%d",
                hwndForLog,
                g_connectionCounter.fetch_add(1, std::memory_order_relaxed));
        }
        else
        {
            swprintf_s(
                connBuf,
                connCch,
                L"WinUIVisualDiagConnection%d",
                g_connectionCounter.fetch_add(1, std::memory_order_relaxed));
        }

        if (!g_tapPathReady.load(std::memory_order_acquire))
            return E_FAIL;

        return pIxde(
            connBuf,
            pid,
            g_tapDllFullPath,
            kTapDllFileName,
            CLSID_GlassXamlTapSite,
            nullptr);
    }

    [[nodiscard]] HRESULT InvokeFrameworkUdkInitializeOnce(
        InitializeXamlDiagnosticsExFn pIxde,
        DWORD pid,
        ResolvedFrameworkUdk& udk) noexcept
    {
        if (g_udkIxdeSucceeded.load(std::memory_order_acquire))
            return S_OK;

        AcquireSRWLockExclusive(&g_udkIxdeLock);
        HRESULT hr = E_FAIL;
        if (g_udkIxdeSucceeded.load(std::memory_order_relaxed))
        {
            hr = S_OK;
        }
        else
        {
            wchar_t conn[128]{};
            hr = CallFrameworkUdkInit(pIxde, pid, nullptr, conn, _countof(conn));
            if (SUCCEEDED(hr))
            {
                g_udkIxdeSucceeded.store(true, std::memory_order_release);
                AdoptFrameworkUdkAfterSuccessfulInit(udk);
                const auto resolveSource = static_cast<UdkResolveSource>(
                    g_lastUdkResolveSource.load(std::memory_order_acquire));
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlDiag] InitializeXamlDiagnosticsEx (FrameworkUdk) OK global name=%s source=%s",
                    conn,
                    UdkResolveSourceToString(resolveSource));
            }
        }
        ReleaseSRWLockExclusive(&g_udkIxdeLock);
        // CreateWindow вне SRW: не держать lock на RegisterClass/CreateWindow.
        if (SUCCEEDED(hr))
            EnsureIxdeReclaimWindowOnCurrentThread();
        return hr;
    }

    // Только глобальный IXDE (TAPSite::InstallUdk(nullptr)). Per-hwnd потоки убраны: повторный IXDE после
    // успешной сессии даёт 0x80070490 и зависание explorer.
    void ScanSubtreeForWinUiBridges(HWND h) noexcept
    {
        if (ShouldAbortInstall() || !h || !IsWindow(h))
            return;

        wchar_t cls[200]{};
        if (GetClassNameW(h, cls, static_cast<int>(_countof(cls))) > 0)
        {
            if (wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0 ||
                wcscmp(cls, L"Microsoft.UI.Content.DesktopWindowHost") == 0)
            {
                ExplorerCompat_NotifyDesktopBridgeWindow(h);
            }
            // PopupWindowSiteBridge НЕ регистрируется через bootstrap scan.
            // Причина: bootstrap scan находит окна, которые уже существуют на момент
            // инициализации IXDE, но для них EVENT_OBJECT_DESTROY может никогда не сработать
            // (окно создано до установки WinEvent hook). Это приводит к накоплению
            // bridge HWND в g_bridgeHwnds и ложному срабатыванию fallback-механизмов.
            // Регистрация PopupWindowSiteBridge должна происходить ТОЛЬКО через
            // EVENT_OBJECT_CREATE в WinEvent (WndCompat → WinUiPopupBridge_Attach),
            // чтобы гарантировать парный EVENT_OBJECT_DESTROY и корректную очистку.
        }

        EnumChildWindows(
            h,
            [](HWND ch, LPARAM) -> BOOL {
                ScanSubtreeForWinUiBridges(ch);
                return !ShouldAbortInstall();
            },
            0);
    }

    DWORD WINAPI BootstrapScanBridgesThread(LPVOID) noexcept
    {
#if 0
        return 0;
#else
        if (ShouldAbortInstall())
            return 0;

        // Немедленный Flush из Start раньше давал Notify→UDK до готовности InitializeXamlDiagnosticsEx (18× FAIL).
        // Короткая пауза + flush здесь: WinUI успевает подтянуть Microsoft.Internal.FrameworkUdk.
        constexpr int kDeferBeforePendingFlushMs = 450;
        InterruptibleSleepMs(kDeferBeforePendingFlushMs);
        if (ShouldAbortInstall())
            return 0;

        if (const HMODULE hWndCompat = GetModuleHandleW(L"WndCompat.dll"))
        {
            using FlushPendingFn = void(WINAPI*)();
            if (const auto pFlush = reinterpret_cast<FlushPendingFn>(
                    GetProcAddress(hWndCompat, "WndCompat_FlushPendingDesktopBridges")))
            {
                pFlush();
            }
        }

        const DWORD selfPid = GetCurrentProcessId();

        EnumWindows(
            [](HWND top, LPARAM) -> BOOL {
                if (ShouldAbortInstall())
                    return FALSE;
                DWORD pid = 0;
                if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                    return TRUE;
                ScanSubtreeForWinUiBridges(top);
                return TRUE;
            },
            0);

        DebugLog(
            DBG_IMPORTANT,
            L"[%ls][XamlDiag] BootstrapScan: обход существующих окон pid=%u завершён",
            kDiagModuleTag,
            static_cast<unsigned>(selfPid));

        // Инициализация XAML Diagnostics теперь происходит в ExplorerCompat_NotifyDesktopBridgeWindow
        return 0;
#endif
    }
} // namespace

extern "C" void WINAPI ExplorerCompat_NotifyDesktopBridgeWindow(HWND hwnd) noexcept
{
    if (!hwnd || !IsWindow(hwnd))
        return;
    if (!g_diagRunning.load(std::memory_order_acquire))
        return;
    if (ShouldAbortInstall())
        return;

    wchar_t cls[200]{};
    if (GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls))) <= 0)
        return;
    if (wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") != 0 &&
        wcscmp(cls, L"Microsoft.UI.Content.DesktopWindowHost") != 0)
        return;

    // Инициализация XAML Diagnostics при первом острове (WinUI уже загрузил FrameworkUdk)
    if (g_udkIxdeSucceeded.load(std::memory_order_acquire))
        return;

    const bool first = !g_globalUdkThreadPosted.exchange(true, std::memory_order_acq_rel);
    if (!first)
        return;

    DebugLog(DBG_IMPORTANT, L"[%ls][XamlDiag] Первый DesktopChildSiteBridge/DesktopWindowHost: инициализация IXDE", kDiagModuleTag);

    ResolvedFrameworkUdk udk = ResolveFrameworkUdkModule();
    if (!udk.module)
    {
        DebugLog(DBG_WARNING, L"[%ls][XamlDiag] FrameworkUdk не найден при создании острова", kDiagModuleTag);
        g_globalUdkThreadPosted.store(false, std::memory_order_release);
        return;
    }

    const auto pIxde = reinterpret_cast<InitializeXamlDiagnosticsExFn>(
        GetProcAddress(udk.module, "InitializeXamlDiagnosticsEx"));
    if (!pIxde)
    {
        DebugLog(DBG_WARNING, L"[%ls][XamlDiag] FrameworkUdk: нет InitializeXamlDiagnosticsEx", kDiagModuleTag);
        ReleaseOwnedUdkIfUnused(udk);
        g_globalUdkThreadPosted.store(false, std::memory_order_release);
        return;
    }

    const DWORD pid = GetCurrentProcessId();
    const HRESULT hr = InvokeFrameworkUdkInitializeOnce(pIxde, pid, udk);

    if (SUCCEEDED(hr))
    {
        DebugLog(DBG_IMPORTANT, L"[%ls][XamlDiag] IXDE инициализирован успешно hr=0x%08X", kDiagModuleTag, static_cast<unsigned>(hr));
    }
    else
    {
        DebugLog(DBG_WARNING, L"[%ls][XamlDiag] IXDE не инициализирован hr=0x%08X", kDiagModuleTag, static_cast<unsigned>(hr));
        g_globalUdkThreadPosted.store(false, std::memory_order_release);
    }

    g_udkIxdeLastHr.store(static_cast<long>(hr), std::memory_order_release);
    g_udkIxdeTerminalResolved.store(true, std::memory_order_release);
}

HRESULT ExplorerCompat_StartXamlDiagnostics()
{
#if 0
    return S_FALSE;
#else
    if (g_diagRunning.load(std::memory_order_acquire))
        return S_OK;

    g_xamlStopRequested.store(false, std::memory_order_release);
    g_globalUdkThreadPosted.store(false, std::memory_order_release);
    g_udkIxdeSucceeded.store(false, std::memory_order_release);
    g_udkIxdeTerminalResolved.store(false, std::memory_order_release);
    g_udkIxdeLastHr.store(static_cast<long>(E_PENDING), std::memory_order_release);
    g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::None), std::memory_order_release);
    g_connectionCounter.store(1, std::memory_order_release);
    GlassExplorerCom::g_shutdownDone.store(false, std::memory_order_release);

    HMODULE selfModule = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ExplorerCompat_StartXamlDiagnostics),
            &selfModule) ||
        !GetModuleFileNameW(selfModule, g_tapDllFullPath, MAX_PATH))
    {
        DebugLog(DBG_ERROR, L"[ExplorerCompat][XamlDiag] GetModuleFileName failed GLE=%u", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }

    g_tapPathReady.store(true, std::memory_order_release);
    g_diagRunning.store(true, std::memory_order_release);

    RegisterGlassCoreXamlModuleNotify();

    HANDLE hBoot = CreateThread(nullptr, 0, BootstrapScanBridgesThread, nullptr, 0, nullptr);
    if (!hBoot)
    {
        g_diagRunning.store(false, std::memory_order_release);
        g_tapPathReady.store(false, std::memory_order_release);
        DebugLog(DBG_ERROR, L"[ExplorerCompat][XamlDiag] CreateThread(BootstrapScan) failed GLE=%u", GetLastError());
        return HRESULT_FROM_WIN32(GetLastError());
    }
    CloseHandle(hBoot);

    DebugLog(
        DBG_IMPORTANT,
            L"[%ls][XamlDiag] Старт: событийная модель (WinEvent + bootstrap scan), TAP=%ls",
            kDiagModuleTag,
        kTapDllFileName);
    return S_OK;
#endif
}

void ExplorerCompat_StopXamlDiagnostics() noexcept
{
#if 0
    return;
#else
    DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] Stop: BEGIN (abort install threads, Unadvise)");

    UnregisterGlassCoreXamlModuleNotify();

    g_xamlStopRequested.store(true, std::memory_order_release);

    // g_shutdownDone до VTW: RestoreHostBar → UI-queue без poisoned popup-VTC (иначе шапка не
    // восстанавливается). Destroy HWND — на потоке-владельце.
    GlassExplorerCom::g_shutdownDone.store(true, std::memory_order_release);

    XamlShutdownMarshal::RunShutdownVisualTreeWatcherOnXamlThread();
    XamlShutdownMarshal::DestroyMessageWindow();
    DestroyIxdeReclaimWindow();

    // После снятия HWND_MESSAGE и освобождения UDK — ещё раз сбросить GDI/DComp кадр на окнах проводника.
    XamlTapSite::RedrawSurfacesAfterFullStop();

    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlDiag] Stop: COM refs objectCount=%ld serverLocks=%ld (DllCanUnloadNow при 0/0)",
        GlassExplorerCom::g_objectCount.load(std::memory_order_acquire),
        GlassExplorerCom::g_serverLocks.load(std::memory_order_acquire));

    constexpr int kUdkThreadDrainMs = 8000;
    for (int w = 0; w < kUdkThreadDrainMs; w += 50)
    {
        if (g_ukdInstallThreadsActive.load(std::memory_order_acquire) <= 0)
            break;
        Sleep(50);
    }
    if (g_ukdInstallThreadsActive.load(std::memory_order_acquire) > 0)
    {
        DebugLog(
            DBG_WARNING,
            L"[ExplorerCompat][XamlDiag] Stop: потоки UDK-install ещё активны — FreeLibrary отложен/рискован");
    }

    if (g_frameworkUdkDll && g_frameworkUdkOwnsLoad)
    {
        FreeLibrary(g_frameworkUdkDll);
        g_frameworkUdkDll = nullptr;
        g_frameworkUdkOwnsLoad = false;
    }

    g_tapPathReady.store(false, std::memory_order_release);
    g_diagRunning.store(false, std::memory_order_release);
    g_globalUdkThreadPosted.store(false, std::memory_order_release);
    g_udkIxdeSucceeded.store(false, std::memory_order_release);
    g_udkIxdeTerminalResolved.store(false, std::memory_order_release);
    g_udkIxdeLastHr.store(static_cast<long>(E_PENDING), std::memory_order_release);
    g_lastUdkResolveSource.store(static_cast<int>(UdkResolveSource::None), std::memory_order_release);

    // g_shutdownDone уже выставлен до VTW (restore шапки). Повторный store — идемпотентен.
    GlassExplorerCom::g_shutdownDone.store(true, std::memory_order_release);

    CoFreeUnusedLibraries();
    DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][XamlDiag] Stop complete");
#endif
}

void ExplorerCompat_RunIxdeComReclaim() noexcept
{
#if 0
    return;
#else
    RunCoFreeUnusedLibrariesOnIxdeThread();
    CoFreeUnusedLibraries();
#endif
}
