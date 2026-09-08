namespace GlassSettings.Services;

/// <summary>Hosted-диалоги GlassSettings: штора + панель, крестик, клик по шторе, проявление 250 мс.</summary>
public static class HostedDialogService
{
    public static Task<HostedDialogResult> ShowAsync(
        HostedDialogSpec spec,
        Action<HostedDialogHostContext>? configure = null)
    {
        if (App.ShellWindow is null)
            return Task.FromResult(HostedDialogResult.Dismissed);

        return App.ShellWindow.ShowHostedDialogAsync(spec, configure);
    }

    /// <summary>
    /// Диалог с Primary: панель скрывается сразу, штора с progress на время procedure, затем Pop.
    /// После возврата true вызывающий код показывает notice на странице.
    /// </summary>
    public static Task<bool> ConfirmThenAsync(HostedDialogSpec spec, Func<Task> procedure)
    {
        if (App.ShellWindow is null)
            return Task.FromResult(false);

        return App.ShellWindow.ShowHostedDialogThenRunAsync(spec, procedure);
    }

    /// <summary>Принудительное обновление clip hosted-диалога после изменения контента.</summary>
    public static void RefreshDialogClip()
    {
        if (App.ShellWindow is null)
            return;

        // Откладываем на кадр — контент должен пройти layout перед обновлением clip.
        App.ShellWindow.DispatcherQueue.TryEnqueue(() =>
            App.ShellWindow.UpdateHostedDialogSurfaceClip());
    }
}
