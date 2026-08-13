using GlassSettings.Navigation;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class SettingsNavigationHost : UserControl
{
    private readonly Dictionary<string, SectionDef> _sections = new();

    public SettingsNavigationHost()
    {
        InitializeComponent();
        SectionList.SizeChanged += OnSectionListSizeChanged;
    }

    private void OnSectionListSizeChanged(object sender, SizeChangedEventArgs e) => SyncSectionRowWidths();

    private void SyncSectionRowWidths()
    {
        var w = SectionList.ActualWidth;
        if (w <= 0)
            return;

        foreach (var child in SectionList.Children)
        {
            if (child is FrameworkElement fe)
            {
                fe.Width = w;
                fe.HorizontalAlignment = HorizontalAlignment.Stretch;
            }
        }
    }

    public bool IsDrillActive => false;

    public event EventHandler? DrillDepthChanged;

    public void ClearSections()
    {
        SectionList.Children.Clear();
        _sections.Clear();
        DrillDepthChanged?.Invoke(this, EventArgs.Empty);
    }

    public SettingsNavigateRow AddNavigateSection(
        string id,
        string header,
        string description,
        string? iconGlyph = null,
        Func<bool>? canNavigate = null,
        Action<SettingsNavigationHost>? onNavigate = null)
    {
        var def = new SectionDef
        {
            Id = id,
            Header = header,
            Description = description,
            CanNavigate = canNavigate
        };
        _sections[id] = def;

        var row = new SettingsNavigateRow
        {
            Title = header,
            Description = description,
            IconGlyph = iconGlyph ?? SettingsSectionGlyphs.TryForNavigateId(id),
            Tag = id,
            HorizontalAlignment = HorizontalAlignment.Stretch
        };
        row.NavigateRequested += (_, _) =>
        {
            if (canNavigate is not null && !canNavigate())
                return;
            onNavigate?.Invoke(this);
        };
        SectionList.Children.Add(row);
        SyncSectionRowWidths();
        return row;
    }

    public void PushPage(Type pageType, object? parameter, string breadcrumbTitle) =>
        SettingsNavigationService.Current.Push(pageType, parameter, breadcrumbTitle);

    public bool TryGoBack() => SettingsNavigationService.Current.TryPop();

    private sealed class SectionDef
    {
        public required string Id { get; init; }
        public required string Header { get; init; }
        public required string Description { get; init; }
        public Func<bool>? CanNavigate { get; init; }
    }
}
