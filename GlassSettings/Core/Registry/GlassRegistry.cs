using Microsoft.Win32;

namespace GlassSettings.Core.Registry;

public static class GlassRegistry

{

    public static string SubKeyPath(string? relativeSubKey)

    {

        if (string.IsNullOrEmpty(relativeSubKey))

            return RegistryPaths.GlassRoot;

        return RegistryPaths.GlassRoot + "\\" + relativeSubKey.Replace('/', '\\');

    }

    public static uint? ReadDword(string? subKey, string valueName)

    {

        using var key = Open(subKey, false);

        if (key is null)

            return null;

        return RawToUInt(key.GetValue(valueName));

    }

    public static uint ReadDwordFb(string? subKey, string valueName, uint defaultValue)

    {

        var v = ReadDword(subKey, valueName);

        if (v.HasValue)

            return v.Value;

        if (!string.IsNullOrEmpty(subKey))

        {

            v = ReadDword(null, valueName);

            if (v.HasValue)

                return v.Value;

        }

        return defaultValue;

    }

    public static bool WriteDword(string? subKey, string valueName, uint value)

    {

        using var key = Open(subKey, true);

        if (key is null)

            return false;

        key.SetValue(valueName, unchecked((int)value), RegistryValueKind.DWord);

        return true;

    }

    public static bool WriteString(string? subKey, string valueName, string value)
    {
        using var key = Open(subKey, true);
        if (key is null)
            return false;

        key.SetValue(valueName, value, RegistryValueKind.String);
        return true;
    }

    public static string? ReadString(string? subKey, string valueName)
    {
        using var key = Open(subKey, false);
        if (key is null)
            return null;

        return key.GetValue(valueName) as string;
    }

    public static void DeleteValue(string? subKey, string valueName)
    {

        using var key = OpenExisting(subKey, writable: true);
        if (key is null)
            return;

        var actualName = FindValueNameIgnoreCase(key, valueName);
        if (actualName is null)
            return;

        try
        {
            key.DeleteValue(actualName, throwOnMissingValue: false);
        }
        catch
        {
            // значение уже отсутствует
        }
    }

    private static string? FindValueNameIgnoreCase(RegistryKey key, string valueName)
    {
        foreach (var name in key.GetValueNames())
        {
            if (string.Equals(name, valueName, StringComparison.OrdinalIgnoreCase))
                return name;
        }

        return null;
    }

    public static byte[]? ReadBinary(string? subKey, string valueName)
    {
        using var key = Open(subKey, false);
        if (key is null)
            return null;

        return key.GetValue(valueName) as byte[];
    }

    public static bool WriteBinary(string? subKey, string valueName, byte[] data)
    {
        using var key = Open(subKey, true);
        if (key is null)
            return false;

        key.SetValue(valueName, data, RegistryValueKind.Binary);
        return true;
    }

    /// <summary>Удалить поддерево HKCU\...\BwmRoot\relativeSubKey целиком.</summary>
    public static void DeleteSubKeyTree(string? relativeSubKey)
    {
        var path = SubKeyPath(relativeSubKey);
        try
        {
            global::Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(path, throwOnMissingSubKey: false);
        }
        catch
        {
            // подраздел уже отсутствует
        }
    }

    private static RegistryKey? Open(string? subKey, bool writable)
    {
        var path = SubKeyPath(subKey);
        return global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(path, writable)
               ?? (writable ? global::Microsoft.Win32.Registry.CurrentUser.CreateSubKey(path) : null);
    }

    private static RegistryKey? OpenExisting(string? subKey, bool writable)
    {
        return global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(SubKeyPath(subKey), writable);
    }

    private static uint? RawToUInt(object? raw) => raw switch

    {

        int i => unchecked((uint)i),

        uint u => u,

        _ => null

    };

}

