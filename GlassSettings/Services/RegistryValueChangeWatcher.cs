using System.Runtime.InteropServices;
using Microsoft.UI.Dispatching;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>
/// RegNotifyChangeKeyValue на HKCU-подключ: оповещение при изменении значений ключа.
/// Опционально — фильтр по одному или нескольким именам значений.
/// </summary>
internal sealed class RegistryValueChangeWatcher : IDisposable
{
    private const int HkeyCurrentUser = unchecked((int)0x80000001);
    private const int HkeyLocalMachine = unchecked((int)0x80000002);
    private const int KeyNotify = 0x0010;
    private const int KeyRead = 0x20019;
    private const int RegNotifyChangeLastSet = 0x00000004;

    private readonly DispatcherQueue _dispatcherQueue;
    private readonly RegistryHive _hive;
    private readonly string _subKey;
    private readonly Action _onChanged;
    private readonly string[]? _watchedValueNames;
    private readonly object _sync = new();

    private IntPtr _hKey;
    private EventWaitHandle? _event;
    private RegisteredWaitHandle? _waitHandle;
    private bool _disposed;
    private Dictionary<string, string?>? _lastWatchedValues;

    public RegistryValueChangeWatcher(
        DispatcherQueue dispatcherQueue,
        RegistryHive hive,
        string subKey,
        Action onChanged,
        string? watchedValueName = null,
        params string[] additionalWatchedValueNames)
    {
        _dispatcherQueue = dispatcherQueue;
        _hive = hive;
        _subKey = subKey;
        _onChanged = onChanged;

        if (watchedValueName is not null || additionalWatchedValueNames.Length > 0)
        {
            var names = new List<string>();
            if (!string.IsNullOrWhiteSpace(watchedValueName))
                names.Add(watchedValueName);
            foreach (var name in additionalWatchedValueNames)
            {
                if (!string.IsNullOrWhiteSpace(name))
                    names.Add(name);
            }

            _watchedValueNames = names.Count > 0 ? names.ToArray() : null;
        }
    }

    public void Start()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        Stop();

        if (RegOpenKeyEx(
                ResolveHiveHandle(_hive),
                _subKey,
                0,
                KeyNotify | KeyRead,
                out _hKey) != 0)
        {
            _hKey = IntPtr.Zero;
            return;
        }

        if (_watchedValueNames is not null)
        {
            _lastWatchedValues = new Dictionary<string, string?>(_watchedValueNames.Length);
            foreach (var name in _watchedValueNames)
                _lastWatchedValues[name] = ReadRegistryValue(_hive, _subKey, name);
        }

        _event = new EventWaitHandle(false, EventResetMode.AutoReset);
        var hEvent = _event.SafeWaitHandle.DangerousGetHandle();

        if (!TryArmNotification(hEvent))
        {
            Stop();
            return;
        }

        _waitHandle = ThreadPool.RegisterWaitForSingleObject(
            _event,
            OnEventSignaled,
            null,
            Timeout.Infinite,
            false);
    }

    public void Stop()
    {
        if (_waitHandle is not null)
        {
            _waitHandle.Unregister(null);
            _waitHandle = null;
        }

        if (_event is not null)
        {
            _event.Dispose();
            _event = null;
        }

        CloseRegistryKey();
        _lastWatchedValues = null;
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        Stop();
    }

    private void OnEventSignaled(object? state, bool timedOut)
    {
        if (_disposed)
            return;

        lock (_sync)
        {
            if (_disposed || _hKey == IntPtr.Zero || _event is null)
                return;

            TryArmNotification(_event.SafeWaitHandle.DangerousGetHandle());
        }

        _dispatcherQueue.TryEnqueue(ProcessRegistrySignal);
    }

    private void ProcessRegistrySignal()
    {
        if (_disposed)
            return;

        if (_watchedValueNames is not null && _lastWatchedValues is not null)
        {
            var anyChanged = false;
            foreach (var name in _watchedValueNames)
            {
                var current = ReadRegistryValue(_hive, _subKey, name);
                if (!string.Equals(_lastWatchedValues[name], current, StringComparison.Ordinal))
                    anyChanged = true;
                _lastWatchedValues[name] = current;
            }

            if (!anyChanged)
                return;
        }

        _onChanged();
    }

    private bool TryArmNotification(IntPtr hEvent)
    {
        if (_hKey == IntPtr.Zero || hEvent == IntPtr.Zero)
            return false;

        return RegNotifyChangeKeyValue(
            _hKey,
            bWatchSubtree: false,
            RegNotifyChangeLastSet,
            hEvent,
            fAsynchronous: true) == 0;
    }

    private void CloseRegistryKey()
    {
        if (_hKey == IntPtr.Zero)
            return;

        RegCloseKey(_hKey);
        _hKey = IntPtr.Zero;
    }

    internal static string? ReadRegistryValue(string subKeyPath, string valueName) =>
        ReadRegistryValue(RegistryHive.CurrentUser, subKeyPath, valueName);

    internal static string? ReadRegistryValue(RegistryHive hive, string subKeyPath, string valueName)
    {
        try
        {
            using var key = OpenHive(hive).OpenSubKey(subKeyPath);
            var raw = key?.GetValue(valueName);
            return NormalizeRegistryValue(raw);
        }
        catch
        {
            return null;
        }
    }

    private static RegistryKey OpenHive(RegistryHive hive) =>
        hive == RegistryHive.CurrentUser ? Registry.CurrentUser : Registry.LocalMachine;

    private static IntPtr ResolveHiveHandle(RegistryHive hive) =>
        hive == RegistryHive.CurrentUser
            ? (IntPtr)HkeyCurrentUser
            : (IntPtr)HkeyLocalMachine;

    internal static string? NormalizeRegistryValue(object? raw)
    {
        if (raw is null)
            return null;

        switch (raw)
        {
            case string s:
                return string.IsNullOrWhiteSpace(s) ? null : s.Trim();
            case int i:
                return i.ToString();
            case uint u:
                return u.ToString();
            case byte[] bytes:
                return bytes.Length > 0
                    ? Convert.ToHexString(bytes, 0, Math.Min(32, bytes.Length))
                    : null;
            default:
                var text = raw.ToString();
                return string.IsNullOrWhiteSpace(text) ? null : text.Trim();
        }
    }

    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, EntryPoint = "RegOpenKeyExW", ExactSpelling = true)]
    private static extern int RegOpenKeyEx(
        IntPtr hKey,
        string lpSubKey,
        int ulOptions,
        int samDesired,
        out IntPtr phkResult);

    [DllImport("advapi32.dll", ExactSpelling = true)]
    private static extern int RegNotifyChangeKeyValue(
        IntPtr hKey,
        bool bWatchSubtree,
        int dwNotifyFilter,
        IntPtr hEvent,
        bool fAsynchronous);

    [DllImport("advapi32.dll", ExactSpelling = true)]
    private static extern int RegCloseKey(IntPtr hKey);
}
