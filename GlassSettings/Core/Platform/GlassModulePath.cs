namespace GlassSettings.Core.Platform;

using GlassSettings.Services;

/// <summary>
/// Поиск модулей: GS своей битности — корень exe; GS x64 → x86 — Wow6432Node InstallPath
/// или каталог из /TR x86-задачи автозапуска.
/// Плоский корень рядом с exe/DLL.
/// </summary>
public static class GlassModulePath
{
    public static string? GetInstallRoot(string? baseDirectory = null)
    {
        var dir = string.IsNullOrWhiteSpace(baseDirectory)
            ? AppContext.BaseDirectory
            : baseDirectory;
        var root = Path.GetFullPath(dir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));

        // Если GlassCompat.dll нет рядом — искать в родительской папке
        // (GlassSettings в Build\x64\Debug\GlassSettings\, платформа в Build\x64\Debug\)
        if (!File.Exists(Path.Combine(root, "GlassCompat.dll")))
        {
            var parent = Directory.GetParent(root)?.FullName;
            if (parent != null && File.Exists(Path.Combine(parent, "GlassCompat.dll")))
                return parent;
        }

        return root;
    }

    public static string? GetInstallRootForArch(GlassPlatformArch targetArch, string? installRoot = null)
    {
        if (targetArch == GlassPlatformNames.NativeArch)
            return GetInstallRoot(installRoot);

        if (targetArch == GlassPlatformArch.X86 && Environment.Is64BitProcess)
            return TryResolveX86InstallRootCrossArch();

        return null;
    }

    private static string? TryResolveX86InstallRootCrossArch()
    {
        var fromRegistry = PlatformInstallRegistry.TryGetInstallPath(GlassPlatformArch.X86);
        if (IsValidPlatformRoot(fromRegistry))
            return fromRegistry;

        var fromTask = NativeHostInterop.TryGetAutorunGlassCompatInstallRoot(GlassPlatformArch.X86);
        if (IsValidPlatformRoot(fromTask))
            return fromTask;

        return null;
    }

    private static bool IsValidPlatformRoot(string? root)
    {
        if (string.IsNullOrEmpty(root))
            return false;
        return File.Exists(Path.Combine(root, "GlassCompat.dll"));
    }

    public static bool IsPlatformInstalled(GlassPlatformArch arch)
    {
        var root = GetInstallRootForArch(arch);
        return IsValidPlatformRoot(root);
    }

    public static string? Resolve(string fileName, GlassPlatformArch targetArch, string? installRoot = null)
    {
        if (string.IsNullOrWhiteSpace(fileName))
            return null;

        var root = GetInstallRootForArch(targetArch, installRoot);
        if (string.IsNullOrEmpty(root))
            return null;

        var atRoot = Path.Combine(root, fileName);
        return File.Exists(atRoot) ? atRoot : null;
    }

    public static string? ResolveNative(string fileName, string? installRoot = null) =>
        Resolve(fileName, GlassPlatformNames.NativeArch, installRoot);

    public static string GetRundll32Path(GlassPlatformArch arch)
    {
        if (arch == GlassPlatformArch.X86 && Environment.Is64BitOperatingSystem)
        {
            var wow = Environment.GetFolderPath(Environment.SpecialFolder.SystemX86);
            return Path.Combine(wow, "rundll32.exe");
        }

        var sys = Environment.GetFolderPath(Environment.SpecialFolder.System);
        return Path.Combine(sys, "rundll32.exe");
    }
}
