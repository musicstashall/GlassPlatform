using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Services;

/// <summary>Строка диалога выбора запущенного процесса.</summary>
public sealed class RunningProcessPickerItem
{
    public required string ExeName { get; init; }

    public required string DisplayName { get; init; }

    public string DetailLine { get; init; } = string.Empty;

    public ImageSource? Icon { get; init; }

    public static RunningProcessPickerItem FromInfo(RunningProcessInfo info, ImageSource? icon) =>
        new()
        {
            ExeName = info.ExeName,
            DisplayName = info.DisplayName,
            DetailLine = info.DetailLine,
            Icon = icon
        };
}
