using System.Diagnostics;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace GlassSettings.Services;

/// <summary>
/// Debug-замеры фаз старта GlassSettings (Output / Debug.WriteLine) + зонд состояния окна.
/// Начало отсчёта — <see cref="Begin"/> в конструкторе <c>MainWindow</c>.
/// План и мотивация: GlassSettings/docs/GlassSettings-startup-window-plan.md (Э0).
/// </summary>
internal static class StartupPhaseTrace
{
    private static readonly Stopwatch Watch = new();
    private static long _lastMs;
    private static bool _started;

    public static void Begin()
    {
#if DEBUG
        Watch.Restart();
        _lastMs = 0;
        _started = true;
        Mark("begin");
#endif
    }

    public static void Mark(string phase)
    {
#if DEBUG
        if (!_started)
            return;

        var totalMs = Watch.ElapsedMilliseconds;
        var deltaMs = totalMs - _lastMs;
        _lastMs = totalMs;
        Debug.WriteLine(
            $"[GlassSettings][Startup] +{totalMs}ms (+{deltaMs}ms) {phase}");
#endif
    }

    /// <summary>
    /// Метка + снимок состояния окна: чем он отличается на «сером» и на «живом» кадре.
    /// visible/foreground/cloaked должны быть одинаковы в первом present и после него.
    /// </summary>
    public static void MarkWindow(string phase, Window? window)
    {
#if DEBUG
        Mark(_started ? $"{phase} | {Describe(window)}" : phase);
#endif
    }

    private static string Describe(Window? window)
    {
        if (window is null)
            return "window=null";

        try
        {
            var hwnd = WindowNative.GetWindowHandle(window);
            var cloaked = QueryCloaked(hwnd);
            var appWindow = window.AppWindow;
            var backdrop = window.SystemBackdrop?.GetType().Name ?? "<null>";
            return $"hwnd=0x{hwnd.ToInt64():X} visible={appWindow.IsVisible} "
                   + $"switchers={appWindow.IsShownInSwitchers} cloaked={cloaked} "
                   + $"fg={Win32GetForegroundWindow() == hwnd} backdrop={backdrop} "
                   + $"size={appWindow.Size.Width}x{appWindow.Size.Height} "
                   + $"pos={appWindow.Position.X},{appWindow.Position.Y}";
        }
        catch (Exception ex)
        {
            return $"describe-failed:{ex.GetType().Name}";
        }
    }

    private static string QueryCloaked(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero)
            return "n/a";

        try
        {
            var value = 0;
            // DWMWA_CLOAKED = 14: 0 — не скрыто, 1 — DWM cloak, 2 — owner, 4 — inode.
            return DwmGetWindowAttribute(hwnd, 14, out value, sizeof(int)) == 0
                ? value.ToString()
                : "err";
        }
        catch
        {
            return "err";
        }
    }

    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(IntPtr hwnd, int dwAttribute, out int pvAttribute, int cbAttribute);

    [System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "GetForegroundWindow")]
    private static extern IntPtr Win32GetForegroundWindow();
}
