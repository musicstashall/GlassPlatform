using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls.Processes;

public enum ProcessViewMode
{
    List,
    Tiles,
    Compact
}

public sealed partial class ProcessViewSwitcher : UserControl
{
    private bool _suppressSelection;

    public static readonly DependencyProperty ViewModeProperty =
        DependencyProperty.Register(
            nameof(ViewMode),
            typeof(ProcessViewMode),
            typeof(ProcessViewSwitcher),
            new PropertyMetadata(ProcessViewMode.List, OnViewModeChanged));

    public event EventHandler<ProcessViewMode>? ViewModeChanged;

    public ProcessViewSwitcher()
    {
        InitializeComponent();
        ApplyLocalizedStrings();
        Bar.SelectedItem = ListItem;
    }

    public ProcessViewMode ViewMode
    {
        get => (ProcessViewMode)GetValue(ViewModeProperty);
        set => SetValue(ViewModeProperty, value);
    }

    public void ApplyLocalizedStrings()
    {
        GlassSettingsToolTipService.SetToolTip(ListItem, GlassSettingsResources.Get("Attach_ViewList"));
        GlassSettingsToolTipService.SetToolTip(TilesItem, GlassSettingsResources.Get("Attach_ViewTiles"));
        GlassSettingsToolTipService.SetToolTip(CompactItem, GlassSettingsResources.Get("Attach_ViewCompact"));
    }

    private static void OnViewModeChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ProcessViewSwitcher switcher)
            switcher.SyncSelectionFromViewMode();
    }

    private void SyncSelectionFromViewMode()
    {
        var item = ViewMode switch
        {
            ProcessViewMode.Tiles => TilesItem,
            ProcessViewMode.Compact => CompactItem,
            _ => ListItem
        };

        if (Bar.SelectedItem == item)
            return;

        _suppressSelection = true;
        try
        {
            Bar.SelectedItem = item;
        }
        finally
        {
            _suppressSelection = false;
        }
    }

    private void Bar_OnSelectionChanged(SelectorBar sender, SelectorBarSelectionChangedEventArgs args)
    {
        if (_suppressSelection || sender.SelectedItem is not SelectorBarItem item)
            return;

        var mode = item switch
        {
            _ when ReferenceEquals(item, TilesItem) => ProcessViewMode.Tiles,
            _ when ReferenceEquals(item, CompactItem) => ProcessViewMode.Compact,
            _ => ProcessViewMode.List
        };

        if (ViewMode == mode)
            return;

        ViewMode = mode;
        ViewModeChanged?.Invoke(this, mode);
    }
}
