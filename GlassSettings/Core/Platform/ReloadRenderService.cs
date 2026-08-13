using System.Runtime.InteropServices;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Live-reload DWM/OpenGlass: паритет OpenGlassGUI <c>MainFrame::NotifySettingsChange</c> —
/// <c>SendNotifyMessage(FindWindow(Dwm), WM_*)</c>.
/// </summary>
public static class ReloadRenderService
{
    private static readonly IntPtr HWND_BROADCAST = new(0xffff);

    /// <summary>WM_DWMCOLORIZATIONCOLORCHANGED (0x0320).</summary>
    private const uint WM_DWMCOLORIZATIONCOLORCHANGED = 0x0320;

    /// <summary>WM_THEMECHANGED (0x031A).</summary>
    private const uint WM_THEMECHANGED = 0x031A;

    private static IntPtr _dwmNotificationWindow;

    public static bool IsGlassPlatformRunning() =>
        IsGlassPlatformRunning(GlassPlatformNames.NativeArch);

    public static bool IsGlassPlatformRunning(GlassPlatformArch arch)
    {
        // SYNCHRONIZE; bInheritHandle — BOOL (4 байта), не C# bool/I1 (на x86 иначе AV).
        var h = OpenMutex(0x00100000, inheritHandle: false, GlassPlatformNames.InstanceMutexFor(arch));
        if (h == IntPtr.Zero) return false;

        CloseHandle(h);
        return true;
    }

    /// <summary>Хотя бы один хост (x86 или x64) жив — для общего Reload IPC.</summary>
    public static bool IsAnyGlassPlatformRunning() =>
        IsGlassPlatformRunning(GlassPlatformArch.X64) || IsGlassPlatformRunning(GlassPlatformArch.X86);

    public static bool SignalReloadRender(uint flags)
    {
        if (!IsAnyGlassPlatformRunning()) return false;

        // UICompat в AppContainer читает TrayXamlPolicy map раньше HKCU.
        if ((flags & ReloadRenderFlags.TaskbarXaml) != 0)
            _ = NativeHostInterop.PublishTrayXamlPolicy();

        var hSem = OpenSemaphore(0x00000002 | 0x00100000, false, GlassPlatformNames.ReloadRenderSemaphore);
        var hMap = OpenFileMapping(0x0002, false, GlassPlatformNames.ReloadGenerationMapping);
        var hFlagsMap = OpenFileMapping(0x0002, false, GlassPlatformNames.ReloadRenderFlagsMapping);

        if (hSem == IntPtr.Zero || hMap == IntPtr.Zero)
        {
            if (hFlagsMap != IntPtr.Zero) CloseHandle(hFlagsMap);
            if (hSem != IntPtr.Zero) CloseHandle(hSem);
            if (hMap != IntPtr.Zero) CloseHandle(hMap);
            return false;
        }

        // Паритет GlassPlatformTask::SignalReloadRender — drain + release 512 (все GUI-потоки GlassCompat).
        while (WaitForSingleObject(hSem, 0) == 0)
        {
        }

        if (hFlagsMap != IntPtr.Zero)
        {
            var pFlags = MapViewOfFile(hFlagsMap, 0x0002, 0, 0, 4);
            if (pFlags != IntPtr.Zero)
            {
                Marshal.WriteInt32(pFlags, unchecked((int)flags));
                UnmapViewOfFile(pFlags);
            }
        }

        var pView = MapViewOfFile(hMap, 0x0002, 0, 0, 4);
        if (pView != IntPtr.Zero)
        {
            Marshal.WriteInt32(pView, Marshal.ReadInt32(pView) + 1);
            UnmapViewOfFile(pView);
        }

        const int guiThreadReleaseBudget = 512;
        var ok = ReleaseSemaphore(hSem, guiThreadReleaseBudget, out _);
        if (hFlagsMap != IntPtr.Zero) CloseHandle(hFlagsMap);
        CloseHandle(hSem);
        CloseHandle(hMap);
        return ok;
    }

    /// <summary>WM_DWMCOLORIZATIONCOLORCHANGED на окно класса Dwm (OpenGlassGUI).</summary>
    public static void NotifyDwmColorizationChanged() =>
        NotifyDwmWindow(WM_DWMCOLORIZATIONCOLORCHANGED);

    /// <summary>WM_THEMECHANGED на окно класса Dwm (Reflection / Material / Atlas).</summary>
    public static void NotifyThemeChanged() =>
        NotifyDwmWindow(WM_THEMECHANGED);

    /// <summary>Dwm-окно OpenGlass; при отсутствии — HWND_BROADCAST (откат при закрытии).</summary>
    public static void NotifyDwmColorizationChangedWithFallback() =>
        NotifyDwmWindowWithFallback(WM_DWMCOLORIZATIONCOLORCHANGED);

    /// <summary>Dwm-окно OpenGlass; при отсутствии — HWND_BROADCAST WM_THEMECHANGED.</summary>
    public static void NotifyThemeChangedWithFallback() =>
        NotifyDwmWindowWithFallback(WM_THEMECHANGED);

    public static void NotifyDwmColorizationChangedForExit() =>
        NotifyDwmColorizationChanged();

    /// <summary>Массовая рассылка WM_DWMCOLORIZATIONCOLORCHANGED (откат BWM / платформа Glass).</summary>
    public static void NotifyDwmColorizationChangedBroadcast() =>
        _ = SendNotifyMessage(HWND_BROADCAST, WM_DWMCOLORIZATIONCOLORCHANGED, IntPtr.Zero, IntPtr.Zero);

    private static void NotifyDwmWindow(uint message)
    {
        var hwnd = ResolveDwmNotificationWindow();
        if (hwnd == IntPtr.Zero)
            return;

        _ = SendNotifyMessage(hwnd, message, IntPtr.Zero, IntPtr.Zero);
    }

    private static void NotifyDwmWindowWithFallback(uint message)
    {
        var hwnd = ResolveDwmNotificationWindow();
        if (hwnd == IntPtr.Zero)
            _ = SendNotifyMessage(HWND_BROADCAST, message, IntPtr.Zero, IntPtr.Zero);
        else
            _ = SendNotifyMessage(hwnd, message, IntPtr.Zero, IntPtr.Zero);
    }

    private static IntPtr ResolveDwmNotificationWindow()
    {
        if (_dwmNotificationWindow != IntPtr.Zero && IsWindow(_dwmNotificationWindow))
            return _dwmNotificationWindow;

        _dwmNotificationWindow = FindWindowW("Dwm", null);
        return _dwmNotificationWindow;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenMutex(
        uint desiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandle,
        string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenSemaphore(
        uint desiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandle,
        string name);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenFileMapping(
        uint desiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool inheritHandle,
        string name);

    [DllImport("kernel32.dll")]
    private static extern IntPtr MapViewOfFile(IntPtr h, uint a, uint oh, uint ol, nuint sz);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnmapViewOfFile(IntPtr p);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ReleaseSemaphore(IntPtr h, int c, out int p);

    [DllImport("kernel32.dll")]
    private static extern uint WaitForSingleObject(IntPtr h, uint ms);

    [DllImport("kernel32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr h);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? className, string? windowName);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SendNotifyMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
}
