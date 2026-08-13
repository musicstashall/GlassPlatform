using GlassSettings.Core.Customization;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Core.Text;
using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>Live-reload GlobalTextGlow → GlassCore + инжектированные процессы + перерисовка контролов.</summary>
public static class TextGlowLiveReload
{
    public static void WriteAndReload(GlobalTextGlowModel model, bool darkMode)
    {
        GlobalTextGlowRegistry.WriteForLivePreview(darkMode, model);
        ReloadEngineAndControls();
    }

    public static void WriteAndReload(GlobalTextGlowModel model) =>
        WriteAndReload(model, ThemeRegistry.IsDarkMode());

    public static void Commit(GlobalTextGlowModel model, bool darkMode)
    {
        GlobalTextGlowRegistry.WriteOrRemoveForTheme(darkMode, model);
        ReloadEngineAndControls();
        SettingsSession.Current.MarkDirty();
    }

    public static void Commit(GlobalTextGlowModel model) =>
        Commit(model, ThemeRegistry.IsDarkMode());

    public static void NotifyChanged()
    {
        SettingsSession.Current.MarkDirty();
        ReloadEngineAndControls();
    }

    private static void ReloadEngineAndControls()
    {
        NativeHostInterop.ReloadRenderTextInkFromRegistryIfEngineLoaded();
        RootWindowsLiveReload.PostLiveRootComposition(compositionOnly: true, repaintOnly: true);
        ReloadRenderService.SignalReloadRender(ReloadRenderFlags.RenderText);
    }
}
