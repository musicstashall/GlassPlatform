#include "GlassShellPch.h"

#include "DesktopShellActions.h"
#include "GlassShellModule.h"
#include "resource.h"

#include <GlassMessages.h>
#include <GlassModulePath.h>
#include <GlassPlatformTask.h>
#include <strsafe.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <thread>

namespace GlassShellExt
{
    HINSTANCE g_module = nullptr;
}

namespace GS = GlassShared::GlassPlatformTask;
namespace OGC = Shared::OpenGlassHostCommand;
namespace PP = Shared::PlatformPresenceQuery;
namespace OG = Shared::OpenGlassHostState;

namespace
{
    // Совпадает с GlassSettings/SingleInstance.cs (суффикс разрядности).
    constexpr wchar_t kGlassSettingsMutexX64[] = L"GlassSettings.Instance.Mutex.x64";
    constexpr wchar_t kGlassSettingsMutexX86[] = L"GlassSettings.Instance.Mutex.x86";
    constexpr wchar_t kGlassSettingsExe[] = L"GlassSettings.exe";

    enum class LaunchDiagCode : DWORD
    {
        ModuleNotBound = 1,
        ModulePathFailed = 2,
        ExePathResolveFailed = 3,
        ExeMissing = 4,
        WorkDirFailed = 5,
        ShellExecuteFailed = 6,
    };

    void AppendShellLaunchLog(const wchar_t* fmt, ...) noexcept
    {
        if (!fmt)
            return;

        wchar_t line[2048]{};
        va_list args;
        va_start(args, fmt);
        (void)::StringCchVPrintfW(line, _countof(line), fmt, args);
        va_end(args);

        ::OutputDebugStringW(line);
        ::OutputDebugStringW(L"\n");

        wchar_t localAppData[MAX_PATH]{};
        if (FAILED(::SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)))
            return;

        wchar_t dir[MAX_PATH]{};
        if (FAILED(::StringCchPrintfW(dir, _countof(dir), L"%s\\GlassProject", localAppData)))
            return;
        (void)::CreateDirectoryW(dir, nullptr);

        wchar_t logPath[MAX_PATH]{};
        if (FAILED(::StringCchPrintfW(logPath, _countof(logPath), L"%s\\GlassShellExt-launch.log", dir)))
            return;

        SYSTEMTIME st{};
        ::GetLocalTime(&st);

        wchar_t stamped[2304]{};
        (void)::StringCchPrintfW(
            stamped,
            _countof(stamped),
            L"[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
            st.wYear,
            st.wMonth,
            st.wDay,
            st.wHour,
            st.wMinute,
            st.wSecond,
            line);

        HANDLE file = ::CreateFileW(
            logPath,
            FILE_APPEND_DATA,
            FILE_SHARE_READ,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;

        const DWORD bytes = static_cast<DWORD>(wcslen(stamped) * sizeof(wchar_t));
        DWORD written = 0;
        (void)::WriteFile(file, stamped, bytes, &written, nullptr);
        ::CloseHandle(file);
    }

    void ReportLaunchFailure(LaunchDiagCode code, const wchar_t* detail) noexcept
    {
        const DWORD codeValue = static_cast<DWORD>(code);
        AppendShellLaunchLog(
            L"[GlassShellExt] FAIL code=%u %s (см. %%LOCALAPPDATA%%\\GlassProject\\GlassShellExt-launch.log)",
            codeValue,
            detail ? detail : L"");
    }

    struct GlassSettingsWindowSearch
    {
        HWND found = nullptr;
        HWND iconicFallback = nullptr;
        HWND hiddenFallback = nullptr;
    };

    [[nodiscard]] bool IsGlassSettingsProcessImage(DWORD pid) noexcept
    {
        if (pid == 0)
            return false;

        HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!proc)
            return false;

        wchar_t imagePath[MAX_PATH]{};
        DWORD cch = _countof(imagePath);
        const BOOL ok = ::QueryFullProcessImageNameW(proc, 0, imagePath, &cch);
        ::CloseHandle(proc);
        if (!ok)
            return false;

        const wchar_t* file = wcsrchr(imagePath, L'\\');
        file = file ? file + 1 : imagePath;
        return _wcsicmp(file, kGlassSettingsExe) == 0;
    }

    BOOL CALLBACK FindGlassSettingsWindowProc(HWND hwnd, LPARAM lParam) noexcept
    {
        DWORD pid = 0;
        (void)::GetWindowThreadProcessId(hwnd, &pid);
        if (!IsGlassSettingsProcessImage(pid))
            return TRUE;

        auto* search = reinterpret_cast<GlassSettingsWindowSearch*>(lParam);

        // Только top-level главного окна (wizard/dialog с owner пропускаем).
        if (::GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;

        const LONG_PTR exStyle = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        if ((exStyle & WS_EX_TOOLWINDOW) != 0)
            return TRUE;

        const bool visible = ::IsWindowVisible(hwnd) != FALSE;
        const bool iconic = ::IsIconic(hwnd) != FALSE;

        if (!visible && !iconic)
        {
            // WinUI иногда без WS_VISIBLE/iconic — всё равно кандидат на activate.
            if (!search->hiddenFallback)
                search->hiddenFallback = hwnd;
            return TRUE;
        }

        if (visible)
        {
            search->found = hwnd;
            return FALSE;
        }

        // Iconic без WS_VISIBLE — запасной кандидат, продолжаем искать visible.
        if (!search->iconicFallback)
            search->iconicFallback = hwnd;
        return TRUE;
    }

    [[nodiscard]] HWND FindGlassSettingsWindow() noexcept
    {
        GlassSettingsWindowSearch search{};
        (void)::EnumWindows(FindGlassSettingsWindowProc, reinterpret_cast<LPARAM>(&search));
        if (search.found)
            return search.found;
        if (search.iconicFallback)
            return search.iconicFallback;
        return search.hiddenFallback;
    }

    [[nodiscard]] bool ActivateGlassSettingsHwnd(HWND hwnd) noexcept
    {
        if (!hwnd || !::IsWindow(hwnd))
            return false;

        DWORD pid = 0;
        (void)::GetWindowThreadProcessId(hwnd, &pid);
        if (pid != 0)
            (void)::AllowSetForegroundWindow(pid);

        // In-process AppWindow restore (MainWindow subclass); Win32 — fallback.
        // Требует ChangeWindowMessageFilterEx в GS (UIPI: medium explorer → elevated GS).
        const BOOL posted = ::PostMessageW(
            hwnd,
            GlassSettingsMsg::WM_GLASS_SETTINGS_ACTIVATE,
            0,
            0);
        AppendShellLaunchLog(
            L"[GlassShellExt] ActivateGlassSettingsHwnd hwnd=0x%p pid=%lu post=%d",
            hwnd,
            pid,
            posted ? 1 : 0);

        (void)::ShowWindow(hwnd, SW_RESTORE);
        (void)::BringWindowToTop(hwnd);
        (void)::SetForegroundWindow(hwnd);
        // Успех: PostMessage дошёл, либо окно уже не iconic после Win32 fallback.
        return posted != FALSE || !::IsIconic(hwnd);
    }

    [[nodiscard]] bool WaitForGlassSettingsWindow(DWORD timeoutMs) noexcept
    {
        const DWORD step = 100;
        const DWORD started = ::GetTickCount();
        while (true)
        {
            if (const HWND hwnd = FindGlassSettingsWindow())
                return ActivateGlassSettingsHwnd(hwnd);

            const DWORD elapsed = ::GetTickCount() - started;
            if (elapsed >= timeoutMs)
                return false;
            ::Sleep(step);
        }
    }

    [[nodiscard]] bool IsGlassSettingsMutexPresent() noexcept
    {
        HANDLE mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, kGlassSettingsMutexX64);
        if (!mutex)
            mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, kGlassSettingsMutexX86);
        if (!mutex)
            return false;
        ::CloseHandle(mutex);
        return true;
    }

    [[nodiscard]] bool TryActivateExistingGlassSettings() noexcept
    {
        // Окно первым: elevated GS, OpenMutex из medium IL explorer может не пройти.
        if (const HWND hwnd = FindGlassSettingsWindow())
            return ActivateGlassSettingsHwnd(hwnd);

        // GS стартует: mutex уже занят, окно ещё не создано.
        if (!IsGlassSettingsMutexPresent())
            return false;

        return WaitForGlassSettingsWindow(5000);
    }

    [[nodiscard]] bool ResolveGlassSettingsExePath(wchar_t* out, size_t cch) noexcept
    {
        if (!out || cch == 0)
            return false;
        out[0] = L'\0';
        if (!GlassShellExt::g_module)
            return false;
        const std::wstring path = GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
            GlassShellExt::g_module, kGlassSettingsExe);
        if (path.empty())
            return false;
        return ::StringCchCopyW(out, cch, path.c_str()) == S_OK;
    }

    [[nodiscard]] bool ResolveGlassSettingsExePathWithDiag(wchar_t* out, size_t cch) noexcept
    {
        if (!GlassShellExt::g_module)
        {
            ReportLaunchFailure(LaunchDiagCode::ModuleNotBound, L"g_module не привязан (DllMain?)");
            return false;
        }

        wchar_t dllPath[MAX_PATH]{};
        if (::GetModuleFileNameW(GlassShellExt::g_module, dllPath, MAX_PATH) == 0)
        {
            ReportLaunchFailure(
                LaunchDiagCode::ModulePathFailed,
                L"GetModuleFileNameW для GlassShellExt.dll не удался");
            return false;
        }

        if (!ResolveGlassSettingsExePath(out, cch))
        {
            ReportLaunchFailure(
                LaunchDiagCode::ExePathResolveFailed,
                L"Не удалось собрать путь к GlassSettings.exe");
            return false;
        }

        return true;
    }

    [[nodiscard]] bool ResolveGlassCompatDllPath(wchar_t* out, size_t cch) noexcept
    {
        if (!out || cch == 0)
            return false;
        out[0] = L'\0';
        if (!GlassShellExt::g_module)
            return false;
        const std::wstring path = GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
            GlassShellExt::g_module, L"GlassCompat.dll");
        if (path.empty())
            return false;
        return ::StringCchCopyW(out, cch, path.c_str()) == S_OK;
    }

    [[nodiscard]] bool ResolveGlassCompatDllPathForArch(
        GS::PlatformArch arch,
        wchar_t* out,
        size_t cch) noexcept
    {
        if (!out || cch == 0 || !GlassShellExt::g_module)
            return false;
        out[0] = L'\0';

        const auto moduleArch = arch == GS::PlatformArch::X64
            ? GlassShared::GlassModulePath::ModuleArch::X64
            : GlassShared::GlassModulePath::ModuleArch::X86;
        const std::wstring path = GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
            GlassShellExt::g_module, L"GlassCompat.dll", moduleArch);
        if (path.empty())
            return false;
        return ::StringCchCopyW(out, cch, path.c_str()) == S_OK;
    }

    [[nodiscard]] bool ResolveRundll32ForArch(GS::PlatformArch arch, wchar_t* out, size_t cch) noexcept
    {
        if (!out || cch == 0)
            return false;
        out[0] = L'\0';

        if (arch == GS::PlatformArch::X86 && GlassShared::GlassModulePath::IsOs64Bit())
        {
            if (::GetSystemWow64DirectoryW(out, static_cast<UINT>(cch)) == 0)
                return false;
            return ::StringCchCatW(out, cch, L"\\rundll32.exe") == S_OK;
        }

        if (::GetSystemDirectoryW(out, static_cast<UINT>(cch)) == 0)
            return false;
        return ::StringCchCatW(out, cch, L"\\rundll32.exe") == S_OK;
    }

    void LaunchPlatformStartElevated(HWND owner, GS::PlatformArch arch) noexcept
    {
        if (GS::IsGlassPlatformRunning(arch))
            return;

        wchar_t compatDll[MAX_PATH]{};
        if (!ResolveGlassCompatDllPathForArch(arch, compatDll, _countof(compatDll)))
            return;

        wchar_t rundll[MAX_PATH]{};
        if (!ResolveRundll32ForArch(arch, rundll, _countof(rundll)))
            return;

        wchar_t args[1024]{};
        if (FAILED(::StringCchPrintfW(
                args, _countof(args), L"\"%s\",Main /shellStartPlatform", compatDll)))
            return;

        (void)::ShellExecuteW(owner, L"runas", rundll, args, nullptr, SW_HIDE);
    }
}

namespace GlassDesktopActions
{
    std::wstring LoadMenuString(UINT resourceId, const wchar_t* fallback) noexcept
    {
        wchar_t text[256]{};
        if (GlassShellExt::g_module)
        {
            const int len = ::LoadStringW(GlassShellExt::g_module, resourceId, text, _countof(text));
            if (len > 0)
                return std::wstring(text, static_cast<size_t>(len));
        }
        return fallback ? std::wstring(fallback) : std::wstring{};
    }

    void BindModule(HINSTANCE hInst) noexcept
    {
        GlassShellExt::g_module = hInst;
    }

    bool ShowDualPlatformMenu() noexcept
    {
#ifdef _WIN64
        return GlassShared::GlassModulePath::IsOs64Bit();
#else
        return false;
#endif
    }

    std::wstring GetPlatformToggleTitle() noexcept
    {
        return GetPlatformToggleTitle(GS::NativePlatformArch());
    }

    std::wstring GetPlatformToggleTitle(GS::PlatformArch arch) noexcept
    {
        // «Включить» только Idle; Stopping/Residual/Running — «Остановить».
        const bool showEnable =
            GS::QueryInjectLifecycleUi(arch) == GS::InjectLifecycleUi::Idle;

        if (!ShowDualPlatformMenu())
        {
            return showEnable
                ? LoadMenuString(IDS_GLASSSHELL_TOGGLE_START, L"Enable Platform")
                : LoadMenuString(IDS_GLASSSHELL_TOGGLE_STOP, L"Disable Platform");
        }

        if (arch == GS::PlatformArch::X86)
        {
            return showEnable
                ? LoadMenuString(IDS_GLASSSHELL_TOGGLE_START_X86, L"Enable Platform x86")
                : LoadMenuString(IDS_GLASSSHELL_TOGGLE_STOP_X86, L"Disable Platform x86");
        }

        return showEnable
            ? LoadMenuString(IDS_GLASSSHELL_TOGGLE_START_X64, L"Enable Platform x64")
            : LoadMenuString(IDS_GLASSSHELL_TOGGLE_STOP_X64, L"Disable Platform x64");
    }

    bool IsPlatformToggleMenuEnabled() noexcept
    {
        return IsPlatformToggleMenuEnabled(GS::NativePlatformArch());
    }

    bool IsPlatformToggleMenuEnabled(GS::PlatformArch arch) noexcept
    {
        // Idle (старт) или Running (стоп); Stopping/Residual — блок.
        const GS::InjectLifecycleUi life = GS::QueryInjectLifecycleUi(arch);
        if (life != GS::InjectLifecycleUi::Idle && life != GS::InjectLifecycleUi::Running)
        {
            return false;
        }
        return GS::GlassAutorunTaskExists(arch);
    }

    UINT GetPlatformToggleMenuState() noexcept
    {
        return GetPlatformToggleMenuState(GS::NativePlatformArch());
    }

    UINT GetPlatformToggleMenuState(GS::PlatformArch arch) noexcept
    {
        return IsPlatformToggleMenuEnabled(arch) ? static_cast<UINT>(MFS_ENABLED)
                                                 : static_cast<UINT>(MFS_DISABLED);
    }

    void ExecutePlatformToggle(HWND owner) noexcept
    {
        ExecutePlatformToggle(owner, GS::NativePlatformArch());
    }

    void ExecutePlatformToggle(HWND owner, GS::PlatformArch arch) noexcept
    {
        if (!GS::GlassAutorunTaskExists(arch))
            return;
        {
            const GS::InjectLifecycleUi life = GS::QueryInjectLifecycleUi(arch);
            if (life != GS::InjectLifecycleUi::Idle && life != GS::InjectLifecycleUi::Running)
                return;
        }

        const GS::AutorunUiState ui = GS::QueryGlassAutorunUiState(arch);
        if (ui == GS::AutorunUiState::Ready || ui == GS::AutorunUiState::Disabled)
        {
            if (!GS::TryBeginLifecycleBusy(arch))
                return;
            LaunchPlatformStartElevated(owner, arch);
            // Busy до Running; при battery/schtasks deny — таймаут и сброс (не вечный lock).
            const auto a = arch;
            std::thread([a]() noexcept {
                const DWORD deadline = ::GetTickCount() + 20000u;
                while (::GetTickCount() < deadline)
                {
                    if (GS::IsGlassPlatformRunning(a))
                        break;
                    ::Sleep(250);
                }
                GS::EndLifecycleBusy(a);
            }).detach();
        }
        else if (ui == GS::AutorunUiState::Running)
        {
            // Сначала Stop: иначе Presence после TryBegin → IsLifecycleBusy(busy+Running
            // без Stop) auto-clear'ит busy, и GS не блокирует кнопки на время unload.
            (void)GS::SignalPlatformStop(arch);
            if (!GS::TryBeginLifecycleBusy(arch))
                return;
            const auto a = arch;
            std::thread([a]() noexcept {
                const DWORD deadline = ::GetTickCount() + 45000u;
                // Пока держим busy, Query==Stopping — ждём !Running и пустой sticky.
                // Несколько пустых подряд: Module32 miss на idle ShellHost.
                int emptyResidualStreak = 0;
                while (::GetTickCount() < deadline)
                {
                    if (!GS::IsGlassPlatformRunning(a))
                    {
                        (void)GS::StopOrphanAutorunTaskInstance(a);
                        if (!GS::RefreshInjectResidualSticky(a))
                        {
                            if (++emptyResidualStreak >= 6)
                                break;
                        }
                        else
                        {
                            emptyResidualStreak = 0;
                        }
                    }
                    ::Sleep(250);
                }
                GS::EndLifecycleBusy(a);
            }).detach();
        }
    }

    bool IsOpenGlassMenuVisible() noexcept
    {
#ifdef _WIN64
        // OpenGlass привязан к x64-хосту / DWM.
        return GS::IsGlassPlatformRunning(GS::PlatformArch::X64);
#else
        return GS::IsGlassPlatformRunning();
#endif
    }

    std::wstring GetOpenGlassToggleTitle() noexcept
    {
        const OG::HostUiState ui = PP::QueryOpenGlassOrchestrationHostUiState();
        if (ui == OG::HostUiState::Running)
            return LoadMenuString(IDS_GLASSSHELL_OPENGLASS_TOGGLE_STOP, L"Stop OpenGlass");
        return LoadMenuString(IDS_GLASSSHELL_OPENGLASS_TOGGLE_START, L"Start OpenGlass");
    }

    UINT GetOpenGlassToggleMenuState() noexcept
    {
        if (!PP::IsHostPresenceAvailable())
            return static_cast<UINT>(MFS_DISABLED);
        if (PP::IsOpenGlassPipelineTransitioning())
            return static_cast<UINT>(MFS_DISABLED);

        const OG::HostUiState ui = PP::QueryOpenGlassOrchestrationHostUiState();
        if (ui == OG::HostUiState::Disabled || ui == OG::HostUiState::NoTask)
            return static_cast<UINT>(MFS_DISABLED);
        return static_cast<UINT>(MFS_ENABLED);
    }

    void ExecuteOpenGlassToggle(HWND) noexcept
    {
        if (!IsOpenGlassMenuVisible())
            return;

        const OG::HostUiState ui = PP::QueryOpenGlassOrchestrationHostUiState();
        if (ui == OG::HostUiState::Running)
            (void)OGC::PostCommand(OGC::Command::Stop);
        else if (ui == OG::HostUiState::Ready)
            (void)OGC::PostCommand(OGC::Command::Start);
    }

    void LaunchGlassSettings(HWND owner) noexcept
    {
        AppendShellLaunchLog(L"[GlassShellExt] LaunchGlassSettings owner=0x%p", owner);

        if (TryActivateExistingGlassSettings())
        {
            AppendShellLaunchLog(L"[GlassShellExt] LaunchGlassSettings: activated existing instance");
            return;
        }

        wchar_t path[MAX_PATH]{};
        if (!ResolveGlassSettingsExePathWithDiag(path, _countof(path)))
            return;

        if (::GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        {
            wchar_t detail[MAX_PATH + 64]{};
            (void)::StringCchPrintfW(
                detail,
                _countof(detail),
                L"Файл не найден:\r\n%s",
                path);
            ReportLaunchFailure(LaunchDiagCode::ExeMissing, detail);
            return;
        }

        wchar_t workDir[MAX_PATH]{};
        if (FAILED(::StringCchCopyW(workDir, _countof(workDir), path)))
        {
            ReportLaunchFailure(LaunchDiagCode::WorkDirFailed, L"StringCchCopyW для workDir");
            return;
        }
        wchar_t* slash = wcsrchr(workDir, L'\\');
        if (!slash)
        {
            ReportLaunchFailure(LaunchDiagCode::WorkDirFailed, L"Каталог workDir не найден в пути");
            return;
        }
        *(slash + 1) = L'\0';

        // Sparse package (GlassShellMenu): прямой ShellExecute на exe наследует package identity
        // → WinUI Application.Start падает с REGDB_E_CLASSNOTREG. Запуск через explorer.exe
        // создаёт дочерний процесс без sparse identity (как из LogViewer).
        wchar_t winDir[MAX_PATH]{};
        if (::GetWindowsDirectoryW(winDir, _countof(winDir)) == 0)
        {
            ReportLaunchFailure(LaunchDiagCode::WorkDirFailed, L"GetWindowsDirectoryW не удался");
            return;
        }

        wchar_t explorerExe[MAX_PATH]{};
        if (FAILED(::StringCchPrintfW(explorerExe, _countof(explorerExe), L"%s\\explorer.exe", winDir)))
        {
            ReportLaunchFailure(LaunchDiagCode::WorkDirFailed, L"Не удалось собрать путь к explorer.exe");
            return;
        }

        wchar_t args[MAX_PATH + 8]{};
        if (FAILED(::StringCchPrintfW(args, _countof(args), L"\"%s\"", path)))
        {
            ReportLaunchFailure(LaunchDiagCode::WorkDirFailed, L"Не удалось собрать аргументы explorer.exe");
            return;
        }

        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = owner;
        sei.lpVerb = L"open";
        sei.lpFile = explorerExe;
        sei.lpParameters = args;
        sei.lpDirectory = workDir;
        sei.nShow = SW_SHOWNORMAL;

        AppendShellLaunchLog(
            L"[GlassShellExt] ShellExecuteEx explorer trampoline target=%s workDir=%s",
            path,
            workDir);
        if (!::ShellExecuteExW(&sei))
        {
            const DWORD err = ::GetLastError();
            wchar_t detail[512]{};
            (void)::StringCchPrintfW(
                detail,
                _countof(detail),
                L"ShellExecuteExW (explorer trampoline) не удался.\r\nWin32: 0x%08X (%u)",
                err,
                err);
            ReportLaunchFailure(LaunchDiagCode::ShellExecuteFailed, detail);
            return;
        }

        AppendShellLaunchLog(L"[GlassShellExt] LaunchGlassSettings: explorer trampoline ok pid=%lu hInst=0x%p",
            sei.hProcess ? ::GetProcessId(sei.hProcess) : 0u,
            sei.hInstApp);

        if (sei.hProcess)
            ::CloseHandle(sei.hProcess);

        if (WaitForGlassSettingsWindow(8000))
        {
            AppendShellLaunchLog(L"[GlassShellExt] LaunchGlassSettings: окно GS появилось");
            return;
        }

        ReportLaunchFailure(
            LaunchDiagCode::ShellExecuteFailed,
            L"Окно GlassSettings не появилось за 8 с после explorer trampoline");
    }
}
