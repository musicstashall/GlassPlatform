using Microsoft.Win32;
using System.Globalization;

namespace GlassSettings.Services;

/// <summary>Bounded ARGB history for reflection colors; never changes Windows accent history.</summary>
internal static class ReflectionColorHistory
{
    private const string ValueName = "RecentColors";
    public static IReadOnlyList<uint> Read()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(ReflectionRenderState.RegistryPath);
            return (key?.GetValue(ValueName) as string[] ?? [])
                .Select(value => uint.TryParse(value, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var color)
                    ? (uint?)color : null)
                .Where(color => color.HasValue).Select(color => color!.Value).Distinct().Take(8).ToArray();
        }
        catch (Exception error) when (error is System.IO.IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection color history read: {0}", error.Message);
            return [];
        }
    }

    public static uint? ReadCustom()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(ReflectionRenderState.RegistryPath);
            return key?.GetValue("CustomColor") is int value ? unchecked((uint)value) : null;
        }
        catch (Exception error) when (error is System.IO.IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection custom color read: {0}", error.Message);
            return null;
        }
    }

    public static void Save(IEnumerable<uint> history, uint? custom)
    {
        try
        {
            var colors = history.Distinct().Take(8)
                .Select(color => color.ToString("X8", CultureInfo.InvariantCulture)).ToArray();
            using var key = Registry.CurrentUser.CreateSubKey(ReflectionRenderState.RegistryPath);
            key.SetValue(ValueName, colors, RegistryValueKind.MultiString);
            if (custom is { } savedCustom)
                key.SetValue("CustomColor", unchecked((int)savedCustom), RegistryValueKind.DWord);
            else key.DeleteValue("CustomColor", throwOnMissingValue: false);
        }
        catch (Exception error) when (error is System.IO.IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection color history save: {0}", error.Message);
        }
    }
}
