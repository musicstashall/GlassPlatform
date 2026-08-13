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

    // Каталог установки: dirname(modulePath); если basename — Sys32/Sys64, поднимаемся на уровень.
    [[nodiscard]] std::wstring GetInstallRootFromPath(const wchar_t* fileOrDirPath) noexcept;

    [[nodiscard]] std::wstring GetInstallRootFromModule(HMODULE hModule) noexcept;

    // OS x64 + x64: Sys64 затем root;
    // OS x64 + x86 из процесса x64: только Sys32;
    // OS x64 + x86 из процесса x86 (WOW64) / OS x86: Sys32 затем root.
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
