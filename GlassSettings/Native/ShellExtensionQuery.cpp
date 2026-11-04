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

    // 0 — пункт не зарегистрирован; 1 — классическое меню (Win32 IContextMenu, HKCU-ключи:
    // Win10 всегда, Win11 — запасной путь без доверия сертификату msix); 2 — компактное меню
    // Win11 (sparse-пакет). Sparse-пакет главнее классических ключей: после его регистрации
    // ключи снимаются, а оставшиеся дублируют пункт только в legacy-меню.
    [[nodiscard]] DWORD QueryShellMenuMode() noexcept
    {
        const bool legacy = Shared::GlassShellRegistration::IsLegacyContextMenuHandlerPresent();
        if (IsWindows11OrGreater() &&
            GlassSettings::ShellSparsePackage::IsSparsePackageInstalledForCurrentUser())
            return 2u;
        return legacy ? 1u : 0u;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_IsShellExtensionRegistered() noexcept
{
    return QueryShellMenuMode() != 0u ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) DWORD WINAPI GlassSettings_GetShellMenuModeV1() noexcept
{
    return QueryShellMenuMode();
}

// Доверие подписанту GlassShellSparse.msix для текущего пользователя (ShellSparsePackage::SparseSignerTrust):
// 0 unknown/не применимо (Win10), 1 trusted, 2 untrusted, 3 msix отсутствует, 4 msix без подписи.
// subjectOut (необязательно) получает Subject подписанта, например «CN=Stanislav K.». Только чтение.
extern "C" __declspec(dllexport) DWORD WINAPI GlassSettings_GetShellSignerTrustV1(
    wchar_t* subjectOut, DWORD cchSubject) noexcept
{
    if (subjectOut && cchSubject)
        subjectOut[0] = L'\0';
    if (!IsWindows11OrGreater())
        return 0u;
    return static_cast<DWORD>(
        GlassSettings::ShellSparsePackage::QuerySparseSignerTrust(nullptr, subjectOut, cchSubject));
}
