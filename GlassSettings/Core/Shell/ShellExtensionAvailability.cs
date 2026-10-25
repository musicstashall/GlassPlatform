using GlassSettings.Core.Platform;

namespace GlassSettings.Core.Shell;

/// <summary>
/// Наличие GlassShellExt и sparse-артефактов в каталоге платформы (рядом с GlassCompat.dll),
/// а не только в папке GlassSettings.exe.
/// </summary>
internal static class ShellExtensionAvailability
{
    private const string ShellExtDll = "GlassShellExt.dll";

    public static bool IsBundledShellExtensionPresent()
    {
        var root = GlassModulePath.GetInstallRoot();
        if (string.IsNullOrEmpty(root))
            return false;

        var dllPath = Path.Combine(root, ShellExtDll);
        if (!File.Exists(dllPath))
            return false;

        if (OperatingSystem.IsWindowsVersionAtLeast(10, 0, 22000))
        {
            var manifest = Path.Combine(root, "GlassShellSparse", "AppxManifest.xml");
            return File.Exists(manifest);
        }

        return true;
    }

    public static bool CanRegisterShellExtension(bool autorunTaskExists) =>
        autorunTaskExists || IsBundledShellExtensionPresent();
}