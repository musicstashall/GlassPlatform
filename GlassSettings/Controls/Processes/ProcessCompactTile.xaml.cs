using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls.Processes;

public sealed partial class ProcessCompactTile : UserControl
{
    public static readonly DependencyProperty EntryProperty =
        DependencyProperty.Register(
            nameof(Entry),
            typeof(ProcessEntryModel),
            typeof(ProcessCompactTile),
            new PropertyMetadata(null, OnEntryChanged));

    public event EventHandler<ProcessActionRequestedEventArgs>? ActionRequested;

    public ProcessCompactTile()
    {
        InitializeComponent();
    }

    public ProcessEntryModel? Entry
    {
        get => (ProcessEntryModel?)GetValue(EntryProperty);
        set => SetValue(EntryProperty, value);
    }

    private static void OnEntryChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ProcessCompactTile tile)
            tile.BindEntry(e.NewValue as ProcessEntryModel);
    }

    private void BindEntry(ProcessEntryModel? entry)
    {
        if (entry is null)
            return;

        TitleText.Text = entry.DisplayName;
        BindCompanyLine(entry);
        BindIcon(entry);
        UpdateForbiddenVisual(entry);
        UpdateMenuVisibility(entry);
        entry.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName is nameof(ProcessEntryModel.Icon))
                BindIcon(entry);
            if (args.PropertyName is nameof(ProcessEntryModel.GridCompanyLine) or nameof(ProcessEntryModel.IsForbidden))
            {
                BindCompanyLine(entry);
                UpdateForbiddenVisual(entry);
            }
        };
    }

    private void BindCompanyLine(ProcessEntryModel entry)
    {
        var company = entry.GridCompanyLine;
        if (string.IsNullOrWhiteSpace(company))
        {
            SubtitleText.Text = string.Empty;
            SubtitleText.Visibility = Visibility.Collapsed;
            return;
        }

        SubtitleText.Text = company;
        SubtitleText.Visibility = Visibility.Visible;
    }

    private void BindIcon(ProcessEntryModel entry)
    {
        var icon = entry.Icon ?? ProcessIconResolver.TryGetDefaultFileTypeIcon(entry.ExeName);
        if (icon is not null)
        {
            IconImage.Source = icon;
            IconImage.Visibility = Visibility.Visible;
            IconFallback.Visibility = Visibility.Collapsed;
            return;
        }

        IconImage.Source = null;
        IconImage.Visibility = Visibility.Collapsed;
        IconFallback.Visibility = Visibility.Visible;
    }

    private void UpdateForbiddenVisual(ProcessEntryModel entry)
    {
        var brushKey = entry.IsForbidden ? "TextFillColorSecondaryBrush" : "TextFillColorPrimaryBrush";
        TitleText.Foreground = (Brush)Application.Current.Resources[brushKey];
    }

    private void UpdateMenuVisibility(ProcessEntryModel entry)
    {
        MoreButton.Visibility = entry.Section == ProcessSection.BlockList && entry.IsBuiltInBlockList
            ? Visibility.Collapsed
            : Visibility.Visible;
    }

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "PointerOver", true);

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "Normal", true);

    private void MoreButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (Entry is null)
            return;

        var flyout = ProcessTileMenu.CreateFlyout(Entry, OnActionRequested);
        flyout.ShowAt(MoreButton);
    }

    private void OnActionRequested(object? sender, ProcessActionRequestedEventArgs e) =>
        ActionRequested?.Invoke(this, e);
}
