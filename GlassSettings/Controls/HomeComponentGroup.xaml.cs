using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

/// <summary>
/// Таблица «Модуль | Версия» и подстрока «Версия продукта» с опциональной ссылкой на обновление.
/// </summary>
public sealed partial class HomeComponentGroup : UserControl
{
    private Action? _linkAction;

    public HomeComponentGroup() => InitializeComponent();

    /// <summary>
    /// Включает/выключает прокрутку строк. При false — MaxHeight снимается (все строки видны).
    /// </summary>
    public void SetRowsScrollable(bool scrollable, double maxHeight = 110)
    {
        if (scrollable)
        {
            RowsScroller.MaxHeight = maxHeight;
            RowsScroller.VerticalScrollBarVisibility = ScrollBarVisibility.Auto;
            RowsScroller.VerticalScrollMode = ScrollMode.Auto;
        }
        else
        {
            RowsScroller.ClearValue(FrameworkElement.MaxHeightProperty);
            RowsScroller.VerticalScrollBarVisibility = ScrollBarVisibility.Disabled;
            RowsScroller.VerticalScrollMode = ScrollMode.Disabled;
        }
    }

    public void SetFooterVisible(bool visible)
    {
        // Нижний разделитель оставляем — в карточке «Процессы» он отделяет сводку от «Посмотреть все».
        FooterTopDivider.Visibility = Visibility.Visible;
        FooterGrid.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
    }

    public void SetTable(
        string moduleHeader,
        string valueHeader,
        IReadOnlyList<(string Name, string Value)> rows)
    {
        if (!string.Equals(ModuleHeader.Text, moduleHeader, StringComparison.Ordinal))
            ModuleHeader.Text = moduleHeader;
        if (!string.Equals(ValueHeader.Text, valueHeader, StringComparison.Ordinal))
            ValueHeader.Text = valueHeader;

        // In-place: без Clear+new при том же числе строк (poll 1 Гц на Обзоре).
        if (RowsHost.Children.Count == rows.Count && TryUpdateRowsInPlace(rows))
            return;

        RowsHost.Children.Clear();
        foreach (var (name, value) in rows)
        {
            var row = new HomeComponentTableRow();
            row.SetValues(name, value);
            RowsHost.Children.Add(row);
        }
    }

    private bool TryUpdateRowsInPlace(IReadOnlyList<(string Name, string Value)> rows)
    {
        for (var i = 0; i < rows.Count; i++)
        {
            if (RowsHost.Children[i] is not HomeComponentTableRow existing)
                return false;
            existing.SetValues(rows[i].Name, rows[i].Value);
        }

        return true;
    }

    public void SetProductFooter(string text, bool hasUpdate, string? linkText, Action? onLinkClick)
    {
        SetFooterVisible(true);
        FooterPrefix.Text = text;
        UpdateDot.Visibility = hasUpdate ? Visibility.Visible : Visibility.Collapsed;
        if (hasUpdate)
            UpdateDot.Fill = SettingsIndicatorChrome.ResolveFillBrush(SettingsNoticeSeverity.Error);

        if (string.IsNullOrWhiteSpace(linkText) || onLinkClick is null)
        {
            FooterLink.Visibility = Visibility.Collapsed;
            FooterLink.Click -= OnLinkClick;
            _linkAction = null;
            return;
        }

        _linkAction = onLinkClick;
        FooterLink.Content = linkText;
        FooterLink.Foreground = AccentSubtitleBrushService.ResolveAccent();
        FooterLink.Visibility = Visibility.Visible;
        FooterLink.Click -= OnLinkClick;
        FooterLink.Click += OnLinkClick;
    }

    /// <summary>Перечитывает жёлтую подсветку ссылки и alert-точку после смены AppTheme.</summary>
    public void RefreshAccentAppearance()
    {
        if (FooterLink.Visibility == Visibility.Visible)
            FooterLink.Foreground = AccentSubtitleBrushService.ResolveAccent();

        if (UpdateDot.Visibility == Visibility.Visible)
            UpdateDot.Fill = SettingsIndicatorChrome.ResolveFillBrush(SettingsNoticeSeverity.Error);
    }

    private void OnLinkClick(object sender, RoutedEventArgs e) => _linkAction?.Invoke();
}
