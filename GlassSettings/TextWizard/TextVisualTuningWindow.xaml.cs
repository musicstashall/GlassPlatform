using GlassSettings.Controls;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Text;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Animation;
using System.Runtime.InteropServices;
using WinRT.Interop;

namespace GlassSettings.TextWizard;

public sealed partial class TextVisualTuningWindow : Window
{
    private static readonly int WindowWidth = HeroPreviewLayoutMetrics.ResolveWizardWindowWidth();
    private const int WindowHeight = 712;
    private const double WizardCardWidth = 316;
    private const int PageTransitionDurationMs = 500;
    private const double FooterButtonHeight = 32;
    /// <summary>Равный зазор: hint→разделитель, разделитель→список, список→футер.</summary>
    private const double WizardChromeSectionGap = 12;
    /// <summary>Сдвиг нижнего разделителя вверх без перемещения кнопок (margin bottom компенсирует).</summary>
    private const double FooterDividerLift = 12;
    /// <summary>Футер: 2× зазор до разделителя + высота кнопок.</summary>
    private static readonly double FooterChromeHeight = WizardChromeSectionGap * 2 + FooterButtonHeight;
    private const double WizardInfoHintLineHeight = 20;
    private const int WizardInfoHintLines = 4;
    private const double WizardInfoHintPaddingVertical = 24;
    private static readonly double WizardInfoHintAreaHeight =
        WizardInfoHintLines * WizardInfoHintLineHeight + WizardInfoHintPaddingVertical;
    private const double WizardPromptRowHeight = 32;
    private static readonly double WizardInfoBlockHeight =
        WizardPromptRowHeight + WizardInfoHintAreaHeight + WizardChromeSectionGap;
    private static readonly double WizardCardListAreaHeight = HeroPreviewLayoutMetrics.WizardCardListAreaHeight;
    private static readonly double WizardCardListHostWidth = HeroPreviewLayoutMetrics.WizardCardListHostWidth;

    private readonly TaskCompletionSource<bool> _closeTcs = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TextVisualTuningWizardEngine _engine;
    private readonly string _sampleText;
    private bool _isDarkTheme;
    private readonly WizardPreviewCacheStore _previewCache = new();
    private readonly WizardPreviewMapRenderer _mapRenderer;
    private ModalChildWindowScope? _modalScope;
    private WizardPageView? _currentPageView;
    private bool _chromeReady;
    private bool _pageTransitionRunning;
    private bool _navPagePopulated;
    private WizardPageView? _navPopulateView;
    private bool _themeSubclassInstalled;
    private bool _themeReloadRunning;
    private Win32WindowSubclass.Proc? _themeSubclassProc;
    private IntPtr _themeSubclassFn;
    private const uint WmThemeChanged = 0x031A;
    private static readonly UIntPtr ThemeSubclassId = new(0x4753_5749); // GSWI

    public TextVisualTuningWindow(Window owner, GlassInkCapsuleModel initial, string sampleText, bool isDarkTheme)
    {
        InitializeComponent();
        ApplyDialogContentMargins();
        _engine = new TextVisualTuningWizardEngine(initial);
        _sampleText = sampleText;
        _isDarkTheme = isDarkTheme;
        _mapRenderer = new WizardPreviewMapRenderer(_engine, _sampleText, _isDarkTheme);

        AppTitleBar.Title = GlassSettingsResources.Get("Text_InkWizardTitle");
        CancelButton.Content = GlassSettingsResources.Get("Text_InkWizardCancel");
        NextButton.Content = GlassSettingsResources.Get("Text_InkWizardNext");
        GlassSettingsToolTipService.SetToolTip(
            TitleBarBackButton,
            GlassSettingsResources.Get("Text_InkWizardBack"));

        ConfigureWindowChrome();
        ApplyAcrylicBackdrop();
        Activated += OnActivated;
        Activated += OnInstallThemeSubclassOnce;
        Closed += OnClosed;
        DispatcherQueue.TryEnqueue(TryInstallThemeSubclass);

        _engine.SelectionChanged += OnEngineSelectionChanged;
        _modalScope = new ModalChildWindowScope(owner, this);
        _ = LoadInitialPageAsync();
    }

    public Task<bool> WaitForCloseAsync() => _closeTcs.Task;

    public GlassInkCapsuleModel ResultCapsule => _engine.Working;

    private void OnEngineSelectionChanged(int stepIndex)
    {
        if (_previewCache.GetPage(stepIndex) is { } currentSlot)
            WizardPreviewSelectionSync.ApplyEngineToSlot(currentSlot, _engine, stepIndex);

        _previewCache.InvalidateFrom(stepIndex + 1);

        _mapRenderer.SyncSelectionChrome();
        _mapRenderer.ScrollBottomRowToTopColumn();

        if (_currentPageView is not null)
            ApplyPageChromeTexts(_currentPageView);
    }

    private async Task LoadInitialPageAsync()
    {
        var view = CreatePageViewShell(showLoadingRing: true);
        ApplyPageChromeTexts(view);
        PageTransitionHost.Children.Clear();
        PageTransitionHost.Children.Add(view.Root);
        _currentPageView = view;
        UpdateNavigationChrome();

        var slot = await EnsurePageSlotAsync(_engine.StepIndex).ConfigureAwait(true);
        PopulateFromMap(view, slot);
        RevealPopulatedPage(view);
    }

    private async Task<WizardPreviewPageSlot> EnsurePageSlotAsync(int stepIndex)
    {
        if (_previewCache.TryGetCompletePage(stepIndex, out var cached))
            return cached;

        var slot = WizardPreviewSlotBuilder.Build(_engine, stepIndex);

        await WizardPreviewCacheStore.RenderAllCellsAsync(slot, _sampleText, _isDarkTheme).ConfigureAwait(false);
        _previewCache.SetPage(stepIndex, slot);

        return slot;
    }

    private void PopulateFromMap(WizardPageView view, WizardPreviewPageSlot slot)
    {
        WizardPreviewSelectionSync.RestoreEngineFromSlot(_engine, slot, _engine.StepIndex);

        view.CardsPanel.Children.Clear();
        _mapRenderer.Reset();
        if (!view.CardArea.Children.Contains(view.CardsPanel))
            view.CardArea.Children.Insert(0, view.CardsPanel);
        view.CardsPanel.Visibility = Visibility.Collapsed;
        _mapRenderer.Populate(view.CardsPanel, slot);
    }

    private void OnActivated(object sender, WindowActivatedEventArgs args)
    {
        if (_chromeReady || args.WindowActivationState == WindowActivationState.Deactivated)
            return;

        _chromeReady = true;
        try
        {
            ExtendsContentIntoTitleBar = true;
            SetTitleBar(AppTitleBar);
        }
        catch
        {
            // TitleBar API может быть недоступен
        }

        TitleBarThemeHelper.Apply(this, extendsContentIntoTitleBar: true);
    }

    private void OnClosed(object sender, WindowEventArgs args)
    {
        _engine.SelectionChanged -= OnEngineSelectionChanged;
        DetachThemeSubclass();
        _modalScope?.Dispose();
        _modalScope = null;
    }

    private void OnInstallThemeSubclassOnce(object sender, WindowActivatedEventArgs args)
    {
        Activated -= OnInstallThemeSubclassOnce;
        TryInstallThemeSubclass();
    }

    private void TryInstallThemeSubclass()
    {
        if (_themeSubclassInstalled)
            return;

        var hwnd = WindowNative.GetWindowHandle(this);
        if (hwnd == IntPtr.Zero)
            return;

        _themeSubclassProc ??= OnThemeSubclassMessage;
        if (!Win32WindowSubclass.Install(hwnd, _themeSubclassProc, ThemeSubclassId, out _themeSubclassFn))
            return;

        _themeSubclassInstalled = true;
    }

    private void DetachThemeSubclass()
    {
        if (!_themeSubclassInstalled)
            return;

        var hwnd = WindowNative.GetWindowHandle(this);
        if (hwnd != IntPtr.Zero && _themeSubclassFn != IntPtr.Zero)
        {
            Win32WindowSubclass.Remove(hwnd, _themeSubclassFn, ThemeSubclassId);
            _themeSubclassFn = IntPtr.Zero;
        }

        _themeSubclassInstalled = false;
    }

    private IntPtr OnThemeSubclassMessage(
        IntPtr hWnd,
        uint uMsg,
        IntPtr wParam,
        IntPtr lParam,
        UIntPtr uIdSubclass,
        UIntPtr dwRefData)
    {
        if (uMsg == WmThemeChanged)
            DispatcherQueue.TryEnqueue(() => _ = OnVisualThemeChangedAsync());

        return Win32WindowSubclass.CallDef(hWnd, uMsg, wParam, lParam);
    }

    private async Task OnVisualThemeChangedAsync()
    {
        if (_themeReloadRunning || _pageTransitionRunning || _currentPageView is null)
            return;

        _themeReloadRunning = true;
        SetNavigationEnabled(false);

        try
        {
            var isDarkTheme = ThemeRegistry.IsDarkMode();
            var markerSnapshots = _previewCache.CaptureAllPageMarkers();
            _previewCache.ClearAll();

            _isDarkTheme = isDarkTheme;
            _mapRenderer.SetIsDarkTheme(isDarkTheme);

            var currentStep = _engine.StepIndex;
            for (var pageIndex = 0; pageIndex <= currentStep; pageIndex++)
            {
                var slot = WizardPreviewSlotBuilder.Build(_engine, pageIndex);
                if (pageIndex < markerSnapshots.Count && markerSnapshots[pageIndex] is { } snapshot)
                    snapshot.ApplyTo(slot.Root);

                await WizardPreviewCacheStore.RenderAllCellsAsync(slot, _sampleText, _isDarkTheme).ConfigureAwait(true);
                _previewCache.SetPage(pageIndex, slot);
            }

            RefreshThemeChrome();

            if (_previewCache.GetPage(currentStep) is not { } currentSlot)
                return;

            PopulateFromMap(_currentPageView, currentSlot);
            RevealPopulatedPage(_currentPageView);
        }
        finally
        {
            _themeReloadRunning = false;
            SetNavigationEnabled(true);
            TitleBarBackButton.IsEnabled = _engine.CanGoBack;
        }
    }

    private void RefreshThemeChrome()
    {
        if (_currentPageView is not null)
            ApplyPromptThemeBrush(_currentPageView.PromptText);

        try
        {
            TitleBarThemeHelper.Apply(this, extendsContentIntoTitleBar: true);
        }
        catch
        {
            // ignore
        }
    }

    private static void ApplyPromptThemeBrush(TextBlock promptText) =>
        promptText.Foreground = AppThemeVisualResolver.ResolveThemeBrush("TextFillColorSecondaryBrush");

    private void ConfigureWindowChrome()
    {
        var hwnd = WindowNative.GetWindowHandle(this);
        var windowId = Win32Interop.GetWindowIdFromWindow(hwnd);
        var appWindow = AppWindow.GetFromWindowId(windowId);
        appWindow.Resize(new Windows.Graphics.SizeInt32(WindowWidth, WindowHeight));

        if (appWindow.Presenter is OverlappedPresenter presenter)
        {
            presenter.IsResizable = false;
            presenter.IsMaximizable = false;
            presenter.PreferredMinimumWidth = WindowWidth;
            presenter.PreferredMinimumHeight = WindowHeight;
            presenter.PreferredMaximumWidth = WindowWidth;
            presenter.PreferredMaximumHeight = WindowHeight;
        }

        try
        {
            appWindow.TitleBar.PreferredHeightOption = TitleBarHeightOption.Tall;
        }
        catch
        {
            // ignore
        }
    }

    private void ApplyDialogContentMargins()
    {
        var pad = HostedDialogLayout.GetContentDialogPadding();
        BodyContentHost.Margin = new Thickness(0);
        FooterDivider.Margin = new Thickness(
            0,
            WizardChromeSectionGap - FooterDividerLift,
            0,
            FooterDividerLift);
        FooterChromeHost.Height = FooterChromeHeight;
        FooterChromeHost.MinHeight = FooterChromeHeight;
        FooterChromeHost.MaxHeight = FooterChromeHeight;
        FooterButtonHost.Margin = new Thickness(pad.Left, 0, pad.Right, 0);
    }

    private void ApplyAcrylicBackdrop()
    {
        try
        {
            SystemBackdrop = new DesktopAcrylicBackdrop();
        }
        catch
        {
            // оставляем фон по умолчанию
        }
    }

    private void ApplyPageChromeTexts(WizardPageView view)
    {
        view.PromptText.Text = FormatWizardPrompt(
            GlassSettingsResources.Get("Text_InkWizardPrompt"),
            _engine.StepIndex + 1,
            _engine.GetPromptStepCount());
        view.HintText.Text = GlassSettingsResources.Get(_engine.GetHint());
    }

    private void ApplyTransitionPageChromeTexts(WizardPageView view, bool forward)
    {
        var step = forward
            ? Math.Min(_engine.StepIndex + 1, Math.Max(0, _engine.StepCount - 1))
            : Math.Max(_engine.StepIndex - 1, 0);

        view.PromptText.Text = FormatWizardPrompt(
            GlassSettingsResources.Get("Text_InkWizardPrompt"),
            step + 1,
            _engine.GetPromptStepCount(step));
        view.HintText.Text = GlassSettingsResources.Get(_engine.GetHintForStep(step));
    }

    private void UpdateNavigationChrome()
    {
        TitleBarBackButton.IsEnabled = _engine.CanGoBack && !_pageTransitionRunning;
        NextButton.Content = GlassSettingsResources.Get("Text_InkWizardNext");
    }

    private static string FormatWizardPrompt(string template, int currentStep, int totalSteps)
    {
        var result = template;
        ReplaceNextPercentD(ref result, currentStep);
        ReplaceNextPercentD(ref result, totalSteps);
        return result;
    }

    private static void ReplaceNextPercentD(ref string text, int value)
    {
        var index = text.IndexOf("%d", StringComparison.Ordinal);
        if (index < 0)
            return;

        text = string.Concat(text.AsSpan(0, index), value.ToString(), text.AsSpan(index + 2));
    }

    private void SetNavigationEnabled(bool enabled)
    {
        TitleBarBackButton.IsEnabled = enabled && _engine.CanGoBack;
        CancelButton.IsEnabled = enabled;
        NextButton.IsEnabled = enabled;
    }

    private static WizardPageView CreatePageViewShell(bool showLoadingRing = true)
    {
        var promptText = new TextBlock
        {
            Margin = new Thickness(0, 0, 0, 12),
            HorizontalAlignment = HorizontalAlignment.Center,
            TextAlignment = TextAlignment.Center,
            TextWrapping = TextWrapping.Wrap,
            FontSize = 14,
        };
        ApplyPromptThemeBrush(promptText);

        var hintText = new TextBlock
        {
            FontSize = 14,
            MaxLines = WizardInfoHintLines,
            TextTrimming = TextTrimming.WordEllipsis,
            TextWrapping = TextWrapping.Wrap,
            VerticalAlignment = VerticalAlignment.Top,
            Style = (Style)Application.Current.Resources["BodyTextBlockStyle"],
        };

        var hintBorder = new Border
        {
            Height = WizardInfoHintAreaHeight,
            MinHeight = WizardInfoHintAreaHeight,
            MaxHeight = WizardInfoHintAreaHeight,
            Margin = new Thickness(0, 0, 0, WizardChromeSectionGap),
            Padding = new Thickness(12, 12, 12, 12),
            Background = null,
            BorderThickness = new Thickness(0),
            Child = hintText,
        };

        var sectionDivider = new Border
        {
            Height = 1,
            Margin = new Thickness(0, 0, 0, WizardChromeSectionGap),
            HorizontalAlignment = HorizontalAlignment.Stretch,
            Background = (Brush)Application.Current.Resources["DividerStrokeColorDefaultBrush"],
        };

        var cardsPanel = new StackPanel
        {
            Spacing = 16,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            Visibility = showLoadingRing ? Visibility.Collapsed : Visibility.Visible,
        };

        var cardArea = new Grid
        {
            Height = WizardCardListAreaHeight,
            MinHeight = WizardCardListAreaHeight,
            MaxHeight = WizardCardListAreaHeight,
            Width = WizardCardListHostWidth,
            MinWidth = WizardCardListHostWidth,
            MaxWidth = WizardCardListHostWidth,
            HorizontalAlignment = HorizontalAlignment.Center,
        };

        Grid? loadingOverlay = null;
        ProgressRing? loadingRing = null;
        if (showLoadingRing)
        {
            loadingRing = new ProgressRing
            {
                IsActive = true,
                Width = 48,
                Height = 48,
                HorizontalAlignment = HorizontalAlignment.Center,
                VerticalAlignment = VerticalAlignment.Center,
            };

            loadingOverlay = new Grid();
            loadingOverlay.Children.Add(loadingRing);
        }

        cardArea.Children.Add(cardsPanel);
        if (loadingOverlay is not null)
            cardArea.Children.Add(loadingOverlay);

        var infoBlock = new StackPanel
        {
            Width = WizardCardListHostWidth,
            Height = WizardInfoBlockHeight,
            MinHeight = WizardInfoBlockHeight,
            MaxHeight = WizardInfoBlockHeight,
            HorizontalAlignment = HorizontalAlignment.Center,
        };
        infoBlock.Children.Add(promptText);
        infoBlock.Children.Add(hintBorder);

        var root = new Grid
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Top,
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        Grid.SetRow(infoBlock, 0);
        Grid.SetRow(sectionDivider, 1);
        Grid.SetRow(cardArea, 2);
        root.Children.Add(infoBlock);
        root.Children.Add(sectionDivider);
        root.Children.Add(cardArea);

        return new WizardPageView
        {
            Root = root,
            PromptText = promptText,
            HintText = hintText,
            CardArea = cardArea,
            CardsPanel = cardsPanel,
            LoadingOverlay = loadingOverlay,
            LoadingRing = loadingRing,
        };
    }

    private void RevealPopulatedPage(WizardPageView view)
    {
        if (view.LoadingOverlay is not null)
            view.CardArea.Children.Remove(view.LoadingOverlay);

        if (view.LoadingRing is not null)
            view.LoadingRing.IsActive = false;
        view.CardsPanel.Visibility = Visibility.Visible;

        _mapRenderer.ScheduleScrollBottomRowToTopColumn();
    }

    private async void TitleBarBackButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (!_engine.CanGoBack || _pageTransitionRunning)
            return;

        await NavigateWithTransitionAsync(forward: false);
    }

    private async void NextButton_OnClick(object sender, RoutedEventArgs e)
    {
        if (_pageTransitionRunning)
            return;

        if (_engine.IsLastStep)
        {
            _engine.GoNext();
            _engine.FinalizeWorkingCapsule();
            CloseAccepted();
            return;
        }

        await NavigateWithTransitionAsync(forward: true);
    }

    private async Task NavigateWithTransitionAsync(bool forward)
    {
        if (_currentPageView is null)
        {
            await LoadInitialPageAsync().ConfigureAwait(true);
            return;
        }

        _pageTransitionRunning = true;
        SetNavigationEnabled(false);
        _navPagePopulated = false;
        _navPopulateView = null;

        try
        {
            var travel = ResolvePageTransitionDistance();
            var outgoingRoot = _currentPageView.Root;

            var targetStep = forward
                ? Math.Min(_engine.StepIndex + 1, Math.Max(0, _engine.StepCount - 1))
                : Math.Max(_engine.StepIndex - 1, 0);
            var cacheHitBeforeNav = _previewCache.TryGetCompletePage(targetStep, out _);

            var incomingView = CreatePageViewShell(showLoadingRing: !cacheHitBeforeNav);
            ApplyTransitionPageChromeTexts(incomingView, forward);
            var incomingRoot = incomingView.Root;
            var incomingStart = forward ? travel : -travel;
            incomingRoot.RenderTransform = new TranslateTransform { X = incomingStart };
            PageTransitionHost.Children.Add(incomingRoot);

            var animTask = AnimatePageCrossSlideAsync(
                outgoingRoot,
                incomingRoot,
                travel,
                forward,
                PageTransitionDurationMs);

            if (forward)
                _engine.GoNext();
            else
                _engine.GoBack();

            ApplyPageChromeTexts(incomingView);
            var prepStep = _engine.StepIndex;
            _navPopulateView = incomingView;

            var prepTask = EnsurePageSlotAsync(prepStep);

            if (cacheHitBeforeNav)
            {
                var cachedSlot = await prepTask.ConfigureAwait(true);
                _navPagePopulated = true;
                PopulateFromMap(incomingView, cachedSlot);
                RevealPopulatedPage(incomingView);
                await animTask.ConfigureAwait(true);
            }
            else
            {
                _ = prepTask.ContinueWith(
                    t =>
                    {
                        if (t.IsFaulted || t.IsCanceled)
                            return;

                        DispatcherQueue.TryEnqueue(() =>
                        {
                            if (_navPopulateView != incomingView || _navPagePopulated)
                                return;

                            _navPagePopulated = true;
                            PopulateFromMap(incomingView, t.Result);
                        });
                    },
                    CancellationToken.None,
                    TaskContinuationOptions.None,
                    TaskScheduler.Default);

                await animTask.ConfigureAwait(true);

                var slot = await prepTask.ConfigureAwait(true);

                if (!_navPagePopulated)
                {
                    _navPagePopulated = true;
                    PopulateFromMap(incomingView, slot);
                }

                RevealPopulatedPage(incomingView);
            }

            PageTransitionHost.Children.Remove(outgoingRoot);
            outgoingRoot.RenderTransform = null;
            incomingRoot.RenderTransform = null;
            _currentPageView = incomingView;
            UpdateNavigationChrome();
        }
        finally
        {
            _navPopulateView = null;
            _pageTransitionRunning = false;
            SetNavigationEnabled(true);
            TitleBarBackButton.IsEnabled = _engine.CanGoBack;
        }
    }

    private double ResolvePageTransitionDistance()
    {
        var width = PageTransitionClip.ActualWidth;
        if (width > 1)
            return width;

        var pad = HostedDialogLayout.GetContentDialogPadding();
        return Math.Max(WindowWidth - pad.Left - pad.Right, WizardCardWidth);
    }

    private async Task AnimatePageCrossSlideAsync(
        FrameworkElement outgoing,
        FrameworkElement incoming,
        double travel,
        bool forward,
        int durationMs)
    {
        var outgoingEnd = forward ? -travel : travel;
        var incomingStart = forward ? travel : -travel;

        var outgoingTransform = EnsureTranslateTransform(outgoing);
        var incomingTransform = EnsureTranslateTransform(incoming);
        outgoingTransform.X = 0;
        incomingTransform.X = incomingStart;

        var storyboard = new Storyboard();
        var outgoingAnimation = CreateTranslateAnimation(outgoingTransform, 0, outgoingEnd, durationMs);
        var incomingAnimation = CreateTranslateAnimation(incomingTransform, incomingStart, 0, durationMs);
        Storyboard.SetTarget(outgoingAnimation, outgoingTransform);
        Storyboard.SetTargetProperty(outgoingAnimation, nameof(TranslateTransform.X));
        Storyboard.SetTarget(incomingAnimation, incomingTransform);
        Storyboard.SetTargetProperty(incomingAnimation, nameof(TranslateTransform.X));
        storyboard.Children.Add(outgoingAnimation);
        storyboard.Children.Add(incomingAnimation);

        var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        storyboard.Completed += (_, _) => tcs.TrySetResult(true);
        storyboard.Begin();

        await tcs.Task.ConfigureAwait(false);
    }

    private static TranslateTransform EnsureTranslateTransform(FrameworkElement element)
    {
        if (element.RenderTransform is TranslateTransform existing)
            return existing;

        var transform = new TranslateTransform();
        element.RenderTransform = transform;
        return transform;
    }

    private static DoubleAnimation CreateTranslateAnimation(
        TranslateTransform transform,
        double from,
        double to,
        int durationMs) =>
        new()
        {
            From = from,
            To = to,
            Duration = TimeSpan.FromMilliseconds(durationMs),
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseInOut },
        };

    private void CancelButton_OnClick(object sender, RoutedEventArgs e) => CloseCanceled();

    private void CloseAccepted()
    {
        if (_closeTcs.Task.IsCompleted)
            return;
        _closeTcs.TrySetResult(true);
        Close();
    }

    private void CloseCanceled()
    {
        if (_closeTcs.Task.IsCompleted)
            return;
        _closeTcs.TrySetResult(false);
        Close();
    }

    private sealed class WizardPageView
    {
        public required Grid Root { get; init; }

        public required TextBlock PromptText { get; init; }

        public required TextBlock HintText { get; init; }

        public required Grid CardArea { get; init; }

        public required StackPanel CardsPanel { get; init; }

        public ProgressRing? LoadingRing { get; init; }

        public Grid? LoadingOverlay { get; init; }
    }

}
