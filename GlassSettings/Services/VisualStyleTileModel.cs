namespace GlassSettings.Services;

/// <summary>View-model плитки темы на странице «Визуальные стили».</summary>
public sealed class VisualStyleTileModel
{
    public required VisualStyleThemeEntry Entry { get; init; }

    public bool IsActive { get; init; }

    public string? PreviewImagePath => Entry.PreviewImagePath;

    public string TooltipText => Entry.TooltipText;
}
