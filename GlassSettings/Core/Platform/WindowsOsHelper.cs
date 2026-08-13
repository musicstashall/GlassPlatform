namespace GlassSettings.Core.Platform;

/// <summary>Версия ОС для UI-политик (порог Win11 = build 22000, как в GlassCore).</summary>
public static class WindowsOsHelper
{
    private const int Windows11Build = 22000;

    public static bool IsWindows11OrGreater() =>
        Environment.OSVersion.Version.Build >= Windows11Build;
}
