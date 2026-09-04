using Microsoft.Win32;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Единый источник истины для путей платформы, OpenGlass и ресурсов GlassSettings.
/// Все методы резолва путей должны использовать этот класс.
/// </summary>
public static class PlatformPaths
{
    private const string BwmRegKey = @"Software\StanislavKa\BlackWindowManager";
    private const string InstallPathValue = "InstallPath";
    private const string HardcodedOpenGlassDll =
        @"C:\Program Files (x86)\BlackWindowManager\openglass\OpenGlass.dll";

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

        if (IsFileAccessible(HardcodedOpenGlassDll))
            return HardcodedOpenGlassDll;

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
        var hkcu = ReadInstallPath(Microsoft.Win32.Registry.CurrentUser);
        if (!string.IsNullOrEmpty(hkcu))
            return hkcu;

        return ReadInstallPath(Microsoft.Win32.Registry.LocalMachine) ?? string.Empty;
    }

    private static string? ReadInstallPath(Microsoft.Win32.RegistryKey root)
    {
        using var key = root.OpenSubKey(BwmRegKey, false);
        return key?.GetValue(InstallPathValue) as string;
    }

    private static string PathOpenGlassDllFromBwmInstall(string installDir)
    {
        if (string.IsNullOrEmpty(installDir))
            return string.Empty;

        return Path.Combine(NormalizeDir(installDir), "OpenGlass", "OpenGlass.dll");
    }

    private static string? PathOpenGlassDllFromInstallDir(string installDir)
    {
        var baseDir = NormalizeDir(installDir);
        string[] candidates =
        [
            Path.Combine(baseDir, "openglass", "OpenGlass.dll"),
            Path.Combine(baseDir, "OpenGlass", "OpenGlass.dll"),
            Path.Combine(baseDir, "OpenGlass.dll")
        ];

        foreach (var candidate in candidates)
        {
            if (IsFileAccessible(candidate))
                return candidate;
        }

        return null;
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
