#include "GlassShellPch.h"

#include "DesktopShellActions.h"
#include "GlassShellModule.h"
#include "resource.h"

#include <GlassMessages.h>
#include <GlassModulePath.h>
#include <GlassPlatformTask.h>
#include <strsafe.h>
#include <shlobj.h>

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

        // Получить каталог GlassShellExt.dll
        wchar_t dllDir[MAX_PATH]{};
        if (::GetModuleFileNameW(GlassShellExt::g_module, dllDir, MAX_PATH) == 0)
            return false;
        wchar_t* slash = wcsrchr(dllDir, L'\\');
        if (!slash)
            return false;
        *(slash + 1) = L'\0';

        // Сначала искать в подпапке GlassSettings\ (новая структура)
        wchar_t candidate[MAX_PATH]{};
        if (SUCCEEDED(::StringCchPrintfW(candidate, _countof(candidate), L"%sGlassSettings\\%s", dllDir, kGlassSettingsExe)))
        {
            if (::GetFileAttributesW(candidate) != INVALID_FILE_ATTRIBUTES)
                return ::StringCchCopyW(out, cch, candidate) == S_OK;
        }

        // Fallback: рядом с GlassShellExt.dll (установленная платформа)
        const std::wstring path = GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(
            GlassShellExt::g_module, kGlassSettingsExe);
        if (!path.empty())
            return ::StringCchCopyW(out, cch, path.c_str()) == S_OK;

        // Fallback для 32-bit GlassShellExt на 64-bit ОС (GlassSettings ставится в 64-bit корень)
        auto tryRoot = [&](const wchar_t* rootDir) noexcept -> bool {
            if (!rootDir || !*rootDir)
                return false;
            std::wstring root(rootDir);
            while (!root.empty() && (root.back() == L'\\' || root.back() == L'/'))
                root.pop_back();
            if (root.empty())
                return false;
            wchar_t sub[MAX_PATH]{};
            if (SUCCEEDED(::StringCchPrintfW(sub, _countof(sub), L"%s\\GlassSettings\\%s", root.c_str(), kGlassSettingsExe))
                && ::GetFileAttributesW(sub) != INVALID_FILE_ATTRIBUTES)
            {
                return ::StringCchCopyW(out, cch, sub) == S_OK;
            }
            if (SUCCEEDED(::StringCchPrintfW(sub, _countof(sub), L"%s\\%s", root.c_str(), kGlassSettingsExe))
                && ::GetFileAttributesW(sub) != INVALID_FILE_ATTRIBUTES)
            {
                return ::StringCchCopyW(out, cch, sub) == S_OK;
            }
            return false;
        };

        auto tryReg = [&](HKEY hive, const wchar_t* valName, REGSAM sam) noexcept -> bool {
            HKEY key = nullptr;
            if (::RegOpenKeyExW(hive, L"Software\\GlassPlatform", 0, KEY_READ | sam, &key) != ERROR_SUCCESS)
                return false;
            wchar_t regBuf[MAX_PATH]{};
            DWORD cb = sizeof(regBuf);
            DWORD type = 0;
            const LSTATUS st = ::RegQueryValueExW(key, valName, nullptr, &type, reinterpret_cast<LPBYTE>(regBuf), &cb);
            ::RegCloseKey(key);
            if (st != ERROR_SUCCESS || type != REG_SZ || !regBuf[0])
                return false;
            return tryRoot(regBuf);
        };

        if (tryReg(HKEY_LOCAL_MACHINE, L"InstallPathX64", KEY_WOW64_64KEY))
            return true;
        if (tryReg(HKEY_LOCAL_MACHINE, L"InstallPath", KEY_WOW64_64KEY))
            return true;
        if (tryReg(HKEY_CURRENT_USER, L"InstallPathX64", 0))
            return true;

        PWSTR pf64 = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramFilesX64, KF_FLAG_DEFAULT, nullptr, &pf64)) && pf64)
        {
            const std::wstring defaultRoot = std::wstring(pf64) + L"\\GlassPlatform";
            ::CoTaskMemFree(pf64);
            if (tryRoot(defaultRoot.c_str()))
                return true;
        }

        return false;
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

    void LaunchPlatformStart(GS::PlatformArch arch) noexcept
    {
        if (GS::IsGlassPlatformRunning(arch))
            return;

        // Только задача планировщика — пути GlassCompat.dll ShellExt не резолвит.
        GS::StartPlatform(arch);
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
        (void)owner;
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
            LaunchPlatformStart(arch);
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

        // Cross-process busy guard: если GS уже управляет OG — не отправляем команду
        {
            constexpr wchar_t kOpenGlassBusy[] = L"Local\\GlassPlatform.OpenGlassBusy";
            HANDLE hBusy = OpenEventW(SYNCHRONIZE, FALSE, kOpenGlassBusy);
            if (hBusy)
            {
                if (WaitForSingleObject(hBusy, 0) == WAIT_OBJECT_0)
                {
                    CloseHandle(hBusy);
                    return; // GS держит busy
                }
                CloseHandle(hBusy);
            }
        }

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
