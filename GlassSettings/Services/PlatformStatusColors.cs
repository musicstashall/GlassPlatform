using GlassSettings.Core.Platform;
using Windows.UI;

namespace GlassSettings.Services;

public static class PlatformStatusColors
{
    public static Color OpenGlass(OpenGlassHostUiState state) => state switch
    {
        OpenGlassHostUiState.Running => Color.FromArgb(255, 0, 180, 70),
        OpenGlassHostUiState.Ready => Color.FromArgb(255, 230, 190, 0),
        _ => Color.FromArgb(255, 140, 140, 140)
    };

    public static Color GlassPlatform(GlassAutorunUiState state) => state switch
    {
        GlassAutorunUiState.Running => Color.FromArgb(255, 0, 180, 70),
        GlassAutorunUiState.Ready => Color.FromArgb(255, 230, 190, 0),
        _ => Color.FromArgb(255, 140, 140, 140)
    };

    /// <summary>Stopping/Residual — красная точка на Главной / тон на Системе.</summary>
    public static Color GlassPlatformUnloading { get; } = Color.FromArgb(255, 196, 43, 28);
}
