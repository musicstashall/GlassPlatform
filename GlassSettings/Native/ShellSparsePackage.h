#pragma once

namespace GlassSettings::ShellSparsePackage
{
    enum class SparsePackageError : int
    {
        None = 0,
        ArtifactsMissing = 1,
        // Deployment rejected the msix signature: the author certificate
        // (manifest Publisher) is not in the user's TrustedPeople/Root stores.
        SignerNotTrusted = 2,
        DeploymentFailed = 3,
        ExternalLocationConflict = 4,
    };

    // Trust state of the signer of GlassShellSparse.msix on this machine for
    // this user, as the AppX deployment stack will see it. The query never
    // writes to a certificate store.
    enum class SparseSignerTrust : int
    {
        Unknown = 0,
        Trusted = 1,
        Untrusted = 2,
        MsixMissing = 3,
        Unsigned = 4,
    };

    // subjectOut (optional) receives the signer subject ("CN=...") when a
    // signature is present. exactPlatformRoot == nullptr resolves the platform
    // folder like RegisterSparsePackageQuiet does for the settings UI.
    [[nodiscard]] SparseSignerTrust QuerySparseSignerTrust(
        const wchar_t* exactPlatformRoot, wchar_t* subjectOut, size_t cchSubject) noexcept;

    // Outcome of TrustSparseSigner. Values are stable: they are returned to
    // GlassSettings (GlassSettings_TrustShellSignerV1) and, OR-ed into
    // 0x47535400, as the exit code of the Setup helper entry point.
    enum class SparseSignerTrustResult : int
    {
        Added = 0,            // certificate added to LocalMachine\TrustedPeople
        AlreadyTrusted = 1,   // thumbprint was already in LocalMachine\TrustedPeople
        MsixMissing = 2,
        Unsigned = 3,
        SubjectMismatch = 4,  // signer is not the author (kSparsePublisher): nothing is trusted
        StoreFailed = 5,      // LocalMachine\TrustedPeople could not be opened or written (lastErrorOut)
        Unknown = 6,          // signature present but the signer certificate could not be read
    };

    // The only place in the platform that writes to a certificate store, and
    // only on the user's explicit choice (Setup certificate page, GlassSettings
    // button). Takes the leaf signer straight out of the signature of the
    // shipped GlassShellSparse.msix — the exact certificate the AppX deployment
    // checks — verifies that its subject is the author (kSparsePublisher) and
    // adds it to LocalMachine\TrustedPeople (all users; requires elevation).
    // Never Root, never TrustedPublisher. The uninstaller never removes it.
    [[nodiscard]] SparseSignerTrustResult TrustSparseSigner(
        const wchar_t* exactPlatformRoot, wchar_t* subjectOut, size_t cchSubject, DWORD* lastErrorOut) noexcept;

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
    // HRESULT of the last failed deployment call (S_OK when the failure had no
    // HRESULT, e.g. missing artifacts). Lets the installer helper report the
    // concrete Windows error instead of a bare "failed".
    [[nodiscard]] HRESULT GetLastSparseDeploymentHr() noexcept;

    // A non-null exactPlatformRoot selects installer mode (Setup/Unins):
    // missing artifacts fail without falling back to other paths and an
    // existing package is never removed or replaced. The msix is shipped
    // signed by the author certificate; nothing is re-signed or trusted here.
    [[nodiscard]] bool RegisterSparsePackageQuiet(const wchar_t* exactPlatformRoot = nullptr) noexcept;

    [[nodiscard]] bool UnregisterSparsePackageQuiet(const wchar_t* exactPlatformRoot = nullptr) noexcept;
}
