using System.Runtime.InteropServices;
using System.Text;
using GlassSettings.Core.Registry;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>
/// Выбор окна под курсором: topmost layered HWND с IDC_CROSS (без SetSystemCursor).
/// Esc/ПКМ — отмена; ЛКМ — root-окно → exe; исключения — продолжаем pick.
/// </summary>
public static class WindowProcessPicker
{
    private const string ClassName = "GlassSettings.WindowProcessPicker.Overlay";
    private const int GaRoot = 2;
    private const int SwHide = 0;
    private const int SwShow = 5;
    private const uint WsPopup = 0x80000000;
    private const uint WsExTopmost = 0x00000008;
    private const uint WsExToolwindow = 0x00000080;
    private const uint WsExLayered = 0x00080000;
    private const uint WsExNoactivate = 0x08000000;
    private const uint LwaAlpha = 0x00000002;
    private const int WmDestroy = 0x0002;
    private const int WmClose = 0x0010;
    private const int WmKeyDown = 0x0100;
    private const int WmLButtonDown = 0x0201;
    private const int WmRButtonDown = 0x0204;
    private const int VkEscape = 0x1B;
    private const uint ProcessQueryLimitedInformation = 0x1000;
    private const int SmXVirtualScreen = 76;
    private const int SmYVirtualScreen = 77;
    private const int SmCxVirtualScreen = 78;
    private const int SmCyVirtualScreen = 79;
    private static readonly IntPtr IdcCross = new(32515);

    private static readonly string[] ExcludedClassNames =
    [
        "Progman",
        "WorkerW",
        "Shell_TrayWnd",
        "Shell_SecondaryTrayWnd",
    ];

    private static readonly object Gate = new();
    private static bool _pickActive;
    private static IntPtr _hwnd;
    private static ushort _atom;
    private static TaskCompletionSource<string?>? _tcs;
    private static WndProcDelegate? _wndProcKeepAlive;
    private static string _selfExeName = string.Empty;

    /// <summary>Запуск pick на UI-потоке с message pump (WinUI). null = отмена.</summary>
    public static Task<string?> PickExeNameAsync(DispatcherQueue dispatcherQueue)
    {
        ArgumentNullException.ThrowIfNull(dispatcherQueue);

        var tcs = new TaskCompletionSource<string?>(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!dispatcherQueue.TryEnqueue(() => StartPick(tcs)))
        {
            tcs.TrySetResult(null);
            return tcs.Task;
        }

        return tcs.Task;
    }

    private static void StartPick(TaskCompletionSource<string?> tcs)
    {
        lock (Gate)
        {
            if (_pickActive)
            {
                tcs.TrySetResult(null);
                return;
            }

            _pickActive = true;
            _tcs = tcs;
            _selfExeName = GetSelfExeName();
        }

        try
        {
            if (!CreateOverlay())
                Complete(null);
        }
        catch
        {
            Complete(null);
        }
    }

    private static bool CreateOverlay()
    {
        _wndProcKeepAlive = WndProc;
        var wndClass = new WndClassEx
        {
            cbSize = (uint)Marshal.SizeOf<WndClassEx>(),
            lpfnWndProc = Marshal.GetFunctionPointerForDelegate(_wndProcKeepAlive),
            hInstance = GetModuleHandle(null),
            lpszClassName = ClassName,
            hCursor = LoadCursor(IntPtr.Zero, IdcCross),
            hbrBackground = IntPtr.Zero,
        };

        _atom = RegisterClassEx(ref wndClass);
        if (_atom == 0 && Marshal.GetLastWin32Error() != 1410 /* ERROR_CLASS_ALREADY_EXISTS */)
            return false;

        var x = GetSystemMetrics(SmXVirtualScreen);
        var y = GetSystemMetrics(SmYVirtualScreen);
        var cx = GetSystemMetrics(SmCxVirtualScreen);
        var cy = GetSystemMetrics(SmCyVirtualScreen);
        if (cx <= 0 || cy <= 0)
        {
            x = 0;
            y = 0;
            cx = GetSystemMetrics(0); // SM_CXSCREEN
            cy = GetSystemMetrics(1); // SM_CYSCREEN
        }

        _hwnd = CreateWindowEx(
            WsExTopmost | WsExToolwindow | WsExLayered | WsExNoactivate,
            ClassName,
            string.Empty,
            WsPopup,
            x,
            y,
            cx,
            cy,
            IntPtr.Zero,
            IntPtr.Zero,
            GetModuleHandle(null),
            IntPtr.Zero);

        if (_hwnd == IntPtr.Zero)
            return false;

        // Почти невидимый, но hit-testable — курсор берётся из класса (IDC_CROSS).
        SetLayeredWindowAttributes(_hwnd, 0, 1, LwaAlpha);
        ShowWindow(_hwnd, SwShow);
        SetForegroundWindow(_hwnd);
        SetFocus(_hwnd);
        return true;
    }

    private static IntPtr WndProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam)
    {
        switch (msg)
        {
            case WmLButtonDown:
                OnLeftClick();
                return IntPtr.Zero;

            case WmRButtonDown:
                Complete(null);
                return IntPtr.Zero;

            case WmKeyDown when wParam == VkEscape:
                Complete(null);
                return IntPtr.Zero;

            case WmClose:
                Complete(null);
                return IntPtr.Zero;

            case WmDestroy:
                return IntPtr.Zero;
        }

        return DefWindowProc(hWnd, msg, wParam, lParam);
    }

    private static void OnLeftClick()
    {
        if (!GetCursorPos(out var pt))
            return;

        var overlay = _hwnd;
        if (overlay != IntPtr.Zero)
            ShowWindow(overlay, SwHide);

        try
        {
            var hit = WindowFromPoint(pt);
            if (hit == IntPtr.Zero)
                return;

            var root = GetAncestor(hit, GaRoot);
            if (root == IntPtr.Zero)
                root = hit;

            if (IsExcludedWindow(root))
                return;

            if (!TryGetExeNameFromWindow(root, out var exeName) || string.IsNullOrWhiteSpace(exeName))
                return;

            if (IsSelfExe(exeName))
                return;

            Complete(AttachListRegistry.NormalizeExeName(exeName));
        }
        finally
        {
            // Если pick ещё активен — снова показать прицел.
            if (_pickActive && _hwnd != IntPtr.Zero && IsWindow(_hwnd))
            {
                ShowWindow(_hwnd, SwShow);
                SetForegroundWindow(_hwnd);
            }
        }
    }

    private static bool IsExcludedWindow(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero || !IsWindow(hwnd))
            return true;

        GetWindowThreadProcessId(hwnd, out var pid);
        if (pid == 0 || pid == (uint)Environment.ProcessId)
            return true;

        var className = GetWindowClassName(hwnd);
        foreach (var excluded in ExcludedClassNames)
        {
            if (string.Equals(className, excluded, StringComparison.OrdinalIgnoreCase))
                return true;
        }

        return false;
    }

    private static bool TryGetExeNameFromWindow(IntPtr hwnd, out string exeName)
    {
        exeName = string.Empty;
        GetWindowThreadProcessId(hwnd, out var pid);
        if (pid == 0)
            return false;

        var path = QueryProcessImagePath(pid);
        if (string.IsNullOrWhiteSpace(path))
            return false;

        exeName = Path.GetFileName(path);
        return !string.IsNullOrWhiteSpace(exeName);
    }

    private static string? QueryProcessImagePath(uint pid)
    {
        var h = OpenProcess(ProcessQueryLimitedInformation, false, pid);
        if (h == IntPtr.Zero)
            return null;

        try
        {
            var sb = new StringBuilder(1024);
            var size = (uint)sb.Capacity;
            if (!QueryFullProcessImageName(h, 0, sb, ref size))
                return null;
            return sb.ToString();
        }
        finally
        {
            CloseHandle(h);
        }
    }

    private static string GetWindowClassName(IntPtr hwnd)
    {
        var sb = new StringBuilder(256);
        return GetClassName(hwnd, sb, sb.Capacity) > 0 ? sb.ToString() : string.Empty;
    }

    private static string GetSelfExeName()
    {
        var path = Environment.ProcessPath;
        if (!string.IsNullOrWhiteSpace(path))
            return AttachListRegistry.NormalizeExeName(Path.GetFileName(path));

        return AttachListRegistry.NormalizeExeName(
            System.Diagnostics.Process.GetCurrentProcess().ProcessName);
    }

    private static bool IsSelfExe(string exeName) =>
        string.Equals(
            AttachListRegistry.NormalizeExeName(exeName),
            _selfExeName,
            StringComparison.OrdinalIgnoreCase);

    private static void Complete(string? exeName)
    {
        TaskCompletionSource<string?>? tcs;
        lock (Gate)
        {
            if (!_pickActive)
                return;

            _pickActive = false;
            tcs = _tcs;
            _tcs = null;
        }

        try
        {
            DestroyOverlay();
        }
        finally
        {
            tcs?.TrySetResult(exeName);
        }
    }

    private static void DestroyOverlay()
    {
        var hwnd = _hwnd;
        _hwnd = IntPtr.Zero;

        if (hwnd != IntPtr.Zero && IsWindow(hwnd))
        {
            DestroyWindow(hwnd);
        }

        // Класс мог быть зарегистрирован в этом процессе ранее — снимаем atom.
        if (_atom != 0 || !string.IsNullOrEmpty(ClassName))
        {
            UnregisterClass(ClassName, GetModuleHandle(null));
            _atom = 0;
        }

        _wndProcKeepAlive = null;
    }

    private delegate IntPtr WndProcDelegate(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct WndClassEx
    {
        public uint cbSize;
        public uint style;
        public IntPtr lpfnWndProc;
        public int cbClsExtra;
        public int cbWndExtra;
        public IntPtr hInstance;
        public IntPtr hIcon;
        public IntPtr hCursor;
        public IntPtr hbrBackground;
        public string? lpszMenuName;
        public string lpszClassName;
        public IntPtr hIconSm;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern ushort RegisterClassEx(ref WndClassEx lpwcx);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnregisterClass(string lpClassName, IntPtr hInstance);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateWindowEx(
        uint dwExStyle,
        string lpClassName,
        string lpWindowName,
        uint dwStyle,
        int x,
        int y,
        int nWidth,
        int nHeight,
        IntPtr hWndParent,
        IntPtr hMenu,
        IntPtr hInstance,
        IntPtr lpParam);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetLayeredWindowAttributes(IntPtr hwnd, uint crKey, byte bAlpha, uint dwFlags);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr SetFocus(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern IntPtr DefWindowProc(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern IntPtr WindowFromPoint(Point point);

    [DllImport("user32.dll")]
    private static extern IntPtr GetAncestor(IntPtr hwnd, int gaFlags);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetCursorPos(out Point lpPoint);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hWnd, StringBuilder lpClassName, int nMaxCount);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint lpdwProcessId);

    [DllImport("user32.dll")]
    private static extern IntPtr LoadCursor(IntPtr hInstance, IntPtr lpCursorName);

    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int nIndex);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr GetModuleHandle(string? lpModuleName);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(uint dwDesiredAccess, bool bInheritHandle, uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr hObject);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool QueryFullProcessImageName(
        IntPtr hProcess,
        uint dwFlags,
        StringBuilder lpExeName,
        ref uint lpdwSize);
}
