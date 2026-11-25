using GlassSettings.Core.Registry;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>
/// Backdrop окна GlassSettings (один <see cref="GlassWindowBackdrop"/> на окно) и in-app acrylic
/// для overlay UI. Порядок старта: [GlassSettings-startup-window-plan.md](../docs/GlassSettings-startup-window-plan.md).
/// </summary>
public static class WindowBackdropService
{
    /// <summary>
    /// Присвоить backdrop окна — ровно один раз и до <c>Activate()</c>. WinUI подключает
    /// цель при готовке острова (SystemBackdrop.OnTargetConnected) и включает прозрачность
    /// острова, поэтому живой материал попадает в первый же кадр; повторное
    /// присваивание только рвет связь с целью (opaque-кадр) — его не делаем.
    /// </summary>
    public static void AttachStartupBackdrop(Window window, Grid rootGrid)
    {
        if (window.SystemBackdrop is not null)
            return;

        if (GlassWindowBackdrop.IsAnyMaterialSupported())
        {
            try
            {
                window.SystemBackdrop = new GlassWindowBackdrop(GlassSettingsRegistry.ReadWindowBackdrop());
                return;
            }
            catch { /* ниже — solid-подложка */ }
        }

        // Win10 17763 / отказ API: материала нет, остров остаётся непрозрачным. Фон корня
        // выставляется сразу (до первого кадра), чтобы не было «дырки» в desktop.
        ApplySolidRootBackground(rootGrid);
    }

    /// <summary>
    /// Смена режима backdrop (панель настроек приложения или внешний WM_SETTINGCHANGE):
    /// материал меняется внутри уже подключённого backdrop — без переприсваивания
    /// <c>Window.SystemBackdrop</c> и без мигания.
    /// </summary>
    public static void ApplyModeFromRegistry(Window window, Grid rootGrid)
    {
        if (window.SystemBackdrop is GlassWindowBackdrop backdrop)
        {
            backdrop.TryApplyMode(GlassSettingsRegistry.ReadWindowBackdrop());
            return;
        }

        AttachStartupBackdrop(window, rootGrid);
    }

    /// <summary>
    /// Синхронизация подложки после смены темы (RootGrid.ActualThemeChanged): пока материал
    /// активен — фона нет (сквозь него видно системный материал), иначе — solid текущей темы;
    /// заодно пересчитывается FallbackColor контроллера.
    /// </summary>
    public static void SyncRootBackground(Window window, Grid rootGrid)
    {
        if (window.SystemBackdrop is GlassWindowBackdrop backdrop)
        {
            backdrop.RefreshThemeColors();
            rootGrid.Background = null;
            return;
        }

        if (window.SystemBackdrop is not null)
            return;

        ApplySolidRootBackground(rootGrid);
    }

    private static void ApplySolidRootBackground(Grid rootGrid) =>
        rootGrid.Background = AppThemeVisualResolver.ResolveThemeResourceBrush("SolidBackgroundFillColorBaseBrush")
            ?? new SolidColorBrush(Color.FromArgb(255, 0x20, 0x20, 0x20));

    /// <summary>In-app acrylic для overlay UI (pane, диалоги).</summary>
    public static Brush GetAcrylicInAppChromeBrush() =>
        GetAcrylicInAppFillBrush(AppThemeVisualResolver.ResolveIsDarkMode());

    /// <summary>In-app acrylic с учётом AppTheme из реестра GlassSettings.</summary>
    public static void ApplyAcrylicInAppChromeBackground(Border border)
    {
        border.Background = AppThemeVisualResolver.ResolveThemeResourceBrush("AcrylicInAppFillColorDefaultBrush")
            ?? GetAcrylicInAppFillBrush(AppThemeVisualResolver.ResolveIsDarkMode());
    }

    /// <summary>Чуть плотнее in-app acrylic для зеркала notice поверх контента.</summary>
    public static void ApplyMirrorNoticeChromeBackground(Border border)
    {
        var isDark = AppThemeVisualResolver.ResolveIsDarkMode();
        var colors = ThemePreviewChromePalette.Create(isDark, accentColor: null);
        var background = ScaleAlpha(colors.AcrylicBackground, MirrorNoticeAcrylicAlphaScale);
        border.Background = ThemePreviewChromeBackground.CreateBrush(background);
    }

    private const float MirrorNoticeAcrylicAlphaScale = 0.78f;

    /// <summary>
    /// Фон overlay-pane NavigationView (LeftMinimal): in-app acrylic во всех режимах backdrop окна.
    /// WinUI по умолчанию использует AcrylicInAppFillColorDefaultBrush; прозрачный pane даёт «дырку».
    /// </summary>
    public static Brush GetNavigationOverlayPaneBrush() =>
        AppThemeVisualResolver.ResolveThemeResourceBrush("AcrylicInAppFillColorDefaultBrush")
        ?? GetAcrylicInAppFillBrush(AppThemeVisualResolver.ResolveIsDarkMode());

    private static Color ScaleAlpha(Color color, float scale)
    {
        var alpha = (byte)Math.Clamp((int)MathF.Round(color.A * scale), 0, 255);
        return Color.FromArgb(alpha, color.R, color.G, color.B);
    }

    private static Brush GetAcrylicInAppFillBrush(bool isDark)
    {
        if (AppThemeVisualResolver.ResolveThemeResourceBrush("AcrylicInAppFillColorDefaultBrush") is Brush themeBrush)
            return themeBrush;

        return new AcrylicBrush
        {
            TintColor = isDark
                ? Color.FromArgb(255, 0x2C, 0x2C, 0x2C)
                : Color.FromArgb(255, 0xFC, 0xFC, 0xFC),
            TintOpacity = isDark ? 0.15 : 0.0,
            FallbackColor = isDark
                ? Color.FromArgb(255, 0x2C, 0x2C, 0x2C)
                : Color.FromArgb(255, 0xF9, 0xF9, 0xF9)
        };
    }
}
