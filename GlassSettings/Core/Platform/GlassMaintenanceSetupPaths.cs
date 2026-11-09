using System.Text;

namespace GlassSettings.Core.Platform;

/// <summary>Product source cache and independent immutable maintenance controller.</summary>
public static class GlassMaintenanceSetupPaths
{
    public static string CurrentDirectory()
    {
        var programData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
        return Path.Combine(programData, "Packages", "GlassPlatform", "Current");
    }

    public static string SetupExecutablePath()
    {
        var root = Path.GetDirectoryName(CurrentDirectory())!;
        var store = Path.Combine(root, "Installer");
        var active = Path.Combine(store, "Active");
        // A present but invalid pointer is not permission to silently downgrade.
        bool hasActive;
        try
        {
            if ((File.GetAttributes(active) & FileAttributes.Directory) != 0) return string.Empty;
            hasActive = true;
        }
        catch (FileNotFoundException) { hasActive = false; }
        catch (DirectoryNotFoundException) { hasActive = false; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            return string.Empty;
        }
        if (hasActive)
        {
            try
            {
                if (new FileInfo(active).Length != 128) return string.Empty;
                var record = File.ReadAllText(active, Encoding.ASCII);
                if (record.Length != 128 || record.Any(c => !(c is >= '0' and <= '9' or >= 'a' and <= 'f')))
                    return string.Empty;
                var folder = Path.Combine(store, record[..64]);
                var setup = Path.Combine(folder, "GlassSetup.exe");
                // No paths are accepted from the record. Native launch performs
                // publisher/ACL validation too; avoid following reparse entries here.
                foreach (var path in new[] { root, store, active, folder, setup })
                    if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0) return string.Empty;
                for (var parent = Directory.GetParent(root); parent is not null; parent = parent.Parent)
                    if ((parent.Attributes & FileAttributes.ReparsePoint) != 0) return string.Empty;
                using var file = File.OpenRead(setup);
                var hash = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(file)).ToLowerInvariant();
                return hash == record[..64] ? setup : string.Empty;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
            {
                return string.Empty;
            }
        }
        var legacy = Path.Combine(CurrentDirectory(), "GlassSetup.exe");
        return File.Exists(legacy) ? legacy : string.Empty;
    }

    public static string SecureUxThemeMsiPath()
    {
        var name = Environment.Is64BitOperatingSystem
            ? "SecureUxTheme-x64.msi"
            : "SecureUxTheme-x86.msi";
        var path = Path.Combine(CurrentDirectory(), name);
        return File.Exists(path) ? path : string.Empty;
    }
}
