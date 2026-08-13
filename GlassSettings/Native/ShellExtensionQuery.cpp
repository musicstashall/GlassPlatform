#include <Windows.h>

#include <GlassShellMenuConstants.h>
#include <GlassShellRegistration.h>

#include "ShellSparsePackage.h"

extern "C" NTSTATUS NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW);

namespace
{
    [[nodiscard]] bool IsWindows11OrGreater() noexcept
    {
        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (RtlGetVersion(&vi) != 0)
            return false;
        return vi.dwBuildNumber >= 22000;
    }

    [[nodiscard]] bool IsWin11SparseRegistrationHealthy(bool sparsePm, bool legacy) noexcept
    {
        return sparsePm && !legacy;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_IsShellExtensionRegistered() noexcept
{
    const bool legacy = Shared::GlassShellRegistration::IsLegacyContextMenuHandlerPresent();
    if (IsWindows11OrGreater())
    {
        const bool sparse = GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser();
        return IsWin11SparseRegistrationHealthy(sparse, legacy) ? TRUE : FALSE;
    }

    return legacy ? TRUE : FALSE;
}
