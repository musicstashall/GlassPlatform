using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>
/// Корректный P/Invoke SetWindowSubclass для x86/x64:
/// CALLBACK/StdCall, UINT_PTR/DWORD_PTR как UIntPtr, стабильный function pointer.
/// </summary>
internal static class Win32WindowSubclass
{
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
        return SetWindowSubclass(hwnd, fnPtr, id, UIntPtr.Zero);
    }

    public static bool Remove(IntPtr hwnd, IntPtr fnPtr, UIntPtr id) =>
        hwnd != IntPtr.Zero && fnPtr != IntPtr.Zero && RemoveWindowSubclass(hwnd, fnPtr, id);

    public static IntPtr CallDef(IntPtr hWnd, uint uMsg, IntPtr wParam, IntPtr lParam) =>
        DefSubclassProc(hWnd, uMsg, wParam, lParam);

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
