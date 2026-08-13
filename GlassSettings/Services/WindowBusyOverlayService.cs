using GlassSettings.Core.Registry;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>Штора WindowBusyOverlay: acrylic chrome (плитки тем) или простое затенение.</summary>
public static class WindowBusyOverlayService
{
    private const float AcrylicChromeAlphaScale = 0.48f;
    private const double DimOpacity = 0.58;

    public static void ApplyFill(Shape fill, FrameworkElement themeRoot)
    {
        _ = themeRoot;
        fill.Opacity = 1.0;

        if (GlassSettingsRegistry.ReadWindowBusyOverlayMode() == WindowBusyOverlayMode.Dim)
        {
            fill.Fill = GetDimBrush();
            fill.Opacity = DimOpacity;
            return;
        }

        ApplyAcrylicChromeFill(fill);
    }

    private static void ApplyAcrylicChromeFill(Shape fill)
    {
        var isDark = AppThemeVisualResolver.ResolveIsDarkMode();
        var colors = ThemePreviewChromePalette.Create(isDark, accentColor: null);
        var background = ScaleAlpha(colors.AcrylicBackground, AcrylicChromeAlphaScale);
        fill.Fill = ThemePreviewChromeBackground.CreateBrush(background);
    }

    private static Brush GetDimBrush() =>
        AppThemeVisualResolver.ResolveThemeResourceBrush("AcrylicInAppFillColorDefaultBrush")
        ?? WindowBackdropService.GetAcrylicInAppChromeBrush();

    private static Color ScaleAlpha(Color color, float scale)
    {
        var alpha = (byte)Math.Clamp((int)MathF.Round(color.A * scale), 0, 255);
        return Color.FromArgb(alpha, color.R, color.G, color.B);
    }

    public static event EventHandler? OverlayBecameIdle;

    public static bool IsActive => App.ShellWindow?.IsWindowBusyOverlayActive ?? false;

    internal static void RaiseOverlayBecameIdle() => OverlayBecameIdle?.Invoke(null, EventArgs.Empty);

    /// <summary>Показать штору. Для hosted-диалогов — <c>showProgress: false</c>.</summary>
    public static void Push(bool showProgress = true) =>
        App.ShellWindow?.PushWindowBusyOverlay(showProgress);

    /// <summary>Скрыть штору (с учётом вложенности).</summary>
    public static void Pop() =>
        App.ShellWindow?.PopWindowBusyOverlay();

    /// <summary>Сброс «залипшей» шторы при смене страницы (например, после прерванного сценария).</summary>
    public static void ForceResetIfActive()
    {
        while (IsActive)
            Pop();
    }

    public static void SetProgressVisible(bool visible) =>
        App.ShellWindow?.SetWindowBusyProgressVisible(visible);

    /// <summary>Штора на время async-операции.</summary>
    public static async Task RunAsync(Func<Task> action, bool showProgress = false)
    {
        Push(showProgress);
        try
        {
            await action().ConfigureAwait(true);
        }
        finally
        {
            Pop();
        }
    }

    /// <summary>Штора на время async-операции с результатом.</summary>
    public static async Task<T?> RunAsync<T>(Func<Task<T?>> action, bool showProgress = false)
    {
        Push(showProgress);
        try
        {
            return await action().ConfigureAwait(true);
        }
        finally
        {
            Pop();
        }
    }

    /// <summary>Штора на время синхронной операции (системный picker и т.п.).</summary>
    public static T? Run<T>(Func<T?> action, bool showProgress = true)
    {
        Push(showProgress);
        try
        {
            return action();
        }
        finally
        {
            Pop();
        }
    }
}
