using GlassSettings.Core.Registry;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.Win32;
using WinRT.Interop;

namespace GlassSettings.Services;

/// <summary>
/// Обои: RegNotify Desktop Slideshow + TranscodedImageCache + Explorer\\Wallpapers.
/// Тема: RegNotify CurrentTheme + Personalize (Apps/System/Transparency) + WM_THEMECHANGED;
///   dark↔light / transparency без смены пути .theme тоже даёт ThemeChanged.
/// Accent: WM_DWMCOLORIZATIONCOLORCHANGED + Desktop\AutoColorization + DWM AccentColor fingerprint.
/// Шрифты: WM_SETTINGCHANGE (без чтения lParam — PostMessage с мусором → AV на x86);
///   после SPI_SETNONCLIENTMETRICS с SpifSendChange → FontsChanged.
/// Порядок с host: GS только reader; baseline после settle fingerprint (ThemeDwmBaselineSync).
/// </summary>
public sealed class DesktopEnvironmentWatcher : IDisposable
{
    private const uint WM_SETTINGCHANGE = 0x001A;
    private const uint WM_THEMECHANGED = 0x031A;
    private const uint WM_DWMCOLORIZATIONCOLORCHANGED = 0x0320;
    private static readonly UIntPtr SubclassId = new(0x4753_4553); // GSES
    private const string DesktopRegistryPath = @"Control Panel\Desktop";
    private const string DesktopSlideshowRegistryPath = @"Control Panel\Personalization\Desktop Slideshow";
    private const string WallpaperHistoryRegistryPath =
        @"Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers";
    private const string ThemesRegistryPath = @"Software\Microsoft\Windows\CurrentVersion\Themes";
    private const string PersonalizeRegistryPath = @"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize";
    private const string TranscodedImageCacheValueName = "TranscodedImageCache_000";
    private const string TranscodedImageCacheFallbackValueName = "TranscodedImageCache";
    private const string LastTickLowValueName = "LastTickLow";
    private const string LastTickHighValueName = "LastTickHigh";
    private const string CurrentThemeValueName = "CurrentTheme";
    private const string AppsUseLightThemeValueName = "AppsUseLightTheme";
    private const string SystemUsesLightThemeValueName = "SystemUsesLightTheme";
    private const string EnableTransparencyValueName = "EnableTransparency";
    private const string ColorPrevalenceValueName = "ColorPrevalence";
    private const string AutoColorizationValueName = "AutoColorization";

    private readonly DispatcherQueue _dispatcherQueue;
    private readonly Win32WindowSubclass.Proc _subclassProc;
    private IntPtr _subclassFn;
    private readonly RegistryValueChangeWatcher _wallpaperSlideshowWatcher;
    private readonly RegistryValueChangeWatcher _wallpaperDesktopWatcher;
    private readonly RegistryValueChangeWatcher _wallpaperHistoryWatcher;
    private readonly RegistryValueChangeWatcher _autoColorizationWatcher;
    private readonly RegistryValueChangeWatcher _themeRegistryWatcher;
    private readonly RegistryValueChangeWatcher _personalizeRegistryWatcher;
    private readonly DispatcherQueueTimer _debounceTimer;

    private IntPtr _hwnd;
    private bool _disposed;
    private bool _debounceTickAttached;
    private bool _pendingWallpaper;
    private bool _pendingTheme;
    private bool _pendingAccent;
    private bool _pendingFonts;
    /// <summary>WM_THEMECHANGED — форс ThemeChanged даже если CurrentTheme/mode ещё не обновились.</summary>
    private bool _forceThemeChanged;
    private string? _lastWallpaperSignal;
    private string? _lastThemeFilePath;
    private string? _lastThemeModeSignal;
    private string? _lastAccentSignal;

    public static DesktopEnvironmentWatcher? Current { get; private set; }

    public event EventHandler<DesktopEnvironmentChangedEventArgs>? Changed;

    public DesktopEnvironmentWatcher(DispatcherQueue dispatcherQueue)
    {
        _dispatcherQueue = dispatcherQueue;
        _subclassProc = OnSubclassMessage;
        _debounceTimer = dispatcherQueue.CreateTimer();
        _debounceTimer.Interval = TimeSpan.FromMilliseconds(110);
        _debounceTimer.IsRepeating = false;

        _wallpaperSlideshowWatcher = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            DesktopSlideshowRegistryPath,
            OnWallpaperRegistrySignal,
            LastTickLowValueName,
            LastTickHighValueName);

        _wallpaperDesktopWatcher = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            DesktopRegistryPath,
            OnWallpaperRegistrySignal,
            TranscodedImageCacheValueName,
            TranscodedImageCacheFallbackValueName,
            "WallpaperStyle",
            "TileWallpaper");

        // Любое изменение BackgroundType / BackgroundHistoryPath* (picture ↔ slideshow вне GS).
        _wallpaperHistoryWatcher = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            WallpaperHistoryRegistryPath,
            OnWallpaperRegistrySignal);

        _autoColorizationWatcher = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            DesktopRegistryPath,
            OnAccentSignal,
            AutoColorizationValueName);

        _themeRegistryWatcher = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            ThemesRegistryPath,
            OnThemeRegistrySignal,
            CurrentThemeValueName);

        _personalizeRegistryWatcher = new RegistryValueChangeWatcher(
            dispatcherQueue,
            RegistryHive.CurrentUser,
            PersonalizeRegistryPath,
            OnThemeRegistrySignal,
            AppsUseLightThemeValueName,
            SystemUsesLightThemeValueName,
            EnableTransparencyValueName);
    }

    public void Attach(Window window)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);

        StartRegistryWatchers();
        DetachSubclass();

        _hwnd = WindowNative.GetWindowHandle(window);
        if (_hwnd != IntPtr.Zero)
            Win32WindowSubclass.Install(_hwnd, _subclassProc, SubclassId, out _subclassFn);

        _lastWallpaperSignal = CurrentWallpaperResolver.TryGetWallpaperRegistrySignal();
        _lastThemeFilePath = TryReadThemeRegistrySignal();
        _lastThemeModeSignal = TryReadThemeModeSignal();
        _lastAccentSignal = TryReadAccentRegistrySignal();
        Current = this;
    }

    public void Dispose()
    {
        if (_disposed)
            return;

        _disposed = true;
        StopRegistryWatchers();
        _wallpaperSlideshowWatcher.Dispose();
        _wallpaperDesktopWatcher.Dispose();
        _wallpaperHistoryWatcher.Dispose();
        _autoColorizationWatcher.Dispose();
        _themeRegistryWatcher.Dispose();
        _personalizeRegistryWatcher.Dispose();
        DetachSubclass();
        _debounceTimer.Stop();
        if (_debounceTickAttached)
            _debounceTimer.Tick -= OnDebounceTick;

        if (ReferenceEquals(Current, this))
            Current = null;
    }

    private void StartRegistryWatchers()
    {
        _wallpaperSlideshowWatcher.Start();
        _wallpaperDesktopWatcher.Start();
        _wallpaperHistoryWatcher.Start();
        _autoColorizationWatcher.Start();
        _themeRegistryWatcher.Start();
        _personalizeRegistryWatcher.Start();
    }

    private void StopRegistryWatchers()
    {
        _wallpaperSlideshowWatcher.Stop();
        _wallpaperDesktopWatcher.Stop();
        _wallpaperHistoryWatcher.Stop();
        _autoColorizationWatcher.Stop();
        _themeRegistryWatcher.Stop();
        _personalizeRegistryWatcher.Stop();
    }

    private void DetachSubclass()
    {
        if (_hwnd == IntPtr.Zero)
            return;

        Win32WindowSubclass.Remove(_hwnd, _subclassFn, SubclassId);
        _subclassFn = IntPtr.Zero;
        _hwnd = IntPtr.Zero;

        if (ReferenceEquals(Current, this))
            Current = null;
    }

    private IntPtr OnSubclassMessage(
        IntPtr hWnd,
        uint uMsg,
        IntPtr wParam,
        IntPtr lParam,
        UIntPtr uIdSubclass,
        UIntPtr dwRefData)
    {
        if (uMsg == WM_THEMECHANGED)
        {
            // Паритет GlassCompat host: не ждать обновления CurrentTheme / COLOR_WINDOW.
            _forceThemeChanged = true;
            OnThemeRegistrySignal();
        }
        else if (uMsg == WM_DWMCOLORIZATIONCOLORCHANGED)
        {
            OnAccentSignal();
        }
        else if (uMsg == WM_SETTINGCHANGE)
        {
            // Не читаем lParam через PtrToStringUni: PostMessage с мусорным указателем → AV на x86.
            // ImmersiveColorSet / accent покрывает WM_DWMCOLORIZATIONCOLORCHANGED.
            // Шрифты: SPI_SETNONCLIENTMETRICS + SpifSendChange → перерисовка MessageFont-превью.
            OnFontsSignal();
        }

        return Win32WindowSubclass.CallDef(hWnd, uMsg, wParam, lParam);
    }

    private void OnWallpaperRegistrySignal() => SchedulePending(wallpaper: true);

    private void OnThemeRegistrySignal() => SchedulePending(theme: true);

    private void OnAccentSignal() => SchedulePending(accent: true);

    private void OnFontsSignal() => SchedulePending(fonts: true);

    private void SchedulePending(
        bool wallpaper = false,
        bool theme = false,
        bool accent = false,
        bool fonts = false)
    {
        if (_disposed)
            return;

        if (wallpaper)
            _pendingWallpaper = true;
        if (theme)
            _pendingTheme = true;
        if (accent)
            _pendingAccent = true;
        if (fonts)
            _pendingFonts = true;

        if (!_debounceTickAttached)
        {
            _debounceTimer.Tick += OnDebounceTick;
            _debounceTickAttached = true;
        }

        _debounceTimer.Stop();
        _debounceTimer.Start();
    }

    private void OnDebounceTick(DispatcherQueueTimer sender, object args)
    {
        if (_disposed)
            return;

        var wallpaperChanged = false;
        var themeChanged = false;
        var accentChanged = false;
        var fontsChanged = false;

        if (_pendingWallpaper)
        {
            _pendingWallpaper = false;
            var signal = CurrentWallpaperResolver.TryGetWallpaperRegistrySignal();
            if (!string.Equals(_lastWallpaperSignal, signal, StringComparison.OrdinalIgnoreCase))
            {
                _lastWallpaperSignal = signal;
                wallpaperChanged = true;
            }
        }

        if (_pendingTheme)
        {
            _pendingTheme = false;
            var themePath = TryReadThemeRegistrySignal();
            var themeMode = TryReadThemeModeSignal();
            var force = _forceThemeChanged;
            _forceThemeChanged = false;
            if (force
                || !string.Equals(_lastThemeFilePath, themePath, StringComparison.OrdinalIgnoreCase)
                || !string.Equals(_lastThemeModeSignal, themeMode, StringComparison.Ordinal))
            {
                _lastThemeFilePath = themePath;
                _lastThemeModeSignal = themeMode;
                themeChanged = true;
            }
        }

        if (_pendingAccent)
        {
            _pendingAccent = false;
            var signal = TryReadAccentRegistrySignal();
            if (!string.Equals(_lastAccentSignal, signal, StringComparison.Ordinal))
            {
                _lastAccentSignal = signal;
                accentChanged = true;
            }
        }

        if (_pendingFonts)
        {
            _pendingFonts = false;
            // Без fingerprint: SPI_GET уже видит новый lfMessageFont к моменту debounce.
            fontsChanged = true;
        }

        if (!wallpaperChanged && !themeChanged && !accentChanged && !fontsChanged)
            return;

        Changed?.Invoke(this, new DesktopEnvironmentChangedEventArgs
        {
            WallpaperChanged = wallpaperChanged,
            ThemeChanged = themeChanged,
            AccentChanged = accentChanged,
            FontsChanged = fontsChanged
        });
    }

    private static string? TryReadThemeRegistrySignal()
    {
        if (!VisualStyleThemeManager.TryGetCurrentThemeRegistryFilePath(out var path)
            || string.IsNullOrWhiteSpace(path))
        {
            return null;
        }

        return path;
    }

    /// <summary>
    /// Сигнал personalize для ThemeChanged без смены пути CurrentTheme:
    /// COLOR_WINDOW + Apps/System light + EnableTransparency.
    /// </summary>
    private static string? TryReadThemeModeSignal()
    {
        var colorWindowDark = ThemeRegistry.IsDarkMode() ? "1" : "0";
        var appsLight = TryReadPersonalizeDword(AppsUseLightThemeValueName);
        var systemLight = TryReadPersonalizeDword(SystemUsesLightThemeValueName);
        var transparency = TryReadPersonalizeDword(EnableTransparencyValueName);
        var startTaskbarPrevalence = TryReadPersonalizeDword(ColorPrevalenceValueName);
        return $"{colorWindowDark}|{appsLight}|{systemLight}|{transparency}|{startTaskbarPrevalence}";
    }

    private static string TryReadPersonalizeDword(string valueName)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(PersonalizeRegistryPath);
            var value = key?.GetValue(valueName);
            return value switch
            {
                int i => i.ToString(),
                long l => l.ToString(),
                _ => "-"
            };
        }
        catch
        {
            return "-";
        }
    }

    /// <summary>Сигнал accent для dedup AccentChanged (DWM colors + AutoColorization).</summary>
    private static string? TryReadAccentRegistrySignal()
    {
        static string Fmt(uint? v) => v.HasValue ? v.Value.ToString("X8") : "-";

        return string.Join('|',
            Fmt(DwmRegistry.ReadDword(DwmRegistryHive.Hkcu, "ColorizationColor")),
            Fmt(DwmRegistry.ReadDword(DwmRegistryHive.Hkcu, "ColorizationColorOverride")),
            Fmt(DwmRegistry.ReadDword(DwmRegistryHive.Hkcu, "ColorizationAfterglow")),
            Fmt(DwmRegistry.ReadDword(DwmRegistryHive.Hkcu, "ColorizationAfterglowOverride")),
            Fmt(DwmRegistry.ReadDword(DwmRegistryHive.Hkcu, "AccentColor")),
            Fmt(DwmRegistry.ReadDword(DwmRegistryHive.Hkcu, "ColorPrevalence")),
            Fmt(TryReadAutoColorization()));
    }

    private static uint? TryReadAutoColorization()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(DesktopRegistryPath);
            var value = key?.GetValue(AutoColorizationValueName);
            return value switch
            {
                int i => unchecked((uint)i),
                long l => unchecked((uint)l),
                _ => null
            };
        }
        catch
        {
            return null;
        }
    }
}
