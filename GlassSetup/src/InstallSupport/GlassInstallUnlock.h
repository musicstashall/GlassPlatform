#pragma once

#include "GlassInstallWatch.h"
#include <windows.h>
#include <string>
#include <vector>

namespace GlassInstallUnlock {

using ModuleHolder = GlassInstallWatch::ModuleHolder;

std::vector<ModuleHolder> CollectHolders(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool scanNativeX64,
    bool& nativeScanFailed) noexcept;

std::wstring FormatHolders(const std::vector<ModuleHolder>& holders);
bool TargetIsWow64(DWORD pid) noexcept;

bool ForceUnloadHolder(
    const ModuleHolder& holder,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept;

void CloseProcessWindows(DWORD pid) noexcept;
void UnlockProcess(DWORD pid, const wchar_t* exe) noexcept;

} // namespace GlassInstallUnlock
