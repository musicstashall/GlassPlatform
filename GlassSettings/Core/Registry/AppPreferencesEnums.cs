namespace GlassSettings.Core.Registry;

/// <summary>Тема интерфейса GlassSettings (реестр AppTheme).</summary>
public enum AppThemeMode : uint
{
    System = 0,
    Light = 1,
    Dark = 2
}

/// <summary>Backdrop окна GlassSettings (реестр WindowBackdrop).</summary>
public enum AppWindowBackdropMode : uint
{
    System = 0,
    Acrylic = 1,
    Mica = 2,
    MicaAlt = 3
}

/// <summary>Стиль шторы WindowBusyOverlay (реестр WindowBusyOverlayMode).</summary>
public enum WindowBusyOverlayMode : uint
{
    AcrylicChrome = 0,
    Dim = 1
}
