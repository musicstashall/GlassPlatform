namespace GlassSettings.Services;

/// <summary>
/// Fade-in контента главной после снятия заставки завершён — можно анимировать стартовое notice
/// (штатный slide-in, не в первом кадре fade).
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
