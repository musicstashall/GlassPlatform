namespace GlassSettings.Core.Platform;

/// <summary>Live-обновление AttachList в инжектированных процессах через GlassCompat.</summary>
public static class AttachListLiveReload
{
    public static void NotifyAttachListChanged() =>
        ReloadRenderService.SignalReloadRender(ReloadRenderFlags.AttachList);
}
