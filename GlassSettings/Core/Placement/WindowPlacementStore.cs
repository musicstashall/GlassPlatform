using GlassSettings.Core.Registry;
using Microsoft.Win32;

namespace GlassSettings.Core.Placement;

public static class WindowPlacementStore
{
    public const int MinWindowW = 600;
    public const int MinWindowH = 460;

    public static (int showCmd, int left, int top, int width, int height) LoadOrDefault()
    {
        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RegistryPaths.GlassSettingsPlacementKey, false);
        if (key?.GetValue(RegistryPaths.MainWindowPlacementValue) is byte[] { Length: >= 44 } blob)
        {
            var show = BitConverter.ToInt32(blob, 8);
            var l = BitConverter.ToInt32(blob, 24);
            var t = BitConverter.ToInt32(blob, 28);
            var r = BitConverter.ToInt32(blob, 32);
            var b = BitConverter.ToInt32(blob, 36);
            if (r > l && b > t) return (show, l, t, r - l, b - t);
        }
        return DefaultPlacement();
    }

    public static void Save(int showCmd, int left, int top, int width, int height)
    {
        var blob = new byte[44];
        BitConverter.GetBytes(44).CopyTo(blob, 0);
        BitConverter.GetBytes(showCmd).CopyTo(blob, 8);
        BitConverter.GetBytes(left).CopyTo(blob, 24);
        BitConverter.GetBytes(top).CopyTo(blob, 28);
        BitConverter.GetBytes(left + width).CopyTo(blob, 32);
        BitConverter.GetBytes(top + height).CopyTo(blob, 36);
        using var key = global::Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RegistryPaths.GlassSettingsPlacementKey);
        key.SetValue(RegistryPaths.MainWindowPlacementValue, blob, RegistryValueKind.Binary);
    }

    public static (int showCmd, int left, int top, int width, int height) DefaultPlacement()
        => (1, 100, 100, 800, 600);
}
