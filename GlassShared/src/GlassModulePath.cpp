#include "SharedPch.h"

#include <GlassModulePath.h>

#include <string>

namespace GlassShared::GlassModulePath
{
    namespace
    {
        [[nodiscard]] bool FileExists(const std::wstring& path) noexcept
        {
            const DWORD attrs = ::GetFileAttributesW(path.c_str());
            return attrs != INVALID_FILE_ATTRIBUTES
                && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        [[nodiscard]] std::wstring TrimTrailingSlashes(std::wstring path) noexcept
        {
            while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
                path.pop_back();
            return path;
        }

        [[nodiscard]] std::wstring ParentDirectory(std::wstring path) noexcept
        {
            path = TrimTrailingSlashes(std::move(path));
            const size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
                return {};
            return path.substr(0, slash);
        }

        [[nodiscard]] std::wstring BaseName(std::wstring path) noexcept
        {
            path = TrimTrailingSlashes(std::move(path));
            const size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
                return path;
            return path.substr(slash + 1);
        }

        [[nodiscard]] bool EqualsIgnoreCase(const std::wstring& a, const wchar_t* b) noexcept
        {
            return ::_wcsicmp(a.c_str(), b) == 0;
        }
    }

    bool IsOs64Bit() noexcept
    {
#ifdef _WIN64
        return true;
#else
        BOOL wow = FALSE;
        if (!::IsWow64Process(::GetCurrentProcess(), &wow))
            return false;
        return wow != FALSE;
#endif
    }

    std::wstring GetInstallRootFromPath(const wchar_t* fileOrDirPath) noexcept
    {
        if (!fileOrDirPath || !*fileOrDirPath)
            return {};

        std::wstring path = fileOrDirPath;
        const DWORD attrs = ::GetFileAttributesW(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0)
            path = ParentDirectory(std::move(path));
        else
            path = TrimTrailingSlashes(std::move(path));

        const std::wstring leaf = BaseName(path);
        if (EqualsIgnoreCase(leaf, L"Sys64") || EqualsIgnoreCase(leaf, L"Sys32"))
            path = ParentDirectory(std::move(path));

        return TrimTrailingSlashes(std::move(path));
    }

    std::wstring GetInstallRootFromModule(HMODULE hModule) noexcept
    {
        wchar_t buf[MAX_PATH]{};
        const DWORD n = ::GetModuleFileNameW(hModule, buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH)
            return {};
        return GetInstallRootFromPath(buf);
    }

    std::wstring ResolveGlassModulePath(
        const wchar_t* installRoot,
        const wchar_t* fileName,
        ModuleArch targetArch) noexcept
    {
        if (!installRoot || !*installRoot || !fileName || !*fileName)
            return {};

        const std::wstring root = TrimTrailingSlashes(installRoot);
        if (root.empty())
            return {};

        const bool os64 = IsOs64Bit();
        const bool want64 = targetArch == ModuleArch::X64;

        if (os64)
        {
            if (want64)
            {
                const std::wstring sys64 = root + L"\\Sys64\\" + fileName;
                if (FileExists(sys64))
                    return sys64;
                const std::wstring atRoot = root + L"\\" + fileName;
                if (FileExists(atRoot))
                    return atRoot;
                return {};
            }

            // OS x64 + цель x86:
            // — процесс x64 (GS x64): только Sys32 (не брать x64-DLL из корня);
            // — процесс x86/WOW64 (GS x86, инжект): Sys32 затем корень своего installRoot.
            const std::wstring sys32 = root + L"\\Sys32\\" + fileName;
            if (FileExists(sys32))
                return sys32;
#ifdef _WIN64
            return {};
#else
            const std::wstring atRoot = root + L"\\" + fileName;
            return FileExists(atRoot) ? atRoot : std::wstring{};
#endif
        }

        const std::wstring sys32 = root + L"\\Sys32\\" + fileName;
        if (FileExists(sys32))
            return sys32;
        const std::wstring atRoot = root + L"\\" + fileName;
        return FileExists(atRoot) ? atRoot : std::wstring{};
    }

    std::wstring ResolveGlassModulePathNextToModule(
        HMODULE hModule,
        const wchar_t* fileName,
        ModuleArch targetArch) noexcept
    {
        const std::wstring root = GetInstallRootFromModule(hModule);
        if (root.empty())
            return {};
        return ResolveGlassModulePath(root.c_str(), fileName, targetArch);
    }
}
