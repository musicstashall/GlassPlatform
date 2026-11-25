using System.Runtime.InteropServices;
using GlassSettings.Core.Registry;
using Microsoft.Win32;

namespace GlassSettings.Core.Placement;

public static class WindowPlacementStore
{
    public const int MinWindowW = 600;
    public const int MinWindowH = 460;

    /// <summary>Минимальное пересечение прямоугольника с виртуальным экраном, чтобы окно считалось «на месте».</summary>
    private const int MinVisiblePx = 64;

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
            if (r > l && b > t) return Sanitize((show, l, t, r - l, b - t));
        }
        return DefaultPlacement();
    }

    public static void Save(int showCmd, int left, int top, int width, int height)
    {
        // Не закрепляем «заэкранный» RECT (эпоха park −32000, отключённый монитор): иначе
        // следующая сессия стартует невидимым окном и «чистка» нужна вручную.
        if (!IsVisibleOnSomeScreen(left, top, width, height))
            return;

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

    /// <summary>
    /// Rect обязан пересекаться с рабочим пространством хотя бы одного экрана. Иначе — центр
    /// рабочего пространства primary, размер вписан в него (позиция из реестра не «тянет» окно в тень).
    /// </summary>
    private static (int showCmd, int left, int top, int width, int height) Sanitize(
        (int showCmd, int left, int top, int width, int height) placement)
    {
        if (IsVisibleOnSomeScreen(placement.left, placement.top, placement.width, placement.height))
            return placement;

        if (!TryGetPrimaryWorkArea(out var x, out var y, out var w, out var h))
            return DefaultPlacement();

        var width = Math.Clamp(placement.width, MinWindowW, Math.Max(MinWindowW, w));
        var height = Math.Clamp(placement.height, MinWindowH, Math.Max(MinWindowH, h));
        return (
            1,
            x + Math.Max(0, (w - width) / 2),
            y + Math.Max(0, (h - height) / 2),
            width,
            height);
    }

    /// <summary>Пересечение с виртуальным экраном (покрывает все мониторы) ≥ <see cref="MinVisiblePx"/> по обеим осям.</summary>
    private static bool IsVisibleOnSomeScreen(int left, int top, int width, int height)
    {
        var vx = GetSystemMetrics(SmXVirtualScreen);
        var vy = GetSystemMetrics(SmYVirtualScreen);
        var vw = GetSystemMetrics(SmCxVirtualScreen);
        var vh = GetSystemMetrics(SmCyVirtualScreen);
        if (vw <= 0 || vh <= 0)
            return true; // метрики недоступны — не чиним то, что не проверили

        var overlapX = Math.Min(left + width, vx + vw) - Math.Max(left, vx);
        var overlapY = Math.Min(top + height, vy + vh) - Math.Max(top, vy);
        return overlapX >= MinVisiblePx && overlapY >= MinVisiblePx;
    }

    private static bool TryGetPrimaryWorkArea(out int left, out int top, out int width, out int height)
    {
        var rect = default(NativeRect);
        left = top = width = height = 0;
        if (!SystemParametersInfo(SpiGetWorkArea, 0, ref rect, 0))
            return false;

        left = rect.Left;
        top = rect.Top;
        width = Math.Max(0, rect.Right - rect.Left);
        height = Math.Max(0, rect.Bottom - rect.Top);
        return width > 0 && height > 0;
    }

    private const int SmXVirtualScreen = 76;
    private const int SmYVirtualScreen = 77;
    private const int SmCxVirtualScreen = 78;
    private const int SmCyVirtualScreen = 79;
    private const int SpiGetWorkArea = 0x0030;

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeRect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int nIndex);

    [DllImport("user32.dll", EntryPoint = "SystemParametersInfoW", CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SystemParametersInfo(uint uiAction, uint uiParam, ref NativeRect pvParam, uint fWinIni);
}
