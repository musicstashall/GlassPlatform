using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class HomeSettingsCard : UserControl
{
    public static readonly DependencyProperty CardTitleProperty =
        DependencyProperty.Register(nameof(CardTitle), typeof(string), typeof(HomeSettingsCard),
            new PropertyMetadata(string.Empty, OnHeaderChanged));

    public static readonly DependencyProperty CardDescriptionProperty =
        DependencyProperty.Register(nameof(CardDescription), typeof(string), typeof(HomeSettingsCard),
            new PropertyMetadata(string.Empty, OnHeaderChanged));

    public HomeSettingsCard() => InitializeComponent();

    public string CardTitle
    {
        get => (string)GetValue(CardTitleProperty);
        set => SetValue(CardTitleProperty, value);
    }

    public string CardDescription
    {
        get => (string)GetValue(CardDescriptionProperty);
        set => SetValue(CardDescriptionProperty, value);
    }

    public IEnumerable<HomeSettingsRow> EnumerateRows()
    {
        foreach (var child in RowHost.Children)
        {
            if (child is HomeSettingsRow row)
                yield return row;
        }
    }

    /// <summary>Корень карточки (для временного suppress ChildrenTransitions при reveal футера).</summary>
    internal Panel CardRootPanel => CardRoot;

    public void ClearRows() => RowHost.Children.Clear();

    public HomeSettingsRow AddRow()
    {
        var row = new HomeSettingsRow { HorizontalAlignment = HorizontalAlignment.Stretch };
        RowHost.Children.Add(row);
        UpdateLastDivider();
        return row;
    }

    public void AddCustomRow(UIElement element)
    {
        RowHost.Children.Add(element);
        UpdateLastDivider();
    }

    public HomeComponentsTable AddComponentsTable()
    {
        var table = new HomeComponentsTable { HorizontalAlignment = HorizontalAlignment.Stretch };
        RowHost.Children.Add(table);
        UpdateLastDivider();
        return table;
    }

    public HomeComponentGroup AddComponentGroup()
    {
        var group = new HomeComponentGroup { HorizontalAlignment = HorizontalAlignment.Stretch };
        RowHost.Children.Add(group);
        UpdateLastDivider();
        return group;
    }

    /// <summary>Визуальный разделитель между таблицами компонентов.</summary>
    public void AddComponentsSeparator()
    {
        var separator = new Border
        {
            Height = 1,
            Margin = new Thickness(12, 12, 12, 12),
            Background = Services.AppThemeVisualResolver.ResolveThemeBrush("DividerStrokeColorDefaultBrush")
        };
        RowHost.Children.Add(separator);
    }

    /// <summary>Подзаголовок-секция внутри карточки (стиль как у заголовка карточки).</summary>
    public void AddSectionHeader(string text)
    {
        var panel = new StackPanel { Spacing = 8, Margin = new Thickness(0, 14, 0, 6) };
        panel.Children.Add(new Border
        {
            Height = 1,
            Margin = new Thickness(0, 0, 0, 6),
            Background = Services.AppThemeVisualResolver.ResolveThemeBrush("DividerStrokeColorDefaultBrush")
        });
        panel.Children.Add(new TextBlock
        {
            Text = text,
            Style = Application.Current.Resources["BodyStrongTextBlockStyle"] as Style
        });
        RowHost.Children.Add(panel);
    }

    public void UpdateLastDivider()
    {
        for (var i = 0; i < RowHost.Children.Count; i++)
        {
            if (RowHost.Children[i] is HomeSettingsRow row)
                row.ShowDivider = i < RowHost.Children.Count - 1;
        }
    }

    public void SetFooter(UIElement? footer)
    {
        FooterHost.Content = footer;
        FooterHost.Visibility = footer is null ? Visibility.Collapsed : Visibility.Visible;
        UpdateLeadLayout();
    }

    /// <summary>Пересчитать отступы lead-карточки после смены видимости футера.</summary>
    public void RefreshLeadChrome() => UpdateLeadLayout();

    public void SetLeadContent(UIElement? content)
    {
        LeadHost.Content = content;
        UpdateLeadLayout();
    }

    private void UpdateLeadLayout()
    {
        var hasLead = LeadHost.Content is not null;
        LeadHost.Visibility = hasLead ? Visibility.Visible : Visibility.Collapsed;

        if (hasLead)
        {
            CardBorder.Padding = new Thickness(0);
            LeadColumn.Width = new GridLength(DesktopWallpaperPreviewTile.LeadSlotWidth, GridUnitType.Pixel);
            LeadHost.MinWidth = DesktopWallpaperPreviewTile.LeadSlotWidth;
            LeadHost.MaxWidth = DesktopWallpaperPreviewTile.LeadSlotWidth;
            LeadHost.VerticalAlignment = VerticalAlignment.Stretch;
            LeadHost.HorizontalAlignment = HorizontalAlignment.Stretch;
            MainPanel.SetValue(Grid.ColumnProperty, 1);
            MainPanel.ClearValue(Grid.ColumnSpanProperty);
            var hasFooter = FooterHost.Visibility == Visibility.Visible &&
                            FooterHost.Content is FrameworkElement { Visibility: Visibility.Visible };
            // Нижний отступ карточки — только во футере; иначе контент прижимается к разделителю.
            MainPanel.Padding = new Thickness(0, 12, 16, hasFooter ? 0 : 12);
            FooterHost.Padding = new Thickness(15, 0, 16, 0);
            FooterHost.HorizontalAlignment = HorizontalAlignment.Stretch;
            FooterHost.VerticalAlignment = VerticalAlignment.Stretch;
            return;
        }

        LeadColumn.Width = GridLength.Auto;
        LeadHost.ClearValue(FrameworkElement.MinWidthProperty);
        LeadHost.ClearValue(FrameworkElement.MaxWidthProperty);

        CardBorder.Padding = (Thickness)Application.Current.Resources["HomeCardPadding"];
        MainPanel.ClearValue(PaddingProperty);
        FooterHost.ClearValue(PaddingProperty);
        FooterHost.HorizontalAlignment = HorizontalAlignment.Stretch;
        MainPanel.SetValue(Grid.ColumnProperty, 0);
        MainPanel.SetValue(Grid.ColumnSpanProperty, 2);
    }

    private static void OnHeaderChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is HomeSettingsCard card)
            card.UpdateHeader();
    }

    private void UpdateHeader()
    {
        TitleText.Text = CardTitle;
        var desc = CardDescription;
        DescriptionText.Text = desc;
        DescriptionText.Visibility = string.IsNullOrEmpty(desc) ? Visibility.Collapsed : Visibility.Visible;
    }
}
