using GlassSettings.Strings;
using GlassSettings.Services;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media.Animation;
using Windows.UI;

namespace GlassSettings.Controls;

public sealed partial class SettingsUnsavedNoticeStrip : UserControl
{
    private static double SlideOffsetY => -GlassSettingsNoticeMetrics.StripMinHeight;
    private const int FadeOutMs = 500;
    private const int SlideInMs = 1000;
    private bool _visuallyOpen;
    private bool _isAnimating;
    private Storyboard? _activeStoryboard;
    private TaskCompletionSource<bool>? _storyboardTcs;

    /// <summary>Идёт slide/fade — повторный OpenImmediate не должен обрывать анимацию (иначе Y зависает над карточкой).</summary>
    internal bool IsAnimating => _isAnimating;

    public static readonly DependencyProperty MessageProperty =
        DependencyProperty.Register(nameof(Message), typeof(string), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(string.Empty, (d, e) => ((SettingsUnsavedNoticeStrip)d).MessageText.Text = e.NewValue as string ?? string.Empty));

    public static readonly DependencyProperty AllowDismissProperty =
        DependencyProperty.Register(nameof(AllowDismiss), typeof(bool), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(true, (d, _) => ((SettingsUnsavedNoticeStrip)d).UpdateCloseVisibility()));

    public static readonly DependencyProperty IsStripOpenProperty =
        DependencyProperty.Register(nameof(IsStripOpen), typeof(bool), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(false));

    public static readonly DependencyProperty IsSaveEnabledProperty =
        DependencyProperty.Register(nameof(IsSaveEnabled), typeof(bool), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(true, (d, e) => ((SettingsUnsavedNoticeStrip)d).SaveButton.IsEnabled = (bool)e.NewValue));

    public static readonly DependencyProperty SeverityProperty =
        DependencyProperty.Register(nameof(Severity), typeof(SettingsNoticeSeverity), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(SettingsNoticeSeverity.Warning, (d, _) => ((SettingsUnsavedNoticeStrip)d).ApplySeverityChrome()));

    public static readonly DependencyProperty ShowSaveButtonProperty =
        DependencyProperty.Register(nameof(ShowSaveButton), typeof(bool), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(true, (d, _) => ((SettingsUnsavedNoticeStrip)d).UpdateInlineButtonsVisibility()));

    public static readonly DependencyProperty ShowActionButtonProperty =
        DependencyProperty.Register(nameof(ShowActionButton), typeof(bool), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(false, (d, _) => ((SettingsUnsavedNoticeStrip)d).UpdateInlineButtonsVisibility()));

    public static readonly DependencyProperty ActionButtonTextProperty =
        DependencyProperty.Register(nameof(ActionButtonText), typeof(string), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(string.Empty, (d, e) =>
            {
                if (d is SettingsUnsavedNoticeStrip strip)
                    strip.ActionButton.Content = e.NewValue as string ?? string.Empty;
            }));

    public static readonly DependencyProperty FloatingOverlayChromeProperty =
        DependencyProperty.Register(nameof(FloatingOverlayChrome), typeof(bool), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(false, (d, _) => ((SettingsUnsavedNoticeStrip)d).ApplySeverityChrome()));

    /// <summary>
    /// Не использовать «зеркало-только» для informational (зелёных) notice: всегда закреплять полосу
    /// в layout страницы (зеркало остаётся отражением при уходе якоря из зоны видимости). Только для «Обзора».
    /// </summary>
    public static readonly DependencyProperty DisableInformationalMirrorProperty =
        DependencyProperty.Register(nameof(DisableInformationalMirror), typeof(bool), typeof(SettingsUnsavedNoticeStrip),
            new PropertyMetadata(false));

    public event EventHandler? SaveRequested;
    public event EventHandler? ActionRequested;
    /// <summary>Начало закрытия по крестику — до анимации (чтобы shell-debounce не переоткрыл notice).</summary>
    public event EventHandler? DismissStarting;
    public event EventHandler? DismissRequested;

    public SettingsUnsavedNoticeStrip()
    {
        InitializeComponent();
        SaveButton.Content = GlassSettingsResources.Get("CmdSave");
        if (Application.Current.Resources["GlassSettingsNoticeSaveButtonStyle"] is Style saveStyle)
            SaveButton.Style = saveStyle;
        SaveButton.Click += (_, _) => SaveRequested?.Invoke(this, EventArgs.Empty);
        ActionButton.Click += (_, _) => ActionRequested?.Invoke(this, EventArgs.Empty);
        GlassSettingsToolTipService.SetToolTip(CloseButton, GlassSettingsResources.Get("Dialog_Close"));
        CloseButton.PointerEntered += CloseButton_OnPointerEntered;
        CloseButton.PointerExited += CloseButton_OnPointerExited;
        CloseButton.PointerCanceled += CloseButton_OnPointerExited;
        Loaded += (_, _) =>
        {
            ApplySeverityChrome();
            UpdateMessageMaxWidth();
        };
        ActualThemeChanged += (_, _) => ApplySeverityChrome();
        ApplySeverityChrome();
        UpdateCloseVisibility();
        UpdateInlineButtonsVisibility();
        ResetAnimationState();
    }

    public string Message
    {
        get => (string)GetValue(MessageProperty);
        set => SetValue(MessageProperty, value);
    }

    public bool AllowDismiss
    {
        get => (bool)GetValue(AllowDismissProperty);
        set => SetValue(AllowDismissProperty, value);
    }

    public bool IsStripOpen
    {
        get => (bool)GetValue(IsStripOpenProperty);
        set => SetIsStripOpen(value);
    }

    public bool IsSaveEnabled
    {
        get => (bool)GetValue(IsSaveEnabledProperty);
        set => SetValue(IsSaveEnabledProperty, value);
    }

    public SettingsNoticeSeverity Severity
    {
        get => (SettingsNoticeSeverity)GetValue(SeverityProperty);
        set => SetValue(SeverityProperty, value);
    }

    public bool ShowSaveButton
    {
        get => (bool)GetValue(ShowSaveButtonProperty);
        set => SetValue(ShowSaveButtonProperty, value);
    }

    public bool ShowActionButton
    {
        get => (bool)GetValue(ShowActionButtonProperty);
        set => SetValue(ShowActionButtonProperty, value);
    }

    public string ActionButtonText
    {
        get => (string)GetValue(ActionButtonTextProperty);
        set => SetValue(ActionButtonTextProperty, value);
    }

    public bool FloatingOverlayChrome
    {
        get => (bool)GetValue(FloatingOverlayChromeProperty);
        set => SetValue(FloatingOverlayChromeProperty, value);
    }

    public bool DisableInformationalMirror
    {
        get => (bool)GetValue(DisableInformationalMirrorProperty);
        set => SetValue(DisableInformationalMirrorProperty, value);
    }

    /// <summary>Стабильный ключ подавления зеркала для pinned-error notice.</summary>
    internal string? MirrorSuppressKey { get; set; }

    public bool IsVisuallyOpen => _visuallyOpen;

    internal void NotifySaveRequested() => SaveRequested?.Invoke(this, EventArgs.Empty);

    internal void NotifyActionRequested() => ActionRequested?.Invoke(this, EventArgs.Empty);

    public void ApplyContent(SettingsNoticeSeverity severity, string message, bool showSave, bool showAction = false, string? actionText = null)
    {
        Severity = severity;
        Message = message;
        ShowSaveButton = showSave;
        ShowActionButton = showAction;
        if (!string.IsNullOrWhiteSpace(actionText))
            ActionButtonText = actionText;
    }

    public async Task OpenAnimatedAsync(
        SettingsNoticeSeverity severity,
        string message,
        bool showSave,
        bool showAction = false,
        string? actionText = null)
    {
        if (FloatingOverlayChrome)
        {
            OpenImmediate(severity, message, showSave, showAction, actionText);
            return;
        }

        if (_visuallyOpen)
            CloseImmediate();

        ApplyContent(severity, message, showSave, showAction, actionText);
        Visibility = Visibility.Visible;
        _visuallyOpen = true;
        SetIsStripOpen(true);
        PrepareSlideInStart();
        UpdateMessageMaxWidth();
        await AnimateSlideInAsync();
    }

    public async Task CloseAnimatedAsync()
    {
        if (!_visuallyOpen)
        {
            Visibility = Visibility.Collapsed;
            SetIsStripOpen(false);
            return;
        }

        // Любое скрытие штатной полосы — через compensate (dirty clean, X, coordinator…).
        if (!FloatingOverlayChrome && !NoticeViewportMirrorService.IsInScrollPreservingClose)
        {
            await NoticeViewportMirrorService.ClosePinnedSourcePreservingScrollAsync(this, animate: true)
                .ConfigureAwait(true);
            return;
        }

        await FadeOutForPreservingCloseAsync().ConfigureAwait(true);
        CloseImmediateCore();
    }

    public Task ReplaceAnimatedAsync(SettingsNoticeSeverity severity, string message, bool showSave) =>
        OpenAnimatedAsync(severity, message, showSave);

    public void CloseImmediate()
    {
        if (!_visuallyOpen)
        {
            Visibility = Visibility.Collapsed;
            SetIsStripOpen(false);
            return;
        }

        // Факт скрытия (не только крестик): dirty clean / restore / policy-hide.
        if (!FloatingOverlayChrome && !NoticeViewportMirrorService.IsInScrollPreservingClose)
        {
            NoticeViewportMirrorService.ClosePinnedSourcePreservingScroll(this);
            return;
        }

        CloseImmediateCore();
    }

    /// <summary>Только визуальный collapse — без повторного входа в ClosePinned*.</summary>
    internal void CloseImmediateCore()
    {
        StopActiveStoryboard();
        Visibility = Visibility.Collapsed;
        ResetAnimationState();
        _visuallyOpen = false;
        SetIsStripOpen(false);
    }

    /// <summary>Fade до collapse в ClosePinned*(animate); высота layout ещё полная.</summary>
    internal Task FadeOutForPreservingCloseAsync() => AnimateFadeOutAsync();

    public void OpenImmediate(SettingsNoticeSeverity severity, string message, bool showSave, bool showAction = false, string? actionText = null)
    {
        // Во время slide-in только обновляем контент — Stop() иначе оставляет Transform на Y=-MinHeight.
        if (_isAnimating)
        {
            ApplyContent(severity, message, showSave, showAction, actionText);
            UpdateMessageMaxWidth();
            _visuallyOpen = true;
            SetIsStripOpen(true);
            return;
        }

        StopActiveStoryboard();
        ApplyContent(severity, message, showSave, showAction, actionText);
        Visibility = Visibility.Visible;
        ResetAnimationState();
        UpdateMessageMaxWidth();
        _visuallyOpen = true;
        SetIsStripOpen(true);
    }

    /// <summary>
    /// Устарело для pinned-политики: крестик зеркала больше не закрывает штатную полосу
    /// (см. MainWindow.MirrorStrip_OnDismissRequested → HideMirrorImmediate + epoch).
    /// </summary>
    internal void RequestDismissFromMirror()
    {
        if (!_visuallyOpen)
            return;

        _ = DismissAnimatedAsync();
    }

    /// <summary>Закрытие информационного notice: полоса не была в layout, только зеркало.</summary>
    internal void NotifyDismissFromEphemeral()
    {
        DismissStarting?.Invoke(this, EventArgs.Empty);
        DismissRequested?.Invoke(this, EventArgs.Empty);
    }

    private async Task DismissAnimatedAsync()
    {
        if (!_visuallyOpen)
            return;

        DismissStarting?.Invoke(this, EventArgs.Empty);
        await CloseAnimatedAsync().ConfigureAwait(true);
        DismissRequested?.Invoke(this, EventArgs.Empty);
    }

    /// <summary>Исчезновение зеркала: снизу вверх (обратно к slide-in).</summary>
    public async Task CloseSlideUpAnimatedAsync()
    {
        if (!_visuallyOpen)
        {
            Visibility = Visibility.Collapsed;
            SetIsStripOpen(false);
            return;
        }

        if (_isAnimating)
            StopActiveStoryboard();

        await WaitForLayoutAsync();

        var storyboard = new Storyboard();

        var fade = new DoubleAnimation
        {
            From = NoticeBorder.Opacity,
            To = 0,
            Duration = TimeSpan.FromMilliseconds(SlideInMs),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
        };
        Storyboard.SetTarget(fade, NoticeBorder);
        Storyboard.SetTargetProperty(fade, nameof(UIElement.Opacity));
        storyboard.Children.Add(fade);

        var slide = new DoubleAnimation
        {
            From = RootTransform.Y,
            To = SlideOffsetY,
            Duration = TimeSpan.FromMilliseconds(SlideInMs),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
        };
        Storyboard.SetTarget(slide, this);
        Storyboard.SetTargetProperty(slide, "(UIElement.RenderTransform).(TranslateTransform.Y)");
        storyboard.Children.Add(slide);

        await RunStoryboardAsync(storyboard);
        Visibility = Visibility.Collapsed;
        ResetAnimationState();
        _visuallyOpen = false;
        SetIsStripOpen(false);
    }

    private void SetIsStripOpen(bool value) => SetValue(IsStripOpenProperty, value);

    /// <summary>Перечитывает severity-оформление после смены темы.</summary>
    public void RefreshSeverityChrome() => ApplySeverityChrome();

    private void ApplySeverityChrome()
    {
        if (FloatingOverlayChrome)
        {
            ApplyFloatingOverlayChrome();
            return;
        }

        Background = null;
        if (Severity == SettingsNoticeSeverity.Success)
        {
            NoticeBorder.Background = ResolveThemeBrush("SystemFillColorSuccessBackground");
            SeverityIcon.Glyph = "\uE73E";
            SeverityIcon.Foreground = ResolveThemeBrush("SystemFillColorSuccessBrush");
            return;
        }

        if (Severity == SettingsNoticeSeverity.Warning)
        {
            NoticeBorder.Background = ResolveThemeBrush("SystemFillColorCautionBackground");
            SeverityIcon.Glyph = "\uE7BA";
            SeverityIcon.Foreground = ResolveThemeBrush("SystemFillColorCautionBrush");
            return;
        }

        NoticeBorder.Background = ResolveThemeBrush("SystemFillColorCriticalBackground");
        SeverityIcon.Glyph = "\uE783";
        SeverityIcon.Foreground = ResolveThemeBrush("SystemFillColorCriticalBrush");
    }

    private void ApplyFloatingOverlayChrome()
    {
        // Severity чуть плотнее; акрил — на host Border (WindowTopNoticeColumn).
        Background = null;
        NoticeBorder.Opacity = 1;

        if (Severity == SettingsNoticeSeverity.Success)
        {
            NoticeBorder.Background = ResolveDenserSeverityBrush("SystemFillColorSuccessBackground");
            SeverityIcon.Glyph = "\uE73E";
            SeverityIcon.Foreground = ResolveThemeBrush("SystemFillColorSuccessBrush");
            return;
        }

        if (Severity == SettingsNoticeSeverity.Warning)
        {
            NoticeBorder.Background = ResolveDenserSeverityBrush("SystemFillColorCautionBackground");
            SeverityIcon.Glyph = "\uE7BA";
            SeverityIcon.Foreground = ResolveThemeBrush("SystemFillColorCautionBrush");
            return;
        }

        NoticeBorder.Background = ResolveDenserSeverityBrush("SystemFillColorCriticalBackground");
        SeverityIcon.Glyph = "\uE783";
        SeverityIcon.Foreground = ResolveThemeBrush("SystemFillColorCriticalBrush");
    }

    private static Brush ResolveDenserSeverityBrush(string resourceKey)
    {
        var brush = AppThemeVisualResolver.ResolveThemeBrush(resourceKey);
        if (brush is SolidColorBrush solid)
        {
            var c = solid.Color;
            var alpha = (byte)Math.Min(255, (int)MathF.Round(c.A * 1.35f));
            return new SolidColorBrush(Color.FromArgb(alpha, c.R, c.G, c.B));
        }

        return brush;
    }

    private static Brush ResolveThemeBrush(string resourceKey) =>
        AppThemeVisualResolver.ResolveThemeBrush(resourceKey);

    private void UpdateCloseVisibility()
    {
        CloseButton.Visibility = AllowDismiss ? Visibility.Visible : Visibility.Collapsed;
        UpdateMessageMaxWidth();
    }

    private void UpdateInlineButtonsVisibility()
    {
        SaveButton.Visibility = ShowSaveButton ? Visibility.Visible : Visibility.Collapsed;
        ActionButton.Visibility = ShowActionButton ? Visibility.Visible : Visibility.Collapsed;
        if (ShowActionButton && Application.Current.Resources["GlassSettingsNoticeSaveButtonStyle"] is Style actionStyle)
            ActionButton.Style = actionStyle;
        UpdateMessageMaxWidth();
    }

    private void OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateMessageMaxWidth();

    private void UpdateMessageMaxWidth()
    {
        if (ActualWidth <= 0)
            return;

        const double columnGap = 12;
        const double borderPadding = 24;
        var iconWidth = 16;
        var closeWidth = CloseButton.Visibility == Visibility.Visible ? 32 : 0;
        var saveWidth = SaveButton.Visibility == Visibility.Visible ? Math.Max(SaveButton.ActualWidth, 88) : 0;
        var actionWidth = ActionButton.Visibility == Visibility.Visible ? Math.Max(ActionButton.ActualWidth, 88) : 0;
        var inlineGap = (SaveButton.Visibility == Visibility.Visible || ActionButton.Visibility == Visibility.Visible) ? 20 : 0;
        var reserved = borderPadding + iconWidth + columnGap + closeWidth + columnGap + saveWidth + actionWidth + inlineGap + columnGap;
        MessageText.MaxWidth = Math.Max(120, ActualWidth - reserved);
    }

    private void PrepareSlideInStart()
    {
        NoticeBorder.Opacity = 0;
        RootTransform.Y = SlideOffsetY;
    }

    private void ResetAnimationState()
    {
        NoticeBorder.Opacity = 1;
        RootTransform.Y = 0;
    }

    private async Task AnimateFadeOutAsync()
    {
        if (_isAnimating)
            StopActiveStoryboard();

        var storyboard = new Storyboard();
        var fade = new DoubleAnimation
        {
            From = NoticeBorder.Opacity,
            To = 0,
            Duration = TimeSpan.FromMilliseconds(FadeOutMs),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
        };
        Storyboard.SetTarget(fade, NoticeBorder);
        Storyboard.SetTargetProperty(fade, nameof(UIElement.Opacity));
        storyboard.Children.Add(fade);
        await RunStoryboardAsync(storyboard);
    }

    private async Task AnimateSlideInAsync()
    {
        if (_isAnimating)
            StopActiveStoryboard();

        await WaitForLayoutAsync();

        var storyboard = new Storyboard();

        var fade = new DoubleAnimation
        {
            From = 0,
            To = 1,
            Duration = TimeSpan.FromMilliseconds(SlideInMs),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
        };
        Storyboard.SetTarget(fade, NoticeBorder);
        Storyboard.SetTargetProperty(fade, nameof(UIElement.Opacity));
        storyboard.Children.Add(fade);

        var slide = new DoubleAnimation
        {
            From = SlideOffsetY,
            To = 0,
            Duration = TimeSpan.FromMilliseconds(SlideInMs),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut }
        };
        Storyboard.SetTarget(slide, this);
        Storyboard.SetTargetProperty(slide, "(UIElement.RenderTransform).(TranslateTransform.Y)");
        storyboard.Children.Add(slide);

        await RunStoryboardAsync(storyboard);
        ResetAnimationState();
    }

    private async Task WaitForLayoutAsync()
    {
        var queue = DispatcherQueue.GetForCurrentThread();
        if (queue is null)
            return;

        var tcs = new TaskCompletionSource<bool>();
        queue.TryEnqueue(DispatcherQueuePriority.Low, () => tcs.TrySetResult(true));
        await tcs.Task;
        UpdateLayout();
    }

    private Task RunStoryboardAsync(Storyboard storyboard)
    {
        var tcs = new TaskCompletionSource<bool>();
        _storyboardTcs = tcs;
        _isAnimating = true;
        _activeStoryboard = storyboard;

        void OnCompleted(object? sender, object e)
        {
            storyboard.Completed -= OnCompleted;
            if (!ReferenceEquals(_activeStoryboard, storyboard))
                return;

            _isAnimating = false;
            _activeStoryboard = null;
            _storyboardTcs = null;
            tcs.TrySetResult(true);
        }

        storyboard.Completed += OnCompleted;
        storyboard.Begin();
        return tcs.Task;
    }

    private void StopActiveStoryboard()
    {
        var board = _activeStoryboard;
        var tcs = _storyboardTcs;
        _activeStoryboard = null;
        _storyboardTcs = null;
        _isAnimating = false;
        if (board is not null)
        {
            try
            {
                board.Stop();
            }
            catch
            {
                /* ignore */
            }
        }

        // Stop() часто не шлёт Completed — разблокируем await, иначе OpenAnimated «висит» на Y=-MinHeight.
        tcs?.TrySetResult(true);
        ResetAnimationState();
    }

    private async void CloseButton_OnClick(object sender, RoutedEventArgs e)
    {
        NoticeMirrorTrace.Write($"CloseButton_OnClick: FloatingOverlayChrome={FloatingOverlayChrome}");
        if (FloatingOverlayChrome)
        {
            NoticeMirrorTrace.Write("CloseButton_OnClick: зеркало — DismissStarting/DismissRequested (БЕЗ HideMirrorImmediate)");
            DismissStarting?.Invoke(this, EventArgs.Empty);
            DismissRequested?.Invoke(this, EventArgs.Empty);
            return;
        }

        StopActiveStoryboard();
        NoticeMirrorTrace.Write(
            "CloseButton_OnClick: штатная полоса — HideMirrorImmediate + ClosePinnedPreservingScroll");
        NoticeViewportMirrorService.HideMirrorImmediate();
        DismissStarting?.Invoke(this, EventArgs.Empty);
        await NoticeViewportMirrorService.ClosePinnedSourcePreservingScrollAsync(this, animate: true)
            .ConfigureAwait(true);
        DismissRequested?.Invoke(this, EventArgs.Empty);
    }

    private void CloseButton_OnPointerEntered(object sender, PointerRoutedEventArgs e)
    {
        if (!FloatingOverlayChrome)
            return;

        CloseButton.Background = ResolveThemeBrush("ControlFillColorSecondaryBrush");
    }

    private void CloseButton_OnPointerExited(object sender, PointerRoutedEventArgs e)
    {
        if (!FloatingOverlayChrome)
            return;

        CloseButton.ClearValue(BackgroundProperty);
    }
}
