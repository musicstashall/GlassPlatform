using System.Diagnostics;
using System.Runtime.InteropServices;
using GlassSettings.Strings;

namespace GlassSettings;

public static class SingleInstance
{
    /// <summary>Имя mutex с суффиксом разрядности процесса — x86 и x64 GS могут жить параллельно.</summary>
    public static string MutexName =>
        Environment.Is64BitProcess
            ? "GlassSettings.Instance.Mutex.x64"
            : "GlassSettings.Instance.Mutex.x86";

    private const int ActivatePollIntervalMs = 150;
    private const int ActivatePollTimeoutMs = 5000;
    private const int GwOwner = 4;
    private const int SwRestore = 9;
    /// <summary>GlassSettingsMsg::WM_GLASS_SETTINGS_ACTIVATE (WM_APP + 0xA03).</summary>
    private const uint WmGlassSettingsActivate = 0x8000 + 0xA03;
    private const uint SmtoAbortIfHung = 0x0002;
    private const uint SmtoTimeoutMs = 2000;

    public static string WindowTitle
    {
        get
        {
            var title = GlassSettingsResources.Get("AppTitle");
            if (Environment.Is64BitOperatingSystem && !Environment.Is64BitProcess)
                return title + " (x86)";
            return title;
        }
    }

    private static Mutex? _mutex;

    public static bool TryAcquire()
    {
        _mutex = new Mutex(true, MutexName, out var created);
        if (created) return true;
        _ = TryActivateExistingWithWait();
        return false;
    }

    public static void ActivateExisting()
    {
        _ = TryActivateExistingWithWait();
    }

    private static bool TryActivateExistingWithWait()
    {
        var deadline = Environment.TickCount64 + ActivatePollTimeoutMs;
        while (Environment.TickCount64 < deadline)
        {
            var hwnd = FindGlassSettingsWindow();
            if (hwnd != IntPtr.Zero)
            {
                ActivateHwnd(hwnd);
                return true;
            }

            Thread.Sleep(ActivatePollIntervalMs);
        }

        return false;
    }

    private static void ActivateHwnd(IntPtr hwnd)
    {
        GetWindowThreadProcessId(hwnd, out var pid);
        if (pid != 0)
            _ = AllowSetForegroundWindow(unchecked((int)pid));

        // In-process AppWindow restore (subclass MainWindow); Win32 — fallback до/без subclass.
        _ = SendMessageTimeoutW(
            hwnd,
            WmGlassSettingsActivate,
            IntPtr.Zero,
            IntPtr.Zero,
            SmtoAbortIfHung,
            SmtoTimeoutMs,
            out _);

        _ = ShowWindow(hwnd, SwRestore);
        _ = BringWindowToTop(hwnd);
        _ = SetForegroundWindow(hwnd);
    }

    private static IntPtr FindGlassSettingsWindow()
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows((hwnd, _) =>
        {
            if (GetWindow(hwnd, GwOwner) != IntPtr.Zero)
                return true;

            if (!IsWindowVisible(hwnd) && !IsIconic(hwnd))
                return true;

            GetWindowThreadProcessId(hwnd, out var pid);
            if (pid == 0)
                return true;

            try
            {
                using var proc = Process.GetProcessById((int)pid);
                if (!proc.ProcessName.Equals("GlassSettings", StringComparison.OrdinalIgnoreCase))
                    return true;
            }
            catch
            {
                return true;
            }

            found = hwnd;
            return false;
        }, IntPtr.Zero);

        if (found != IntPtr.Zero)
            return found;

        var byTitle = FindWindowW(null, WindowTitle);
        return byTitle;
    }

    public static void ReleaseForExit()
    {
        try
        {
            _mutex?.ReleaseMutex();
        }
        catch
        {
            /* уже отпущен */
        }

        _mutex?.Dispose();
        _mutex = null;
    }

    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr GetWindow(IntPtr hWnd, int uCmd);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowW(string? cls, string title);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr h, int cmd);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr h);

    [DllImport("user32.dll")]
    private static extern bool BringWindowToTop(IntPtr h);

    [DllImport("user32.dll")]
    private static extern bool AllowSetForegroundWindow(int dwProcessId);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr SendMessageTimeoutW(
        IntPtr hWnd,
        uint Msg,
        IntPtr wParam,
        IntPtr lParam,
        uint fuFlags,
        uint uTimeout,
        out IntPtr lpdwResult);
}
