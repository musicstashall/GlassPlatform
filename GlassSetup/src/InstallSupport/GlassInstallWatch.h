#pragma once

#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

namespace GlassInstallWatch {

struct ModuleHolder {
    DWORD pid = 0;
    std::wstring exe;
    std::wstring modules;
};

#ifdef GLASS_SETUP_NATIVE_MONITOR
// Load the embedded x86 GlassSettings.Native into Setup for this monitoring
// session. Free it before any later MSI removal or rollback.
bool BeginLocalNativeMonitor() noexcept;
void EndLocalNativeMonitor() noexcept;
bool ForceUnloadInLocalNative(DWORD pid) noexcept;
#endif

// Local scan plus the GlassSettings process list; false means the result is incomplete.
bool CollectInProcessHolders(std::vector<ModuleHolder>& holders) noexcept;

// Токенизированное слияние списка модулей ("GlassCompat.dll, UICompat.dll") без коллизий подстрок.
void MergeModuleTokens(std::wstring& into, const std::wstring& add);

// Один resident /watchGlassModules + локальный скан до timeoutMs или пока держателей нет.
// nativeScanFailed also reports an incomplete local scan or a broken helper protocol.
bool WaitUntilClearOrTimeout(
    DWORD timeoutMs,
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool scanNativeX64,
    std::vector<ModuleHolder>& lastHolders,
    bool& nativeScanFailed,
    const std::atomic<bool>* cancelRequested = nullptr) noexcept;

bool ParseNativePipeUtf8(const std::string& bytes, std::vector<ModuleHolder>& holders);

// "#fail <diag>": the x64 helper's strict scan stopped (its last line before
// exit). diag receives the UTF-8 text as wide characters ("-" when empty).
bool ParseNativeFailLine(const std::string& line, std::wstring& diag);
// Any "#fail" line within a complete helper output (one-shot scan file).
bool FindNativeFailLine(const std::string& bytes, std::wstring& diag);

} // namespace GlassInstallWatch
