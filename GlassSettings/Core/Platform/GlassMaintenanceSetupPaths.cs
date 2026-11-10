using System.Text;
using System.Security.AccessControl;
using System.Security.Principal;

namespace GlassSettings.Core.Platform;

/// <summary>Product source cache and independent immutable maintenance controller.</summary>
public static class GlassMaintenanceSetupPaths
{
    public static string CurrentDirectory()
    {
        var programData = Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData);
        return Path.Combine(programData, "Packages", "GlassPlatform", "Current");
    }

    private static bool TrustedObject(string path, bool directory)
    {
        var attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.ReparsePoint) != 0 ||
            ((attributes & FileAttributes.Directory) != 0) != directory) return false;
        FileSystemSecurity security = directory
            ? new DirectoryInfo(path).GetAccessControl(AccessControlSections.Owner | AccessControlSections.Access)
            : new FileInfo(path).GetAccessControl(AccessControlSections.Owner | AccessControlSections.Access);
        var descriptor = new RawSecurityDescriptor(security.GetSecurityDescriptorBinaryForm(), 0);
        static bool Authority(SecurityIdentifier? sid) => sid is not null &&
            (sid.IsWellKnown(WellKnownSidType.BuiltinAdministratorsSid) || sid.IsWellKnown(WellKnownSidType.LocalSystemSid));
        if (!Authority(descriptor.Owner) || descriptor.DiscretionaryAcl is null) return false;
        const uint writes = 0x10000000 | 0x40000000 | 0x00010000 | 0x00040000 | 0x00080000 |
                            0x00000002 | 0x00000004 | 0x00000040 | 0x00000010 | 0x00000100;
        foreach (GenericAce ace in descriptor.DiscretionaryAcl)
        {
            if (ace is not CommonAce common || common.IsCallback) return false;
            if (common.AceQualifier == AceQualifier.AccessDenied) continue;
            if (common.AceQualifier != AceQualifier.AccessAllowed ||
                (((uint)common.AccessMask & writes) != 0 && !Authority(common.SecurityIdentifier))) return false;
        }
        return true;
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
                    if (!TrustedObject(path, path == root || path == store || path == folder)) return string.Empty;
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
        try
        {
            return TrustedObject(root, true) && TrustedObject(CurrentDirectory(), true) && TrustedObject(legacy, false)
                ? legacy : string.Empty;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            return string.Empty;
        }
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
