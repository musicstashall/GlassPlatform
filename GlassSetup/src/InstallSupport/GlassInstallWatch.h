#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace GlassInstallWatch {

struct ModuleHolder {
    DWORD pid = 0;
    std::wstring exe;
    std::wstring modules;
};

// Локальный скан (x86 in-process + inject map) без x64 rundll32.
void CollectInProcessHolders(std::vector<ModuleHolder>& holders) noexcept;

// Один resident /watchGlassModules + локальный скан до timeoutMs или пока держателей нет.
// nativeScanFailed — не удалось поднять x64 watch (нет Native.dll / pipe).
bool WaitUntilClearOrTimeout(
    DWORD timeoutMs,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool scanNativeX64,
    std::vector<ModuleHolder>& lastHolders,
    bool& nativeScanFailed) noexcept;

void ParseNativePipeUtf8(const std::string& bytes, std::vector<ModuleHolder>& holders);

} // namespace GlassInstallWatch
