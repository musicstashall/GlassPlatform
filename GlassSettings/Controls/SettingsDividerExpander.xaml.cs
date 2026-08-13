using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

[ContentProperty(Name = nameof(BodyContent))]
public sealed partial class SettingsDividerExpander : UserControl, ISettingsUnsavedScopeHost
{
    private const int MaxChromeRetryCount = 5;

    public static readonly DependencyProperty TitleProperty =
        DependencyProperty.Register(
            nameof(Title),
            typeof(string),
            typeof(SettingsDividerExpander),
            new PropertyMetadata(string.Empty, OnTitleChanged));

    public static readonly DependencyProperty BodyContentProperty =
        DependencyProperty.Register(
            nameof(BodyContent),
            typeof(object),
            typeof(SettingsDividerExpander),
            new PropertyMetadata(null, OnBodyContentChanged));

    public static readonly DependencyProperty UnsavedScopeProperty =
        DependencyProperty.Register(
            nameof(UnsavedScope),
            typeof(SettingsCommitScope?),
            typeof(SettingsDividerExpander),
            new PropertyMetadata(null, (d, _) => ((SettingsDividerExpander)d).RefreshUnsavedIndicator()));

    private bool _chromeRetryPending;
    private bool _layoutUpdatedHooked;
    private int _chromeRetryCount;
    private bool _isUnsavedPending;
    private ToggleButton? _headerButton;
    private Brush? _transparentBrush;
    private bool _headerInteractionAttached;
    private bool _headerSizeHooked;

    public event EventHandler<RoutedEventArgs>? Expanded;

    /// <summary>Дополнительная проверка dirty (напр. OR нескольких MenuItem kind).</summary>
    public Func<bool>? HasUnsavedOverride { get; set; }

    public SettingsDividerExpander()
    {
        InitializeComponent();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        RootExpander.Loaded += OnRootExpanderLoaded;
        RootExpander.RegisterPropertyChangedCallback(
            Expander.IsExpandedProperty,
            OnIsExpandedChanged);
        ActualThemeChanged += (_, _) => ApplyUnsavedHeaderChrome();
    }

    public string Title
    {
        get => (string)GetValue(TitleProperty);
        set => SetValue(TitleProperty, value);
    }

    public object? BodyContent
    {
        get => GetValue(BodyContentProperty);
        set => SetValue(BodyContentProperty, value);
    }

    public SettingsCommitScope? UnsavedScope
    {
        get => (SettingsCommitScope?)GetValue(UnsavedScopeProperty);
        set => SetValue(UnsavedScopeProperty, value);
    }

    public bool IsUnsavedPending
    {
        get => _isUnsavedPending;
        set
        {
            if (_isUnsavedPending == value)
                return;

            _isUnsavedPending = value;
            ApplyUnsavedHeaderChrome();
        }
    }

    public bool IsExpanded
    {
        get => RootExpander.IsExpanded;
        set => RootExpander.IsExpanded = value;
    }

    /// <summary>
    /// Компактные отступы divider-заголовка (hosted-диалоги, списки групп).
    /// Только top у expander — иначе сумма bottom+top соседних групп даёт ×2 над линией.
    /// По умолчанию у RootExpander/BodyHost — по 20 (для страниц «Дополнительно»).
    /// </summary>
    public void SetChromeSpacing(double headerTopMargin, double bodyTopMargin)
    {
        RootExpander.Margin = new Thickness(0, headerTopMargin, 0, 0);
        BodyHost.Margin = new Thickness(0, bodyTopMargin, 0, 0);
    }

    public void RefreshUnsavedIndicator()
    {
        IsUnsavedPending = ComputeUnsavedPending();
    }

    /// <summary>Повторное применение divider-chrome (host с Visibility=Collapsed до первого показа).</summary>
    public void RefreshDividerChrome()
    {
        EnsureDividerChromeApplied(animateChevron: false, resetRetry: true);
        ApplyUnsavedHeaderChrome();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        EnsureDividerChromeApplied(animateChevron: false, resetRetry: true);
        RefreshUnsavedIndicator();
    }

    private void OnRootExpanderLoaded(object sender, RoutedEventArgs e) =>
        EnsureDividerChromeApplied(animateChevron: false, resetRetry: true);

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        DetachHeaderButtonHooks();
        _headerButton = null;
    }

    private void OnIsExpandedChanged(DependencyObject sender, DependencyProperty prop)
    {
        Expanded?.Invoke(this, new RoutedEventArgs());
        if (!IsLoaded)
            return;

        EnsureDividerChromeApplied(animateChevron: ExpanderBodyTransition.ShouldAnimate());
        ApplyUnsavedHeaderChrome();
    }

    private bool ComputeUnsavedPending()
    {
        if (HasUnsavedOverride?.Invoke() == true)
            return true;

        return UnsavedScope is { } scope && SettingsSession.Current.HasUnsavedInScope(scope);
    }

    private bool ShouldShowUnsavedHeaderBackground() =>
        IsLoaded && !RootExpander.IsExpanded && _isUnsavedPending;

    private void ApplyUnsavedHeaderChrome()
    {
        if (!IsLoaded)
            return;

        EnsureHeaderButton();
        ApplyHeaderCornerRadius();
        ApplyHeaderBackground();
    }

    private void EnsureHeaderButton()
    {
        if (_headerButton is not null)
            return;

        _headerButton = SpoilerExpanderChrome.TryGetHeaderToggleButton(RootExpander);
        if (_headerButton is null)
            return;

        _transparentBrush ??= Application.Current.Resources["SystemControlTransparentBrush"] as Brush
                            ?? new SolidColorBrush(Microsoft.UI.Colors.Transparent);

        if (_headerInteractionAttached)
            return;

        _headerInteractionAttached = true;
        _headerButton.PointerEntered += HeaderButton_OnPointerEntered;
        _headerButton.PointerExited += HeaderButton_OnPointerBackgroundRestore;
        _headerButton.LostFocus += HeaderButton_OnPointerBackgroundRestore;
        _headerButton.PointerReleased += HeaderButton_OnPointerReleased;

        if (_headerSizeHooked)
            return;

        _headerSizeHooked = true;
        _headerButton.SizeChanged += HeaderButton_OnSizeChanged;
    }

    private void DetachHeaderButtonHooks()
    {
        if (_headerButton is not null)
        {
            if (_headerInteractionAttached)
            {
                _headerButton.PointerEntered -= HeaderButton_OnPointerEntered;
                _headerButton.PointerExited -= HeaderButton_OnPointerBackgroundRestore;
                _headerButton.LostFocus -= HeaderButton_OnPointerBackgroundRestore;
                _headerButton.PointerReleased -= HeaderButton_OnPointerReleased;
            }

            if (_headerSizeHooked)
                _headerButton.SizeChanged -= HeaderButton_OnSizeChanged;
        }

        _headerInteractionAttached = false;
        _headerSizeHooked = false;
    }

    private void HeaderButton_OnSizeChanged(object sender, SizeChangedEventArgs e) =>
        ApplyHeaderCornerRadius();

    private void HeaderButton_OnPointerEntered(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        if (_headerButton is null)
            return;

        _headerButton.Background = ResolveHoverBrush();
    }

    private Brush ResolveHoverBrush()
    {
        if (ShouldShowUnsavedHeaderBackground())
            return SettingsIndicatorChrome.ResolveHoverBackgroundBrush(SettingsNoticeSeverity.Warning);

        // AppThemeVisualResolver — не Application.Current.Resources (тема приложения после старта не меняется).
        return AppThemeVisualResolver.ResolveThemeBrush("SubtleFillColorSecondaryBrush");
    }

    private void HeaderButton_OnPointerBackgroundRestore(object sender, RoutedEventArgs e) =>
        ApplyHeaderBackground();

    private void HeaderButton_OnPointerReleased(object sender, Microsoft.UI.Xaml.Input.PointerRoutedEventArgs e)
    {
        if (_headerButton is null || _headerButton.IsPointerOver)
            return;

        ApplyHeaderBackground();
    }

    private void ApplyHeaderBackground()
    {
        if (_headerButton is null)
            return;

        _headerButton.Background = ShouldShowUnsavedHeaderBackground()
            ? SettingsIndicatorChrome.ResolveBackgroundBrush(SettingsNoticeSeverity.Warning)
            : ResolveTransparentBrush();
    }

    private void ApplyHeaderCornerRadius()
    {
        if (_headerButton is null)
            return;

        var height = _headerButton.ActualHeight;
        if (height <= 0)
            return;

        var radius = height / 2.0;
        _headerButton.CornerRadius = RootExpander.IsExpanded
            ? new CornerRadius(radius, radius, 0, 0)
            : new CornerRadius(radius);
    }

    private Brush ResolveTransparentBrush() =>
        _transparentBrush ??= Application.Current.Resources["SystemControlTransparentBrush"] as Brush
                              ?? new SolidColorBrush(Microsoft.UI.Colors.Transparent);

    private void EnsureDividerChromeApplied(bool animateChevron, bool resetRetry = false)
    {
        if (resetRetry)
            _chromeRetryCount = 0;

        SpoilerExpanderChrome.ApplyDividerExpanderChrome(RootExpander);
        SpoilerExpanderChrome.EnsureChevronRotation(TitleChevron);
        EnsureHeaderButton();

        if (SpoilerExpanderChrome.IsSystemChevronHidden(RootExpander))
        {
            _chromeRetryCount = 0;
            UnhookLayoutUpdated();
            if (IsLoaded)
            {
                SpoilerExpanderChrome.AnimateChevron(
                    TitleChevron,
                    RootExpander.IsExpanded,
                    animate: animateChevron);
            }

            ApplyUnsavedHeaderChrome();
            return;
        }

        ScheduleChromeRetry();
    }

    private void ScheduleChromeRetry()
    {
        EnsureLayoutUpdatedHook();

        if (_chromeRetryPending || _chromeRetryCount >= MaxChromeRetryCount)
            return;

        _chromeRetryPending = true;
        _chromeRetryCount++;
        DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Normal, () =>
        {
            _chromeRetryPending = false;
            EnsureDividerChromeApplied(animateChevron: false);
        });
    }

    private void EnsureLayoutUpdatedHook()
    {
        if (_layoutUpdatedHooked || SpoilerExpanderChrome.IsSystemChevronHidden(RootExpander))
            return;

        _layoutUpdatedHooked = true;
        RootExpander.LayoutUpdated += OnRootExpanderLayoutUpdated;
    }

    private void UnhookLayoutUpdated()
    {
        if (!_layoutUpdatedHooked)
            return;

        RootExpander.LayoutUpdated -= OnRootExpanderLayoutUpdated;
        _layoutUpdatedHooked = false;
    }

    private void OnRootExpanderLayoutUpdated(object? sender, object e)
    {
        if (SpoilerExpanderChrome.IsSystemChevronHidden(RootExpander))
        {
            _chromeRetryCount = 0;
            UnhookLayoutUpdated();
            return;
        }

        EnsureDividerChromeApplied(animateChevron: false);
    }

    private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsDividerExpander section)
            section.TitleText.Text = e.NewValue as string ?? string.Empty;
    }

    private static void OnBodyContentChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsDividerExpander section)
            section.BodyHost.Content = e.NewValue;
    }
}
