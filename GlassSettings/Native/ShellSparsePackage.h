#pragma once

namespace GlassSettings::ShellSparsePackage
{
    enum class SparsePackageError : int
    {
        None = 0,
        ArtifactsMissing = 1,
        DevCertFailed = 2,
        DeploymentFailed = 3,
        ExternalLocationConflict = 4,
    };

    [[nodiscard]] bool IsSparsePackageInstalledForCurrentUser() noexcept;

    // Installer must distinguish a verified current-user registration at its
    // exact root from another user's/another root's package and query failures.
    enum class InstallerSparseState { Absent, Matching, Conflict, Unknown };
    [[nodiscard]] InstallerSparseState QueryInstallerSparseState(const wchar_t* exactPlatformRoot) noexcept;
    // The package identity alone is also used by the unregister stub.
    [[nodiscard]] bool InstallerSparseHasVerifiedMenu(const wchar_t* exactPlatformRoot) noexcept;
    [[nodiscard]] bool UnregisterInstallerSparsePackageQuiet(const wchar_t* exactPlatformRoot) noexcept;

    /// Синоним для обратной совместимости вызовов из ShellExtensionOps.
    [[nodiscard]] inline bool IsSparsePackageRegisteredForCurrentUser() noexcept
    {
        return IsSparsePackageInstalledForCurrentUser();
    }

    [[nodiscard]] SparsePackageError GetLastSparsePackageError() noexcept;

    // A non-null exactPlatformRoot is used only by the installer helper; in
    // that mode missing artifacts fail without falling back to other paths.
    [[nodiscard]] bool RegisterSparsePackageQuiet(bool provisionDevCertificate = true,
        const wchar_t* exactPlatformRoot = nullptr) noexcept;

    [[nodiscard]] bool UnregisterSparsePackageQuiet(const wchar_t* exactPlatformRoot = nullptr) noexcept;
}
