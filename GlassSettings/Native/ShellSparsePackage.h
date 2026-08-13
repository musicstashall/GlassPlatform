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

    /// Синоним для обратной совместимости вызовов из ShellExtensionOps.
    [[nodiscard]] inline bool IsSparsePackageRegisteredForCurrentUser() noexcept
    {
        return IsSparsePackageInstalledForCurrentUser();
    }

    [[nodiscard]] SparsePackageError GetLastSparsePackageError() noexcept;

    [[nodiscard]] bool RegisterSparsePackageQuiet() noexcept;

    [[nodiscard]] bool UnregisterSparsePackageQuiet() noexcept;
}
