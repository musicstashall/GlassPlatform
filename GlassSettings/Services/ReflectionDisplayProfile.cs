using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>Same versioned native geometry/DPI identity used by the optional platform host.</summary>
internal sealed record ReflectionDisplayProfile(string Key, int Width, int Height, ReflectionDisplayProfile.DisplayMonitor[] Monitors)
{
    internal const string PublicationMutex = @"Local\GlassPlatform.ReflectionProfiles";
    [StructLayout(LayoutKind.Sequential)]
    internal struct DisplayMonitor { public int X, Y, Width, Height; public uint DpiX, DpiY; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct Snapshot
    {
        public uint Size, Version;
        public int Width, Height;
        public uint Count;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public DisplayMonitor[] Monitors;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Key;
    }
    [DllImport("GlassSettings.Native.dll", ExactSpelling = true, CallingConvention = CallingConvention.StdCall)]
    private static extern int GlassSettings_CaptureReflectionProfileV1(ref Snapshot snapshot);

    public static ReflectionDisplayProfile Capture()
    {
        var value = new Snapshot { Size = (uint)Marshal.SizeOf<Snapshot>(), Monitors = new DisplayMonitor[32], Key = "" };
        Marshal.ThrowExceptionForHR(GlassSettings_CaptureReflectionProfileV1(ref value));
        if (value.Version != 1 || value.Count is 0 or > 32 || string.IsNullOrEmpty(value.Key))
            throw new InvalidOperationException("Invalid native display profile snapshot.");
        return new(value.Key, value.Width, value.Height, value.Monitors.Take((int)value.Count).ToArray());
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MonitorInfo
    {
        public uint Size;
        public Rect Monitor, Work;
        public uint Flags;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string Device;
    }
    private const uint MonitorDefaultToNearest = 2;
    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromRect(ref Rect rect, uint flags);
    [DllImport("user32.dll", EntryPoint = "GetMonitorInfoW", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);

    // Device suffix supplies Windows display numbering, independent of left-to-right sort.
    // This does not participate in the canonical profile identity.
    internal static int GetDisplayNumber(DisplayMonitor monitor, int fallback)
    {
        var rect = new Rect { Left = monitor.X, Top = monitor.Y, Right = monitor.X + monitor.Width, Bottom = monitor.Y + monitor.Height };
        var info = new MonitorInfo { Size = (uint)Marshal.SizeOf<MonitorInfo>(), Device = "" };
        if (GetMonitorInfo(MonitorFromRect(ref rect, MonitorDefaultToNearest), ref info))
        {
            string digits = new(info.Device.Reverse().TakeWhile(character => char.IsDigit(character)).Reverse().ToArray());
            if (int.TryParse(digits, out int number)) return number;
        }
        return fallback;
    }

    public string Label => $"{Width} × {Height} · "
        + string.Join(" / ", Monitors.Select(m => m.DpiX == m.DpiY ? $"{m.DpiX}" : $"{m.DpiX}×{m.DpiY}").Distinct()) + " DPI";
}
