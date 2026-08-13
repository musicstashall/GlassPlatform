using System.Diagnostics;

namespace GlassSettings.Services;

/// <summary>
/// Env-gated трасса notice/mirror/coordinator/session (Debug Output).
/// $env:NoticeMirrorTrace=1 | NoticeCoordTrace=1 | NoticeSessionTrace=1
/// </summary>
internal static class NoticeMirrorTrace
{
    private static int _initialized;
    private static bool _mirrorEnabled;
    private static bool _coordEnabled;
    private static bool _sessionEnabled;

    public static void WriteMirror(string message) => WriteChannel("NoticeMirror", _mirrorEnabled, message);

    public static void WriteCoord(string message) => WriteChannel("NoticeCoord", _coordEnabled, message);

    public static void WriteSession(string message) => WriteChannel("NoticeSession", _sessionEnabled, message);

    /// <summary>Alias для существующих вызовов mirror-трассы.</summary>
    public static void Write(string message) => WriteMirror(message);

    public static void WriteHome(string message) => WriteMirror($"Home: {message}");

    private static void WriteChannel(string prefix, bool enabled, string message)
    {
#if DEBUG
        EnsureInitialized();
        if (!enabled)
            return;

        Debug.WriteLine($"[{prefix}] {message}");
#endif
    }

    private static void EnsureInitialized()
    {
        if (Interlocked.Exchange(ref _initialized, 1) == 1)
            return;

        _mirrorEnabled = IsEnvEnabled("NoticeMirrorTrace");
        _coordEnabled = IsEnvEnabled("NoticeCoordTrace");
        _sessionEnabled = IsEnvEnabled("NoticeSessionTrace");
    }

    private static bool IsEnvEnabled(string name)
    {
        var value = Environment.GetEnvironmentVariable(name);
        return string.Equals(value, "1", StringComparison.Ordinal)
               || string.Equals(value, "true", StringComparison.OrdinalIgnoreCase);
    }
}
