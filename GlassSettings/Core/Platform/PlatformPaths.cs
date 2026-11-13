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
    /// Корень дополнений GlassPlatform, содержащий Resources. Не зависит от
    /// каталога upstream OG, разрядности Settings или наличия GlassCompat.dll.
    /// Совпадает с ProgramFiles64Folder\GlassPlatform\OpenGlass в Resources MSI.
    /// </summary>
    public static string? OpenGlassResourcesRoot => ResolveOpenGlassResourcesRoot();

    public static string? OpenGlassResourcesDirectory => OpenGlassResourcesRoot is { } root
        ? Path.Combine(root, "Resources")
        : null;

    /// <summary>
    /// Единственный корень каталога графических дополнений. ThemeImageCatalog
    /// добавляет Resources/AtlasTheme, Resources/GlassReflection и т. п.
    /// Внешняя установка OG используется для DLL/Host/GUI, но не для дополнений.
    /// </summary>
    public static IReadOnlyList<string> GetResourceSearchRoots() =>
        OpenGlassResourcesRoot is { } root ? new[] { root } : Array.Empty<string>();

    private static string? ResolveOpenGlassResourcesRoot()
    {
        try
        {
            // HKLM64 gives native Program Files even to x86 GlassSettings.
            // Do not use the external OG InstallLocation or process AppBase.
            using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine,
                Environment.Is64BitOperatingSystem ? RegistryView.Registry64 : RegistryView.Registry32);
            using var key = machine.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion");
            if (key is null || key.GetValueKind("ProgramFilesDir") != RegistryValueKind.String ||
                key.GetValue("ProgramFilesDir", null, RegistryValueOptions.DoNotExpandEnvironmentNames) is not string programFiles ||
                string.IsNullOrWhiteSpace(programFiles) || !Path.IsPathFullyQualified(programFiles))
                return null;

            return Path.Combine(Path.GetFullPath(programFiles), "GlassPlatform", "OpenGlass");
        }
        catch
        {
            // Unknown root means no built-in catalog, not a fallback into an
            // unrelated upstream or x86 directory. Custom paths remain intact.
            return null;
        }
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
        // Upstream Inno installation may live anywhere, independently of Glass.
        var upstream = ReadUpstreamOpenGlassInstallPath();
        if (!string.IsNullOrWhiteSpace(upstream))
        {
            var candidate = Path.Combine(upstream, "OpenGlass.dll");
            if (IsFileAccessible(candidate)) return Path.GetFullPath(candidate);
        }

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

    private static string? ReadUpstreamOpenGlassInstallPath()
    {
        try
        {
            using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64);
            const string prefix = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\";
            string? result = null;
            foreach (var identity in new[] { "{D3D1BC7D-5E24-4B33-9383-7934271A3B05}_is1", "{D3D1BC7D-5E24-4B33-9383-7934271A3B05}}_is1" })
            {
                using var key = machine.OpenSubKey(prefix + identity, false);
                if (key is null) continue;
                if (result is not null) return null; // Conflicting exact upstream identities.
                var value = key.GetValue("InstallLocation") as string;
                if (string.IsNullOrWhiteSpace(value) || !Path.IsPathFullyQualified(value)) return null;
                result = Path.GetFullPath(value);
            }
            return result;
        }
        catch { return null; }
    }

    private static string ReadBlackWindowManagerInstallPath()
    {
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
