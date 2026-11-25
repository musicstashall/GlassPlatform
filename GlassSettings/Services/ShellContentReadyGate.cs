namespace GlassSettings.Services;

/// <summary>
/// Экран старта сменился готовым интерфейсом — можно показать стартовое уведомление.
/// </summary>
internal static class ShellContentReadyGate
{
    private static event EventHandler? Ready;

    public static bool IsReady { get; private set; }

    public static void MarkReady()
    {
        if (IsReady)
            return;

        IsReady = true;
        Ready?.Invoke(null, EventArgs.Empty);
        Ready = null;
    }

    public static void SubscribeOnce(EventHandler handler)
    {
        if (IsReady)
        {
            handler(null, EventArgs.Empty);
            return;
        }

        Ready += handler;
    }
}
