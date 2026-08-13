namespace GlassSettings.Services;

/// <summary>Детализация изменений рабочего стола (обои / тема / акцент / системные шрифты).</summary>
public sealed class DesktopEnvironmentChangedEventArgs : EventArgs
{
    public bool WallpaperChanged { get; init; }

    public bool ThemeChanged { get; init; }

    public bool AccentChanged { get; init; }

    /// <summary>WM_SETTINGCHANGE после SPI (в т.ч. SPI_SETNONCLIENTMETRICS с SpifSendChange).</summary>
    public bool FontsChanged { get; init; }
}
