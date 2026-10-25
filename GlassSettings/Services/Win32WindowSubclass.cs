using System.Collections.Concurrent;
using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>
/// Корректный P/Invoke SetWindowSubclass для x86/x64:
/// CALLBACK/StdCall, UINT_PTR/DWORD_PTR как UIntPtr, стабильный function pointer.
/// </summary>
internal static class Win32WindowSubclass
{
    private static readonly ConcurrentDictionary<IntPtr, GCHandle> s_delegatePins = new();

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    public delegate IntPtr Proc(
        IntPtr hWnd,
        uint uMsg,
        IntPtr wParam,
        IntPtr lParam,
        UIntPtr uIdSubclass,
        UIntPtr dwRefData);

    public static bool Install(IntPtr hwnd, Proc proc, UIntPtr id, out IntPtr fnPtr)
    {
        fnPtr = Marshal.GetFunctionPointerForDelegate(proc);
        if (!SetWindowSubclass(hwnd, fnPtr, id, UIntPtr.Zero))
            return false;

        s_delegatePins.TryAdd(fnPtr, GCHandle.Alloc(proc));
        return true;
    }

    public static bool Remove(IntPtr hwnd, IntPtr fnPtr, UIntPtr id)
    {
        if (hwnd == IntPtr.Zero || fnPtr == IntPtr.Zero)
            return false;

        if (!RemoveWindowSubclass(hwnd, fnPtr, id))
            return false;

        if (s_delegatePins.TryRemove(fnPtr, out var handle) && handle.IsAllocated)
            handle.Free();

        return true;
    }

    public static IntPtr CallDef(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam)
    {
        if (hWnd == IntPtr.Zero || !IsWindow(hWnd))
            return IntPtr.Zero;

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool IsWindow(IntPtr hWnd);

    [DllImport("comctl32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetWindowSubclass(
        IntPtr hWnd,
        IntPtr pfnSubclass,
        UIntPtr uIdSubclass,
        UIntPtr dwRefData);

    [DllImport("comctl32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool RemoveWindowSubclass(
        IntPtr hWnd,
        IntPtr pfnSubclass,
        UIntPtr uIdSubclass);

    [DllImport("comctl32.dll")]
    private static extern IntPtr DefSubclassProc(
        IntPtr hWnd,
        uint uMsg,
        IntPtr wParam,
        IntPtr lParam);
}
