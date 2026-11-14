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
    bool& nativeScanFailed,
    const std::atomic_bool* cancelRequested = nullptr) noexcept;

std::wstring FormatHolders(const std::vector<ModuleHolder>& holders);
bool TargetIsWow64(DWORD pid) noexcept;

bool ForceUnloadHolder(
    const ModuleHolder& holder,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept;

void CloseMatchingProcessWindows(DWORD pid, const wchar_t* exe) noexcept;
void UnlockProcess(DWORD pid, const wchar_t* exe) noexcept;

} // namespace GlassInstallUnlock
