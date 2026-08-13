using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Markup;

namespace GlassSettings.Controls;

[ContentProperty(Name = nameof(HeaderActions))]
public sealed partial class SettingsOptionRow : UserControl, ISettingsUnsavedScopeHost
{
    public static readonly DependencyProperty TitleProperty =
        DependencyProperty.Register(
            nameof(Title),
            typeof(string),
            typeof(SettingsOptionRow),
            new PropertyMetadata(string.Empty, OnTitleChanged));

    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(
            nameof(Description),
            typeof(string),
            typeof(SettingsOptionRow),
            new PropertyMetadata(string.Empty, OnDescriptionChanged));

    public static readonly DependencyProperty HeaderActionsProperty =
        DependencyProperty.Register(
            nameof(HeaderActions),
            typeof(object),
            typeof(SettingsOptionRow),
            new PropertyMetadata(null, OnHeaderActionsChanged));

    public static readonly DependencyProperty IconGlyphProperty =
        DependencyProperty.Register(
            nameof(IconGlyph),
            typeof(string),
            typeof(SettingsOptionRow),
            new PropertyMetadata(string.Empty, OnIconGlyphChanged));

    public static readonly DependencyProperty UnsavedScopeProperty =
        DependencyProperty.Register(
            nameof(UnsavedScope),
            typeof(SettingsCommitScope?),
            typeof(SettingsOptionRow),
            new PropertyMetadata(null, (d, _) => ((SettingsOptionRow)d).RefreshUnsavedIndicator()));

    private readonly SpoilerHeaderLayoutHost _layoutHost = new();
    private object? _headerActionsContent;

    public SettingsOptionRow()
    {
        InitializeComponent();
        // Нет expand/navigate — card PointerOver не показываем (контролы внутри живут сами).
        Loaded += (_, _) =>
        {
            SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
            RefreshUnsavedIndicator();
            _layoutHost.Attach(
                RootBorder,
                HeaderChrome,
                ActionsBelowHost,
                SpoilerHeaderRowLayoutBridge.CreateOptions(HeaderChrome, hasGateRail: false));
            ApplyHeaderControls();
        };
        Unloaded += (_, _) =>
        {
            SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
            _layoutHost.Detach();
        };
    }

    public string Title
    {
        get => (string)GetValue(TitleProperty);
        set => SetValue(TitleProperty, value);
    }

    public string Description
    {
        get => (string)GetValue(DescriptionProperty);
        set => SetValue(DescriptionProperty, value);
    }

    public object? HeaderActions
    {
        get => GetValue(HeaderActionsProperty);
        set => SetValue(HeaderActionsProperty, value);
    }

    public string IconGlyph
    {
        get => (string)GetValue(IconGlyphProperty);
        set => SetValue(IconGlyphProperty, value);
    }

    public SettingsCommitScope? UnsavedScope
    {
        get => (SettingsCommitScope?)GetValue(UnsavedScopeProperty);
        set => SetValue(UnsavedScopeProperty, value);
    }

    public SpoilerHeaderControlsTable HeaderControlsTable => HeaderChrome.HeaderControlsTable;

    public bool IsUnsavedPending
    {
        get => HeaderChrome.IsUnsavedPending;
        set => HeaderChrome.IsUnsavedPending = value;
    }

    public void RefreshUnsavedIndicator()
    {
        IsUnsavedPending = UnsavedScope is { } scope && SettingsSession.Current.HasUnsavedInScope(scope);
    }

    /// <summary>Визуал «gate»: при false — приглушённый заголовок (как FlatGateRow OFF).</summary>
    public void ApplyActiveVisual(bool active) =>
        HeaderChrome.ApplyGateVisual(active);

    /// <summary>Жёлтая caution-подстрока под Description (политика / parallel warning).</summary>
    public void SetParallelBranchWarning(bool visible, string? toolTip, string? markerText = null)
    {
        HeaderChrome.SetParallelBranchWarning(visible, toolTip, markerText);
        if (IsLoaded)
            _layoutHost.Invalidate();
    }

    /// <summary>Не приглушать HeaderActions при ApplyActiveVisual(false) (тумблер остаётся ярким).</summary>
    public bool DimHeaderActionsWhenInactive
    {
        get => HeaderChrome.DimHeaderActionsWithGate;
        set => HeaderChrome.DimHeaderActionsWithGate = value;
    }

    public void RefreshHeaderLayout() => _layoutHost.Invalidate();

    private void Session_OnDirtyChanged(object? sender, EventArgs e) => RefreshUnsavedIndicator();

    private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsOptionRow row)
            row.HeaderChrome.Title = e.NewValue as string ?? string.Empty;
    }

    private static void OnDescriptionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsOptionRow row)
            row.HeaderChrome.Description = e.NewValue as string ?? string.Empty;
    }

    private static void OnHeaderActionsChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not SettingsOptionRow row)
            return;

        row._headerActionsContent = e.NewValue;
        row.ApplyHeaderControls();
    }

    private static void OnIconGlyphChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsOptionRow row)
            row.HeaderChrome.IconGlyph = e.NewValue as string ?? string.Empty;
    }

    private void ApplyHeaderControls()
    {
        if (!IsLoaded)
            return;

        SpoilerHeaderRowLayoutBridge.ApplyHeaderControls(
            HeaderChrome,
            _layoutHost,
            _headerActionsContent,
            null);
    }

}
