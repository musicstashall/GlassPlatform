using GlassSettings.Core.Registry;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Win32;

namespace GlassSettings.Services;

/// <summary>RegNotify на HKCU\...\GlassSettings — только AppTheme/WindowBackdrop/overlay без лишних reapply.</summary>
public sealed class GlassSettingsPreferencesWatcher : IDisposable
{
    private const string GlassSettingsSubKey = @"Software\StanislavKa\BlackWindowManager\GlassSettings";

    private readonly RegistryValueChangeWatcher _watcher;
    private AppThemeMode _theme;
    private AppWindowBackdropMode _backdrop;
    private WindowBusyOverlayMode _busyOverlay;
    private bool _disposed;

    public GlassSettingsPreferencesWatcher(DispatcherQueue dispatcherQueue)
    {
        SnapshotPreferences();
        _watcher = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            GlassSettingsSubKey,
            OnRegistryChanged);
    }

    public void Attach(Window window)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        _ = window;
        _watcher.Start();
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        _watcher.Dispose();
    }

    private void OnRegistryChanged()
    {
        var theme = GlassSettingsRegistry.ReadAppTheme();
        var backdrop = GlassSettingsRegistry.ReadWindowBackdrop();
        var overlay = GlassSettingsRegistry.ReadWindowBusyOverlayMode();

        if (theme == _theme && backdrop == _backdrop && overlay == _busyOverlay)
            return;

        _theme = theme;
        _backdrop = backdrop;
        _busyOverlay = overlay;
        AppPreferencesCoordinator.ApplyFromRegistry();
    }

    private void SnapshotPreferences()
    {
        _theme = GlassSettingsRegistry.ReadAppTheme();
        _backdrop = GlassSettingsRegistry.ReadWindowBackdrop();
        _busyOverlay = GlassSettingsRegistry.ReadWindowBusyOverlayMode();
    }
}