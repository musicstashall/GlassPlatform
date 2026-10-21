#include "SharedPch.h"

#include "framework.h"
#include "LogViewerOpenGlass.h"

#include <shellapi.h>
#include <taskschd.h>
#include <comdef.h>
#include <string>
#include <string_view>
#include <sstream>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Taskschd.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "comsuppw.lib")

namespace
{
    constexpr wchar_t kOgHostTaskNameCurrent[] = L"OpenGlass Host";
    constexpr wchar_t kOgHostTaskNameLegacy[]  = L"OpenGlass Legacy Host";

    constexpr wchar_t kBwmRegKey[] = L"Software\\StanislavKa\\GlassPlatform";
    constexpr wchar_t kInstallPathValue[] = L"InstallPath";

    [[nodiscard]] bool FileExists(const std::wstring& p) noexcept
    {
        if (p.empty())
            return false;
        const DWORD attr = ::GetFileAttributesW(p.c_str());
        return (attr != INVALID_FILE_ATTRIBUTES) && ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0);
    }

    [[nodiscard]] std::wstring ReadBlackWindowManagerInstallPathFromRegistry() noexcept
    {
        wchar_t buf[4096]{};
        DWORD type = 0;
        DWORD size = sizeof(buf);
        if (::RegGetValueW(
                HKEY_CURRENT_USER,
                kBwmRegKey,
                kInstallPathValue,
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                &type,
                buf,
                &size) == ERROR_SUCCESS
            && buf[0] != L'\0')
        {
            return std::wstring(buf);
        }
        size = sizeof(buf);
        if (::RegGetValueW(
                HKEY_LOCAL_MACHINE,
                kBwmRegKey,
                kInstallPathValue,
                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                &type,
                buf,
                &size) == ERROR_SUCCESS
            && buf[0] != L'\0')
        {
            return std::wstring(buf);
        }
        return {};
    }

    [[nodiscard]] std::wstring NormalizeDir(std::wstring p) noexcept
    {
        while (!p.empty() && (p.back() == L'\\' || p.back() == L'/'))
            p.pop_back();
        return p;
    }

    [[nodiscard]] std::wstring PathOpenGlassDllFromBwmInstall() noexcept
    {
        std::wstring base = NormalizeDir(ReadBlackWindowManagerInstallPathFromRegistry());
        if (base.empty())
            return {};
        return base + L"\\OpenGlass\\OpenGlass.dll";
    }

    [[nodiscard]] std::wstring PathOpenGlassDllFromInstallDir(std::wstring_view installDir) noexcept
    {
        if (installDir.empty())
            return {};

        const std::wstring base = NormalizeDir(std::wstring(installDir));

        // BWM установки встречались как минимум в двух вариантах:
        //  - <InstallPath>\\OpenGlass\\OpenGlass.dll
        //  - <InstallPath>\\openglass\\OpenGlass.dll
        const std::wstring candidates[] = {
            base + L"\\openglass\\OpenGlass.dll",
            base + L"\\OpenGlass\\OpenGlass.dll",
            base + L"\\OpenGlass.dll",
        };

        for (const std::wstring& c : candidates)
        {
            if (FileExists(c))
                return c;
        }
        return {};
    }

    // Временно: при отсутствии InstallPath в реестре (см. PathOpenGlassDllFromBwmInstall).
    constexpr wchar_t kHardcodedOpenGlassDll[] =
        L"C:\\Program Files (x86)\\BlackWindowManager\\openglass\\OpenGlass.dll";

    [[nodiscard]] std::wstring PathOpenGlassDllNextToExe() noexcept
    {
        wchar_t buf[MAX_PATH]{};
        if (::GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0)
            return {};
        std::wstring p(buf);
        const size_t sl = p.find_last_of(L"\\/");
        if (sl != std::wstring::npos)
            p.resize(sl + 1);
        else
            p.clear();
        p += L"OpenGlass.dll";
        return p;
    }

    [[nodiscard]] std::wstring ResolveOpenGlassDllPath() noexcept
    {
        const std::wstring installDir = ReadBlackWindowManagerInstallPathFromRegistry();
        if (!installDir.empty())
        {
            std::wstring fromInstall = PathOpenGlassDllFromInstallDir(installDir);
            if (!fromInstall.empty())
                return fromInstall;
        }

        // Исторический путь (через InstallPath + \\OpenGlass\\OpenGlass.dll), но теперь с проверкой существования.
        std::wstring fromReg = PathOpenGlassDllFromBwmInstall();
        if (FileExists(fromReg))
            return fromReg;

        // Временно: без InstallPath в реестре — типовая установка x86; если файла нет — рядом с LogViewer.
        if (FileExists(std::wstring(kHardcodedOpenGlassDll)))
            return std::wstring(kHardcodedOpenGlassDll);
        std::wstring nextToExe = PathOpenGlassDllNextToExe();
        if (FileExists(nextToExe))
            return nextToExe;
        return std::wstring(kHardcodedOpenGlassDll);
    }

    [[nodiscard]] std::wstring ParentDirOfPath(const std::wstring& p) noexcept
    {
        const size_t sl = p.find_last_of(L"\\/");
        if (sl == std::wstring::npos)
            return {};
        return p.substr(0, sl);
    }

    [[nodiscard]] std::wstring ResolveRundll32Path() noexcept
    {
        wchar_t sysDir[MAX_PATH]{};
        if (::GetSystemDirectoryW(sysDir, MAX_PATH) == 0)
            return {};
        return std::wstring(sysDir) + L"\\rundll32.exe";
    }

    [[nodiscard]] std::wstring TryGetShortPath(const std::wstring& p) noexcept
    {
        if (p.empty())
            return {};

        DWORD n = ::GetShortPathNameW(p.c_str(), nullptr, 0);
        if (n == 0)
            return {};

        std::wstring out;
        out.resize(n);
        n = ::GetShortPathNameW(p.c_str(), out.data(), n);
        if (n == 0)
            return {};

        // n не включает терминатор, но resize(n) оставляет лишний '\0' внутри строки.
        out.resize(n);
        return out;
    }

    [[nodiscard]] bool ShellExecuteRunasWaitForExit(
        const std::wstring& file,
        const std::wstring& params,
        const std::wstring& dir,
        DWORD timeoutMs,
        DWORD& exitCodeOut) noexcept
    {
        exitCodeOut = 0xFFFFFFFFu;

        DWORD lastErr = 0;
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.hwnd = nullptr;
        sei.lpVerb = L"runas";
        sei.lpFile = file.c_str();
        sei.lpParameters = params.c_str();
        sei.lpDirectory = dir.empty() ? nullptr : dir.c_str();
        sei.nShow = SW_SHOWNORMAL;

        if (!::ShellExecuteExW(&sei) || !sei.hProcess)
        {
            lastErr = ::GetLastError();
            std::wstringstream ss;
            ss << L"[LogViewer][OpenGlass] ShellExecuteExW failed: lastError=" << lastErr;
            ::OutputDebugStringW(ss.str().c_str());
            ::OutputDebugStringW(L"\n");
            return false;
        }

        const DWORD pid = ::GetProcessId(sei.hProcess);
        {
            std::wstringstream ss;
            ss << L"[LogViewer][OpenGlass] rundll32 pid=" << pid << L" waitMs=" << timeoutMs;
            ::OutputDebugStringW(ss.str().c_str());
            ::OutputDebugStringW(L"\n");
        }

        const DWORD w = ::WaitForSingleObject(sei.hProcess, timeoutMs);
        DWORD ec = 0xFFFFFFFFu;
        if (w == WAIT_OBJECT_0)
        {
            if (!::GetExitCodeProcess(sei.hProcess, &ec))
                ec = 0xFFFFFFFFu;
        }
        else if (w == WAIT_TIMEOUT)
        {
            // Если не дождались — процесс, скорее всего, жив и работает; exitCode не трогаем.
            ec = 0xFFFFFFFEu;
        }
        else
        {
            // WAIT_FAILED и пр.
            ec = 0xFFFFFFFDu;
        }
        ::CloseHandle(sei.hProcess);

        exitCodeOut = ec;
        return true;
    }

    struct ComInitGuard
    {
        HRESULT hr = E_FAIL;
        bool needUninit = false;

        ComInitGuard() noexcept
        {
            hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(hr))
                needUninit = true;
            if (hr == RPC_E_CHANGED_MODE)
                hr = S_OK; // COM уже инициализирован в другом режиме.

            const HRESULT secHr = ::CoInitializeSecurity(
                nullptr,
                -1,
                nullptr,
                nullptr,
                RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                nullptr,
                0,
                nullptr);
            (void)secHr; // RPC_E_TOO_LATE допустим, если security уже задана в процессе.
        }

        ~ComInitGuard() noexcept
        {
            if (needUninit)
                ::CoUninitialize();
        }
    };

    [[nodiscard]] bool OpenLegacyHostTask(IRegisteredTask** taskOut) noexcept
    {
        if (!taskOut)
            return false;
        *taskOut = nullptr;

        ITaskService* service = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService, reinterpret_cast<void**>(&service));
        if (FAILED(hr) || !service)
            return false;

        VARIANT empty{};
        VariantInit(&empty);
        hr = service->Connect(empty, empty, empty, empty);
        if (FAILED(hr))
        {
            service->Release();
            return false;
        }

        ITaskFolder* root = nullptr;
        hr = service->GetFolder(_bstr_t(L"\\"), &root);
        if (FAILED(hr) || !root)
        {
            service->Release();
            return false;
        }

        IRegisteredTask* task = nullptr;
        hr = root->GetTask(_bstr_t(kOgHostTaskNameCurrent), &task);
        if (FAILED(hr) || !task)
        {
            task = nullptr;
            hr = root->GetTask(_bstr_t(kOgHostTaskNameLegacy), &task);
        }
        root->Release();
        service->Release();
        if (FAILED(hr) || !task)
            return false;

        *taskOut = task;
        return true;
    }

    [[nodiscard]] bool QueryLegacyHostTaskRunningCom() noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return false;

        IRegisteredTask* task = nullptr;
        if (!OpenLegacyHostTask(&task))
            return false;

        TASK_STATE state = TASK_STATE_UNKNOWN;
        const HRESULT hr = task->get_State(&state);
        task->Release();
        if (FAILED(hr))
            return false;
        return state == TASK_STATE_RUNNING;
    }

    [[nodiscard]] bool StartLegacyHostTaskCom() noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return false;

        IRegisteredTask* task = nullptr;
        if (!OpenLegacyHostTask(&task))
            return false;

        IRunningTask* running = nullptr;
        VARIANT empty{};
        VariantInit(&empty);
        const HRESULT hr = task->Run(empty, &running);
        if (running)
            running->Release();
        task->Release();
        if (FAILED(hr))
        {
            std::wstringstream ss;
            ss << L"[LogViewer][OpenGlass] COM Run failed hr=0x" << std::hex << static_cast<unsigned long>(hr);
            ::OutputDebugStringW(ss.str().c_str());
            ::OutputDebugStringW(L"\n");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool StopLegacyHostTaskCom() noexcept
    {
        ComInitGuard com;
        if (FAILED(com.hr))
            return false;

        IRegisteredTask* task = nullptr;
        if (!OpenLegacyHostTask(&task))
            return false;

        const HRESULT hr = task->Stop(0);
        task->Release();
        if (FAILED(hr))
        {
            std::wstringstream ss;
            ss << L"[LogViewer][OpenGlass] COM Stop failed hr=0x" << std::hex << static_cast<unsigned long>(hr);
            ::OutputDebugStringW(ss.str().c_str());
            ::OutputDebugStringW(L"\n");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool WaitForLegacyHostState(bool shouldRun, DWORD timeoutMs) noexcept
    {
        const DWORD stepMs = 300;
        DWORD elapsed = 0;
        while (elapsed <= timeoutMs)
        {
            const bool running = QueryLegacyHostTaskRunningCom();
            if (running == shouldRun)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
            elapsed += stepMs;
        }
        return false;
    }

    [[nodiscard]] bool TryElevatedRundllOpenGlass(std::wstring_view argAfterComma) noexcept
    {
        const std::wstring dll = ResolveOpenGlassDllPath();
        if (dll.empty())
            return false;
        const std::wstring rundll = ResolveRundll32Path();
        if (rundll.empty())
            return false;

        // Rundll32: "path\OpenGlass.dll",OpenGlass_RunDLL /startup
        const std::wstring dllDir = ParentDirOfPath(dll);

        const std::wstring dllShort = TryGetShortPath(dll);
        const std::wstring dllDirShort = TryGetShortPath(dllDir);

        const std::wstring dllForCmd = dllShort.empty() ? dll : dllShort;
        const std::wstring dirForCmd = dllDirShort.empty() ? dllDir : dllDirShort;

        std::wstring params = L"\"" + dllForCmd + L"\"," + std::wstring(argAfterComma);

        {
            std::wstringstream ss;
            ss << L"[LogViewer][OpenGlass] Launch(v3) rundll=\"" << rundll
               << L"\" dir=\"" << dirForCmd
               << L"\" params=\"" << params << L"\"";
            ::OutputDebugStringW(ss.str().c_str());
            ::OutputDebugStringW(L"\n");
        }

        DWORD exitCode = 0xFFFFFFFFu;
        ::OutputDebugStringW(L"[LogViewer][OpenGlass] Before ShellExecuteExW\n");
        const bool started = ShellExecuteRunasWaitForExit(rundll, params, dirForCmd, 30000, exitCode);
        ::OutputDebugStringW(L"[LogViewer][OpenGlass] After ShellExecuteExW\n");
        {
            std::wstringstream ss;
            ss << L"[LogViewer][OpenGlass] ShellExecuteExW started=" << (started ? 1 : 0)
               << L" exitCode=0x" << std::hex << exitCode;
            ::OutputDebugStringW(ss.str().c_str());
            ::OutputDebugStringW(L"\n");
        }
        return started;
    }
} // namespace

namespace LogViewer::OpenGlass
{
    bool TryLaunchOpenGlassStartup() noexcept
    {
        const bool cmdOk = StartLegacyHostTaskCom();
        const bool reached = WaitForLegacyHostState(true, 7000);
        {
            std::wstringstream ss;
            ss << L"[LogViewer][OpenGlass] startup(com) cmdOk=" << (cmdOk ? 1 : 0)
               << L" reachedRunning=" << (reached ? 1 : 0);
            ::OutputDebugStringW(ss.str().c_str());
            ::OutputDebugStringW(L"\n");
        }
        return cmdOk && reached;
    }

    bool TryLaunchOpenGlassShutdown() noexcept
    {
        const bool cmdOk = StopLegacyHostTaskCom();
        const bool reached = WaitForLegacyHostState(false, 7000);
        {
            std::wstringstream ss;
            ss << L"[LogViewer][OpenGlass] shutdown(com) cmdOk=" << (cmdOk ? 1 : 0)
               << L" reachedStopped=" << (reached ? 1 : 0);
            ::OutputDebugStringW(ss.str().c_str());
            ::OutputDebugStringW(L"\n");
        }
        return cmdOk && reached;
    }

    bool IsLegacyHostTaskRunning() noexcept
    {
        return QueryLegacyHostTaskRunningCom();
    }
} // namespace LogViewer::OpenGlass
