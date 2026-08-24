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
        ModuleArch /*targetArch*/) noexcept
    {
        if (!installRoot || !*installRoot || !fileName || !*fileName)
            return {};

        const std::wstring root = TrimTrailingSlashes(installRoot);
        if (root.empty())
            return {};

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

        // Искать рядом с модулем
        auto result = ResolveGlassModulePath(root.c_str(), fileName, targetArch);
        if (!result.empty())
            return result;

        // Fallback: родительская папка
        // (GlassSettings в Build\x64\Debug\GlassSettings\, платформа в Build\x64\Debug\)
        const auto parent = ParentDirectory(root);
        if (!parent.empty())
            return ResolveGlassModulePath(parent.c_str(), fileName, targetArch);

        return {};
    }
}
