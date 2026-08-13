namespace GlassSettings.Services;

/// <summary>Запущенный процесс для выбора в AttachList.</summary>
public sealed record RunningProcessInfo(
    string ExeName,
    string? FilePath,
    string DisplayName,
    string? FileDescription,
    string? CompanyName,
    string DetailLine);
