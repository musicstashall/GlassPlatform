using System.Runtime.InteropServices;
using Microsoft.UI.Xaml;
using WinRT.Interop;

namespace GlassSettings.Services;

/// <summary>Модальный дочерний WinUI-окно: owner disabled, GWLP_HWNDPARENT.</summary>
internal sealed class ModalChildWindowScope : IDisposable
{
    private readonly nint _parentHwnd;
    private bool _disposed;

    public ModalChildWindowScope(Window owner, Window child)
    {
        _parentHwnd = WindowNative.GetWindowHandle(owner);
        var childHwnd = WindowNative.GetWindowHandle(child);
        _ = SetWindowLongPtr(childHwnd, GWLP_HWNDPARENT, _parentHwnd);
        _ = EnableWindow(_parentHwnd, false);
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        _ = EnableWindow(_parentHwnd, true);
    }

    private const int GWLP_HWNDPARENT = -8;

    [DllImport("user32.dll", SetLastError = true)]
    private static extern nint SetWindowLongPtr(nint hWnd, int nIndex, nint dwNewLong);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool EnableWindow(
        nint hWnd,
        [MarshalAs(UnmanagedType.Bool)] bool enable);
}
