#include "GlassMaintenanceLaunch.h"

#include "GlassMaintenanceCache.h"
#include "GlassProductInspect.h"
#include "GlassInstallLog.h"

#include <GlassInstallerLaunch.h>

#include <shellapi.h>
#include <tlhelp32.h>
#include <shlwapi.h>

namespace {

[[nodiscard]] std::wstring LongPath(const std::wstring& path) noexcept
{
    if (path.empty())
        return {};
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD cch = GetLongPathNameW(path.c_str(), buffer, static_cast<DWORD>(std::size(buffer)));
    if (cch == 0 || cch >= std::size(buffer))
        return path;
    return buffer;
}

[[nodiscard]] std::wstring ModulePath() noexcept
{
    wchar_t buffer[MAX_PATH * 2]{};
    if (GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer))) == 0)
        return {};
    return buffer;
}

[[nodiscard]] DWORD ParentProcessId(DWORD pid) noexcept
{
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD parent = 0;
    if (Process32FirstW(snap, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return parent;
}

[[nodiscard]] std::wstring ProcessBaseName(DWORD pid) noexcept
{
    if (!pid)
        return {};
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return {};
    wchar_t path[MAX_PATH * 2]{};
    DWORD cch = static_cast<DWORD>(std::size(path));
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &cch) != FALSE;
    CloseHandle(process);
    if (!ok)
        return {};
    const wchar_t* name = PathFindFileNameW(path);
    return name ? std::wstring(name) : std::wstring{};
}

[[nodiscard]] bool IsShellMaintenanceParent() noexcept
{
    static constexpr const wchar_t* kAllowed[] = {
        L"explorer.exe",
        L"SystemSettings.exe",
        L"ApplicationFrameHost.exe",
        L"ShellExperienceHost.exe",
        L"sihost.exe",
    };
    const std::wstring parent = ProcessBaseName(ParentProcessId(GetCurrentProcessId()));
    if (parent.empty())
        return false;
    for (const wchar_t* allowed : kAllowed) {
        if (_wcsicmp(parent.c_str(), allowed) == 0)
            return true;
    }
    return false;
}

[[nodiscard]] bool IsTrustedCachedSetupImage() noexcept
{
    const std::wstring self = LongPath(ModulePath());
    const std::wstring canonical = LongPath(GlassMaintenanceCache::CurrentFile(L"GlassSetup.exe"));
    if (self.empty() || canonical.empty())
        return false;
    if (!GlassMaintenanceCache::TrustedPath(self, false))
        return false;
    return PathsEqualCanonical(self, canonical);
}

} // namespace

SetupCommandLine ParseSetupCommandLine() noexcept
{
    SetupCommandLine out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return out;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--modify") == 0)
            out.modify = true;
        else if (_wcsicmp(argv[i], L"--product") == 0 && i + 1 < argc)
            out.productCode = argv[++i];
    }
    LocalFree(argv);
    return out;
}

bool PathsEqualCanonical(const std::wstring& left, const std::wstring& right) noexcept
{
    if (left.empty() || right.empty())
        return false;
    return CompareStringOrdinal(LongPath(left).c_str(), -1, LongPath(right).c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool AuthorizeMaintenanceModifyLaunch() noexcept
{
    if (!IsTrustedCachedSetupImage()) {
        GlassInstallLog::Line(L"Maintenance modify refused: setup image is not the trusted cache copy");
        return false;
    }
    const std::wstring self = ModulePath();
    if (!GlassProductInspect::FileHasSameSignerAsSetup(self)) {
        GlassInstallLog::Line(L"Maintenance modify refused: publisher mismatch");
        return false;
    }
    if (Shared::GlassInstallerLaunch::TryConsumeMaintenanceTicket()) {
        GlassInstallLog::Line(L"Maintenance modify authorized via GlassSettings ticket");
        return true;
    }
    if (IsShellMaintenanceParent()) {
        GlassInstallLog::Line(L"Maintenance modify authorized via shell parent process");
        return true;
    }
    GlassInstallLog::Line(L"Maintenance modify refused: no ticket and parent is not shell");
    return false;
}
