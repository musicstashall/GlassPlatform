using GlassSettings.Services;

namespace GlassSettings.Core.Platform;

/// <summary>Live-обновление Root\NoBackdrop. См. GlassSettings/docs/GlassSettings-host-glass.md.</summary>
public static class RootBackdropLiveReload
{
    /// <param name="noBackdrop">0 = backdrop разрешён; 1 = подавлен (NONE).</param>
    public static void NotifyNoBackdropChange(uint noBackdrop)
    {
        // PublishRootBackdropMode (в NotifyRootNoBackdropChanged) уже шлёт SignalReloadRender при смене mode.
        // NotifyDwmColorizationChanged здесь давал второй sweep (~2.5 с) и мерцание шапки проводника.
        NativeHostInterop.NotifyRootNoBackdropChanged(noBackdrop);
        NativeHostInterop.ResetReapplyFingerprintIfEngineLoaded();
    }
}
