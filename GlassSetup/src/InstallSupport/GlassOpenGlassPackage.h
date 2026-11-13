#pragma once
#include <Windows.h>
#include <atomic>
#include <string>
#include <cstdint>

namespace GlassOpenGlassInstall { struct Record; }

namespace GlassOpenGlassPackage {
struct Package {
    std::wstring version,setup,dll,host,gui;
    bool milComp=false;
};
// Metadata only: no workspace, payload download or execution.
bool Describe(const std::atomic<bool>& cancel,std::wstring& version,std::uint64_t& size,std::wstring& error) noexcept;
// Official-installer staging: adjacent signed EXE first, otherwise release.
// No payload extraction and no execution; used by the Inno servicing transition.
bool PrepareInstallerForCurrentSystem(const std::wstring& workspace,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept;
bool PrepareInstaller(const std::wstring& workspace,unsigned windowsBuild,
    const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept;
// Runs only a verified protected staged installer, with no shell. Waits for
// completion; never terminates an Inno transaction. Caller owns commit policy
// and must interpret raw exitCode (0 success, 3010 reboot with the supplied flag).
// Official registered Inno uninstaller only; protected EXE/data and publisher
// verification are mandatory. No mutation of the original EXE/ADS/config.
// Presets are explicitly removed; config stays unless the caller has cleanup consent.
bool RunUninstaller(const GlassOpenGlassInstall::Record& record,DWORD& exitCode,std::wstring& error, bool deleteConfig=false) noexcept;
bool RunInstaller(const Package& package,const std::wstring& directory,DWORD& exitCode,std::wstring& error) noexcept;
// Protected staging ONLY: no installation, service registration or upstream EXE
// execution. Caller must implement product snapshot/commit/rollback separately.
bool PrepareForCurrentSystem(const std::wstring& workspace,const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept;
bool Prepare(const std::wstring& workspace,unsigned windowsBuild,
    const std::atomic<bool>& cancel,Package& package,std::wstring& error) noexcept;
}
