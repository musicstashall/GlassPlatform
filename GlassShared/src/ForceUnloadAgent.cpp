#include "SharedPch.h"

#include <ForceUnloadAgent.h>
#include <GlassModulePath.h>
#include <GlassPlatformTask.h>
#include <InjectModuleScan.h>

namespace Shared::ForceUnloadAgent
{
    namespace
    {
        [[nodiscard]] std::wstring TrimTrailingSlashes(std::wstring path) noexcept
        {
            while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
                path.pop_back();
            return path;
        }

        [[nodiscard]] bool ProcessStillHasGlassModules(DWORD pid) noexcept
        {
            const DWORD pids[1] = { pid };
            InjectModuleScan::Result results[1]{};
            DWORD count = 0;
            if (!InjectModuleScan::Scan(pids, 1, results, 1, &count))
                return true;
            if (count == 0)
                return false;
            return results[0].moduleBits != 0;
        }

        [[nodiscard]] bool LaunchRundllForceUnload(DWORD pid, const wchar_t* x86InstallRoot) noexcept
        {
            const std::wstring dllPath = GlassShared::GlassModulePath::ResolveGlassModulePath(
                x86InstallRoot,
                L"GlassCompat.dll",
                GlassShared::GlassModulePath::ModuleArch::X86);
            if (dllPath.empty())
                return false;

            wchar_t sysWow64[MAX_PATH]{};
            const UINT n = GetSystemWow64DirectoryW(sysWow64, MAX_PATH);
            if (n == 0 || n >= MAX_PATH)
                return false;

            const std::wstring rundll = TrimTrailingSlashes(std::wstring(sysWow64)) + L"\\rundll32.exe";
            std::wstring cmdLine = L'"' + rundll + L"\" \"" + dllPath + L"\",Main /forceUnload "
                + std::to_wstring(static_cast<unsigned long>(pid));

            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;

            PROCESS_INFORMATION pi{};
            std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
            cmdBuf.push_back(L'\0');

            if (!CreateProcessW(
                    nullptr,
                    cmdBuf.data(),
                    nullptr,
                    nullptr,
                    FALSE,
                    CREATE_NO_WINDOW,
                    nullptr,
                    nullptr,
                    &si,
                    &pi))
            {
                return false;
            }

            const DWORD wait = WaitForSingleObject(pi.hProcess, 45000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return wait == WAIT_OBJECT_0;
        }

        [[nodiscard]] bool RootHasGlassCompat(const std::wstring& root) noexcept
        {
            if (root.empty())
                return false;
            const std::wstring dll = GlassShared::GlassModulePath::ResolveGlassModulePath(
                root.c_str(),
                L"GlassCompat.dll",
                GlassShared::GlassModulePath::ModuleArch::X86);
            return !dll.empty();
        }

        [[nodiscard]] std::wstring TryGetX86InstallRootFromRegistry() noexcept
        {
#if defined(_WIN64)
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(
                    HKEY_CURRENT_USER,
                    L"Software\\Wow6432Node\\GlassPlatform",
                    0,
                    KEY_READ,
                    &hKey)
                != ERROR_SUCCESS)
            {
                return {};
            }

            wchar_t buf[MAX_PATH * 4]{};
            DWORD cb = sizeof(buf);
            DWORD type = 0;
            const LSTATUS st = RegQueryValueExW(
                hKey,
                L"InstallPath",
                nullptr,
                &type,
                reinterpret_cast<LPBYTE>(buf),
                &cb);
            RegCloseKey(hKey);

            if (st != ERROR_SUCCESS || type != REG_SZ || !buf[0])
                return {};

            return TrimTrailingSlashes(buf);
#else
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(
                    HKEY_CURRENT_USER,
                    L"Software\\GlassPlatform",
                    0,
                    KEY_READ,
                    &hKey)
                != ERROR_SUCCESS)
            {
                return {};
            }

            wchar_t buf[MAX_PATH * 4]{};
            DWORD cb = sizeof(buf);
            DWORD type = 0;
            const LSTATUS st = RegQueryValueExW(
                hKey,
                L"InstallPath",
                nullptr,
                &type,
                reinterpret_cast<LPBYTE>(buf),
                &cb);
            RegCloseKey(hKey);

            if (st != ERROR_SUCCESS || type != REG_SZ || !buf[0])
                return {};

            return TrimTrailingSlashes(buf);
#endif
        }
    }

    std::wstring TryGetX86InstallRoot() noexcept
    {
        std::wstring root = TryGetX86InstallRootFromRegistry();
        if (RootHasGlassCompat(root))
            return root;

        root = GlassShared::GlassPlatformTask::TryGetAutorunGlassCompatInstallRoot(
            GlassShared::GlassPlatformTask::PlatformArch::X86);
        if (RootHasGlassCompat(root))
            return root;

        return {};
    }

    bool SpawnX86ForceUnloadAgent(DWORD pid, const wchar_t* x86InstallRoot) noexcept
    {
        if (pid == 0 || !x86InstallRoot || !x86InstallRoot[0])
            return false;

        if (!LaunchRundllForceUnload(pid, x86InstallRoot))
            return false;

        return !ProcessStillHasGlassModules(pid);
    }
}
