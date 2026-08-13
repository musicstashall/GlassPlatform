using GlassSettings.Core.Registry;
using GlassSettings.Core.Text;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>Live-reload GlassInkAlphaCapsule → GlassCore + перерисовка текста.</summary>
public static class InkCapsuleLiveReload
{
    /// <summary>Только запись REG_BINARY (без reload). Для live-drag с отдельным reload — SyncDesktopFromRegistry.</summary>
    public static void WriteRegistryForLivePreview(GlassInkCapsuleModel model) =>
        GlassInkAlphaCapsuleRegistry.WriteForLivePreview(model);

    public static void WriteForLivePreview(GlassInkCapsuleModel model)
    {
        WriteRegistryForLivePreview(model);
        ReloadEngineAndControls();
        // LiveImmediate: MarkDirty только в onGestureCommitted (см. GlassSettings-slider-commit.md).
    }

    public static void ReloadEngineAndControls() => ReloadEngineAndControlsCore();

    /// <summary>Запись REG_BINARY + reload/invalidate injected Edit/RichEdit (hero — из RAM override).</summary>
    public static void SyncDesktopFromRegistry(GlassInkCapsuleModel model)
    {
        WriteRegistryForLivePreview(model);
        ReloadEngineAndControls();
    }

    public static void Commit(GlassInkCapsuleModel model)
    {
        GlassInkAlphaCapsuleRegistry.WriteOrRemove(model);
        ReloadEngineAndControls();
        SettingsSession.Current.MarkDirty();
    }

    public static void NotifyChanged()
    {
        SettingsSession.Current.MarkDirty();
        ReloadEngineAndControls();
    }

    private static void ReloadEngineAndControlsCore()
    {
        // Hero-превью: per-call ProfilePayload в ExtTextOutWithAlpha; reload нужен injected Edit/RichEdit.
        NativeHostInterop.ReloadRenderTextInkFromRegistryIfEngineLoaded();
        if (!ReloadRenderService.SignalReloadRender(ReloadRenderFlags.RenderText))
        {
            System.Diagnostics.Debug.WriteLine(
                "GlassSettings InkCapsuleLiveReload: SignalReloadRender(RenderText) failed — " +
                "GlassPlatform reload objects unavailable or ReleaseSemaphore failed; " +
                "injected processes will not reload ink until platform is running.");
        }
    }
}
