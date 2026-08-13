namespace GlassSettings.Core.Platform;

/// <summary>
/// Поиск модулей относительно install root (каталог exe/модуля):
/// OS x64 + x64 → Sys64, иначе root;
/// OS x64 + x86 из процесса x64 → только Sys32;
/// OS x64 + x86 из процесса x86 (WOW64) / OS x86 → Sys32, иначе root.
/// </summary>
public static class GlassModulePath
{
    public static string? GetInstallRoot(string? baseDirectory = null)
    {
        var dir = string.IsNullOrWhiteSpace(baseDirectory)
            ? AppContext.BaseDirectory
            : baseDirectory;
        dir = Path.GetFullPath(dir.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        var leaf = Path.GetFileName(dir);
        if (leaf.Equals("Sys64", StringComparison.OrdinalIgnoreCase)
            || leaf.Equals("Sys32", StringComparison.OrdinalIgnoreCase))
        {
            var parent = Path.GetDirectoryName(dir);
            if (!string.IsNullOrEmpty(parent))
                dir = parent;
        }

        return dir;
    }

    public static string? Resolve(string fileName, GlassPlatformArch targetArch, string? installRoot = null)
    {
        var root = GetInstallRoot(installRoot);
        if (string.IsNullOrEmpty(root) || string.IsNullOrWhiteSpace(fileName))
            return null;

        var os64 = Environment.Is64BitOperatingSystem;
        var want64 = targetArch == GlassPlatformArch.X64;

        if (os64)
        {
            if (want64)
            {
                var sys64 = Path.Combine(root, "Sys64", fileName);
                if (File.Exists(sys64))
                    return sys64;
                var atRoot = Path.Combine(root, fileName);
                return File.Exists(atRoot) ? atRoot : null;
            }

            // OS x64 + x86: Sys32; из x86-процесса — ещё корень своего installRoot.
            var sys32 = Path.Combine(root, "Sys32", fileName);
            if (File.Exists(sys32))
                return sys32;
            if (!Environment.Is64BitProcess)
            {
                var rootX86 = Path.Combine(root, fileName);
                return File.Exists(rootX86) ? rootX86 : null;
            }

            return null;
        }

        var sys32Os86 = Path.Combine(root, "Sys32", fileName);
        if (File.Exists(sys32Os86))
            return sys32Os86;
        var rootOs86 = Path.Combine(root, fileName);
        return File.Exists(rootOs86) ? rootOs86 : null;
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
