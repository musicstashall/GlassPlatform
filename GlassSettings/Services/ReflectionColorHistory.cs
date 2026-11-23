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

    public static void Remember(uint argb)
    {
        try
        {
            var colors = new[] { argb }.Concat(Read()).Distinct().Take(8)
                .Select(color => color.ToString("X8", CultureInfo.InvariantCulture)).ToArray();
            using var key = Registry.CurrentUser.CreateSubKey(ReflectionRenderState.RegistryPath);
            key.SetValue(ValueName, colors, RegistryValueKind.MultiString);
        }
        catch (Exception error) when (error is System.IO.IOException or UnauthorizedAccessException or System.Security.SecurityException)
        {
            System.Diagnostics.Trace.TraceWarning("Reflection color history save: {0}", error.Message);
        }
    }
}
