using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls.Processes;

public sealed partial class RunningProcessPickerRow : UserControl
{
    public RunningProcessPickerRow()
    {
        InitializeComponent();
        DataContextChanged += OnDataContextChanged;
    }

    private void OnDataContextChanged(FrameworkElement sender, DataContextChangedEventArgs args) =>
        BindItem(DataContext as RunningProcessPickerItem);

    private void BindItem(RunningProcessPickerItem? item)
    {
        if (item is null)
            return;

        TitleText.Text = string.IsNullOrWhiteSpace(item.DisplayName) ? item.ExeName : item.DisplayName;
        SubtitleText.Text = string.IsNullOrWhiteSpace(item.DetailLine) ? item.ExeName : item.DetailLine;
        BindIcon(item);
    }

    private void BindIcon(RunningProcessPickerItem item)
    {
        var icon = item.Icon ?? ProcessIconResolver.TryGetDefaultFileTypeIcon(item.ExeName);
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
}
