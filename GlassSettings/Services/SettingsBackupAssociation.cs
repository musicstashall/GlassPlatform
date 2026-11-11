using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>Ассоциация .glassbackup в HKCU. Уже существующую команду не переписывает.</summary>
public static class SettingsBackupAssociation
{
    public const string Extension = ".glassbackup";
    public const string ProgId = "GlassPlatform.SettingsBackup";

    public static void EnsureIfMissing()
    {
        try
        {
            using var classes = Registry.CurrentUser.OpenSubKey(@"Software\Classes", writable: true);
            if (classes is null)
                return;

            var extensionTarget = classes.OpenSubKey(Extension)?.GetValue(null) as string;
            var command = classes.OpenSubKey(ProgId + @"\shell\open\command")?.GetValue(null) as string;
            if (string.Equals(extensionTarget, ProgId, StringComparison.OrdinalIgnoreCase)
                && !string.IsNullOrWhiteSpace(command))
                return;

            var exe = Environment.ProcessPath;
            if (string.IsNullOrWhiteSpace(exe))
                return;

            using (var extension = classes.CreateSubKey(Extension))
                extension?.SetValue(null, ProgId);
            using (var prog = classes.CreateSubKey(ProgId))
                prog?.SetValue(null, Strings.GlassSettingsResources.Get("System_BackupFileType"));
            using (var icon = classes.CreateSubKey(ProgId + @"\DefaultIcon"))
                icon?.SetValue(null, exe + ",0");
            using (var open = classes.CreateSubKey(ProgId + @"\shell\open\command"))
                open?.SetValue(null, "\"" + exe + "\" \"%1\"");
        }
        catch (Exception error)
        {
            System.Diagnostics.Debug.WriteLine("GlassSettings backup association: " + error.Message);
        }
    }

}
