using System.Runtime.InteropServices;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Cross-process busy guard для OpenGlass (start/stop operations).
/// Named event: один процесс захватывает, другой видит что занято.
/// GlassShellExt и GlassSettings координируются через этот event.
/// </summary>
public static class OpenGlassLifecycleBusy
{
    public const string EventName = @"Local\GlassPlatform.OpenGlassBusy";
    private const string PresenceChangedName = "GlassPlatform.PresenceChanged";

    public static bool IsBusy()
    {
        var h = OpenEventW(0x00100000 /* SYNCHRONIZE */, false, EventName);
        if (h == IntPtr.Zero)
            return false;
        try
        {
            return WaitForSingleObject(h, 0) == 0;
        }
        finally
        {
            CloseHandle(h);
        }
    }

    /// <summary>
    /// Try-захват: возвращает null если busy уже захвачен.
    /// Вызывающий код продолжает операцию без busy guard.
    /// </summary>
    public static IDisposable? TryBegin()
    {
        var handle = CreateEventW(IntPtr.Zero, true, false, EventName);
        if (handle == IntPtr.Zero)
            return null;

        if (WaitForSingleObject(handle, 0) == 0)
        {
            CloseHandle(handle);
            return null;
        }

        SetEvent(handle);
        PulsePresenceChanged();
        return new Guard(handle);
    }

    private static void PulsePresenceChanged()
    {
        var h = OpenEventW(0x0002 /* EVENT_MODIFY_STATE */, false, PresenceChangedName);
        if (h == IntPtr.Zero)
            return;
        try
        {
            SetEvent(h);
        }
        finally
        {
            CloseHandle(h);
        }
    }

    private sealed class Guard : IDisposable
    {
        private IntPtr _handle;
        private bool _disposed;

        public Guard(IntPtr handle)
        {
            _handle = handle;
        }

        public void Dispose()
        {
            if (_disposed)
                return;
            _disposed = true;
            if (_handle != IntPtr.Zero)
            {
                ResetEvent(_handle);
                CloseHandle(_handle);
                _handle = IntPtr.Zero;
                PulsePresenceChanged();
            }
        }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateEventW(
        IntPtr lpEventAttributes,
        [MarshalAs(UnmanagedType.Bool)] bool bManualReset,
        [MarshalAs(UnmanagedType.Bool)] bool bInitialState,
        string lpName);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenEventW(
        uint dwDesiredAccess,
        [MarshalAs(UnmanagedType.Bool)] bool bInheritHandle,
        string lpName);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetEvent(IntPtr hEvent);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ResetEvent(IntPtr hEvent);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr hObject);
}
