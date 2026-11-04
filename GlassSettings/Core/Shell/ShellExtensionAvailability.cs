using GlassSettings.Core.Platform;

namespace GlassSettings.Core.Shell;

/// <summary>
/// Наличие компонентов контекстного меню в каталоге платформы: подкаталог
/// <c>GlassShellSparse\</c> (GlassShellExt.dll, GlassShellHost.exe, AppxManifest.xml, msix) —
/// одновременно ExternalLocation sparse-пакета. См. GlassShared\include\GlassShellMenuConstants.h.
/// </summary>
internal static class ShellExtensionAvailability
{
    public const string SparseSubdir = "GlassShellSparse";
    public const string ShellExtDll = "GlassShellExt.dll";
    public const string AuthorCertificateFile = "DeveloperCodeSigning.cer";
    public const string AuthorCertificateReadme = "DeveloperCodeSigning.txt";

    public static bool IsBundledShellExtensionPresent()
    {
        var root = GlassModulePath.GetInstallRoot();
        if (string.IsNullOrEmpty(root))
            return false;

        var dllPath = Path.Combine(root, SparseSubdir, ShellExtDll);
        if (!File.Exists(dllPath))
            return false;

        if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000))
        {
            var manifest = Path.Combine(root, SparseSubdir, "AppxManifest.xml");
            return File.Exists(manifest);
        }

        return true;
    }

    /// <summary>Путь к DeveloperCodeSigning.cer в корне платформы (или пустая строка).</summary>
    public static string GetAuthorCertificatePath()
    {
        var root = GlassModulePath.GetInstallRoot();
        if (string.IsNullOrEmpty(root))
            return string.Empty;
        var path = Path.Combine(root, AuthorCertificateFile);
        return File.Exists(path) ? path : string.Empty;
    }

    public static bool CanRegisterShellExtension(bool autorunTaskExists) =>
        autorunTaskExists || IsBundledShellExtensionPresent();
}