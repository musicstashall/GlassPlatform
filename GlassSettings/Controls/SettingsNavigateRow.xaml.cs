using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

public sealed partial class SettingsNavigateRow : UserControl, ISettingsUnsavedScopeHost
{
    public static readonly DependencyProperty TitleProperty =
        DependencyProperty.Register(nameof(Title), typeof(string), typeof(SettingsNavigateRow),
            new PropertyMetadata(string.Empty, OnTitleChanged));

    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(nameof(Description), typeof(string), typeof(SettingsNavigateRow),
            new PropertyMetadata(string.Empty, OnDescriptionChanged));

    public static readonly DependencyProperty IconGlyphProperty =
        DependencyProperty.Register(nameof(IconGlyph), typeof(string), typeof(SettingsNavigateRow),
            new PropertyMetadata(string.Empty, OnIconGlyphChanged));

    public static readonly DependencyProperty CanNavigateProperty =
        DependencyProperty.Register(nameof(CanNavigate), typeof(bool), typeof(SettingsNavigateRow),
            new PropertyMetadata(true, (d, _) => ((SettingsNavigateRow)d).UpdateVisualState()));

    public static readonly DependencyProperty UnsavedScopeProperty =
        DependencyProperty.Register(nameof(UnsavedScope), typeof(SettingsCommitScope?), typeof(SettingsNavigateRow),
            new PropertyMetadata(null, (d, _) => ((SettingsNavigateRow)d).RefreshUnsavedIndicator()));

    private uint? _activePointerId;
    private readonly SpoilerHeaderLayoutHost _layoutHost = new();
    private object? _headerActionsContent;
    private object? _headerSliderContent;
    private object? _trailingContent;

    public event EventHandler? NavigateRequested;

    public SettingsNavigateRow()
    {
        InitializeComponent();
        RootBorder.PointerPressed += RootBorder_OnPointerPressed;
        RootBorder.PointerReleased += RootBorder_OnPointerReleased;
        RootBorder.PointerCanceled += RootBorder_OnPointerCanceled;
        RootBorder.PointerEntered += RootBorder_OnPointerEntered;
        RootBorder.PointerExited += RootBorder_OnPointerExited;
        Loaded += (_, _) =>
        {
            UpdateVisualState();
            SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
            RefreshUnsavedIndicator();
            HeaderChrome.Trailing = _trailingContent;
            _layoutHost.Attach(
                RootBorder,
                HeaderChrome,
                ActionsBelowHost,
                CreateHeaderLayoutOptions(),
                () => CanNavigate);
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

    public string IconGlyph
    {
        get => (string)GetValue(IconGlyphProperty);
        set => SetValue(IconGlyphProperty, value);
    }

    public bool CanNavigate
    {
        get => (bool)GetValue(CanNavigateProperty);
        set => SetValue(CanNavigateProperty, value);
    }

    /// <summary>Строка планировщика на этой navigate-строке (OpenGlass / single Glass).</summary>
    public PlatformSchedulerRow? PlatformSchedulerOwner { get; set; }

    public SettingsCommitScope? UnsavedScope
    {
        get => (SettingsCommitScope?)GetValue(UnsavedScopeProperty);
        set => SetValue(UnsavedScopeProperty, value);
    }

    /// <summary>Устарело: ширина измеряется автоматически.</summary>
    [Obsolete("Порог inline/stack вычисляется measure-based; свойство игнорируется.")]
    public double HeaderActionsInlineWidthEstimate { get; set; }

    public bool IsUnsavedPending
    {
        get => HeaderChrome.IsUnsavedPending;
        set => HeaderChrome.IsUnsavedPending = value;
    }

    public void RefreshUnsavedIndicator()
    {
        IsUnsavedPending = UnsavedScope is { } scope && SettingsSession.Current.HasUnsavedInScope(scope);
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e) => RefreshUnsavedIndicator();

    public UIElement? HeaderActions
    {
        get => _headerActionsContent as UIElement;
        set
        {
            _headerActionsContent = value;
            ApplyHeaderControls();
        }
    }

    public UIElement? HeaderSlider
    {
        get => _headerSliderContent as UIElement;
        set
        {
            _headerSliderContent = value;
            ApplyHeaderControls();
        }
    }

    /// <summary>Фиксированный trailing (toggle и т.п.) — не переносится на новую строку.</summary>
    public UIElement? TrailingContent
    {
        get => _trailingContent as UIElement;
        set
        {
            _trailingContent = value;
            if (IsLoaded)
                HeaderChrome.Trailing = value;
            _layoutHost.Invalidate();
        }
    }

    public SpoilerHeaderControlsTable HeaderControlsTable => HeaderChrome.HeaderControlsTable;

    public void RefreshHeaderLayout() => _layoutHost.Invalidate();

    public void SetUpdateLine(bool hasUpdate, string? lineText, Action? onLineClick) =>
        HeaderChrome.SetUpdateLine(hasUpdate, lineText, onLineClick);

    private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsNavigateRow row)
            row.HeaderChrome.Title = e.NewValue as string ?? string.Empty;
    }

    private static void OnDescriptionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsNavigateRow row)
            row.HeaderChrome.Description = e.NewValue as string ?? string.Empty;
    }

    private static void OnIconGlyphChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsNavigateRow row)
            row.HeaderChrome.IconGlyph = e.NewValue as string ?? string.Empty;
    }

    private void ApplyHeaderControls()
    {
        if (!IsLoaded)
            return;

        HeaderChrome.Trailing = _trailingContent;
        SpoilerHeaderRowLayoutBridge.ApplyHeaderControls(
            HeaderChrome,
            _layoutHost,
            _headerActionsContent,
            _headerSliderContent);
    }

    private SpoilerHeaderLayoutOptions CreateHeaderLayoutOptions() =>
        SpoilerHeaderRowLayoutBridge.CreateOptions(HeaderChrome, hasGateRail: _trailingContent is not null);

    private void UpdateVisualState()
    {
        var canNav = CanNavigate;
        VisualStateManager.GoToState(this, canNav ? "Normal" : "Disabled", true);
        HeaderChrome.ApplyGateVisual(canNav);
        HeaderChrome.TitleStack.IsHitTestVisible = canNav;
        HeaderChrome.Chevron.IsHitTestVisible = canNav;
        EnsureTrailingGateEnabled();
        _layoutHost.SyncBelowHostDim(canNav);
    }

    private void EnsureTrailingGateEnabled()
    {
        if (PlatformSchedulerOwner?.IsInstallGateBusy == true)
            return;

        // Dual Glass: trailing — StackPanel с двумя gate; их IsEnabled ведёт PlatformSchedulerRow.
        if (HeaderChrome.Trailing is not GlassSettingsLabeledToggleSwitch gate)
            return;

        if (!CanNavigate)
            gate.IsEnabled = true;
    }

    private void RootBorder_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!CanNavigate || IsInteractiveSource(e.OriginalSource as DependencyObject))
            return;
        _activePointerId = e.Pointer.PointerId;
        VisualStateManager.GoToState(this, "PointerOver", true);
    }

    private void RootBorder_OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (_activePointerId != e.Pointer.PointerId)
            return;
        _activePointerId = null;
        UpdateVisualState();

        if (!CanNavigate || IsInteractiveSource(e.OriginalSource as DependencyObject))
            return;

        var pt = e.GetCurrentPoint(RootBorder);
        if (pt.Position.X >= 0 && pt.Position.Y >= 0 &&
            pt.Position.X <= RootBorder.ActualWidth &&
            pt.Position.Y <= RootBorder.ActualHeight)
            NavigateRequested?.Invoke(this, EventArgs.Empty);
    }

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e)
    {
        if (CanNavigate && !_activePointerId.HasValue)
            VisualStateManager.GoToState(this, "PointerOver", true);
    }

    private void RootBorder_OnPointerCanceled(object sender, PointerRoutedEventArgs e)
    {
        if (_activePointerId == e.Pointer.PointerId)
            _activePointerId = null;
        UpdateVisualState();
    }

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (_activePointerId == e.Pointer.PointerId)
            return;
        if (CanNavigate)
            VisualStateManager.GoToState(this, "Normal", true);
    }

    private bool IsInteractiveSource(DependencyObject? src)
    {
        while (src is not null)
        {
            if (ReferenceEquals(src, HeaderChrome.ControlsTablePresenter) ||
                ReferenceEquals(src, HeaderChrome.StaticHostPresenter) ||
                ReferenceEquals(src, ActionsBelowHost))
                return true;
            if (src is ToggleSwitch or SplitButton or ToggleSplitButton or GlassSettingsLabeledToggleSwitch or Button or ComboBox or Slider or MenuFlyoutItem)
                return true;
            src = VisualTreeHelper.GetParent(src);
        }
        return false;
    }
}
