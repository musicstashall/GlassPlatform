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

    /// <summary>Обычный захват: выбрасывает если busy уже захвачен другим процессом.</summary>
    public static IDisposable Begin(GlassPlatformArch arch) =>
        TryBegin(arch) ?? throw new InvalidOperationException("Lifecycle busy already held by another process");

    /// <summary>
    /// Try-захват: возвращает null если busy уже захвачен (ShellExt или другой процесс).
    /// Вызывающий код продолжает операцию без busy guard — UI обновляется из snapshot.
    /// </summary>
    public static IDisposable? TryBegin(GlassPlatformArch arch)
    {
        var handle = CreateEventW(IntPtr.Zero, true, false, EventName(arch));
        if (handle == IntPtr.Zero)
            return null;

        if (WaitForSingleObject(handle, 0) == 0)
        {
            // Уже signaled (busy) — другой процесс держит
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
