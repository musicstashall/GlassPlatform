#pragma once

#include <Windows.h>

#include <string>

namespace Shared::ForceUnloadAgent
{
    /// HKCU InstallPath (Wow6432Node из x64) или каталог из /TR x86-задачи автозапуска.
    [[nodiscard]] std::wstring TryGetX86InstallRoot() noexcept;

    /// SysWOW64\rundll32 + x86 GlassCompat Main /forceUnload. Возвращает true, если после агента
    /// Toolhelp не видит Glass-модулей в целевом PID (same-bitness FreeLibrary внутри агента).
    [[nodiscard]] bool SpawnX86ForceUnloadAgent(DWORD pid, const wchar_t* x86InstallRoot) noexcept;
}
