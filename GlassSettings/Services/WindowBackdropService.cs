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
    /// Подключить полнооконный Desktop Acrylic до <c>Activate()</c>. Сохранённый режим
    /// возвращается при показе shell, без повторного присваивания <c>SystemBackdrop</c>.
    /// </summary>
    public static void AttachStartupBackdrop(Window window, Grid rootGrid)
    {
        if (window.SystemBackdrop is not null)
            return;

        if (GlassWindowBackdrop.IsAnyMaterialSupported())
        {
            try
            {
                var backdrop = new GlassWindowBackdrop(AppWindowBackdropMode.Acrylic);
                window.SystemBackdrop = backdrop;
                if (!backdrop.NeedsSolidFallback)
                    return;
            }
            catch { /* ниже — solid-подложка */ }
        }

        // Акрил недоступен или контроллер не подключился: фон корня выставляется
        // до первого кадра, чтобы даже прозрачный остров не оставил «дырку» в desktop.
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
            try { backdrop.TryApplyMode(GlassSettingsRegistry.ReadWindowBackdrop()); }
            catch (Exception ex) { System.Diagnostics.Debug.WriteLine($"GlassSettings backdrop: {ex}"); }
        }

        SyncRootBackground(window, rootGrid);
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
            if (!backdrop.NeedsSolidFallback)
            {
                backdrop.RefreshThemeColors();
                rootGrid.Background = null;
                return;
            }
        }
        else if (window.SystemBackdrop is not null)
        {
            return;
        }

        ApplySolidRootBackground(rootGrid);
    }

    private static void ApplySolidRootBackground(Grid rootGrid) =>
        rootGrid.Background = AppThemeVisualResolver.ResolveThemeResourceBrush("SolidBackgroundFillColorBaseBrush")
            ?? new SolidColorBrush(AppThemeVisualResolver.ResolveIsDarkMode()
                ? Color.FromArgb(255, 0x20, 0x20, 0x20)
                : Color.FromArgb(255, 0xF3, 0xF3, 0xF3));

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
