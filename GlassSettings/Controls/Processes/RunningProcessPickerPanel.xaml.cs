using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls.Processes;

public sealed partial class RunningProcessPickerPanel : UserControl
{
    private readonly IReadOnlyList<RunningProcessPickerItem> _processes;
    private readonly List<RunningProcessPickerItem> _filtered = new();
    private HostedDialogHostContext? _dialogContext;
    private RunningProcessPickerItem? _selected;

    public RunningProcessPickerPanel(IReadOnlyList<RunningProcessPickerItem> processes)
    {
        _processes = processes;
        SearchPlaceholderText = GlassSettingsResources.Get("Attach_ProcessPickerSearch");
        EmptyListText = GlassSettingsResources.Get("Attach_ProcessPickerEmpty");
        InitializeComponent();
        ResetFilter(string.Empty);
    }

    public string SearchPlaceholderText { get; }

    public string EmptyListText { get; }

    public void AttachDialogContext(HostedDialogHostContext context)
    {
        _dialogContext = context;
        context.SetPrimaryEnabled(false);
    }

    public string? GetSelectedExeName() => _selected?.ExeName;

    private void SearchBox_OnTextChanged(object sender, TextChangedEventArgs e)
    {
        ResetFilter(SearchBox.Text.Trim());
        UpdateSearchGlyphVisibility();
    }

    private void ProcessListView_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        _selected = ProcessListView.SelectedItem as RunningProcessPickerItem;
        _dialogContext?.SetPrimaryEnabled(_selected is not null);
    }

    private void ResetFilter(string query)
    {
        _filtered.Clear();
        if (string.IsNullOrEmpty(query))
        {
            _filtered.AddRange(_processes);
        }
        else
        {
            _filtered.AddRange(_processes.Where(p =>
                p.ExeName.Contains(query, StringComparison.OrdinalIgnoreCase) ||
                p.DisplayName.Contains(query, StringComparison.OrdinalIgnoreCase) ||
                p.DetailLine.Contains(query, StringComparison.OrdinalIgnoreCase)));
        }

        ProcessListView.ItemsSource = null;
        ProcessListView.ItemsSource = _filtered;
        UpdateEmptyState();
    }

    private void UpdateEmptyState()
    {
        var hasItems = _filtered.Count > 0;
        EmptyText.Visibility = hasItems ? Visibility.Collapsed : Visibility.Visible;
        ProcessListView.Visibility = hasItems ? Visibility.Visible : Visibility.Collapsed;
    }

    private void UpdateSearchGlyphVisibility() =>
        SearchGlyph.Visibility = string.IsNullOrEmpty(SearchBox.Text)
            ? Visibility.Visible
            : Visibility.Collapsed;
}
