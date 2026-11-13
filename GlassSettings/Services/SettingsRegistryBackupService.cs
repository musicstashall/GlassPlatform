using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text.Json;

namespace GlassSettings.Services;

/// <summary>Часть резервной копии параметров текущего профиля.</summary>
public enum SettingsBackupSection
{
    Appearance,
    Processes,
    OpenGlass,
    App
}

/// <summary>Какие части реально есть в выбранном файле.</summary>
public sealed class SettingsBackupInspection
{
    public bool IsValid { get; init; }
    public string? Error { get; init; }
    public IReadOnlySet<SettingsBackupSection> Present { get; init; } =
        new HashSet<SettingsBackupSection>();
}

/// <summary>Что удалось записать и какие сигналы после этого нужны.</summary>
public sealed class SettingsBackupApplyResult
{
    public IReadOnlySet<SettingsBackupSection> Applied { get; init; } =
        new HashSet<SettingsBackupSection>();
    public bool ThemeNotify { get; init; }
    public bool LanguageChanged { get; init; }
}

/// <summary>
/// Копия параметров только HKCU текущего пользователя. Чужие профили и HKLM не читаются.
/// </summary>
public static class SettingsRegistryBackupService
{
    public const string FileExtension = ".glassbackup";
    public const string FormatName = "GlassPlatform.RegistryBackup";

    private const string GlassPath = @"Software\StanislavKa\GlassPlatform";
    private const string DwmPath = @"Software\Microsoft\Windows\DWM";
    private const int MaxValues = 100000;
    private const int MaxBytes = 64 * 1024 * 1024;

    // Совпадает с RegistrySanity::kDwmOpenGlassRules: DeleteConfig минус пять штатных значений Windows.
    private static readonly string[] DwmNames =
    [
        "ColorizationColorOverride", "ColorizationAfterglowOverride", "ColorizationColorBalanceOverride",
        "ColorizationAfterglowBalanceOverride", "ColorizationBlurBalanceOverride", "ColorizationColorInactive",
        "GlassOpacity", "GlassOpacityInactive", "ColorizationColorCaption", "ColorizationColorCaptionInactive",
        "ColorizationColorCaptionMaximized", "ColorizationColorCaptionInactiveMaximized", "ColorizationOpaqueBlend",
        "ColorizationBaseTransparent", "ColorizationBaseMaximized", "ColorizationBaseOpaque", "ColorizationOpaqueBlendPriority",
        "ColorizationOpacity", "ColorizationOpacityInactive", "ColorizationOpacityMaximized", "ColorizationOpacityInactiveMaximized",
        "GlassType", "GlassOverrideAccent", "ColorizationGlassReflectionIntensity", "ColorizationGlassReflectionOpacity",
        "ColorizationGlassReflectionOpacityInactive", "ColorizationGlassReflectionOpacityMaximized",
        "ColorizationGlassReflectionOpacityInactiveMaximized", "ColorizationGlassReflectionParallaxIntensity",
        "ColorizationGlassReflectionPolicy", "BlurDeviation", "BlurOptimization", "RoundRectRadius", "MaterialOpacity",
        "UseDirect3DRendering", "CaptionButtons", "CenterCaption", "TextGlowMode", "DisableModernBorders",
        "DisableGlassOnBattery", "DisabledHooks", "GlassSafetyZoneMode", "MINMAXBUTTONGLOWid", "CLOSEBUTTONGLOWid",
        "TOOLCLOSEBUTTONGLOWid", "EnableUxMitigation", "CustomThemeReflection", "CustomThemeMaterial", "CustomThemeAtlas"
    ];

    private static readonly string[] ThemeNames =
    [
        "CustomThemeReflection", "CustomThemeMaterial", "CustomThemeAtlas"
    ];

    private static readonly HashSet<string> InstallMetadata = new(StringComparer.OrdinalIgnoreCase)
    {
        "InstallPath", "InstallPathX64", "InstallPathX86", "OpenGlassInstallPath"
    };

    private static readonly HashSet<string> DwmNameSet = new(DwmNames, StringComparer.OrdinalIgnoreCase);

    private sealed class StoredValue
    {
        public string? Section { get; set; }
        public string Hive { get; set; } = "";
        public string View { get; set; } = "";
        public string Key { get; set; } = "";
        public string Name { get; set; } = "";
        public uint? Type { get; set; }
        public byte[]? Data { get; set; }
    }

    private sealed class Capture
    {
        public List<StoredValue> Values { get; } = [];
        private long _bytes;

        public void Add(StoredValue value)
        {
            _bytes += value.Data?.Length ?? 0;
            if (Values.Count >= MaxValues || _bytes > MaxBytes)
                throw new IOException("Registry export limit exceeded.");
            Values.Add(value);
        }
    }

    public static Task<string> SaveAsync(IReadOnlyCollection<SettingsBackupSection> sections) =>
        Task.Run(() => Save(sections));

    public static Task<SettingsBackupInspection> InspectAsync(string path) =>
        Task.Run(() => Inspect(path));

    public static Task<SettingsBackupApplyResult> ApplyAsync(
        string path,
        IReadOnlyCollection<SettingsBackupSection> sections) =>
        Task.Run(() => Apply(path, sections));

    public static string SectionId(SettingsBackupSection section) => section switch
    {
        SettingsBackupSection.Appearance => "appearance",
        SettingsBackupSection.Processes => "processes",
        SettingsBackupSection.OpenGlass => "openglass",
        SettingsBackupSection.App => "app",
        _ => "appearance"
    };

    private static string Save(IReadOnlyCollection<SettingsBackupSection> sections)
    {
        if (sections.Count == 0)
            throw new IOException("No backup section selected.");

        var selected = sections.ToHashSet();
        var capture = new Capture();
        foreach (var view in Views())
        {
            using var user = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, view);
            if (selected.Contains(SettingsBackupSection.Appearance))
                ExportAppearance(user, view, capture);
            if (selected.Contains(SettingsBackupSection.Processes))
                ExportChild(user, view, "AttachList", SettingsBackupSection.Processes, capture);
            if (selected.Contains(SettingsBackupSection.Processes))
                ExportChild(user, view, "BlockList", SettingsBackupSection.Processes, capture);
            if (selected.Contains(SettingsBackupSection.App))
                ExportChild(user, view, "GlassSettings", SettingsBackupSection.App, capture);
            if (selected.Contains(SettingsBackupSection.OpenGlass))
                ExportOpenGlass(user, view, capture);
        }

        var payload = JsonSerializer.SerializeToUtf8Bytes(capture.Values);
        var digest = Convert.ToHexString(SHA256.HashData(payload));
        var manifest = JsonSerializer.SerializeToUtf8Bytes(new
        {
            format = FormatName,
            formatVersion = 2,
            createdUtc = DateTimeOffset.UtcNow,
            scope = "Current user HKCU only",
            sections = selected.Select(SectionId).OrderBy(static id => id).ToArray(),
            includesUserFiles = false,
            includesInstallMetadata = false,
            restoreImplemented = true,
            registrySha256 = digest,
            valueCount = capture.Values.Count
        });

        var documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        if (string.IsNullOrWhiteSpace(documents))
            throw new IOException("Documents folder is unavailable.");

        var path = Path.Combine(
            documents,
            $"GlassSettingsBackup-{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}{FileExtension}");
        using var file = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None);
        try
        {
            using (var zip = new ZipArchive(file, ZipArchiveMode.Create, leaveOpen: true))
            {
                Write(zip, "manifest.json", manifest);
                Write(zip, "registry.json", payload);
            }

            file.Flush(true);
            file.Position = 0;
            using (var zip = new ZipArchive(file, ZipArchiveMode.Read, leaveOpen: true))
            {
                if (zip.Entries.Count != 2)
                    throw new IOException("Archive entry verification failed.");
                Verify(zip, "manifest.json", manifest);
                Verify(zip, "registry.json", payload);
            }

            return path;
        }
        catch
        {
            var disposition = new Disposition { DeleteFile = true };
            _ = SetFileInformationByHandle(file.SafeFileHandle, 4, ref disposition, (uint)Marshal.SizeOf<Disposition>());
            throw;
        }
    }

    private static SettingsBackupInspection Inspect(string path)
    {
        try
        {
            var values = ReadValues(path, out _);
            var present = new HashSet<SettingsBackupSection>();
            foreach (var value in values)
            {
                // Type/Data = null — запись «значения не было», не само значение.
                if (value.Type is null || !IsCurrentUserHive(value.Hive))
                    continue;
                if (Classify(value.Key, value.Name) is { } section)
                    present.Add(section);
            }

            return new SettingsBackupInspection { IsValid = true, Present = present };
        }
        catch (Exception error)
        {
            return new SettingsBackupInspection { IsValid = false, Error = error.Message };
        }
    }

    private static SettingsBackupApplyResult Apply(
        string path,
        IReadOnlyCollection<SettingsBackupSection> sections)
    {
        var selected = sections.ToHashSet();
        if (selected.Count == 0)
            throw new IOException("No restore section selected.");

        var languageBefore = Core.Registry.GlassSettingsRegistry.ReadAppUiLanguage();
        var values = ReadValues(path, out _);
        var applied = new HashSet<SettingsBackupSection>();
        var themeNotify = false;
        foreach (var value in values)
        {
            if (!IsCurrentUserHive(value.Hive))
                continue;
            if (Classify(value.Key, value.Name) is not { } section || !selected.Contains(section))
                continue;
            if (!TryParseView(value.View, out var view))
                throw new IOException("Unknown registry view: " + value.View);

            WriteValue(view, value, section);
            applied.Add(section);
            if (section == SettingsBackupSection.OpenGlass
                && ThemeNames.Contains(value.Name, StringComparer.OrdinalIgnoreCase))
                themeNotify = true;
        }

        var languageAfter = Core.Registry.GlassSettingsRegistry.ReadAppUiLanguage();
        return new SettingsBackupApplyResult
        {
            Applied = applied,
            ThemeNotify = themeNotify,
            LanguageChanged = applied.Contains(SettingsBackupSection.App)
                && !string.Equals(languageBefore, languageAfter, StringComparison.Ordinal)
        };
    }

    private static List<StoredValue> ReadValues(string path, out JsonElement manifest)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
            throw new IOException("Backup file was not found.");

        using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using var zip = new ZipArchive(file, ZipArchiveMode.Read, leaveOpen: false);
        var manifestBytes = ReadEntry(zip, "manifest.json");
        var registryBytes = ReadEntry(zip, "registry.json");
        using var document = JsonDocument.Parse(manifestBytes);
        manifest = document.RootElement.Clone();
        if (!manifest.TryGetProperty("format", out var format)
            || !string.Equals(format.GetString(), FormatName, StringComparison.Ordinal))
            throw new IOException("Backup format is not recognized.");
        if (!manifest.TryGetProperty("formatVersion", out var version)
            || version.ValueKind != JsonValueKind.Number
            || version.GetInt32() is < 1 or > 2)
            throw new IOException("Backup format version is not supported.");
        if (manifest.TryGetProperty("registrySha256", out var digest)
            && digest.ValueKind == JsonValueKind.String
            && !string.Equals(digest.GetString(), Convert.ToHexString(SHA256.HashData(registryBytes)), StringComparison.OrdinalIgnoreCase))
            throw new IOException("Archive checksum verification failed.");

        return JsonSerializer.Deserialize<List<StoredValue>>(registryBytes)
            ?? throw new IOException("Backup registry entry is empty.");
    }

    private static void ExportAppearance(RegistryKey user, RegistryView view, Capture capture)
    {
        using var glass = Open(user, GlassPath);
        if (glass is null)
            return;
        Tree(glass, view, GlassPath, SettingsBackupSection.Appearance, capture, 0, skipExcludedChildren: true);
    }

    private static void ExportChild(
        RegistryKey user,
        RegistryView view,
        string child,
        SettingsBackupSection section,
        Capture capture)
    {
        var path = GlassPath + "\\" + child;
        using var key = Open(user, path);
        if (key is null)
            return;
        Tree(key, view, path, section, capture, 0, skipExcludedChildren: false);
    }

    private static void ExportOpenGlass(RegistryKey user, RegistryView view, Capture capture)
    {
        using var dwm = Open(user, DwmPath);
        foreach (var name in DwmNames)
            capture.Add(Read(dwm, view, DwmPath, name, SettingsBackupSection.OpenGlass));
    }

    private static void Tree(
        RegistryKey key,
        RegistryView view,
        string path,
        SettingsBackupSection section,
        Capture capture,
        int depth,
        bool skipExcludedChildren)
    {
        if (depth > 64)
            throw new IOException("Registry depth limit exceeded.");

        foreach (var name in key.GetValueNames())
        {
            if (skipExcludedChildren
                && path.Equals(GlassPath, StringComparison.OrdinalIgnoreCase)
                && InstallMetadata.Contains(name))
                continue;
            capture.Add(Read(key, view, path, name, section));
        }

        foreach (var child in key.GetSubKeyNames())
        {
            if (skipExcludedChildren && path.Equals(GlassPath, StringComparison.OrdinalIgnoreCase)
                && IsExcludedGlassChild(child))
                continue;
            using var sub = Open(key, child) ?? throw new IOException("Registry key disappeared: " + path);
            Tree(sub, view, path + "\\" + child, section, capture, depth + 1, skipExcludedChildren: false);
        }
    }

    private static bool IsExcludedGlassChild(string child) =>
        child.Equals("AttachList", StringComparison.OrdinalIgnoreCase)
        || child.Equals("BlockList", StringComparison.OrdinalIgnoreCase)
        || child.Equals("Crashes", StringComparison.OrdinalIgnoreCase)
        || child.Equals("Installer", StringComparison.OrdinalIgnoreCase)
        || child.Equals("GlassSettings", StringComparison.OrdinalIgnoreCase);

    private static StoredValue Read(
        RegistryKey? key,
        RegistryView view,
        string path,
        string name,
        SettingsBackupSection section)
    {
        uint type = 0;
        uint size = 0;
        var status = key is null
            ? 2
            : RegQueryValueExW(key.Handle, name, IntPtr.Zero, out type, null, ref size);
        if (status == 2)
            return new StoredValue
            {
                Section = SectionId(section),
                Hive = "HKCU",
                View = view.ToString(),
                Key = path,
                Name = name
            };
        if (status != 0 || size > MaxBytes)
            throw new IOException($"Registry read failed: HKCU\\{path}\\{name} ({status}).");

        var data = new byte[checked((int)size)];
        status = RegQueryValueExW(key!.Handle, name, IntPtr.Zero, out type, data, ref size);
        if (status != 0 || type == 6 || size != data.Length)
            throw new IOException("Registry value changed or is a link: " + name);
        return new StoredValue
        {
            Section = SectionId(section),
            Hive = "HKCU",
            View = view.ToString(),
            Key = path,
            Name = name,
            Type = type,
            Data = data
        };
    }

    private static void WriteValue(RegistryView view, StoredValue value, SettingsBackupSection section)
    {
        if (value.Type == 6)
            throw new IOException("Registry symbolic link refused.");
        if (value.Key.Equals(GlassPath, StringComparison.OrdinalIgnoreCase) && InstallMetadata.Contains(value.Name))
            return;

        using var root = RegistryKey.OpenBaseKey(RegistryHive.CurrentUser, view);
        using var key = Create(root, value.Key);
        if (value.Type is null)
        {
            if (section != SettingsBackupSection.OpenGlass)
                return;
            var deleted = RegDeleteValueW(key.Handle, value.Name);
            if (deleted != 0 && deleted != 2)
                throw new Win32Exception(deleted, $"Delete registry value HKCU\\{value.Key}\\{value.Name}: {new Win32Exception(deleted).Message}");
            return;
        }

        var data = value.Data ?? [];
        var status = RegSetValueExW(key.Handle, value.Name, 0, value.Type.Value, data, (uint)data.Length);
        if (status != 0)
            throw new Win32Exception(status, $"Write registry value HKCU\\{value.Key}\\{value.Name}: {new Win32Exception(status).Message}");
    }

    private static SettingsBackupSection? Classify(string key, string name)
    {
        if (key.Equals(DwmPath, StringComparison.OrdinalIgnoreCase))
            return DwmNameSet.Contains(name) ? SettingsBackupSection.OpenGlass : null;
        if (!key.StartsWith(GlassPath, StringComparison.OrdinalIgnoreCase))
            return null;
        if (key.Length > GlassPath.Length && key[GlassPath.Length] != '\\')
            return null;

        var rest = key.Length == GlassPath.Length ? "" : key[(GlassPath.Length + 1)..];
        if (rest.Length == 0)
            return InstallMetadata.Contains(name) ? null : SettingsBackupSection.Appearance;

        var first = rest.Split('\\', 2)[0];
        if (first.Equals("AttachList", StringComparison.OrdinalIgnoreCase)
            || first.Equals("BlockList", StringComparison.OrdinalIgnoreCase))
            return SettingsBackupSection.Processes;
        if (first.Equals("GlassSettings", StringComparison.OrdinalIgnoreCase))
            return SettingsBackupSection.App;
        if (first.Equals("Crashes", StringComparison.OrdinalIgnoreCase)
            || first.Equals("Installer", StringComparison.OrdinalIgnoreCase))
            return null;
        return SettingsBackupSection.Appearance;
    }

    private static bool IsCurrentUserHive(string hive)
    {
        if (hive.Equals("HKCU", StringComparison.OrdinalIgnoreCase))
            return true;
        var sid = WindowsIdentity.GetCurrent().User?.Value;
        return sid is not null && hive.Equals(@"HKU\" + sid, StringComparison.OrdinalIgnoreCase);
    }

    private static bool TryParseView(string view, out RegistryView parsed)
    {
        if (view.Contains("64", StringComparison.Ordinal))
        {
            parsed = RegistryView.Registry64;
            return true;
        }

        if (view.Contains("32", StringComparison.Ordinal))
        {
            parsed = RegistryView.Registry32;
            return true;
        }

        parsed = RegistryView.Default;
        return false;
    }

    private static RegistryView[] Views() =>
        Environment.Is64BitOperatingSystem
            ? [RegistryView.Registry64, RegistryView.Registry32]
            : [RegistryView.Registry32];

    private static RegistryKey? Open(RegistryKey parent, string path)
    {
        RegistryKey? result = null;
        try
        {
            foreach (var part in path.Split('\\', StringSplitOptions.RemoveEmptyEntries))
            {
                var status = RegOpenKeyExW((result ?? parent).Handle, part, 8, 0x0009 | (int)parent.View, out var handle);
                if (status == 2)
                {
                    handle?.Dispose();
                    result?.Dispose();
                    return null;
                }

                if (status != 0)
                {
                    handle?.Dispose();
                    throw new Win32Exception(status, $"Read registry key {parent.Name}\\{path} ({parent.View}), component {part}: {new Win32Exception(status).Message}");
                }

                var next = RegistryKey.FromHandle(handle, parent.View);
                result?.Dispose();
                result = next;
                RefuseLink(next);
            }

            return result;
        }
        catch
        {
            result?.Dispose();
            throw;
        }
    }

    private static RegistryKey Create(RegistryKey parent, string path)
    {
        RegistryKey? result = null;
        try
        {
            foreach (var part in path.Split('\\', StringSplitOptions.RemoveEmptyEntries))
            {
                var status = RegCreateKeyExW(
                    (result ?? parent).Handle,
                    part,
                    0,
                    null,
                    0,
                    0x000F | (int)parent.View,
                    IntPtr.Zero,
                    out var handle,
                    out _);
                if (status != 0)
                {
                    handle?.Dispose();
                    throw new Win32Exception(status, $"Write registry key {parent.Name}\\{path} ({parent.View}), component {part}: {new Win32Exception(status).Message}");
                }

                var next = RegistryKey.FromHandle(handle, parent.View);
                result?.Dispose();
                result = next;
                RefuseLink(next);
            }

            return result ?? throw new IOException("Empty registry path.");
        }
        catch
        {
            result?.Dispose();
            throw;
        }
    }

    private static void RefuseLink(RegistryKey key)
    {
        uint size = 0;
        var status = RegQueryValueExW(key.Handle, "SymbolicLinkValue", IntPtr.Zero, out var type, null, ref size);
        if (status != 0 && status != 2)
            throw new Win32Exception(status);
        if (status == 0 && type == 6)
            throw new IOException("Registry symbolic link refused.");
    }

    private static void Write(ZipArchive zip, string name, byte[] data)
    {
        using var output = zip.CreateEntry(name, CompressionLevel.Optimal).Open();
        output.Write(data);
    }

    private static void Verify(ZipArchive zip, string name, byte[] expected)
    {
        var actual = ReadEntry(zip, name);
        if (actual.Length != expected.Length
            || !CryptographicOperations.FixedTimeEquals(SHA256.HashData(actual), SHA256.HashData(expected)))
            throw new IOException("Archive checksum verification failed.");
    }

    private static byte[] ReadEntry(ZipArchive zip, string name)
    {
        var entry = zip.GetEntry(name) ?? throw new IOException("Missing archive entry.");
        using var input = entry.Open();
        using var buffer = new MemoryStream();
        input.CopyTo(buffer);
        if (buffer.Length > MaxBytes)
            throw new IOException("Archive entry is too large.");
        return buffer.ToArray();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Disposition
    {
        [MarshalAs(UnmanagedType.U1)]
        public bool DeleteFile;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetFileInformationByHandle(SafeFileHandle file, int kind, ref Disposition data, uint size);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int RegOpenKeyExW(SafeRegistryHandle key, string subKey, uint options, int access, out SafeRegistryHandle result);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int RegCreateKeyExW(
        SafeRegistryHandle key,
        string subKey,
        int reserved,
        string? className,
        uint options,
        int access,
        IntPtr security,
        out SafeRegistryHandle result,
        out uint disposition);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int RegQueryValueExW(SafeRegistryHandle key, string name, IntPtr reserved, out uint type, byte[]? data, ref uint size);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int RegSetValueExW(SafeRegistryHandle key, string name, int reserved, uint type, byte[] data, uint size);

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int RegDeleteValueW(SafeRegistryHandle key, string name);
}
