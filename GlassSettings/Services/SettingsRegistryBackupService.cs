using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;
using System.ComponentModel;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;

namespace GlassSettings.Services;

/// <summary>Explicit registry-only export; never changes settings or mounts/unloads user hives.</summary>
public static class SettingsRegistryBackupService
{
    private const string GlassPath = @"Software\StanislavKa\GlassPlatform";
    private const string DwmPath = @"Software\Microsoft\Windows\DWM";
    private const int MaxValues = 100000, MaxBytes = 64 * 1024 * 1024;
    // Kept identical to RegistrySanity's uninstall allowlist by the source test.
    private static readonly string[] DwmNames = [
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
    private sealed record Value(string Hive, string View, string Key, string Name, uint? Type, byte[]? Data);
    private sealed record Profile(string Sid, string Path);
    private sealed class Capture
    {
        public List<Value> Values { get; } = [];
        private long bytes;
        public void Add(Value value)
        {
            bytes += value.Data?.Length ?? 0;
            if (Values.Count >= MaxValues || bytes > MaxBytes) throw new IOException("Registry export limit exceeded.");
            Values.Add(value);
        }
    }

    public static Task<string> SaveAsync() => Task.Run(Save);

    private static string Save()
    {
        var capture = new Capture();
        var profiles = Profiles();
        foreach (var view in Environment.Is64BitOperatingSystem
                     ? new[] { RegistryView.Registry64, RegistryView.Registry32 }
                     : new[] { RegistryView.Registry32 })
        {
            using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, view);
            Export(machine, "HKLM", view, capture);
            using var users = RegistryKey.OpenBaseKey(RegistryHive.Users, view);
            foreach (var profile in profiles)
            {
                // Absence is not an empty profile. No offline mounting or fabricated success.
                using var user = Open(users, profile.Sid)
                    ?? throw new IOException($"Profile hive is not loaded: {profile.Sid}. No complete backup was created.");
                Export(user, @"HKU\" + profile.Sid, view, capture);
            }
        }
        if (!profiles.OrderBy(p => p.Sid).SequenceEqual(Profiles().OrderBy(p => p.Sid)))
            throw new IOException("Profile inventory changed during export.");
        var payload = JsonSerializer.SerializeToUtf8Bytes(capture.Values);
        var digest = Convert.ToHexString(SHA256.HashData(payload));
        var manifest = JsonSerializer.SerializeToUtf8Bytes(new
        {
            format = "GlassPlatform.RegistryBackup", formatVersion = 1,
            createdUtc = DateTimeOffset.UtcNow, profiles,
            scope = "All enumerated initialized user profiles and HKLM, GlassPlatform config and 49 OpenGlass DWM values; registry only",
            includesUserFiles = false, includesInstallMetadata = false,
            restoreImplemented = false, registrySha256 = digest, valueCount = capture.Values.Count
        });
        var documents = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        if (string.IsNullOrWhiteSpace(documents)) throw new IOException("Documents folder is unavailable.");
        using var parents = PinDirectory(documents);
        var path = Path.Combine(documents, $"GlassSettingsBackup-{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}.zip");
        using var handle = CreateFileW(path, 0xC0010000, 0, IntPtr.Zero, 1, 0x00200000, IntPtr.Zero);
        if (handle.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
        using var file = new FileStream(handle, FileAccess.ReadWrite);
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
                if (zip.Entries.Count != 2) throw new IOException("Archive entry verification failed.");
                Verify(zip, "manifest.json", manifest);
                Verify(zip, "registry.json", payload);
            }
            return path;
        }
        catch
        {
            // Delete this exact open object, never a path potentially replaced by another process.
            var disposition = new Disposition { DeleteFile = true };
            _ = SetFileInformationByHandle(handle, 4, ref disposition, (uint)Marshal.SizeOf<Disposition>());
            throw;
        }
    }

    private static void Write(ZipArchive zip, string name, byte[] data)
    {
        using var output = zip.CreateEntry(name, CompressionLevel.Optimal).Open();
        output.Write(data);
    }
    private static void Verify(ZipArchive zip, string name, byte[] expected)
    {
        var entry = zip.GetEntry(name) ?? throw new IOException("Missing archive entry.");
        if (entry.Length != expected.Length) throw new IOException("Archive length verification failed.");
        using var input = entry.Open();
        if (!CryptographicOperations.FixedTimeEquals(SHA256.HashData(input), SHA256.HashData(expected)))
            throw new IOException("Archive checksum verification failed.");
    }
    private static List<Profile> Profiles()
    {
        using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine,
            Environment.Is64BitOperatingSystem ? RegistryView.Registry64 : RegistryView.Registry32);
        using var list = machine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion\ProfileList")
            ?? throw new IOException("Profile inventory unavailable.");
        var profiles = new List<Profile>();
        foreach (var sid in list.GetSubKeyNames())
        {
            if (!sid.StartsWith("S-1-5-21-", StringComparison.Ordinal) && !sid.StartsWith("S-1-12-1-", StringComparison.Ordinal)) continue;
            using var key = Open(list, sid) ?? throw new IOException("Profile disappeared: " + sid);
            var path = key.GetValue("ProfileImagePath") as string;
            if (string.IsNullOrWhiteSpace(path)) throw new IOException("Unknown profile path: " + sid);
            // A SID without an initialized hive has no settings to export.
            var hivePath = Path.Combine(Environment.ExpandEnvironmentVariables(path), "NTUSER.DAT");
            try { _ = File.GetAttributes(hivePath); }
            catch (FileNotFoundException) { continue; }
            catch (DirectoryNotFoundException) { continue; }
            profiles.Add(new Profile(sid, path));
        }
        if (profiles.Count == 0) throw new IOException("No initialized user profiles found.");
        return profiles;
    }
    private static void Export(RegistryKey root, string hive, RegistryView view, Capture capture)
    {
        using (var glass = Open(root, GlassPath))
            if (glass != null) Tree(glass, hive, view, GlassPath, capture, 0);
        using var dwm = Open(root, DwmPath);
        foreach (var name in DwmNames) capture.Add(Read(dwm, hive, view, DwmPath, name));
    }
    private static void Tree(RegistryKey key, string hive, RegistryView view, string path, Capture capture, int depth)
    {
        if (depth > 64) throw new IOException("Registry depth limit exceeded.");
        foreach (var name in key.GetValueNames())
        {
            if (path == GlassPath && new[] { "InstallPath", "InstallPathX64", "InstallPathX86", "OpenGlassInstallPath" }
                    .Contains(name, StringComparer.OrdinalIgnoreCase)) continue;
            capture.Add(Read(key, hive, view, path, name));
        }
        foreach (var child in key.GetSubKeyNames())
        {
            using var sub = Open(key, child) ?? throw new IOException("Registry key disappeared: " + path);
            Tree(sub, hive, view, path + @"\" + child, capture, depth + 1);
        }
    }
    private static Value Read(RegistryKey? key, string hive, RegistryView view, string path, string name)
    {
        uint type = 0, size = 0;
        var status = key == null ? 2 : RegQueryValueExW(key.Handle, name, IntPtr.Zero, out type, null, ref size);
        if (status == 2) return new(hive, view.ToString(), path, name, null, null);
        if (status != 0 || size > MaxBytes) throw new IOException($"Registry read failed: {hive}\\{path}\\{name} ({status}).");
        var data = new byte[checked((int)size)];
        status = RegQueryValueExW(key!.Handle, name, IntPtr.Zero, out type, data, ref size);
        if (status != 0 || type == 6 || size != data.Length) throw new IOException("Registry value changed or is a link: " + name);
        return new(hive, view.ToString(), path, name, type, data);
    }
    private static RegistryKey? Open(RegistryKey parent, string path)
    {
        // Open each component with OPEN_LINK; never follow registry symbolic links.
        RegistryKey? result = null;
        try
        {
            foreach (var part in path.Split('\\', StringSplitOptions.RemoveEmptyEntries))
            {
                var status = RegOpenKeyExW((result ?? parent).Handle, part, 8, 0x20019 | (int)parent.View, out var handle);
                if (status == 2) { handle?.Dispose(); result?.Dispose(); return null; }
                if (status != 0) { handle?.Dispose(); throw new Win32Exception(status); }
                var next = RegistryKey.FromHandle(handle, parent.View);
                result?.Dispose(); result = next;
                uint size = 0;
                status = RegQueryValueExW(next.Handle, "SymbolicLinkValue", IntPtr.Zero, out var type, null, ref size);
                if (status != 0 && status != 2) throw new Win32Exception(status);
                if (status == 0 && type == 6) throw new IOException("Registry symbolic link refused.");
            }
            return result;
        }
        catch { result?.Dispose(); throw; }
    }

    private sealed class Pins : IDisposable
    {
        public List<SafeFileHandle> Handles { get; } = [];
        public void Dispose() { foreach (var handle in Handles) handle.Dispose(); }
    }
    private static Pins PinDirectory(string directory)
    {
        var pins = new Pins();
        try
        {
            var full = Path.GetFullPath(directory);
            var root = Path.GetPathRoot(full)!;
            if (root.Length != 3 || root[1] != ':') throw new IOException("Only a local Documents folder is supported.");
            var current = root;
            foreach (var part in new[] { "" }.Concat(full[root.Length..].Split('\\', StringSplitOptions.RemoveEmptyEntries)))
            {
                current = part.Length == 0 ? current : Path.Combine(current, part);
                var h = CreateFileW(current, 0x80, 3, IntPtr.Zero, 3, 0x02200000, IntPtr.Zero);
                if (h.IsInvalid) { h.Dispose(); throw new Win32Exception(Marshal.GetLastWin32Error()); }
                pins.Handles.Add(h);
                if (!GetFileInformationByHandle(h, out var info) || (info.Attributes & 0x410) != 0x10)
                    throw new IOException("Documents path contains a link or inaccessible directory.");
            }
            return pins;
        }
        catch { pins.Dispose(); throw; }
    }
    [StructLayout(LayoutKind.Sequential)] private struct Info
    {
        public uint Attributes; public System.Runtime.InteropServices.ComTypes.FILETIME Creation, Access, Write;
        public uint Volume, SizeHigh, SizeLow, Links, IndexHigh, IndexLow;
    }
    [StructLayout(LayoutKind.Sequential)] private struct Disposition { [MarshalAs(UnmanagedType.U1)] public bool DeleteFile; }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetFileInformationByHandle(SafeFileHandle file, out Info info);
    [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetFileInformationByHandle(SafeFileHandle file, int kind, ref Disposition data, uint size);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int RegOpenKeyExW(SafeRegistryHandle key, string subKey, uint options, int access, out SafeRegistryHandle result);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    private static extern int RegQueryValueExW(SafeRegistryHandle key, string name, IntPtr reserved, out uint type, byte[]? data, ref uint size);
}
