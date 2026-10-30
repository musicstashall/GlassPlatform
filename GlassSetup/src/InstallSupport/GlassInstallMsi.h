#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>

namespace GlassInstallMsi {

inline constexpr wchar_t kProductPlatformX64[] = L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5071}";
inline constexpr wchar_t kProductPlatformX86[] = L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5072}";
inline constexpr wchar_t kProductOpenGlass[]   = L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5073}";
inline constexpr wchar_t kProductSettingsX64[] = L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5074}";
inline constexpr wchar_t kProductSettingsX86[] = L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5075}";

bool MsiSuccess(UINT code) noexcept;
bool IsProductInstalled(const wchar_t* productCode) noexcept;
bool UninstallProduct(const wchar_t* productCode) noexcept;

std::wstring ProductCodeFromMsiPath(const std::wstring& msiPath);
std::wstring KnownProductCodeFromFileName(const std::wstring& msiPath);
std::wstring ResolveProductCode(const std::wstring& msiPath);

std::wstring BuildInstallPropertyString(const std::wstring& installDir, bool restoreNoCab);
std::wstring UninsExePathForProduct(const std::wstring& productCode, const std::wstring& installDir);
void PatchArpUninstallString(const std::wstring& productCode, const std::wstring& installDir) noexcept;

bool CopyLocalPackage(const wchar_t* productCode, const std::wstring& destPath,
    const std::atomic_bool* cancelled = nullptr) noexcept;
bool PrepareRestoreMsiCopy(const std::wstring& sourceMsi, const std::wstring& destMsi) noexcept;

struct ProgressSink {
    std::function<void(const std::wstring& label, int percentUnits)> onProgress;
    std::function<void(const std::wstring&)> onError;
    const std::atomic<bool>* cancelRequested = nullptr;
    std::wstring label;
    int base = 0;
    int span = 0;
    int total = 0;
    int sofar = 0;
    bool script = false;
    bool forward = true;
    bool cancellationSent = false;
};

UINT InstallProduct(const std::wstring& msiPath, const std::wstring& props, ProgressSink* progress) noexcept;

std::wstring QueryDisplayVersion(const wchar_t* productCode) noexcept;

} // namespace GlassInstallMsi
