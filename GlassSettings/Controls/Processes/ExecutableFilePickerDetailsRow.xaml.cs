using System.ComponentModel;
using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls.Processes;

public sealed partial class ExecutableFilePickerDetailsRow : UserControl
{
    private ExecutableFilePickerItem? _boundItem;

    public ExecutableFilePickerDetailsRow()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
        Unloaded += OnUnloaded;
    }

    private void OnDataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args) =>
        BindItem(DataContext as ExecutableFilePickerItem);

    private void OnUnloaded(object sender, RoutedEventArgs e) =>
        UnsubscribeItem();

    private void BindItem(ExecutableFilePickerItem? item)
    {
        UnsubscribeItem();
        _boundItem = item;

        if (item is null)
            return;

        item.PropertyChanged += Item_OnPropertyChanged;
        NameText.Text = item.Name;
        TypeText.Text = item.DetailLine;
        BindIcon(item);
    }

    private void UnsubscribeItem()
    {
        if (_boundItem is null)
            return;

        _boundItem.PropertyChanged -= Item_OnPropertyChanged;
        _boundItem = null;
    }

    private void Item_OnPropertyChanged(object? sender, PropertyChangedEventArgs e)
    {
        if (sender is not ExecutableFilePickerItem item)
            return;

        if (e.PropertyName is nameof(ExecutableFilePickerItem.Icon))
            BindIcon(item);
    }

    private void BindIcon(ExecutableFilePickerItem item)
    {
        if (item.Icon is not null)
        {
            IconImage.Source = item.Icon;
            IconImage.Visibility = Visibility.Visible;
            IconFallback.Visibility = Visibility.Collapsed;
            return;
        }

        IconImage.Source = null;
        IconImage.Visibility = Visibility.Collapsed;
        IconFallback.Visibility = Visibility.Visible;
        IconFallback.Glyph = item.IsFolder ? "\uE8B7" : "\uE8A5";
    }
}
