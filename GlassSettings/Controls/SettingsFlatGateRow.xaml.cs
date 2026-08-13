using GlassSettings.Core.Snapshot;

using GlassSettings.Services;
using GlassSettings.Strings;

using Microsoft.UI.Xaml;

using Microsoft.UI.Xaml.Controls;

using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;



namespace GlassSettings.Controls;



public sealed partial class SettingsFlatGateRow : UserControl, ISettingsGateSection, ISettingsUnsavedScopeHost

{

    public static readonly DependencyProperty TitleProperty =

        DependencyProperty.Register(

            nameof(Title),

            typeof(string),

            typeof(SettingsFlatGateRow),

            new PropertyMetadata(string.Empty, OnTitleChanged));



    public static readonly DependencyProperty DescriptionProperty =

        DependencyProperty.Register(

            nameof(Description),

            typeof(string),

            typeof(SettingsFlatGateRow),

            new PropertyMetadata(string.Empty, OnDescriptionChanged));



    public static readonly DependencyProperty HeaderActionsProperty =

        DependencyProperty.Register(

            nameof(HeaderActions),

            typeof(object),

            typeof(SettingsFlatGateRow),

            new PropertyMetadata(null, OnHeaderActionsChanged));



    public static readonly DependencyProperty HeaderSliderProperty =

        DependencyProperty.Register(

            nameof(HeaderSlider),

            typeof(object),

            typeof(SettingsFlatGateRow),

            new PropertyMetadata(null, OnHeaderSliderChanged));



    public static readonly DependencyProperty IconGlyphProperty =

        DependencyProperty.Register(

            nameof(IconGlyph),

            typeof(string),

            typeof(SettingsFlatGateRow),

            new PropertyMetadata(string.Empty, OnIconGlyphChanged));



    public static readonly DependencyProperty UnsavedScopeProperty =

        DependencyProperty.Register(

            nameof(UnsavedScope),

            typeof(SettingsCommitScope?),

            typeof(SettingsFlatGateRow),

            new PropertyMetadata(null, (d, _) => ((SettingsFlatGateRow)d).RefreshUnsavedIndicator()));



    private bool _suppress;
    private bool _hasParallelWarning;

    private readonly SpoilerHeaderLayoutHost _layoutHost = new();

    private object? _headerActionsContent;

    private object? _headerSliderContent;



    public event EventHandler<RoutedEventArgs>? GateToggled;



    public SettingsFlatGateRow()

    {

        InitializeComponent();

        // Нет expand/navigate — card PointerOver не показываем.

        Loaded += (_, _) =>

        {

            SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;

            RefreshUnsavedIndicator();

            UpdateCardVisualState();

            _layoutHost.Attach(
                RootBorder,
                HeaderChrome,
                ActionsBelowHost,
                SpoilerHeaderRowLayoutBridge.CreateOptions(HeaderChrome, hasGateRail: true),
                () => GateSwitch.IsOn);

            ApplyHeaderControls();

        };

        Unloaded += (_, _) =>
        {
            SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
            _layoutHost.Detach();
        };

    }



    public SettingsCommitScope? UnsavedScope

    {

        get => (SettingsCommitScope?)GetValue(UnsavedScopeProperty);

        set => SetValue(UnsavedScopeProperty, value);

    }



    public bool IsUnsavedPending

    {

        get => HeaderChrome.IsUnsavedPending;

        set => HeaderChrome.IsUnsavedPending = value;

    }



    public void RefreshUnsavedIndicator()

    {

        IsUnsavedPending = UnsavedScope is { } scope && SettingsSession.Current.HasUnsavedInScope(scope);

    }

    public void SetParallelBranchWarning(
        bool visible,
        string? toolTip,
        string? markerText = null,
        bool highlightBackground = false,
        string? conflictSuffix = null)
    {
        _hasParallelWarning = highlightBackground;
        HeaderChrome.SetParallelBranchWarning(
            visible,
            toolTip,
            FormatParallelDuplicateMarker(markerText, highlightBackground ? conflictSuffix : null));
        ApplyParallelWarningHighlight();
    }



    private void Session_OnDirtyChanged(object? sender, EventArgs e) => RefreshUnsavedIndicator();



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



    public object? HeaderSlider

    {

        get => GetValue(HeaderSliderProperty);

        set => SetValue(HeaderSliderProperty, value);

    }



    public string IconGlyph

    {

        get => (string)GetValue(IconGlyphProperty);

        set => SetValue(IconGlyphProperty, value);

    }



    public GlassSettingsLabeledToggleSwitch GateSwitchControl => GateSwitch;

    public SpoilerHeaderControlsTable HeaderControlsTable => HeaderChrome.HeaderControlsTable;

    public bool IsGateOn

    {

        get => GateSwitch.IsOn;

        set

        {

            _suppress = true;

            try

            {

                GateSwitch.IsOn = value;

                UpdateCardVisualState();

            }

            finally

            {

                _suppress = false;

            }

        }

    }



    private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)

    {

        if (d is SettingsFlatGateRow row)

            row.HeaderChrome.Title = e.NewValue as string ?? string.Empty;

    }



    private static void OnDescriptionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)

    {

        if (d is SettingsFlatGateRow row)

            row.HeaderChrome.Description = e.NewValue as string ?? string.Empty;

    }



    private static void OnHeaderActionsChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)

    {

        if (d is SettingsFlatGateRow row)

        {

            row._headerActionsContent = e.NewValue;

            row.ApplyHeaderControls();

        }

    }



    private static void OnHeaderSliderChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)

    {

        if (d is SettingsFlatGateRow row)

        {

            row._headerSliderContent = e.NewValue;

            row.ApplyHeaderControls();

        }

    }



    private static void OnIconGlyphChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)

    {

        if (d is SettingsFlatGateRow row)

            row.HeaderChrome.IconGlyph = e.NewValue as string ?? string.Empty;

    }



    private void ApplyHeaderControls()
    {
        // Прямое заполнение HeaderControlsTable — не затирать bridge'ом.
        if (_headerActionsContent is null && _headerSliderContent is null && !HeaderChrome.HeaderControlsTable.IsEmpty)
        {
            SpoilerHeaderLayoutMetrics.DetachFromVisualParent(HeaderChrome.HeaderControlsTable);
            HeaderChrome.AttachControlsTable(HeaderChrome.HeaderControlsTable);
            _layoutHost.Invalidate();
            return;
        }

        SpoilerHeaderRowLayoutBridge.ApplyHeaderControls(
            HeaderChrome,
            _layoutHost,
            _headerActionsContent,
            _headerSliderContent);
    }

    public void RefreshHeaderLayout() => _layoutHost.Invalidate();

    private void GateSwitch_OnToggled(object sender, RoutedEventArgs e)

    {

        if (_suppress) return;

        UpdateCardVisualState();

        GateToggled?.Invoke(this, e);

    }



    private void UpdateCardVisualState()

    {

        GateSwitch.IsEnabled = true;

        HeaderChrome.ApplyGateVisual(GateSwitch.IsOn);

        // Dim только снижает Opacity — комбо остаётся кликабельным. Явно блокируем HeaderActions.
        if (_headerActionsContent is Microsoft.UI.Xaml.Controls.Control actionsControl)
            actionsControl.IsEnabled = GateSwitch.IsOn;

        SyncActionsBelowGateChrome();

        VisualStateManager.GoToState(this, "Normal", false);
        ApplyParallelWarningHighlight();

    }



    private void SyncActionsBelowGateChrome() => _layoutHost.SyncBelowHostDim(GateSwitch.IsOn);



    private static string? FormatParallelDuplicateMarker(string? markerText, string? conflictSuffix)
    {
        if (string.IsNullOrWhiteSpace(conflictSuffix))
            return markerText;

        var baseText = string.IsNullOrWhiteSpace(markerText)
            ? GlassSettingsResources.Get("Dwm_ParallelDuplicateMarker")
            : markerText;
        return baseText + conflictSuffix;
    }

    private void ApplyParallelWarningHighlight()
    {
        RootBorder.Background = AppThemeVisualResolver.ResolveThemeBrush("LayerFillColorDefaultBrush");
        RootBorder.BorderBrush = _hasParallelWarning
            ? AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCautionBrush")
            : AppThemeVisualResolver.ResolveThemeBrush("CardStrokeColorDefaultBrush");
    }

}

