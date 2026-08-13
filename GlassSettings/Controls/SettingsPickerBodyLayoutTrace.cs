using System.Diagnostics;
using System.Globalization;

namespace GlassSettings.Controls;

/// <summary>
/// Env-gated трасса picker body layout (Debug Output).
/// $env:PickerBodyLayoutTrace=1 | PickerBodyLayoutTrace=true
/// </summary>
internal static class SettingsPickerBodyLayoutTrace
{
    private static int _initialized;
    private static bool _enabled;
    private static readonly Dictionary<string, string> LastMessages = new();

    public static string Format(double value) =>
        value.ToString("F1", CultureInfo.InvariantCulture);

    public static void WriteIfChanged(string key, string tag, string message)
    {
#if DEBUG
        EnsureInitialized();
        if (!_enabled)
            return;

        if (LastMessages.TryGetValue(key, out var prev) && prev == message)
            return;

        LastMessages[key] = message;
        Debug.WriteLine($"[PickerBodyLayout] {tag} {message}");
#endif
    }

    private static void EnsureInitialized()
    {
#if DEBUG
        if (Interlocked.Exchange(ref _initialized, 1) == 1)
            return;

        var value = Environment.GetEnvironmentVariable("PickerBodyLayoutTrace");
        _enabled = string.Equals(value, "1", StringComparison.Ordinal)
                   || string.Equals(value, "true", StringComparison.OrdinalIgnoreCase);
#endif
    }
}
