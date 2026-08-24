#pragma once

#include <Windows.h>

#include <string>

namespace GlassShared::GlassModulePath
{
    enum class ModuleArch : int
    {
        X86 = 0,
        X64 = 1,
    };

    [[nodiscard]] constexpr ModuleArch NativeModuleArch() noexcept
    {
#ifdef _WIN64
        return ModuleArch::X64;
#else
        return ModuleArch::X86;
#endif
    }

    [[nodiscard]] bool IsOs64Bit() noexcept;

    // Каталог установки: dirname(modulePath) или сам каталог.
    [[nodiscard]] std::wstring GetInstallRootFromPath(const wchar_t* fileOrDirPath) noexcept;

    [[nodiscard]] std::wstring GetInstallRootFromModule(HMODULE hModule) noexcept;

    // Плоский корень: <installRoot>\<fileName>. targetArch сохранён для ABI; путь не зависит от arch.
    [[nodiscard]] std::wstring ResolveGlassModulePath(
        const wchar_t* installRoot,
        const wchar_t* fileName,
        ModuleArch targetArch) noexcept;

    [[nodiscard]] std::wstring ResolveGlassModulePathNextToModule(
        HMODULE hModule,
        const wchar_t* fileName,
        ModuleArch targetArch) noexcept;

    [[nodiscard]] inline std::wstring ResolveGlassModulePathNextToModule(
        HMODULE hModule,
        const wchar_t* fileName) noexcept
    {
        return ResolveGlassModulePathNextToModule(hModule, fileName, NativeModuleArch());
    }
}
