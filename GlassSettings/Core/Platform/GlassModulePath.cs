namespace GlassSettings.Core.Platform;

using GlassSettings.Services;

/// <summary>
/// Поиск модулей: GS своей битности — корень exe; GS x64 → x86 — Wow6432Node InstallPath
/// или каталог из /TR x86-задачи автозапуска.
/// Плоский корень рядом с exe/DLL; MSI — GlassSettings\ под [INSTALLDIR].
/// </summary>
public static class GlassModulePath
{
    public static string? GetInstallRoot(string? baseDirectory = null)
    {
        var dir = string.IsNullOrWhiteSpace(baseDirectory)
            ? AppContext.BaseDirectory
            : baseDirectory;
        var root = Path.GetFullPath(dir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));

        if (IsValidPlatformRoot(root))
            return root;

        var parent = Directory.GetParent(root)?.FullName;
        if (parent != null && IsValidPlatformRoot(parent))
            return parent;

        var fromRegistry = PlatformInstallRegistry.TryGetInstallPath(GlassPlatformNames.NativeArch);
        if (IsValidPlatformRoot(fromRegistry))
            return fromRegistry;

        return null;
    }

    public static string? GetInstallRootForArch(GlassPlatformArch targetArch, string? installRoot = null)
    {
        if (targetArch == GlassPlatformArch.X64)
            return ResolveX64InstallRoot(installRoot);

        if (targetArch == GlassPlatformArch.X86)
        {
            if (Environment.Is64BitProcess)
                return TryResolveX86InstallRootCrossArch();
            return GetInstallRoot(installRoot);
        }

        return GetInstallRoot(installRoot);
    }

    private static string? ResolveX64InstallRoot(string? installRoot)
    {
        if (Environment.Is64BitProcess)
        {
            var local = GetInstallRoot(installRoot);
            if (IsValidPlatformRoot(local))
                return local;
        }

        if (Environment.Is64BitOperatingSystem)
        {
            var pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
            var candidate = Path.Combine(pf, "GlassPlatform");
            if (IsValidPlatformRoot(candidate))
                return candidate;
        }

        var fromRegistry = PlatformInstallRegistry.TryGetInstallPath(GlassPlatformArch.X64);
        if (IsValidPlatformRoot(fromRegistry))
            return fromRegistry;

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

        if (Environment.Is64BitOperatingSystem)
        {
            var pfX86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
            var candidate = Path.Combine(pfX86, "GlassPlatform");
            if (IsValidPlatformRoot(candidate))
                return candidate;
        }

        return null;
    }

    internal static bool IsValidPlatformRoot(string? root)
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
