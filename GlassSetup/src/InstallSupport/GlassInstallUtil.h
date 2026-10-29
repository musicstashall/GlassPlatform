#pragma once

#include <GlassPlatformTask.h>
#include <string>

namespace GlassInstallUtil {

bool PeMatches(const std::wstring& path, WORD machine, const char* exportName) noexcept;
std::wstring NativeRundll64();

std::wstring NativeDllForArch(
    GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir) noexcept;

bool RunHiddenCommand(const std::wstring& commandLine, DWORD* exitCode, DWORD timeoutMs) noexcept;
bool RunHidden(const wchar_t* file, const std::wstring& args) noexcept;

void CloseGlassSettingsProcesses() noexcept;
bool ImageRunning(const wchar_t* imageName) noexcept;

} // namespace GlassInstallUtil
