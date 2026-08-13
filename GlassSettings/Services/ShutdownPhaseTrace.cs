using System.Diagnostics;

namespace GlassSettings.Services;

/// <summary>Debug-замеры фаз закрытия GlassSettings (Output / Debug.WriteLine).</summary>
internal static class ShutdownPhaseTrace
{
    private static readonly Stopwatch Watch = new();
    private static long _lastMs;

    public static void Begin()
    {
#if DEBUG
        Watch.Restart();
        _lastMs = 0;
        Mark("begin");
#endif
    }

    public static void Mark(string phase)
    {
#if DEBUG
        var totalMs = Watch.ElapsedMilliseconds;
        var deltaMs = totalMs - _lastMs;
        _lastMs = totalMs;
        Debug.WriteLine(
            $"[GlassSettings][Shutdown] +{totalMs}ms (+{deltaMs}ms) {phase}");
#endif
    }
}
