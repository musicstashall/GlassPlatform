using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>Фоновый prewarm blur pipeline «Композиции» (HKCU) + read-only cache для sync apply на странице.</summary>
internal static class CompositionWallpaperLayersCache
{
    private static readonly object Gate = new();

    private static DispatcherQueue? _dispatcherQueue;
    private static bool _policyListenerAttached;
    private static Task? _prewarmTask;
    private static Dictionary<CompositionWallpaperPipelineBuilder.BlurPipelineKey, CompositionWallpaperPipelineBuilder.PipelineSnapshot>? _pipelines;

    public static bool ShouldPrewarm() => OpenGlassUiPolicy.IsFeatureExposedInUi();

    public static Task? PrewarmTask
    {
        get
        {
            lock (Gate)
                return _prewarmTask;
        }
    }

    public static void AttachPolicyListener(DispatcherQueue dispatcherQueue)
    {
        if (_policyListenerAttached)
            return;

        _policyListenerAttached = true;
        _dispatcherQueue = dispatcherQueue;
        OpenGlassUiPolicy.Changed += OnOpenGlassUiPolicyChanged;
    }

    public static void BeginPrewarm(DispatcherQueue dispatcherQueue)
    {
        if (!ShouldPrewarm())
            return;

        _dispatcherQueue = dispatcherQueue;

        lock (Gate)
        {
            if (_prewarmTask is { IsCompleted: false })
                return;

            _prewarmTask = PrewarmCoreAsync(dispatcherQueue);
        }
    }

    public static void InvalidateAndPrewarmIfNeeded(DispatcherQueue dispatcherQueue)
    {
        Invalidate();
        BeginPrewarm(dispatcherQueue);
    }

    public static void Invalidate()
    {
        lock (Gate)
        {
            _pipelines = null;
            _prewarmTask = null;
        }
    }

    public static bool TryGetPipeline(
        CompositionWallpaperPipelineBuilder.BlurPipelineKey key,
        int width,
        int height,
        out CompositionWallpaperPipelineBuilder.PipelineSnapshot snapshot)
    {
        snapshot = default;

        if (width != CompositionWallpaperPipelineBuilder.BlurLayerWidth
            || height != CompositionWallpaperPipelineBuilder.BlurLayerHeight)
        {
            return false;
        }

        lock (Gate)
        {
            if (_pipelines is null)
                return false;

            return _pipelines.TryGetValue(key, out snapshot);
        }
    }

    private static void OnOpenGlassUiPolicyChanged(object? sender, EventArgs e)
    {
        var dq = _dispatcherQueue;
        if (dq is null)
            return;

        if (ShouldPrewarm())
            dq.TryEnqueue(() => InvalidateAndPrewarmIfNeeded(dq));
        else
            dq.TryEnqueue(Invalidate);
    }

    private sealed record PrewarmResult(
        Dictionary<CompositionWallpaperPipelineBuilder.BlurPipelineKey, CompositionWallpaperPipelineBuilder.PipelineSnapshot> Pipelines,
        byte[]? WallpaperBytes,
        string? Identity,
        string? Path);

    private static async Task PrewarmCoreAsync(DispatcherQueue dispatcherQueue)
    {
        try
        {
            if (!ShouldPrewarm())
                return;

            var result = await Task.Run(BuildPipelinesFromRegistry).ConfigureAwait(false);
            if (result?.Pipelines is null || result.Pipelines.Count == 0)
                return;

            lock (Gate)
            {
                if (!ShouldPrewarm())
                    return;

                _pipelines = result.Pipelines;
            }

            if (result.WallpaperBytes is { Length: > 0 }
                && !string.IsNullOrWhiteSpace(result.Identity)
                && !string.IsNullOrWhiteSpace(result.Path))
            {
                DesktopWallpaperImageCache.PublishDecodedWallpaper(
                    result.WallpaperBytes,
                    result.Identity,
                    result.Path,
                    dispatcherQueue);
            }
        }
        catch
        {
            // prewarm не должен ронять host
        }
    }

    private static PrewarmResult? BuildPipelinesFromRegistry()
    {
        if (!ShouldPrewarm())
            return null;

        var (vistaState, win7State) = CompositionPreviewRegistryStates.BuildFromRuntimeEffective();
        var wallpaperPath = CurrentWallpaperResolver.TryGetDesktopWallpaperPath();
        var wallpaperBytes = CompositionWallpaperPipelineBuilder.ReadWallpaperFileBytes(wallpaperPath);
        var identity = CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity();
        var width = CompositionWallpaperPipelineBuilder.BlurLayerWidth;
        var height = CompositionWallpaperPipelineBuilder.BlurLayerHeight;

        var vistaKey = CompositionWallpaperPipelineBuilder.BuildKey(vistaState, wallpaperPath);
        var win7Key = CompositionWallpaperPipelineBuilder.BuildKey(win7State, wallpaperPath);

        var pipelines = new Dictionary<CompositionWallpaperPipelineBuilder.BlurPipelineKey, CompositionWallpaperPipelineBuilder.PipelineSnapshot>
        {
            [vistaKey] = CompositionWallpaperPipelineBuilder.Build(wallpaperPath, width, height, vistaState),
            [win7Key] = CompositionWallpaperPipelineBuilder.Build(wallpaperPath, width, height, win7State),
        };

        return new PrewarmResult(pipelines, wallpaperBytes, identity, wallpaperPath);
    }
}
