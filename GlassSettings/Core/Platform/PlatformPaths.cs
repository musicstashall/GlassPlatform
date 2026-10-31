using Microsoft.Win32;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Единый источник истины для путей платформы, OpenGlass и ресурсов GlassSettings.
/// Все методы резолва путей должны использовать этот класс.
/// </summary>
public static class PlatformPaths
{
    private const string BwmRegKey = @"Software\StanislavKa\GlassPlatform";
    private const string InstallPathValue = "InstallPath";

    private static string? _platformRoot;
    private static string? _openGlassDllPath;
    private static string? _openGlassDir;
    private static string? _openGlassInstallRoot;
    private static string? _openGlassHostPath;
    private static string? _openGlassGuiPath;

    /// <summary>Каталог платформы (содержит GlassCompat.dll).</summary>
    public static string PlatformRoot =>
        _platformRoot ??= GlassModulePath.GetInstallRoot() ?? AppContext.BaseDirectory;

    /// <summary>Каталог GlassSettings.exe.</summary>
    public static string AppBase => AppContext.BaseDirectory;

    /// <summary>Полный путь к OpenGlass.dll (или null, если не найден).</summary>
    public static string? OpenGlassDllPath
    {
        get
        {
            if (_openGlassDllPath is null)
                _openGlassDllPath = ResolveOpenGlassDllCore();
            return _openGlassDllPath;
        }
    }

    /// <summary>Каталог OpenGlass.dll (или null).</summary>
    public static string? OpenGlassDir =>
        _openGlassDir ??= OpenGlassDllPath is { Length: > 0 } dll
            ? Path.GetDirectoryName(dll)
            : null;

    /// <summary>Корень установки OpenGlass (родитель OpenGlass\ или registry InstallPath).</summary>
    public static string? OpenGlassInstallRoot =>
        _openGlassInstallRoot ??= ResolveOpenGlassInstallRootCore();

    /// <summary>Полный путь к OpenGlassHost.exe (или null).</summary>
    public static string? OpenGlassHostPath =>
        _openGlassHostPath ??= ResolveOpenGlassHostCore();

    /// <summary>Полный путь к OpenGlassGUI.exe / OpenGlass.exe (или null).</summary>
    public static string? OpenGlassGuiPath =>
        _openGlassGuiPath ??= ResolveOpenGlassGuiCore();

    /// <summary>
    /// Упорядоченный список корней для поиска ресурсов (атласы, отражения, материалы).
    /// Гарантирует поиск в каталоге OpenGlass.dll, корне установки, платформенном корне и AppBase.
    /// </summary>
    public static IReadOnlyList<string> GetResourceSearchRoots()
    {
        var roots = new List<string>(4);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        void AddRoot(string? path)
        {
            if (string.IsNullOrWhiteSpace(path)) return;
            var full = Path.GetFullPath(path);
            if (seen.Add(full))
                roots.Add(full);
        }

        AddRoot(OpenGlassDir);
        AddRoot(OpenGlassInstallRoot);
        AddRoot(PlatformRoot);
        AddRoot(AppBase);

        return roots;
    }

    /// <summary>Резолв модуля платформы по имени файла и архитектуре.</summary>
    public static string? ResolvePlatformModule(string fileName, GlassPlatformArch targetArch) =>
        GlassModulePath.Resolve(fileName, targetArch);

    /// <summary>Резолв нативного модуля платформы (native arch).</summary>
    public static string? ResolvePlatformModuleNative(string fileName) =>
        GlassModulePath.ResolveNative(fileName);

    // ── Внутренняя логика резолва (перенесена из OpenGlassVersionService) ──────

    private static string? ResolveOpenGlassDllCore()
    {
        var installDir = ReadBlackWindowManagerInstallPath();
        if (!string.IsNullOrEmpty(installDir))
        {
            var fromInstall = PathOpenGlassDllFromInstallDir(installDir);
            if (!string.IsNullOrEmpty(fromInstall))
                return fromInstall;
        }

        var fromReg = PathOpenGlassDllFromBwmInstall(installDir);
        if (IsFileAccessible(fromReg))
            return fromReg;

        foreach (var candidate in OpenGlassPathPolicy.EnumerateBundledOpenGlassDllCandidates())
        {
            if (IsFileAccessible(candidate))
                return Path.GetFullPath(candidate);
        }

        var fromPlatformRoot = Path.Combine(PlatformRoot, "OpenGlass", "OpenGlass.dll");
        if (IsFileAccessible(fromPlatformRoot))
            return Path.GetFullPath(fromPlatformRoot);

        return null;
    }

    private static string? ResolveOpenGlassInstallRootCore()
    {
        var dll = OpenGlassDllPath;
        if (!string.IsNullOrWhiteSpace(dll))
        {
            var oglassDir = Path.GetDirectoryName(dll);
            if (!string.IsNullOrWhiteSpace(oglassDir))
            {
                var leaf = Path.GetFileName(oglassDir);
                if (leaf.Equals("OpenGlass", StringComparison.OrdinalIgnoreCase)
                    || leaf.Equals("openglass", StringComparison.OrdinalIgnoreCase))
                {
                    var parent = Path.GetDirectoryName(oglassDir);
                    if (!string.IsNullOrWhiteSpace(parent))
                        return NormalizeDir(parent);
                }

                return NormalizeDir(oglassDir);
            }
        }

        var installDir = ReadBlackWindowManagerInstallPath();
        return string.IsNullOrWhiteSpace(installDir) ? null : NormalizeDir(installDir);
    }

    private static string? ResolveOpenGlassHostCore()
    {
        var dll = OpenGlassDllPath;
        if (string.IsNullOrEmpty(dll))
            return null;

        var dir = Path.GetDirectoryName(dll);
        if (string.IsNullOrEmpty(dir))
            return null;

        string[] candidates =
        [
            Path.Combine(dir, "OpenGlassHost.exe"),
            Path.Combine(Path.GetDirectoryName(dir) ?? dir, "OpenGlassHost.exe"),
        ];

        foreach (var c in candidates)
        {
            if (File.Exists(c))
                return c;
        }

        return null;
    }

    private static string? ResolveOpenGlassGuiCore()
    {
        foreach (var root in EnumerateOpenGlassGuiSearchRoots())
        {
            foreach (var name in new[] { "OpenGlassGUI.exe", "OpenGlass.exe" })
            {
                var candidate = Path.Combine(root, name);
                if (File.Exists(candidate))
                    return Path.GetFullPath(candidate);
            }
        }

        return null;
    }

    private static IEnumerable<string> EnumerateOpenGlassGuiSearchRoots()
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        string? TryAdd(string? path)
        {
            if (string.IsNullOrWhiteSpace(path))
                return null;
            var full = Path.GetFullPath(path);
            return seen.Add(full) ? full : null;
        }

        if (TryAdd(OpenGlassInstallRoot) is { } fromInstall)
            yield return fromInstall;

        var dll = OpenGlassDllPath;
        if (!string.IsNullOrWhiteSpace(dll))
        {
            var dir = Path.GetDirectoryName(dll);
            if (TryAdd(dir) is { } fromDll)
                yield return fromDll;

            if (!string.IsNullOrWhiteSpace(dir))
            {
                var parent = Path.GetDirectoryName(dir);
                if (TryAdd(parent) is { } fromParent)
                    yield return fromParent;
            }
        }

        if (TryAdd(AppBase) is { } fromExe)
            yield return fromExe;
    }

    private static string ReadBlackWindowManagerInstallPath()
    {
        var hkcu = ReadInstallPath(Microsoft.Win32.RegistryHive.CurrentUser, Microsoft.Win32.RegistryView.Default);
        if (!string.IsNullOrEmpty(hkcu))
            return hkcu;

        if (Environment.Is64BitOperatingSystem)
        {
            var hklm64 = ReadInstallPath(Microsoft.Win32.RegistryHive.LocalMachine, Microsoft.Win32.RegistryView.Registry64);
            if (!string.IsNullOrEmpty(hklm64))
                return hklm64;
        }

        return ReadInstallPath(Microsoft.Win32.RegistryHive.LocalMachine, Microsoft.Win32.RegistryView.Default) ?? string.Empty;
    }

    private static string? ReadInstallPath(Microsoft.Win32.RegistryHive hive, Microsoft.Win32.RegistryView view)
    {
        try
        {
            using var baseKey = Microsoft.Win32.RegistryKey.OpenBaseKey(hive, view);
            using var key = baseKey.OpenSubKey(BwmRegKey, false);
            if (key is null)
                return null;
            if (key.GetValue(InstallPathValue) is string root && !string.IsNullOrWhiteSpace(root))
                return root;
            if (key.GetValue("OpenGlassInstallPath") is string ogDir && !string.IsNullOrWhiteSpace(ogDir))
                return ogDir;
            return null;
        }
        catch
        {
            return null;
        }
    }

    private static string PathOpenGlassDllFromBwmInstall(string installDir)
    {
        if (string.IsNullOrEmpty(installDir))
            return string.Empty;

        var baseDir = NormalizeDir(installDir);
        var direct = Path.Combine(baseDir, "OpenGlass.dll");
        if (baseDir.EndsWith("\\OpenGlass", StringComparison.OrdinalIgnoreCase) && IsFileAccessible(direct))
            return direct;
        return Path.Combine(baseDir, "OpenGlass", "OpenGlass.dll");
    }

    private static string? PathOpenGlassDllFromInstallDir(string installDir)
    {
        var baseDir = NormalizeDir(installDir);
        var candidate = Path.Combine(baseDir, "OpenGlass", "OpenGlass.dll");
        if (IsFileAccessible(candidate))
            return candidate;
        var direct = Path.Combine(baseDir, "OpenGlass.dll");
        return IsFileAccessible(direct) ? direct : null;
    }

    /// <summary>
    /// Проверяет, что файл существует и доступен для чтения (не заблокирован ACL OG-установщика).
    /// </summary>
    private static bool IsFileAccessible(string path)
    {
        if (string.IsNullOrEmpty(path))
            return false;

        try
        {
            using var stream = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            return true;
        }
        catch (IOException)
        {
            return File.Exists(path);
        }
        catch (UnauthorizedAccessException)
        {
            return false;
        }
    }

    private static string NormalizeDir(string path)
    {
        path = path.TrimEnd('\\', '/');
        return path;
    }
}
