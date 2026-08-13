using System.Collections.Generic;
using GlassSettings.Core.Snapshot;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Markup;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.Foundation;

namespace GlassSettings.Controls;

[ContentProperty(Name = nameof(BodyContent))]
public sealed partial class SettingsSystemGateExpander : UserControl, ISettingsGateSection, ISettingsUnsavedScopeHost
{
    public static readonly DependencyProperty TitleProperty =
        DependencyProperty.Register(
            nameof(Title),
            typeof(string),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(string.Empty, OnTitleChanged));

    public static readonly DependencyProperty DescriptionProperty =
        DependencyProperty.Register(
            nameof(Description),
            typeof(string),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(string.Empty, OnDescriptionChanged));

    public static readonly DependencyProperty BodyContentProperty =
        DependencyProperty.Register(
            nameof(BodyContent),
            typeof(object),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(null, OnBodyContentChanged));

    public static readonly DependencyProperty HeaderActionsProperty =
        DependencyProperty.Register(
            nameof(HeaderActions),
            typeof(object),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(null, OnHeaderActionsChanged));

    public static readonly DependencyProperty HeaderSliderProperty =
        DependencyProperty.Register(
            nameof(HeaderSlider),
            typeof(object),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(null, OnHeaderSliderChanged));

    public static readonly DependencyProperty IconGlyphProperty =
        DependencyProperty.Register(
            nameof(IconGlyph),
            typeof(string),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(string.Empty, OnIconGlyphChanged));

    public static readonly DependencyProperty DimHeaderActionsWithGateProperty =
        DependencyProperty.Register(
            nameof(DimHeaderActionsWithGate),
            typeof(bool),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(true, OnDimHeaderActionsWithGateChanged));

    public static readonly DependencyProperty IsExpansionEnabledProperty =
        DependencyProperty.Register(
            nameof(IsExpansionEnabled),
            typeof(bool),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(true, OnIsExpansionEnabledChanged));

    public static readonly DependencyProperty IsGateSwitchEnabledProperty =
        DependencyProperty.Register(
            nameof(IsGateSwitchEnabled),
            typeof(bool),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(true, OnIsGateSwitchEnabledChanged));

    public static readonly DependencyProperty AllowExpandWhenGateOffProperty =
        DependencyProperty.Register(
            nameof(AllowExpandWhenGateOff),
            typeof(bool),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(false, OnAllowExpandWhenGateOffChanged));

    /// <summary>
    /// Разрешить разворот тела независимо от визуального gate.
    /// false — шапка остаётся «вкл.» (не dim), тело не раскрывается (Light/Dark mode, accent Auto).
    /// </summary>
    public static readonly DependencyProperty AllowBodyExpandProperty =
        DependencyProperty.Register(
            nameof(AllowBodyExpand),
            typeof(bool),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(true, OnAllowBodyExpandChanged));

    public static readonly DependencyProperty ShowGateSwitchProperty =
        DependencyProperty.Register(
            nameof(ShowGateSwitch),
            typeof(bool),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(true, OnShowGateSwitchChanged));

    public static readonly DependencyProperty ShowGateStateTextProperty =
        DependencyProperty.Register(
            nameof(ShowGateStateText),
            typeof(bool),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(true, OnShowGateStateTextChanged));

    public static readonly DependencyProperty UnsavedScopeProperty =
        DependencyProperty.Register(
            nameof(UnsavedScope),
            typeof(SettingsCommitScope?),
            typeof(SettingsSystemGateExpander),
            new PropertyMetadata(null, (d, _) => ((SettingsSystemGateExpander)d).RefreshUnsavedIndicator()));

    private static readonly TimeSpan ChevronRotationDuration = TimeSpan.FromMilliseconds(250);

    private bool _suppress;
    private bool _suppressExpandRevert;
    private int _programmaticGateChangeDepth;
    private bool _hasParallelWarning;
    private Brush? _headerDefaultBackground;
    private Storyboard? _chevronStoryboard;
    private readonly SpoilerHeaderLayoutHost _layoutHost = new();
    private object? _headerActionsContent;
    private object? _headerSliderContent;

    public event EventHandler<RoutedEventArgs>? GateToggled;

    public SettingsSystemGateExpander()
    {
        InitializeComponent();
        HeaderChrome.DimHeaderActionsWithGate = DimHeaderActionsWithGate;
        RootExpander.Expanding += RootExpander_OnExpanding;
        RootExpander.RegisterPropertyChangedCallback(
            Expander.IsExpandedProperty,
            (_, _) => AnimateChevronTo(RootExpander.IsExpanded));
        // После смены ElementTheme: не трогаем ResourceDictionary синхронно в ActualThemeChanged
        // (WinUI 0x800F0902 на Insert) — отложенный refresh на UI-очереди.
        ActualThemeChanged += (_, _) =>
        {
            DispatcherQueue.TryEnqueue(() =>
            {
                if (IsLoaded)
                    RefreshThemeChrome();
            });
        };
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
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

    public object? BodyContent
    {
        get => GetValue(BodyContentProperty);
        set => SetValue(BodyContentProperty, value);
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

    /// <summary>Оценка ширины HeaderActions — удалено; ширина измеряется автоматически.</summary>
    [Obsolete("Порог inline/stack вычисляется measure-based; свойство игнорируется.")]
    public double HeaderActionsInlineWidthEstimate { get; set; }

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

    /// <summary>Приглушать HeaderActions при выключенном gate (default true).</summary>
    public bool DimHeaderActionsWithGate
    {
        get => (bool)GetValue(DimHeaderActionsWithGateProperty);
        set => SetValue(DimHeaderActionsWithGateProperty, value);
    }

    /// <summary>Разрешить разворот тела (false — шапка активна, тело свёрнуто и не раскрывается).</summary>
    public bool IsExpansionEnabled
    {
        get => (bool)GetValue(IsExpansionEnabledProperty);
        set => SetValue(IsExpansionEnabledProperty, value);
    }

    /// <summary>Gate switch доступен для взаимодействия (false — только блокировка switch, напр. режим «Авто»).</summary>
    public bool IsGateSwitchEnabled
    {
        get => (bool)GetValue(IsGateSwitchEnabledProperty);
        set => SetValue(IsGateSwitchEnabledProperty, value);
    }

    public bool AllowExpandWhenGateOff
    {
        get => (bool)GetValue(AllowExpandWhenGateOffProperty);
        set => SetValue(AllowExpandWhenGateOffProperty, value);
    }

    public bool AllowBodyExpand
    {
        get => (bool)GetValue(AllowBodyExpandProperty);
        set => SetValue(AllowBodyExpandProperty, value);
    }

    /// <summary>Скрыть gate на шапке (режим задаётся HeaderActions, напр. combo «Авто»).</summary>
    public bool ShowGateSwitch
    {
        get => (bool)GetValue(ShowGateSwitchProperty);
        set => SetValue(ShowGateSwitchProperty, value);
    }

    /// <summary>Скрыть подпись «Вкл/Откл» у gate-switch (оставить только переключатель).</summary>
    public bool ShowGateStateText
    {
        get => (bool)GetValue(ShowGateStateTextProperty);
        set => SetValue(ShowGateStateTextProperty, value);
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
        if (IsLoaded)
            _layoutHost.Invalidate();
    }

    public void RefreshHeaderActionsLayout(bool resetStackState = false)
    {
        if (!IsLoaded)
            return;

        if (resetStackState)
            _layoutHost.Invalidate();
        else
            _layoutHost.UpdateLayout();
    }

    public GlassSettingsLabeledToggleSwitch GateSwitchControl => GateSwitch;

    public SpoilerHeaderControlsTable HeaderControlsTable => HeaderChrome.HeaderControlsTable;

    /// <summary>
    /// Правый inset для контролов в теле спойлера, чтобы совпасть с правым краем HeaderActions.
    /// ShowGateSwitch=false: actions в StaticHost → справа только spacing+chevron;
    /// иначе actions слева от TrailingRail → весь rail (+ margin).
    /// </summary>
    public double MeasureHeaderActionsBodyAlignInset()
    {
        var trailing = HeaderChrome.TrailingRail;
        var staticHost = HeaderChrome.StaticHostPresenter;

        if (trailing.Visibility != Visibility.Visible)
            return 0;

        // Combo/actions в StaticHost (режим без gate) — не резервируем ширину самого combo.
        if (staticHost.Content is not null && staticHost.Visibility == Visibility.Visible)
        {
            if (trailing.ActualWidth > 0.5 && staticHost.ActualWidth > 0.5)
                return Math.Max(0, Math.Ceiling(trailing.ActualWidth - staticHost.ActualWidth));

            trailing.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            staticHost.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            return Math.Max(0, Math.Ceiling(trailing.DesiredSize.Width - staticHost.DesiredSize.Width));
        }

        return HeaderChrome.MeasureTrailingRailWidth();
    }

    /// <summary>Подстрока шапки с Inlines (цветные маркеры групп и т.п.).</summary>
    public void SetDescriptionInlines(IEnumerable<Inline> inlines) =>
        HeaderChrome.SetDescriptionInlines(inlines);

    public bool IsGateOn
    {
        get => ShowGateSwitch && GateSwitch.IsOn;
        set
        {
            if (!ShowGateSwitch)
                return;

            BeginProgrammaticGateChange();
            _suppress = true;
            try
            {
                GateSwitch.IsOn = value;
                ApplyInteractionState();
            }
            finally
            {
                _suppress = false;
                EndProgrammaticGateChangeDeferred();
            }
        }
    }

    public bool IsExpanded
    {
        get => RootExpander.IsExpanded;
        set => RootExpander.IsExpanded = value;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        HideSystemChevron();
        ApplyGateSwitchChrome();
        ApplyHeaderPadding();
        EnsureChevronRotation();
        ApplyInteractionState();
        AnimateChevronTo(RootExpander.IsExpanded, animate: false);
        AttachHeaderHover();
        RefreshThemeChrome();
        EnsureLayoutHost();
        ApplyHeaderControls();
        SettingsSession.Current.DirtyChanged += Session_OnDirtyChanged;
        RefreshUnsavedIndicator();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        SettingsSession.Current.DirtyChanged -= Session_OnDirtyChanged;
        _layoutHost.Detach();
    }

    private void Session_OnDirtyChanged(object? sender, EventArgs e) => RefreshUnsavedIndicator();

    /// <summary>
    /// StaticResource ExpanderHeaderBackground не следует за сменой темы — обновляем ресурс и фон заголовка.
    /// </summary>
    public void RefreshThemeChrome()
    {
        if (!IsLoaded)
            return;

        try
        {
            HideSystemChevron();
            ApplyHeaderPadding();
            ExpanderThemeRefresh.RefreshExpanderChrome(RootExpander);
            RefreshHeaderDefaultBackground();
            ResetHeaderBackground();
            ApplyParallelWarningHighlight();
            HeaderChrome.ApplyGateVisual(IsVisualGateOn());
            AnimateChevronTo(RootExpander.IsExpanded, animate: false);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings SettingsSystemGateExpander.RefreshThemeChrome: {ex}");
        }
    }

    private void ApplyGateSwitchChrome()
    {
        GateSwitch.Visibility = ShowGateSwitch ? Visibility.Visible : Visibility.Collapsed;
        GateSwitch.ShowStateText = ShowGateStateText;
    }

    private bool CanExpandBody() =>
        AllowBodyExpand
        && IsExpansionEnabled
        && (!ShowGateSwitch || GateSwitch.IsOn || AllowExpandWhenGateOff);

    private bool IsVisualGateOn() =>
        IsGateSwitchEnabled
        && IsExpansionEnabled
        && (ShowGateSwitch ? GateSwitch.IsOn : true);

    private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section)
            section.HeaderChrome.Title = e.NewValue as string ?? string.Empty;
    }

    private static void OnDescriptionChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section)
            section.HeaderChrome.Description = e.NewValue as string ?? string.Empty;
    }

    private static void OnBodyContentChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section)
        {
            section.BodyHost.Content = e.NewValue;
            section.ApplyBodyEnabled(section.CanExpandBody());
        }
    }

    private static void OnHeaderActionsChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section)
        {
            section._headerActionsContent = e.NewValue;
            section.ApplyHeaderControls();
        }
    }

    private static void OnHeaderSliderChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section)
        {
            section._headerSliderContent = e.NewValue;
            section.ApplyHeaderControls();
        }
    }

    private static void OnDimHeaderActionsWithGateChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section)
        {
            section.HeaderChrome.DimHeaderActionsWithGate = (bool)e.NewValue;
            section.ApplyInteractionState();
            section.SyncActionsBelowGateChrome();
        }
    }

    private static void OnIsExpansionEnabledChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section && section.IsLoaded)
            section.ApplyInteractionState();
    }

    private static void OnIsGateSwitchEnabledChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section && section.IsLoaded)
            section.ApplyInteractionState();
    }

    private static void OnShowGateSwitchChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section && section.IsLoaded)
        {
            section.ApplyGateSwitchChrome();
            section.ApplyInteractionState();
            section.ApplyHeaderControls();
        }
    }

    private static void OnShowGateStateTextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section)
            section.GateSwitch.ShowStateText = (bool)e.NewValue;
    }

    private static void OnAllowExpandWhenGateOffChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section && section.IsLoaded)
            section.ApplyInteractionState();
    }

    private static void OnAllowBodyExpandChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section && section.IsLoaded)
            section.ApplyInteractionState();
    }

    private static void OnIconGlyphChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsSystemGateExpander section)
            section.HeaderChrome.IconGlyph = e.NewValue as string ?? string.Empty;
    }

    private void RootExpander_OnExpanding(Expander sender, ExpanderExpandingEventArgs args)
    {
        if (_suppressExpandRevert || CanExpandBody())
            return;

        _suppressExpandRevert = true;
        try
        {
            RootExpander.IsExpanded = false;
        }
        finally
        {
            _suppressExpandRevert = false;
        }
    }

    private void GateSwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_suppress || _programmaticGateChangeDepth > 0 || !ShowGateSwitch)
            return;

        ApplyInteractionState();
        GateToggled?.Invoke(this, e);
    }

    /// <summary>Reload/tab-sync: WinUI может отложить Toggled после IsOn — depth блокирует GateToggled.</summary>
    private void BeginProgrammaticGateChange() => _programmaticGateChangeDepth++;

    private void EndProgrammaticGateChangeDeferred()
    {
        var dispatcher = DispatcherQueue.GetForCurrentThread();
        if (dispatcher is null)
        {
            if (_programmaticGateChangeDepth > 0)
                _programmaticGateChangeDepth--;
            return;
        }

        // Двойной Low — после layout и отложенного Toggled от ToggleSwitch.
        _ = dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
            _ = dispatcher.TryEnqueue(DispatcherQueuePriority.Low, () =>
            {
                if (_programmaticGateChangeDepth > 0)
                    _programmaticGateChangeDepth--;
            }));
    }

    private void ApplyInteractionState()
    {
        var canExpand = CanExpandBody();
        if (!canExpand)
            RootExpander.IsExpanded = false;

        // Без AllowBodyExpand — плоский вид (нет шеврона), шапка остаётся активной.
        HeaderChrome.ShowChevron = AllowBodyExpand && IsExpansionEnabled;

        ApplyBodyEnabled(canExpand);
        if (ShowGateSwitch)
        {
            GateSwitch.IsEnabled = IsGateSwitchEnabled;
            // Локальный IsEnabled у InnerSwitch может остаться false после disable,
            // если UserControl.IsEnabled уже был в целевом состоянии (no-op).
            GateSwitch.SyncInnerEnabled();
        }
        HeaderChrome.ApplyGateVisual(IsVisualGateOn());
        ResetHeaderBackground();
        ApplyParallelWarningHighlight();
        SyncActionsBelowGateChrome();
        AnimateChevronTo(RootExpander.IsExpanded, animate: ExpanderBodyTransition.ShouldAnimate() && IsLoaded);
    }

    private void ApplyBodyEnabled(bool enabled)
    {
        if (BodyHost.Content is Control ctrl)
            ctrl.IsEnabled = enabled;
    }

    private void EnsureChevronRotation()
    {
        var chevron = HeaderChrome.Chevron;
        chevron.Glyph = "\uE76C";
        chevron.RenderTransformOrigin = new Point(0.5, 0.5);
        if (chevron.RenderTransform is not RotateTransform)
            chevron.RenderTransform = new RotateTransform();
    }

    private void AnimateChevronTo(bool expanded, bool animate = true)
    {
        EnsureChevronRotation();
        HeaderChrome.ApplyGateVisual(IsVisualGateOn());

        if (HeaderChrome.Chevron.RenderTransform is not RotateTransform rotate)
            return;

        var targetAngle = expanded ? 90.0 : 0.0;
        var fromAngle = rotate.Angle;
        _chevronStoryboard?.Stop();
        _chevronStoryboard = null;
        // Stop() откатывает Angle к базовому значению — восстанавливаем перед новой анимацией.
        rotate.Angle = fromAngle;

        if (!animate || !ExpanderBodyTransition.ShouldAnimate())
        {
            rotate.Angle = targetAngle;
            return;
        }

        if (Math.Abs(fromAngle - targetAngle) < 0.01)
            return;

        var animation = new DoubleAnimation
        {
            From = fromAngle,
            To = targetAngle,
            Duration = ChevronRotationDuration,
            FillBehavior = FillBehavior.HoldEnd,
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut }
        };

        _chevronStoryboard = new Storyboard();
        Storyboard.SetTarget(animation, rotate);
        Storyboard.SetTargetProperty(animation, nameof(RotateTransform.Angle));
        _chevronStoryboard.Children.Add(animation);
        _chevronStoryboard.Begin();
    }

    private void HideSystemChevron()
    {
        if (FindDescendantByName(RootExpander, "ExpandCollapseChevronBorder") is UIElement host)
            host.Visibility = Visibility.Collapsed;
    }

    /// <summary>
    /// ExpanderHeaderPadding из Expander.Resources не попадает в шаблон (StaticResource темы).
    /// Кастомные спойлеры: Border Padding 12,14 — тот же горизонтальный отступ задаём на ToggleButton.
    /// </summary>
    private void ApplyHeaderPadding()
    {
        if (FindDescendant<ToggleButton>(RootExpander) is not ToggleButton header)
            return;

        header.HorizontalAlignment = HorizontalAlignment.Stretch;
        header.HorizontalContentAlignment = HorizontalAlignment.Stretch;
        header.VerticalContentAlignment = VerticalAlignment.Center;

        if (Application.Current.Resources["GlassSettingsSystemExpanderHeaderPadding"] is Thickness padding)
            header.Padding = padding;
    }

    private void AttachHeaderHover()
    {
        var header = FindDescendant<ToggleButton>(RootExpander);
        if (header is null)
            return;

        _headerDefaultBackground = header.Background;
        var hoverBrush = Application.Current.Resources["SubtleFillColorSecondaryBrush"] as Brush;

        header.PointerEntered += (_, _) =>
        {
            if (!CanExpandBody() || _hasParallelWarning || hoverBrush is null)
                return;
            header.Background = hoverBrush;
        };
        header.PointerExited += (_, _) => ResetHeaderBackground();
    }

    private void ResetHeaderBackground()
    {
        var header = FindDescendant<ToggleButton>(RootExpander);
        if (header is null)
            return;

        if (_headerDefaultBackground is null)
            RefreshHeaderDefaultBackground();

        if (_headerDefaultBackground is not null)
            header.Background = _headerDefaultBackground;
    }

    private void RefreshHeaderDefaultBackground()
    {
        var header = FindDescendant<ToggleButton>(RootExpander);
        if (header is null)
            return;

        _headerDefaultBackground = ResolveParallelHeaderBackground();
    }

    private Brush ResolveParallelHeaderBackground() =>
        _hasParallelWarning
            ? AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCautionBackground")
            : AppThemeVisualResolver.ResolveThemeBrush("LayerFillColorDefaultBrush");

    private void ApplyParallelWarningHighlight()
    {
        RootExpander.Background = AppThemeVisualResolver.ResolveThemeBrush("LayerFillColorDefaultBrush");
        RootExpander.BorderBrush = _hasParallelWarning
            ? AppThemeVisualResolver.ResolveThemeBrush("SystemFillColorCautionBrush")
            : AppThemeVisualResolver.ResolveThemeBrush("CardStrokeColorDefaultBrush");

        RefreshHeaderDefaultBackground();
        ResetHeaderBackground();
    }

    private static T? FindDescendant<T>(DependencyObject root) where T : DependencyObject
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is T match)
                return match;

            var nested = FindDescendant<T>(child);
            if (nested is not null)
                return nested;
        }

        return null;
    }

    private static DependencyObject? FindDescendantByName(DependencyObject root, string name)
    {
        var count = VisualTreeHelper.GetChildrenCount(root);
        for (var i = 0; i < count; i++)
        {
            var child = VisualTreeHelper.GetChild(root, i);
            if (child is FrameworkElement fe && fe.Name == name)
                return child;

            var nested = FindDescendantByName(child, name);
            if (nested is not null)
                return nested;
        }

        return null;
    }

    private void EnsureLayoutHost()
    {
        _layoutHost.Attach(
            RootExpander,
            HeaderChrome,
            ActionsBelowHost,
            SpoilerHeaderRowLayoutBridge.CreateOptions(HeaderChrome, ShowGateSwitch),
            () => IsVisualGateOn());
    }

    private void ApplyHeaderControls()
    {
        if (!IsLoaded)
            return;

        // Прямое заполнение HeaderControlsTable (напр. DwmThemeDetailPage) — не затирать bridge'ом.
        if (_headerActionsContent is null && _headerSliderContent is null && !HeaderChrome.HeaderControlsTable.IsEmpty)
        {
            SpoilerHeaderLayoutMetrics.DetachFromVisualParent(HeaderChrome.HeaderControlsTable);
            HeaderChrome.AttachControlsTable(HeaderChrome.HeaderControlsTable);
            _layoutHost.Invalidate();
            ApplyHeaderPadding();
            return;
        }

        if (ShowGateSwitch)
        {
            SpoilerHeaderRowLayoutBridge.ApplyHeaderControls(
                HeaderChrome,
                _layoutHost,
                _headerActionsContent,
                _headerSliderContent);
        }
        else
        {
            SpoilerHeaderRowLayoutBridge.ApplyHeaderControlsWithoutGate(
                HeaderChrome,
                _layoutHost,
                _headerActionsContent,
                _headerSliderContent);
        }

        ApplyHeaderPadding();
    }

    private static string? FormatParallelDuplicateMarker(string? markerText, string? conflictSuffix)
    {
        if (string.IsNullOrWhiteSpace(conflictSuffix))
            return markerText;

        var baseText = string.IsNullOrWhiteSpace(markerText)
            ? GlassSettingsResources.Get("Dwm_ParallelDuplicateMarker")
            : markerText;
        return baseText + conflictSuffix;
    }

    private void SyncActionsBelowGateChrome() => _layoutHost.SyncBelowHostDim(IsVisualGateOn());
}
