#pragma once

#include <windows.h>
#include <functional>
#include <string>

namespace GlassInstallMsi {

bool MsiSuccess(UINT code) noexcept;
bool UninstallProduct(const wchar_t* productCode) noexcept;
bool ReinstallProduct(const wchar_t* productCode, const wchar_t* installDir) noexcept;

std::wstring ProductCodeFromMsiPath(const std::wstring& msiPath);
std::wstring KnownProductCodeFromFileName(const std::wstring& msiPath);
std::wstring ResolveProductCode(const std::wstring& msiPath);
std::wstring SettingsProductCodeForPlatform(const wchar_t* platformProductCode) noexcept;

std::wstring BuildInstallPropertyString(const std::wstring& installDir, bool restoreNoCab);

bool CopyLocalPackage(const wchar_t* productCode, const std::wstring& destPath) noexcept;
bool PrepareRestoreMsiCopy(const std::wstring& sourceMsi, const std::wstring& destMsi) noexcept;

struct ProgressSink {
    std::function<void(const std::wstring& label, int percentUnits)> onProgress;
    std::wstring label;
    int base = 0;
    int span = 0;
};

UINT InstallProduct(const std::wstring& msiPath, const std::wstring& props, ProgressSink* progress) noexcept;

std::wstring QueryDisplayVersion(const wchar_t* productCode) noexcept;

} // namespace GlassInstallMsi
