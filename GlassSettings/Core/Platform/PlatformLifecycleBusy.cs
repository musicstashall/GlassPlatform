using System.Runtime.InteropServices;

namespace GlassSettings.Core.Platform;

/// <summary>
/// Named Event: сигнал = lifecycle start/stop в полёте.
/// Home тумблеры и GlassShellExt меню смотрят один флаг.
/// </summary>
public static class PlatformLifecycleBusy
{
    private const string NameX64 = @"Local\GlassPlatform.LifecycleBusy.x64";
    private const string NameX86 = @"Local\GlassPlatform.LifecycleBusy.x86";
    private const string PresenceChangedName = "GlassPlatform.PresenceChanged";

    public static string EventName(GlassPlatformArch arch) =>
        arch == GlassPlatformArch.X64 ? NameX64 : NameX86;

    public static bool IsBusy(GlassPlatformArch arch)
    {
        var h = OpenEventW(0x00100000 /* SYNCHRONIZE */, false, EventName(arch));
        if (h == IntPtr.Zero)
            return false;
        try
        {
            return WaitForSingleObject(h, 0) == 0; // WAIT_OBJECT_0 = signaled = busy
        }
        finally
        {
            CloseHandle(h);
        }
    }

    public static IDisposable Begin(GlassPlatformArch arch) => new Guard(arch);

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

        public Guard(GlassPlatformArch arch)
        {
            _handle = CreateEventW(IntPtr.Zero, true, false, EventName(arch));
            if (_handle != IntPtr.Zero)
            {
                SetEvent(_handle);
                PulsePresenceChanged();
            }
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
