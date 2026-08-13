using GlassSettings.Controls;
using GlassSettings.Core.Customization;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Platform;
using GlassSettings.Core.Registry;
using GlassSettings.Core.Snapshot;
using GlassSettings.Core.Text;
using GlassSettings.Navigation;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Navigation;
using Windows.Foundation;
using WinRT.Interop;

namespace GlassSettings.Pages.Text;

public sealed partial class TextAndFontsPage : Page, IExternalRegistrySyncReloadable
{
    private readonly ToggleButton _glowEnableToggle;
    private readonly TextBlock _radiusLabel = new();
    private readonly TextBlock _spreadLabel = new();
    private readonly Slider _radiusSlider;
    private readonly TextBlock _radiusValueLabel = new();
    private readonly ComboBox _spreadCombo;

    private readonly ComboBox _renderModeCombo = new() { MinWidth = 140 };
    private readonly Button _visualTuneButton = new();
    private readonly TextBlock _gammaLabel = new();
    private readonly TextBlock _blendLabel = new();
    private Slider _gammaSlider = null!;
    private Slider _blendSlider = null!;
    private readonly TextBlock _gammaValueLabel = new();
    private readonly TextBlock _blendValueLabel = new();
    private Slider _pseudoRadiusSlider = null!;
    private Slider _pseudoIntensitySlider = null!;
    private readonly TextBlock _pseudoRadiusLabel = new();
    private readonly TextBlock _pseudoIntensityLabel = new();
    private readonly TextBlock _pseudoRadiusValueLabel = new();
    private readonly TextBlock _pseudoIntensityValueLabel = new();

    private Grid? _textRenderControlTable;
    private TextBlock? _textRenderCardTitle;
    private TextBlock? _inkProfileModeLabel;
    private GlassSettingsLabeledToggleSwitch? _inkMasterToggle;
    private bool _inkMasterArmed;
    private bool _gammaGateArmed;

    private readonly StackPanel _inkHeaderActionsStack = new()
    {
        Orientation = Orientation.Horizontal,
        Spacing = 8,
        VerticalAlignment = VerticalAlignment.Center
    };
    private Grid? _textRenderBodyCenterHost;
    private Grid? _textRenderFooterGrid;
    private double _textCardMinInnerWidth;
    private double _inkComboNaturalWidth;
    private double _inkVisualTuneButtonNaturalWidth;
    private bool _textHeroSideBySide = true;
    private bool _textHeroLayoutInitialized;
    private bool _textHeroLayoutUpdating;
    private double _lastAppliedTextCardTableWidth = -1;

    private const double TextHeroColumnGap = 24;
    private const double TextCardRightGapMin = 20;

    private GlobalTextGlowModel _model = GlobalTextGlowModel.Factory;
    private GlassInkCapsuleModel _inkModel = GlassInkCapsuleModel.Factory;
    private bool _loading;
    private CancellationTokenSource? _inkRegistryDebounceCts;
    private const int InkRegistryDebounceMs = 100;
    private EventHandler<DesktopEnvironmentChangedEventArgs>? _environmentChangedHandler;
    private EventHandler? _sessionDirtyHandler;
    private EventHandler? _windowMetricsWatchHandler;

    private readonly SystemFontsModel _systemFontsModel = new();
    private readonly SettingsPivotTabHeader[] _systemFontsPivotHeaders = new SettingsPivotTabHeader[SystemFontsCatalog.SlotCount];
    private readonly TextBlock _systemFontNameText = new();
    private readonly TextBlock _systemFontSizeText = new();
    private readonly Button _systemFontChooseButton = new();
    private WindowMetricsRegistryWatchService? _windowMetricsWatch;
    private int _lastSystemFontsPivotIndex;
    private int _systemFontsPivotIndex;
    private bool _isSelectiveSystemFontsScope = true;
    private string[] _systemFontsPivotTabLabels = [];
    private string? _pendingScrollSection;
    private int _scrollRetryCount;

    public TextAndFontsPage()
    {
        InitializeComponent();

        PageShell.CommitScope = SettingsCommitScope.ForArea(CustomizationAreaId.Text);
        EffectsExpander.UnsavedScope = TextScopeCatalog.GlowScope;
        EffectsExpander.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.EffectType);

        GammaExpander.UnsavedScope = TextScopeCatalog.InkCapsuleScope;
        GammaExpander.IconGlyph = SettingsSectionGlyphs.TextSmoothing;
        PseudoExpander.UnsavedScope = null;
        PseudoExpander.IconGlyph = SettingsSectionGlyphs.TextSmoothing;

        _glowEnableToggle = new ToggleButton
        {
            MinWidth = 140,
            Content = GlassSettingsResources.Get("Text_GlowEnableToggle"),
        };
        _glowEnableToggle.Checked += (_, _) => OnGlowEnableCommitted(true);
        _glowEnableToggle.Unchecked += (_, _) => OnGlowEnableCommitted(false);
        EffectsExpander.HeaderActions = _glowEnableToggle;

        _radiusSlider = SettingsSliderFactory.Create(0, 1000);
        _radiusValueLabel.Style = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _radiusValueLabel.MinWidth = 40;
        _radiusValueLabel.TextAlignment = TextAlignment.Right;
        SettingsSliderFactory.Apply(_radiusSlider, stretch: true);
        _radiusSlider.HorizontalAlignment = HorizontalAlignment.Left;

        _spreadCombo = new ComboBox();
        TextGlowSpreadNames.PopulateCombo(_spreadCombo);
        _spreadCombo.SelectionChanged += (_, _) =>
        {
            if (_loading || !EffectsExpander.IsGateOn || _glowEnableToggle.IsChecked != true)
                return;

            _model.GlowSpreadMode = (uint)Math.Clamp(_spreadCombo.SelectedIndex, 0, 2);
            TextGlowLiveReload.Commit(_model);
            RefreshHeroPreview(TextPreviewRefreshScope.Full);
            UpdatePickerDefaultStates();
        };

        BuildGlowBodyTable();
        BuildTextRenderTopControls();
        InitializeTextRenderFlatRow();
        BuildInkBodyTables();
        WireInkExpanders();

        AttachGlowSliders();
        AttachInkSliders();

        GlowPicker.UseLivePreview = true;
        GlowPicker.LiveGestureStarted += (_, _) =>
        {
            if (!CanSampleGlowLivePreview())
                return;

            LiveReloadScheduler.BeginTextGlowLivePreview(DispatcherQueue, SampleGlowModelFromUi);
        };
        GlowPicker.LiveGestureEnded += (_, _) =>
        {
            LiveReloadScheduler.EndTextGlowLivePreview();
            RefreshHeroPreview(TextPreviewRefreshScope.Full);
        };
        GlowPicker.ColorLivePreview += (_, _) =>
        {
            if (!CanSampleGlowLivePreview())
                return;

            RefreshHeroPreview(TextPreviewRefreshScope.LiveSlider);
            UpdatePickerDefaultStates();
        };
        GlowPicker.ColorChanged += (_, argb) => OnGlowColorCommitted(argb);
        GlowPanel.DefaultColorClick += (_, _) => OnGlowResetToDefaults();
        GammaPanel.DefaultClick += (_, _) => OnInkGammaResetToDefaults();
        PseudoPanel.DefaultClick += (_, _) => OnInkPseudoResetToDefaults();

        _renderModeCombo.SelectionChanged += (_, _) => OnRenderModeSelectionChanged();
        _visualTuneButton.Click += (_, _) => OnVisualTuneClick();

        EffectsExpander.WireExpandedLayoutRefresh(() => GlowPanel.RefreshLayout());

        ApplyLocalizedStrings();
        CacheTextCardMinInnerWidth();
        BuildSystemFontsSection();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
        TextHeroLayoutHost.SizeChanged += (_, _) => UpdateTextHeroLayout();
    }

    private void BuildTextRenderTopControls()
    {
        var cardIcon = new FontIcon
        {
            Glyph = SettingsSectionGlyphs.TextSmoothing,
            VerticalAlignment = VerticalAlignment.Center
        };
        if (Application.Current.Resources["GlassSettingsSectionIconSize"] is double iconSize)
            cardIcon.FontSize = iconSize;
        if (Application.Current.Resources["TextFillColorSecondaryBrush"] is Brush iconBrush)
            cardIcon.Foreground = iconBrush;

        _textRenderCardTitle = new TextBlock
        {
            Style = Application.Current.Resources["GlassSettingsSpoilerTitleTextStyle"] as Style,
            VerticalAlignment = VerticalAlignment.Center
        };
        var titleRow = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Children = { cardIcon, _textRenderCardTitle }
        };
        Grid.SetRow(titleRow, 0);
        TextRenderControlBody.Children.Add(titleRow);

        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _textRenderControlTable = SettingsPickerBodyLayout.CreateBodyTable();
        _textRenderControlTable.HorizontalAlignment = HorizontalAlignment.Left;

        _inkProfileModeLabel = new TextBlock { Style = labelStyle };
        InkRenderModeNames.PopulateCombo(_renderModeCombo);
        SettingsPickerBodyLayout.AddLabeledRow(_textRenderControlTable, _inkProfileModeLabel, _renderModeCombo);

        _textRenderBodyCenterHost = new Grid { VerticalAlignment = VerticalAlignment.Stretch };
        _textRenderControlTable.VerticalAlignment = VerticalAlignment.Center;
        _textRenderBodyCenterHost.Children.Add(_textRenderControlTable);
        Grid.SetRow(_textRenderBodyCenterHost, 1);
        TextRenderControlBody.Children.Add(_textRenderBodyCenterHost);
        BuildInkMasterToggle();
        ApplyInkControlsSideBySideChrome();
    }

    private void InitializeTextRenderFlatRow()
    {
        TextRenderFlatRow.IconGlyph = SettingsSectionGlyphs.TextSmoothing;
        TextRenderFlatRow.UnsavedScope = TextScopeCatalog.InkCapsuleScope;
        GlassSettingsToolTipService.SetToolTip(
            TextRenderFlatRow.GateSwitchControl,
            GlassSettingsResources.Get("Text_InkMasterToggleTooltip"));
    }

    private void BuildInkMasterToggle()
    {
        _textRenderFooterGrid = new Grid();
        _textRenderFooterGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _textRenderFooterGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _textRenderFooterGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        _visualTuneButton.HorizontalAlignment = HorizontalAlignment.Left;
        _visualTuneButton.VerticalAlignment = VerticalAlignment.Center;

        _inkMasterToggle = new GlassSettingsLabeledToggleSwitch
        {
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center
        };
        _inkMasterToggle.Toggled += (_, _) => OnInkMasterToggled(_inkMasterToggle.IsOn);
        GlassSettingsToolTipService.SetToolTip(
            _inkMasterToggle,
            GlassSettingsResources.Get("Text_InkMasterToggleTooltip"));

        TextRenderControlFooterHost.Child = _textRenderFooterGrid;
    }

    private void ApplyInkControlsSideBySideChrome()
    {
        _inkHeaderActionsStack.Children.Clear();
        if (TextRenderFlatRow.HeaderActions is not null)
            TextRenderFlatRow.HeaderActions = null;

        if (_inkProfileModeLabel is not null)
            _inkProfileModeLabel.Visibility = Visibility.Visible;

        if (_textRenderControlTable is not null
            && !_textRenderControlTable.Children.Contains(_renderModeCombo))
        {
            Grid.SetRow(_renderModeCombo, 0);
            Grid.SetColumn(_renderModeCombo, 1);
            _textRenderControlTable.Children.Add(_renderModeCombo);
        }

        if (_textRenderFooterGrid is null)
            return;

        if (_visualTuneButton.Parent is Panel visualParent)
            visualParent.Children.Remove(_visualTuneButton);
        if (_inkMasterToggle is not null && _inkMasterToggle.Parent is Panel gateParent)
            gateParent.Children.Remove(_inkMasterToggle);

        Grid.SetColumn(_visualTuneButton, 0);
        if (!_textRenderFooterGrid.Children.Contains(_visualTuneButton))
            _textRenderFooterGrid.Children.Add(_visualTuneButton);

        if (_inkMasterToggle is not null)
        {
            Grid.SetColumn(_inkMasterToggle, 2);
            if (!_textRenderFooterGrid.Children.Contains(_inkMasterToggle))
                _textRenderFooterGrid.Children.Add(_inkMasterToggle);
        }

        ApplyInkHeroNaturalControlWidths();
    }

    private void ApplyInkControlsStackedChrome(bool masterOn)
    {
        if (_textRenderControlTable is not null)
            _textRenderControlTable.Children.Remove(_renderModeCombo);

        if (_textRenderFooterGrid is not null)
        {
            _textRenderFooterGrid.Children.Remove(_visualTuneButton);
            if (_inkMasterToggle is not null)
                _textRenderFooterGrid.Children.Remove(_inkMasterToggle);
        }

        if (_inkProfileModeLabel is not null)
            _inkProfileModeLabel.Visibility = Visibility.Collapsed;

        _inkHeaderActionsStack.Children.Clear();
        _inkHeaderActionsStack.Children.Add(_visualTuneButton);
        _inkHeaderActionsStack.Children.Add(_renderModeCombo);
        TextRenderFlatRow.HeaderActions = _inkHeaderActionsStack;

        _loading = true;
        try
        {
            TextRenderFlatRow.IsGateOn = masterOn;
        }
        finally
        {
            _loading = false;
        }

        TextRenderFlatRow.RefreshHeaderLayout();
        ApplyInkHeroNaturalControlWidths();
    }

    private void SyncInkMasterToggleUi(bool on)
    {
        _loading = true;
        try
        {
            if (_inkMasterToggle is not null && _inkMasterToggle.IsOn != on)
                _inkMasterToggle.IsOn = on;

            if (TextRenderFlatRow.GateSwitchControl.IsOn != on)
                TextRenderFlatRow.IsGateOn = on;
        }
        finally
        {
            _loading = false;
        }
    }

    private GlassSettingsLabeledToggleSwitch? ActiveInkMasterToggle =>
        _textHeroSideBySide ? _inkMasterToggle : TextRenderFlatRow.GateSwitchControl;

    private void CacheTextCardMinInnerWidth()
    {
        _inkComboNaturalWidth = MeasureElementDesiredWidth(_renderModeCombo);
        _inkVisualTuneButtonNaturalWidth = MeasureElementDesiredWidth(_visualTuneButton);

        var tableMin = MeasureTextCardTableMinWidth();
        var footerMin = MeasureTextCardFooterMinWidth();
        _textCardMinInnerWidth = Math.Max(tableMin + TextCardRightGapMin, footerMin);

        ApplyInkHeroNaturalControlWidths();
    }

    private void ApplyInkHeroNaturalControlWidths()
    {
        if (_inkComboNaturalWidth <= 0)
            _inkComboNaturalWidth = MeasureElementDesiredWidth(_renderModeCombo);
        if (_inkVisualTuneButtonNaturalWidth <= 0)
            _inkVisualTuneButtonNaturalWidth = MeasureElementDesiredWidth(_visualTuneButton);

        _renderModeCombo.MinWidth = _inkComboNaturalWidth;
        _renderModeCombo.Width = _inkComboNaturalWidth;
        _renderModeCombo.ClearValue(FrameworkElement.MaxWidthProperty);
        _renderModeCombo.HorizontalAlignment = HorizontalAlignment.Left;

        _visualTuneButton.MinWidth = _inkVisualTuneButtonNaturalWidth;
        _visualTuneButton.ClearValue(FrameworkElement.WidthProperty);
        _visualTuneButton.ClearValue(FrameworkElement.MaxWidthProperty);
        _visualTuneButton.HorizontalAlignment = HorizontalAlignment.Left;
    }

    private static double MeasureElementDesiredWidth(FrameworkElement element)
    {
        element.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
        var width = Math.Ceiling(element.DesiredSize.Width) + element.Margin.Left + element.Margin.Right;
        if (element.MinWidth > 0)
            width = Math.Max(width, element.MinWidth + element.Margin.Left + element.Margin.Right);

        return width;
    }

    private double MeasureTextCardTableMinWidth()
    {
        if (_inkProfileModeLabel is null)
            return _renderModeCombo.MinWidth;

        if (_textRenderControlTable is not null)
            SettingsPickerBodyLayout.InvalidateLabelSync(_textRenderControlTable);

        var labelWidth = MeasureElementDesiredWidth(_inkProfileModeLabel);
        labelWidth = Math.Max(labelWidth, SettingsPickerBodyLayout.LabelColumnMinWidth);
        var comboWidth = _inkComboNaturalWidth > 0
            ? _inkComboNaturalWidth
            : MeasureElementDesiredWidth(_renderModeCombo);

        return labelWidth + SettingsPickerBodyLayout.LabelControlGap + comboWidth;
    }

    private double MeasureTextCardFooterMinWidth()
    {
        if (_textRenderFooterGrid is not null
            && _textRenderFooterGrid.Children.Contains(_visualTuneButton)
            && _inkMasterToggle is not null
            && _textRenderFooterGrid.Children.Contains(_inkMasterToggle))
        {
            _textRenderFooterGrid.Measure(new Windows.Foundation.Size(double.PositiveInfinity, double.PositiveInfinity));
            var gridWidth = Math.Ceiling(_textRenderFooterGrid.DesiredSize.Width);
            if (gridWidth > 0)
                return gridWidth;
        }

        var buttonWidth = MeasureElementDesiredWidth(_visualTuneButton);
        if (_inkMasterToggle is null)
            return buttonWidth;

        return buttonWidth + MeasureElementDesiredWidth(_inkMasterToggle);
    }

    private double GetTextControlsCardHorizontalPadding()
    {
        var padding = TextControlsCard.Padding;
        return padding.Left + padding.Right;
    }

    private void UpdateTextHeroLayout()
    {
        if (_textHeroLayoutUpdating)
            return;

        var hostWidth = TextHeroLayoutHost.ActualWidth;
        if (hostWidth <= 0)
            return;

        _textHeroLayoutUpdating = true;
        try
        {
            var cardHorizontalPadding = GetTextControlsCardHorizontalPadding();
            var besideCardInner = hostWidth
                - HeroPreviewLayoutMetrics.FootprintWidth
                - TextHeroColumnGap
                - cardHorizontalPadding;

            var sideBySide = ResolveTextHeroSideBySide(besideCardInner);
            ApplyTextHeroPlacement(sideBySide);

            if (!sideBySide || _textRenderControlTable is null)
                return;

            var tableMinWidth = MeasureTextCardTableMinWidth();
            var availableForTable = besideCardInner - TextCardRightGapMin;
            var tableWidth = Math.Clamp(availableForTable, tableMinWidth, tableMinWidth);
            ApplyTextCardTableWidth(tableWidth);
        }
        finally
        {
            _textHeroLayoutUpdating = false;
        }
    }

    private bool ResolveTextHeroSideBySide(double besideCardInner)
    {
        if (!_textHeroLayoutInitialized)
            return besideCardInner >= _textCardMinInnerWidth;

        if (_textHeroSideBySide)
            return besideCardInner >= _textCardMinInnerWidth;

        return besideCardInner >= _textCardMinInnerWidth + 1;
    }

    private void ApplyTextCardTableWidth(double tableWidth)
    {
        if (_textRenderControlTable is null)
            return;

        var naturalTableWidth = MeasureTextCardTableMinWidth();
        tableWidth = Math.Max(tableWidth, naturalTableWidth);

        if (Math.Abs(_lastAppliedTextCardTableWidth - tableWidth) < 0.5)
        {
            ApplyInkHeroNaturalControlWidths();
            return;
        }

        _lastAppliedTextCardTableWidth = tableWidth;
        _textRenderControlTable.HorizontalAlignment = HorizontalAlignment.Left;
        _textRenderControlTable.Width = tableWidth;
        _textRenderControlTable.MaxWidth = tableWidth;
        SettingsPickerBodyLayout.UpdateBodyTableLayout(_textRenderControlTable, tableWidth);
        ApplyInkHeroNaturalControlWidths();
    }

    private void ApplyTextHeroPlacement(bool sideBySide)
    {
        if (_textHeroLayoutInitialized && _textHeroSideBySide == sideBySide)
            return;

        _lastAppliedTextCardTableWidth = -1;

        var previousSideBySide = _textHeroSideBySide;
        var masterOn = IsInkCardActive();
        _textHeroSideBySide = sideBySide;
        _textHeroLayoutInitialized = true;

        if (sideBySide)
        {
            TextPreviewColumn.Width = GridLength.Auto;
            TextControlsColumn.Width = new GridLength(2, GridUnitType.Star);

            Grid.SetColumn(TextPreviewHost, 0);
            Grid.SetRow(TextPreviewHost, 0);
            Grid.SetColumnSpan(TextPreviewHost, 1);

            Grid.SetColumn(TextControlsCard, 1);
            Grid.SetRow(TextControlsCard, 0);
            Grid.SetColumnSpan(TextControlsCard, 1);
            TextControlsCard.Visibility = Visibility.Visible;
            TextRenderFlatRow.Visibility = Visibility.Collapsed;

            var heroHeight = HeroPreviewLayoutMetrics.OuterHeight;
            TextControlsCard.MinHeight = heroHeight;
            TextControlsCard.Height = heroHeight;

            ApplyInkControlsSideBySideChrome();
        }
        else
        {
            TextPreviewColumn.Width = GridLength.Auto;
            TextControlsColumn.Width = new GridLength(1, GridUnitType.Star);

            Grid.SetColumn(TextPreviewHost, 0);
            Grid.SetRow(TextPreviewHost, 0);
            Grid.SetColumnSpan(TextPreviewHost, 2);

            TextControlsCard.Visibility = Visibility.Collapsed;
            TextRenderFlatRow.Visibility = Visibility.Visible;
            Grid.SetColumn(TextRenderFlatRow, 0);
            Grid.SetRow(TextRenderFlatRow, 1);
            Grid.SetColumnSpan(TextRenderFlatRow, 2);

            TextControlsCard.ClearValue(FrameworkElement.MinHeightProperty);
            TextControlsCard.ClearValue(FrameworkElement.HeightProperty);

            ApplyInkControlsStackedChrome(masterOn);
        }

        if (previousSideBySide != sideBySide)
            ReloadInkMaster();
        else
            SyncInkMasterToggleUi(masterOn);

        if (!sideBySide)
            TextRenderFlatRow.RefreshHeaderLayout();
    }

    private void TextRenderFlatRow_OnGateToggled(object sender, RoutedEventArgs e) =>
        OnInkMasterToggled(TextRenderFlatRow.IsGateOn);

    private void BuildInkBodyTables()
    {
        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _gammaLabel.Style = labelStyle;
        _blendLabel.Style = labelStyle;
        _pseudoRadiusLabel.Style = labelStyle;
        _pseudoIntensityLabel.Style = labelStyle;

        _gammaValueLabel.Style = labelStyle;
        _gammaValueLabel.MinWidth = 48;
        _gammaValueLabel.TextAlignment = TextAlignment.Right;
        _blendValueLabel.Style = labelStyle;
        _blendValueLabel.MinWidth = 48;
        _blendValueLabel.TextAlignment = TextAlignment.Right;
        _pseudoRadiusValueLabel.Style = labelStyle;
        _pseudoRadiusValueLabel.MinWidth = 48;
        _pseudoRadiusValueLabel.TextAlignment = TextAlignment.Right;
        _pseudoIntensityValueLabel.Style = labelStyle;
        _pseudoIntensityValueLabel.MinWidth = 48;
        _pseudoIntensityValueLabel.TextAlignment = TextAlignment.Right;

        _gammaSlider = SettingsSliderFactory.Create(
            InkCapsuleInterop.UiGammaMinThousandths,
            InkCapsuleInterop.UiGammaMaxThousandths);
        _blendSlider = SettingsSliderFactory.Create(InkCapsuleInterop.UiBlendMin, InkCapsuleInterop.UiBlendMax);
        _pseudoRadiusSlider = SettingsSliderFactory.Create(
            InkCapsuleInterop.UiPseudoRadiusMinThousandths,
            InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
        _pseudoIntensitySlider = SettingsSliderFactory.Create(
            InkCapsuleInterop.UiPseudoIntensityMin,
            InkCapsuleInterop.UiPseudoIntensityMax);

        var gammaSliderPanel = SettingsSliderFactory.CreateSliderValueSuffixPanel(_gammaSlider, _gammaValueLabel);
        var blendSliderPanel = SettingsSliderFactory.CreateSliderValueSuffixPanel(_blendSlider, _blendValueLabel);
        var pseudoRadiusSliderPanel = SettingsSliderFactory.CreateSliderValueSuffixPanel(
            _pseudoRadiusSlider,
            _pseudoRadiusValueLabel);
        var pseudoIntensitySliderPanel = SettingsSliderFactory.CreateSliderValueSuffixPanel(
            _pseudoIntensitySlider,
            _pseudoIntensityValueLabel);

        var gammaTable = SettingsParameterBodyLayout.CreateBodyTable();
        SettingsParameterBodyLayout.AddSingleRow(gammaTable, _gammaLabel, gammaSliderPanel);
        SettingsParameterBodyLayout.AddSingleRow(gammaTable, _blendLabel, blendSliderPanel, topMargin: true);
        GammaPanel.BodyContent = gammaTable;

        var pseudoTable = SettingsParameterBodyLayout.CreateBodyTable();
        SettingsParameterBodyLayout.AddSingleRow(pseudoTable, _pseudoRadiusLabel, pseudoRadiusSliderPanel);
        SettingsParameterBodyLayout.AddSingleRow(
            pseudoTable,
            _pseudoIntensityLabel,
            pseudoIntensitySliderPanel,
            topMargin: true);
        PseudoPanel.BodyContent = pseudoTable;
    }

    private void WireInkExpanders()
    {
        GammaExpander.WireExpandedLayoutRefresh(() => GammaPanel.RefreshLayout());
        PseudoExpander.WireExpandedLayoutRefresh(() => PseudoPanel.RefreshLayout());
    }

    private void RefreshInkSliderRows()
    {
        GammaPanel.RefreshLayout();
        PseudoPanel.RefreshLayout();
    }

    private void AttachGlowSliders()
    {
        SettingsSliderCommit.Attach(
            _radiusSlider,
            CanSampleGlowLivePreview,
            value =>
            {
                UpdateRadiusValueLabel(value);
                RefreshHeroPreview(TextPreviewRefreshScope.LiveSlider);
            },
            value => _model.GlowRadiusThousandths = (uint)value,
            SettingsSliderCommit.CommitMode.LiveImmediate,
            () =>
            {
                LiveReloadScheduler.EndTextGlowLivePreview();
                RefreshHeroPreview(TextPreviewRefreshScope.Full);
                SettingsSession.Current.MarkDirty();
                UpdatePickerDefaultStates();
                EffectsExpander.RefreshUnsavedIndicator();
            });
        _radiusSlider.AddHandler(
            UIElement.PointerPressedEvent,
            new PointerEventHandler(OnRadiusSliderPointerPressed),
            true);
    }

    private void AttachInkSliders()
    {
        AttachInkSlider(
            _gammaSlider,
            CanSampleInkGammaLivePreview,
            value => _gammaValueLabel.Text = ((int)value).ToString(),
            value => ApplyInkSliderToActiveProfile(p => p.CoverageGammaThousandths = (uint)value));

        AttachInkSlider(
            _blendSlider,
            CanSampleInkGammaLivePreview,
            value => _blendValueLabel.Text = ((int)value).ToString(),
            value => ApplyInkSliderToActiveProfile(p => p.TargetBlendAlpha = (uint)value));

        AttachInkSlider(
            _pseudoRadiusSlider,
            CanSampleInkPseudoLivePreview,
            value => _pseudoRadiusValueLabel.Text = ((int)value).ToString(),
            value => ApplyInkSliderToActiveProfile(p =>
            {
                p.PseudoGlowRadiusThousandths = (uint)value;
                p.SyncPseudoEnabledFlag();
            }));

        AttachInkSlider(
            _pseudoIntensitySlider,
            CanSampleInkPseudoLivePreview,
            value => _pseudoIntensityValueLabel.Text = ((int)value).ToString(),
            value => ApplyInkSliderToActiveProfile(p =>
            {
                p.PseudoGlowIntensity = (uint)value;
                p.SyncPseudoEnabledFlag();
            }));
    }

    private void AttachInkSlider(
        Slider slider,
        Func<bool> canSampleLivePreview,
        Action<double> updateLabel,
        Action<double> applyToProfile)
    {
        SettingsSliderCommit.Attach(
            slider,
            canSampleLivePreview,
            value =>
            {
                updateLabel(value);
                if (!canSampleLivePreview())
                    return;

                applyToProfile(value);
                RefreshHeroPreview(TextPreviewRefreshScope.LiveSlider);
                ScheduleInkRegistryWriteDebounced();
                GammaExpander.RefreshUnsavedIndicator();
                RefreshPseudoUnsavedIndicator();
            },
            _ => { },
            SettingsSliderCommit.CommitMode.LiveImmediate,
            OnInkSliderGestureCommitted);
    }

    /// <summary>
    /// Конец жеста ink-слайдера: модель и реестр синхронизированы на UI; reload движка — здесь.
    /// </summary>
    private void OnInkSliderGestureCommitted()
    {
        SaveInkUiToActiveProfile();
        FlushInkRegistryLivePreviewWithReload();
        SettingsSession.Current.MarkDirty();
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
        UpdateInkDefaultStates();
        GammaExpander.RefreshUnsavedIndicator();
        RefreshPseudoUnsavedIndicator();
        PageShell.FlushSessionNotice();
    }

    private void BuildGlowBodyTable()
    {
        var labelStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _radiusLabel.Style = labelStyle;
        _spreadLabel.Style = labelStyle;

        var table = SettingsPickerBodyLayout.CreateBodyTable();
        SettingsPickerBodyLayout.AddLabeledRow(table, _radiusLabel, _radiusSlider);
        SettingsPickerBodyLayout.AddTrailingContent(table, row: 0, trailing: _radiusValueLabel, leftGap: 8);
        SettingsPickerBodyLayout.AddLabeledRow(table, _spreadLabel, _spreadCombo, topMargin: true);
        GlowPanel.FooterContent = table;
    }

    private void ApplyLocalizedStrings()
    {
        if (_textRenderCardTitle is not null)
            _textRenderCardTitle.Text = GlassSettingsResources.Get("Text_RenderControlCardTitle");
        TextRenderFlatRow.Title = GlassSettingsResources.Get("Text_RenderControlCardTitle");
        EffectsExpander.Title = GlassSettingsResources.Get("Text_EffectsTitle");
        EffectsExpander.Description = GlassSettingsResources.Get("Text_EffectsDesc");
        GammaExpander.Title = GlassSettingsResources.Get("Text_InkGammaBlendTitle");
        GammaExpander.Description = GlassSettingsResources.Get("Text_InkGammaBlendDesc");
        PseudoExpander.Title = GlassSettingsResources.Get("Text_InkPseudoTitle");
        PseudoExpander.Description = GlassSettingsResources.Get("Text_InkPseudoDesc");
        _glowEnableToggle.Content = GlassSettingsResources.Get("Text_GlowEnableToggle");
        _visualTuneButton.Content = GlassSettingsResources.Get("Text_InkVisualTuneButton");
        if (_inkProfileModeLabel is not null)
            _inkProfileModeLabel.Text = GlassSettingsResources.Get("Text_InkProfileModeLabel");
        _radiusLabel.Text = GlassSettingsResources.Get("Text_GlowRadiusLabel");
        _spreadLabel.Text = GlassSettingsResources.Get("Text_GlowSpreadLabel");
        GlowPanel.Description = GlassSettingsResources.Get("Text_GlowPanelIntro");
        GammaPanel.Description = GlassSettingsResources.Get("Text_InkGammaBlendIntro");
        PseudoPanel.Description = GlassSettingsResources.Get("Text_InkPseudoIntro");
        _gammaLabel.Text = GlassSettingsResources.Get("Text_InkGammaLabel");
        _blendLabel.Text = GlassSettingsResources.Get("Text_InkBlendLabel");
        _pseudoRadiusLabel.Text = GlassSettingsResources.Get("Text_InkPseudoRadiusLabel");
        _pseudoIntensityLabel.Text = GlassSettingsResources.Get("Text_InkPseudoIntensityLabel");
        if (GlowPanel.FooterContent is Grid glowTable && SettingsPickerBodyLayout.IsBodyTable(glowTable))
            SettingsPickerBodyLayout.InvalidateLabelSync(glowTable);
        if (_textRenderControlTable is not null)
            SettingsPickerBodyLayout.InvalidateLabelSync(_textRenderControlTable);
        RefreshInkSliderRows();
        CacheTextCardMinInnerWidth();
        UpdateTextHeroLayout();

        SystemFontsInfoBlock.Title = GlassSettingsResources.Get("Text_SystemFontsTitle");
        SystemFontsInfoBlock.Message = GlassSettingsResources.Get("Text_SystemFontsInfoMessage");
        SystemFontsRow.Description = GlassSettingsResources.Get("Text_SystemFontsDesc");
        _systemFontChooseButton.Content = GlassSettingsResources.Get("Text_SystemFontsChoose");
        SystemFontsScopeBar.ApplyLocalizedStrings();

        var tabLabels = GlassSettingsResources.GetSemicolonList("Text_SystemFontsPivotTabs");
        _systemFontsPivotTabLabels = tabLabels;
        for (var i = 0; i < SystemFontsCatalog.SlotCount && i < _systemFontsPivotHeaders.Length; i++)
        {
            var label = i < tabLabels.Length ? tabLabels[i] : $"#{i}";
            _systemFontsPivotHeaders[i]?.SetLabel(label);
        }

        UpdateSystemFontsRowTitle();
    }

    protected override void OnNavigatedTo(NavigationEventArgs e)
    {
        base.OnNavigatedTo(e);
        if (e.Parameter is string section
            && string.Equals(section, TextPageNavIds.SystemFonts, StringComparison.Ordinal))
        {
            RequestScrollToSection(section);
        }
    }

    /// <summary>Скролл к секции после перехода с других страниц (в т.ч. уже открытой).</summary>
    internal void RequestScrollToSection(string section)
    {
        if (string.IsNullOrWhiteSpace(section))
            return;

        _pendingScrollSection = section;
        _scrollRetryCount = 0;
        SchedulePendingScroll();
    }

    private void SchedulePendingScroll()
    {
        if (_pendingScrollSection is null)
            return;

        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, TryApplyPendingScroll);
    }

    private void TryApplyPendingScroll()
    {
        if (!IsLoaded || _pendingScrollSection is null)
            return;

        if (!TryScrollToSection(_pendingScrollSection))
        {
            if (_scrollRetryCount++ < 8)
            {
                SchedulePendingScroll();
                return;
            }

            _pendingScrollSection = null;
            return;
        }

        _pendingScrollSection = null;
        _scrollRetryCount = 0;
    }

    private bool TryScrollToSection(string section)
    {
        if (!string.Equals(section, TextPageNavIds.SystemFonts, StringComparison.Ordinal))
            return true;

        // Шапка блока «Системные шрифты» внизу страницы.
        var target = SystemFontsInfoBlock;
        if (target.ActualHeight <= 0)
            return false;

        var scroll = PageShell.PageScrollViewer;
        if (scroll.Content is not UIElement contentRoot)
            return false;

        var y = GetTopInScrollContent(target, contentRoot);
        if (y >= double.MaxValue / 2)
            return false;

        scroll.ChangeView(null, Math.Max(0, y - 12), null, disableAnimation: false);
        return true;
    }

    private static double GetTopInScrollContent(FrameworkElement element, UIElement contentRoot)
    {
        try
        {
            var transform = element.TransformToVisual(contentRoot);
            return transform.TransformPoint(new Point(0, 0)).Y;
        }
        catch
        {
            return double.MaxValue;
        }
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Text preview → GlassCore_PaintTextPreview; UI-хост init только через Ensure (не DllMain).
        try
        {
            _ = NativeHostInterop.EnsureGlassRuntime();
        }
        catch
        {
            /* Ensure best-effort */
        }
        HookSessionDirty();
        StartWindowMetricsWatch();
        Reload();
        HookEnvironmentWatcher();
        RemeasureTextCardMinInnerWidthAfterLayout();
        SchedulePendingScroll();
    }

    private void RemeasureTextCardMinInnerWidthAfterLayout()
    {
        DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Normal, () =>
        {
            if (!IsLoaded)
                return;

            var wasStacked = !_textHeroSideBySide;
            var masterOn = IsInkCardActive();
            if (wasStacked)
                ApplyInkControlsSideBySideChrome();

            CacheTextCardMinInnerWidth();

            if (wasStacked)
                ApplyInkControlsStackedChrome(masterOn);

            UpdateTextHeroLayout();
        });
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        LiveReloadScheduler.EndTextGlowLivePreview();
        CancelInkRegistryDebounce();
        StopWindowMetricsWatch();
        UnhookSessionDirty();
        UnhookEnvironmentWatcher();
    }

    private void HookSessionDirty()
    {
        if (_sessionDirtyHandler is not null)
            return;

        _sessionDirtyHandler = (_, _) =>
        {
            RefreshPseudoUnsavedIndicator();
            RefreshSystemFontsPivotIndicators();
            RefreshSystemFontsRowIndicator();
        };
        SettingsSession.Current.DirtyChanged += _sessionDirtyHandler;
    }

    private void UnhookSessionDirty()
    {
        if (_sessionDirtyHandler is null)
            return;

        SettingsSession.Current.DirtyChanged -= _sessionDirtyHandler;
        _sessionDirtyHandler = null;
    }

    private void HookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is not null)
            return;

        if (DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            DispatcherQueue.TryEnqueue(Microsoft.UI.Dispatching.DispatcherQueuePriority.Low, HookEnvironmentWatcher);
            return;
        }

        _environmentChangedHandler = (_, e) =>
        {
            if (!IsLoaded)
                return;

            if (e.ThemeChanged || e.AccentChanged)
                DispatcherQueue.TryEnqueue(ReloadForTheme);

            // После SPI_SETNONCLIENTMETRICS (Save notice) — перерисовать ink по актуальному MessageFont.
            if (e.FontsChanged)
                DispatcherQueue.TryEnqueue(() => RefreshHeroPreview(TextPreviewRefreshScope.Full));
        };
        watcher.Changed += _environmentChangedHandler;
    }

    private void UnhookEnvironmentWatcher()
    {
        if (_environmentChangedHandler is null ||
            DesktopEnvironmentWatcher.Current is not { } watcher)
        {
            return;
        }

        watcher.Changed -= _environmentChangedHandler;
        _environmentChangedHandler = null;
    }

    private void ReloadForTheme()
    {
        if (_loading)
            return;

        _loading = true;
        GlobalTextGlowRegistry.TryLoadForTheme(ThemeRegistry.IsDarkMode(), out _model);
        SyncModelToUi();
        SyncInkModelToUi();
        _loading = false;
        UpdatePickerDefaultStates();
        UpdateInkDefaultStates();
        RefreshInkSliderRows();
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
        RefreshPseudoUnsavedIndicator();
        ReloadInkMaster();
    }

    private void Reload()
    {
        _loading = true;

        GlobalTextGlowRegistry.TryLoadForTheme(ThemeRegistry.IsDarkMode(), out _model);
        GlassInkAlphaCapsuleRegistry.TryLoad(out _inkModel);

        EffectsExpander.IsGateOn = GlobalTextGlowRegistry.IsPresent();
        SyncModelToUi();
        SyncInkModelToUi();

        _loading = false;
        UpdatePickerDefaultStates();
        UpdateInkDefaultStates();
        RefreshInkSliderRows();
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
        RefreshPseudoUnsavedIndicator();
        ReloadSystemFontsFromModel();
        ReloadInkMaster();
    }

    private void SyncModelToUi()
    {
        _glowEnableToggle.IsChecked = _model.GlowEnabled != 0;
        _radiusSlider.Value = Math.Clamp(_model.GlowRadiusThousandths, 0, 1000);
        SettingsSliderCommit.SyncCommittedValue(_radiusSlider, _radiusSlider.Value);
        UpdateRadiusValueLabel(_radiusSlider.Value);
        _spreadCombo.SelectedIndex = (int)Math.Clamp(_model.GlowSpreadMode, 0u, 2u);
        GlowPicker.SetColor(TextGlowColorHelper.ToPickerArgb(_model));
        ApplyGlowDeps(_model.GlowEnabled != 0);
        UpdatePickerDefaultStates();
    }

    private void SyncInkModelToUi()
    {
        _renderModeCombo.SelectedIndex = (int)Math.Clamp(_inkModel.RenderMode, 0u, 2u);
        var profile = _inkModel.GetActiveProfileCopy();
        _gammaSlider.Value = Math.Clamp(
            profile.CoverageGammaThousandths,
            InkCapsuleInterop.UiGammaMinThousandths,
            InkCapsuleInterop.UiGammaMaxThousandths);
        _blendSlider.Value = Math.Clamp(
            profile.TargetBlendAlpha,
            InkCapsuleInterop.UiBlendMin,
            InkCapsuleInterop.UiBlendMax);
        if (profile.PseudoGlowEnabled != 0)
        {
            _pseudoRadiusSlider.Value = Math.Clamp(
                profile.PseudoGlowRadiusThousandths,
                InkCapsuleInterop.UiPseudoRadiusMinThousandths,
                InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
            _pseudoIntensitySlider.Value = Math.Clamp(
                profile.PseudoGlowIntensity,
                InkCapsuleInterop.UiPseudoIntensityMin,
                InkCapsuleInterop.UiPseudoIntensityMax);
        }
        else
        {
            _pseudoRadiusSlider.Value = InkCapsuleInterop.UiPseudoRadiusMinThousandths;
            _pseudoIntensitySlider.Value = InkCapsuleInterop.UiPseudoIntensityMin;
        }
        SettingsSliderCommit.SyncCommittedValue(_gammaSlider, _gammaSlider.Value);
        SettingsSliderCommit.SyncCommittedValue(_blendSlider, _blendSlider.Value);
        SettingsSliderCommit.SyncCommittedValue(_pseudoRadiusSlider, _pseudoRadiusSlider.Value);
        SettingsSliderCommit.SyncCommittedValue(_pseudoIntensitySlider, _pseudoIntensitySlider.Value);
        UpdateGammaValueLabel(_gammaSlider.Value);
        UpdateBlendValueLabel(_blendSlider.Value);
        UpdatePseudoRadiusValueLabel(_pseudoRadiusSlider.Value);
        UpdatePseudoIntensityValueLabel(_pseudoIntensitySlider.Value);
        SyncPseudoGateFromModel();
        SyncGammaGateFromModel();
        ApplyInkAutoLockState();
        UpdateInkDefaultStates();
    }

    private bool IsInkCardActive() =>
        (ActiveInkMasterToggle?.IsOn ?? false)
        || _inkMasterArmed
        || GlassInkAlphaCapsuleRegistry.IsPresent();

    private void ReloadInkMaster()
    {
        if (_inkMasterArmed && GlassInkAlphaCapsuleRegistry.IsPresent())
            _inkMasterArmed = false;

        var on = IsInkCardActive();
        if (ActiveInkMasterToggle is null)
            return;

        if (ActiveInkMasterToggle.IsOn == on)
        {
            SyncInkMasterToggleUi(on);
            UpdateInkCardEnabled(on);
            ApplyInkAutoLockState();
            return;
        }

        SyncInkMasterToggleUi(on);
        UpdateInkCardEnabled(on);
        ApplyInkAutoLockState();
    }

    private void UpdateInkCardEnabled(bool enabled)
    {
        _renderModeCombo.IsEnabled = enabled;
    }

    private async void OnInkMasterToggled(bool on)
    {
        if (_loading)
            return;

        if (on)
        {
            _inkMasterArmed = true;
            UpdateInkCardEnabled(true);
            ApplyInkAutoLockState();
            return;
        }

        await DisableInkMasterAsync().ConfigureAwait(true);
    }

    private async Task DisableInkMasterAsync()
    {
        _inkMasterArmed = false;
        _gammaGateArmed = false;
        CancelInkRegistryDebounce();

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(async () =>
            {
                GlassInkAlphaCapsuleRegistry.Delete();
                _inkModel = GlassInkCapsuleModel.Factory;
                _inkModel.EnterAutoMode();
                InkCapsuleLiveReload.Commit(_inkModel);
                SettingsSession.Current.MarkDirty();

                await Task.CompletedTask.ConfigureAwait(false);
            }).ConfigureAwait(true);

            await seq.CosmeticAsync(() =>
            {
                _loading = true;
                try
                {
                    SyncInkModelToUi();
                }
                finally
                {
                    _loading = false;
                }

                UpdateInkCardEnabled(false);
                ReloadInkMaster();
                UpdateInkDefaultStates();
                RefreshInkSliderRows();
                GammaExpander.RefreshUnsavedIndicator();
                RefreshPseudoUnsavedIndicator();
                RefreshHeroPreview(TextPreviewRefreshScope.Full);
            }).ConfigureAwait(true);

            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    private void SyncGlowEnableFromModel()
    {
        var glowOn = _model.GlowEnabled != 0;
        _loading = true;
        _glowEnableToggle.IsChecked = glowOn;
        _loading = false;
        ApplyGlowDeps(glowOn);
    }

    private void ApplyGlowDeps(bool glowOn)
    {
        var enabled = EffectsExpander.IsGateOn && glowOn;
        GlowPanel.IsEnabled = EffectsExpander.IsGateOn;
        _radiusSlider.IsEnabled = enabled;
        _spreadCombo.IsEnabled = enabled;
        GlowPicker.IsEnabled = EffectsExpander.IsGateOn;
        _glowEnableToggle.IsEnabled = EffectsExpander.IsGateOn;
    }

    private void ApplyInkAutoLockState()
    {
        var auto = _inkModel.IsAutoMode;
        var inkActive = IsInkCardActive();
        GammaExpander.IsGateSwitchEnabled = !auto && inkActive;
        GammaExpander.IsExpansionEnabled = !auto && inkActive;
        GammaPanel.IsEnabled = !auto && inkActive && GammaExpander.IsGateOn;
        PseudoExpander.IsGateSwitchEnabled = !auto && inkActive;
        PseudoExpander.IsExpansionEnabled = !auto && inkActive;
        PseudoPanel.IsEnabled = !auto && inkActive && PseudoExpander.IsGateOn;

        if (!inkActive || auto)
        {
            GammaExpander.IsGateOn = false;
            PseudoExpander.IsGateOn = false;
        }
    }

    /// <summary>После commit/reload: разблокировать gate и восстановить IsGateOn из профиля.</summary>
    private void ApplyInkGateStateFromModel()
    {
        ApplyInkAutoLockState();
        if (_inkModel.IsAutoMode || !IsInkCardActive())
            return;

        SyncGammaGateFromModel();
        SyncPseudoGateFromModel();
        ApplyInkAutoLockState();
    }

    private static bool IsGammaGateOnFromProfile(GlassInkProfileModel profile, uint renderMode)
    {
        var factory = renderMode == 2
            ? GlassInkProfileModel.FactoryDual
            : GlassInkProfileModel.FactorySingle;
        return profile.CoverageGammaThousandths != factory.CoverageGammaThousandths
               || profile.TargetBlendAlpha != factory.TargetBlendAlpha;
    }

    private void SyncGammaGateFromModel()
    {
        if (_inkModel.IsAutoMode)
            return;

        var profile = _inkModel.GetActiveProfileCopy();
        GammaExpander.IsGateOn = _gammaGateArmed
                                 || IsGammaGateOnFromProfile(profile, _inkModel.RenderMode);
    }

    private static bool IsPseudoGateOnFromProfile(GlassInkProfileModel profile) =>
        profile.PseudoGlowEnabled != 0;

    private void SyncPseudoGateFromModel()
    {
        var profile = _inkModel.GetActiveProfileCopy();
        PseudoExpander.IsGateOn = IsPseudoGateOnFromProfile(profile);
    }

    private void UpdateRadiusValueLabel(double value) =>
        _radiusValueLabel.Text = ((int)value).ToString();

    private void UpdateGammaValueLabel(double value) =>
        _gammaValueLabel.Text = ((int)value).ToString();

    private void UpdateBlendValueLabel(double value) =>
        _blendValueLabel.Text = ((int)value).ToString();

    private void UpdatePseudoRadiusValueLabel(double value) =>
        _pseudoRadiusValueLabel.Text = ((int)value).ToString();

    private void UpdatePseudoIntensityValueLabel(double value) =>
        _pseudoIntensityValueLabel.Text = ((int)value).ToString();

    private bool CanSampleGlowLivePreview() =>
        !_loading && EffectsExpander.IsGateOn && _glowEnableToggle.IsChecked == true;

    private bool CanSampleInkGammaLivePreview() =>
        !_loading && !_inkModel.IsAutoMode && IsInkCardActive() && GammaExpander.IsGateOn;

    private bool CanSampleInkPseudoLivePreview() =>
        CanSampleInkGammaLivePreview() && PseudoExpander.IsGateOn;

    private void OnRadiusSliderPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (!CanSampleGlowLivePreview())
            return;

        LiveReloadScheduler.BeginTextGlowLivePreview(DispatcherQueue, SampleGlowModelFromUi);
    }

    private GlobalTextGlowModel SampleGlowModelFromUi()
    {
        var model = _model;
        model.GlowRadiusThousandths = (uint)Math.Clamp(_radiusSlider.Value, 0, 1000);
        TextGlowColorHelper.ApplyPickerArgb(ref model, ThemeRegistry.IsDarkMode(), GlowPicker.GetColorArgb());
        return model;
    }

    private GlobalTextGlowModel BuildPreviewModel()
    {
        if (!EffectsExpander.IsGateOn)
        {
            var off = GlobalTextGlowModel.Factory;
            off.GlowEnabled = 0;
            return off;
        }

        return CanSampleGlowLivePreview() ? SampleGlowModelFromUi() : _model;
    }

    private GlassInkCapsuleModel? BuildPreviewInkCapsule()
    {
        if (!IsInkCardActive() || _inkModel.IsAutoMode)
            return null;

        var preview = _inkModel.Clone();
        if (CanSampleInkGammaLivePreview())
            preview.SetActiveProfile(ReadInkProfileFromUi());
        return preview;
    }

    private void RefreshHeroPreview(TextPreviewRefreshScope scope) =>
        GlassPreview.ApplyGlowState(BuildPreviewModel(), BuildPreviewInkCapsule(), scope);

    private static void ApplyPseudoOff(ref GlassInkProfileModel profile)
    {
        profile.PseudoGlowEnabled = 0;
        profile.PseudoGlowRadiusThousandths = 0;
        profile.PseudoGlowIntensity = 0;
    }

    private static bool PolicyProvidesPseudoDefaults(GlassInkProfileModel factory) =>
        factory.PseudoGlowRadiusThousandths > 0
        || factory.PseudoGlowIntensity > 0;

    private static GlassInkProfileModel ResolveFactoryProfile(uint renderMode) =>
        renderMode == 2 ? GlassInkProfileModel.FactoryDual : GlassInkProfileModel.FactorySingle;

    private static void PersistInkProfileToActiveSlot(GlassInkCapsuleModel model, GlassInkProfileModel profile)
    {
        if (model.RenderMode == 2)
            model.Dual = profile;
        else
            model.Single = profile;
    }

    private static void PersistInkProfileToDisplaySlot(GlassInkCapsuleModel model, uint renderMode, GlassInkProfileModel profile)
    {
        if (renderMode == 2)
            model.Dual = profile;
        else
            model.Single = profile;
    }

    private static bool IsPseudoFieldsUnset(GlassInkProfileModel profile) =>
        profile.PseudoGlowEnabled == 0
        && profile.PseudoGlowRadiusThousandths == 0
        && profile.PseudoGlowIntensity == 0;

    private static void ClampPseudoSliderRange(ref GlassInkProfileModel profile)
    {
        if (profile.PseudoGlowEnabled != 0)
        {
            profile.PseudoGlowRadiusThousandths = (uint)Math.Clamp(
                (int)profile.PseudoGlowRadiusThousandths,
                (int)InkCapsuleInterop.UiPseudoRadiusMinThousandths,
                (int)InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
            profile.PseudoGlowIntensity = (uint)Math.Clamp(
                (int)profile.PseudoGlowIntensity,
                (int)InkCapsuleInterop.UiPseudoIntensityMin,
                (int)InkCapsuleInterop.UiPseudoIntensityMax);
            return;
        }

        profile.PseudoGlowRadiusThousandths = Math.Min(
            profile.PseudoGlowRadiusThousandths,
            InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
        profile.PseudoGlowIntensity = Math.Min(
            profile.PseudoGlowIntensity,
            InkCapsuleInterop.UiPseudoIntensityMax);
    }

    /// <summary>
    /// Factory pseudo из политики Shared (single: 0/0; dual: 610/113).
    /// При отсутствии policy-defaults и keepGateOn — Gate остаётся вкл. для ручной настройки.
    /// </summary>
    private static void ApplyFactoryPseudoDefaults(
        ref GlassInkProfileModel profile,
        uint renderMode,
        bool keepGateOnWhenPolicyEmpty)
    {
        var factory = ResolveFactoryProfile(renderMode);
        profile.PseudoGlowRadiusThousandths = factory.PseudoGlowRadiusThousandths;
        profile.PseudoGlowIntensity = factory.PseudoGlowIntensity;

        if (PolicyProvidesPseudoDefaults(factory))
        {
            profile.Clamp();
            return;
        }

        profile.PseudoGlowRadiusThousandths = InkCapsuleInterop.UiPseudoRadiusMinThousandths;
        profile.PseudoGlowIntensity = InkCapsuleInterop.UiPseudoIntensityMin;
        ClampPseudoSliderRange(ref profile);

        if (keepGateOnWhenPolicyEmpty)
        {
            profile.PseudoGlowEnabled = 1u;
            profile.WriteNeutralDeprecatedSlots();
            return;
        }

        profile.SyncPseudoEnabledFlag();
        profile.WriteNeutralDeprecatedSlots();
    }

    private static void ApplyPolicyPseudoOnGateEnable(ref GlassInkProfileModel profile, uint renderMode) =>
        ApplyFactoryPseudoDefaults(ref profile, renderMode, keepGateOnWhenPolicyEmpty: true);

    private static void SanitizeNoPseudoCapsule(GlassInkCapsuleModel model)
    {
        var active = model.GetActiveProfileCopy();
        if (active.PseudoGlowEnabled != 0
            || active.PseudoGlowRadiusThousandths > 0
            || active.PseudoGlowIntensity > 0)
        {
            return;
        }

        var single = model.Single;
        var dual = model.Dual;
        ApplyPseudoOff(ref single);
        ApplyPseudoOff(ref dual);
        model.Single = single;
        model.Dual = dual;
    }

    private static GlassInkProfileModel GetActiveProfileForCapsule(GlassInkCapsuleModel capsule) =>
        capsule.RenderMode == 2 ? capsule.Dual : capsule.Single;

    private static bool PseudoFieldsEqual(GlassInkProfileModel a, GlassInkProfileModel b) =>
        a.PseudoGlowEnabled == b.PseudoGlowEnabled
        && a.PseudoGlowRadiusThousandths == b.PseudoGlowRadiusThousandths
        && a.PseudoGlowIntensity == b.PseudoGlowIntensity;

    private static GlassInkCapsuleModel ResolveInkCapsuleBaseline()
    {
        var bytes = SettingsSession.Current.RegistrySnapshot
            .TryGetGeneralRootSnapshotBytes(GlassInkAlphaCapsuleRegistry.ValueName);
        return bytes is { Length: >= GlassInkCapsuleModel.BinarySize }
            ? GlassInkCapsuleModel.FromBytes(bytes)
            : GlassInkCapsuleModel.Factory;
    }

    private void RefreshPseudoUnsavedIndicator()
    {
        if (!SettingsSession.Current.IsBaselineCaptured)
        {
            PseudoExpander.IsUnsavedPending = false;
            return;
        }

        if (!SettingsSession.Current.HasUnsavedInScope(TextScopeCatalog.InkCapsuleScope))
        {
            PseudoExpander.IsUnsavedPending = false;
            return;
        }

        var baseline = ResolveInkCapsuleBaseline();
        var currentActive = GetActiveProfileForCapsule(_inkModel);
        var baselineActive = GetActiveProfileForCapsule(baseline);
        PseudoExpander.IsUnsavedPending = !PseudoFieldsEqual(currentActive, baselineActive);
    }

    private void UpdatePickerDefaultStates()
    {
        if (_loading)
            return;

        GlowPanel.SetDefaultColorActionState(
            EffectsExpander.IsGateOn,
            TextGlowColorHelper.AreResettableParametersAtFactory(_model));
    }

    private void UpdateInkDefaultStates()
    {
        if (_loading)
        {
            GammaPanel.SetDefaultActionState(showDefaultAction: false, disableDefaultButton: true);
            PseudoPanel.SetDefaultActionState(showDefaultAction: false, disableDefaultButton: true);
            return;
        }

        if (_inkModel.IsAutoMode)
        {
            GammaPanel.SetDefaultActionState(showDefaultAction: true, disableDefaultButton: true, isEnabled: false);
            PseudoPanel.SetDefaultActionState(showDefaultAction: true, disableDefaultButton: true, isEnabled: false);
            return;
        }

        if (!IsInkCardActive())
        {
            GammaPanel.SetDefaultActionState(showDefaultAction: true, disableDefaultButton: true, isEnabled: false);
            PseudoPanel.SetDefaultActionState(showDefaultAction: true, disableDefaultButton: true, isEnabled: false);
            return;
        }

        if (!GammaExpander.IsGateOn)
        {
            GammaPanel.SetDefaultActionState(showDefaultAction: true, disableDefaultButton: true, isEnabled: false);
        }

        if (!PseudoExpander.IsGateOn)
        {
            PseudoPanel.SetDefaultActionState(showDefaultAction: true, disableDefaultButton: true, isEnabled: false);
        }

        var profile = _inkModel.GetActiveProfileCopy();
        var factory = _inkModel.RenderMode == 2
            ? GlassInkProfileModel.FactoryDual
            : GlassInkProfileModel.FactorySingle;

        var gammaAtFactory = profile.CoverageGammaThousandths == factory.CoverageGammaThousandths
                             && profile.TargetBlendAlpha == factory.TargetBlendAlpha;
        GammaPanel.SetDefaultActionState(showDefaultAction: true, disableDefaultButton: gammaAtFactory, isEnabled: true);

        var pseudoAtFactory = profile.PseudoGlowRadiusThousandths == factory.PseudoGlowRadiusThousandths
                              && profile.PseudoGlowIntensity == factory.PseudoGlowIntensity
                              && profile.PseudoGlowEnabled == factory.PseudoGlowEnabled;
        if (PseudoExpander.IsGateOn)
        {
            PseudoPanel.SetDefaultActionState(showDefaultAction: true, disableDefaultButton: pseudoAtFactory, isEnabled: true);
        }
    }

    private void SaveInkUiToActiveProfile()
    {
        if (_inkModel.IsAutoMode)
            return;

        var profile = ReadInkProfileFromUi();
        _inkModel.SetActiveProfile(profile);
    }

    private GlassInkProfileModel ReadInkProfileFromUi()
    {
        var profile = _inkModel.GetActiveProfileCopy();
        profile.CoverageGammaThousandths = (uint)Math.Clamp(
            _gammaSlider.Value,
            InkCapsuleInterop.UiGammaMinThousandths,
            InkCapsuleInterop.UiGammaMaxThousandths);
        profile.TargetBlendAlpha = (uint)Math.Clamp(
            _blendSlider.Value,
            InkCapsuleInterop.UiBlendMin,
            InkCapsuleInterop.UiBlendMax);
        if (PseudoExpander.IsGateOn)
        {
            profile.PseudoGlowRadiusThousandths = (uint)Math.Clamp(
                _pseudoRadiusSlider.Value,
                InkCapsuleInterop.UiPseudoRadiusMinThousandths,
                InkCapsuleInterop.UiPseudoRadiusMaxThousandths);
            profile.PseudoGlowIntensity = (uint)Math.Clamp(
                _pseudoIntensitySlider.Value,
                InkCapsuleInterop.UiPseudoIntensityMin,
                InkCapsuleInterop.UiPseudoIntensityMax);
        }
        else
        {
            profile.PseudoGlowRadiusThousandths = 0u;
            profile.PseudoGlowIntensity = 0u;
        }

        profile.Clamp();
        profile.PseudoGlowEnabled = PseudoExpander.IsGateOn ? 1u : 0u;
        if (!PseudoExpander.IsGateOn)
        {
            profile.PseudoGlowRadiusThousandths = 0u;
            profile.PseudoGlowIntensity = 0u;
        }

        return profile;
    }

    private void ApplyInkSliderToActiveProfile(Action<GlassInkProfileModel> mutate)
    {
        var profile = ReadInkProfileFromUi();
        mutate(profile);
        profile.Clamp();
        _inkModel.SetActiveProfile(profile);
    }

    private void ScheduleInkRegistryWriteDebounced()
    {
        if (!CanSampleInkGammaLivePreview())
            return;

        _inkRegistryDebounceCts?.Cancel();
        _inkRegistryDebounceCts?.Dispose();
        _inkRegistryDebounceCts = new CancellationTokenSource();
        var token = _inkRegistryDebounceCts.Token;
        _ = RunInkRegistryDebouncedAsync(token);
    }

    private async Task RunInkRegistryDebouncedAsync(CancellationToken token)
    {
        try
        {
            await Task.Delay(InkRegistryDebounceMs, token).ConfigureAwait(true);
        }
        catch (OperationCanceledException)
        {
            return;
        }

        if (token.IsCancellationRequested || !CanSampleInkGammaLivePreview())
            return;

        if (SettingsSession.Current.IsShuttingDown)
            return;

        WriteInkRegistryFromUiForLivePreview();
    }

    private void WriteInkRegistryFromUiForLivePreview()
    {
        if (!CanSampleInkGammaLivePreview())
            return;

        var preview = _inkModel.Clone();
        preview.SetActiveProfile(ReadInkProfileFromUi());
        InkCapsuleLiveReload.SyncDesktopFromRegistry(preview);
    }

    private void FlushInkRegistryLivePreviewWithReload()
    {
        CancelInkRegistryDebounce();

        if (!CanSampleInkGammaLivePreview())
            return;

        var preview = _inkModel.Clone();
        preview.SetActiveProfile(ReadInkProfileFromUi());
        InkCapsuleLiveReload.WriteForLivePreview(preview);
    }

    private void CancelInkRegistryDebounce()
    {
        _inkRegistryDebounceCts?.Cancel();
        _inkRegistryDebounceCts?.Dispose();
        _inkRegistryDebounceCts = null;
    }

    private void OnRenderModeSelectionChanged()
    {
        if (_loading || !IsInkCardActive())
            return;

        var newMode = (uint)Math.Clamp(_renderModeCombo.SelectedIndex, 0, 2);
        if (newMode == _inkModel.RenderMode)
            return;

        var prevMode = _inkModel.RenderMode;

        if (newMode == 0)
        {
            if (prevMode == 1)
                _inkModel.Single = ReadInkProfileFromUi();
            else if (prevMode == 2)
                _inkModel.Dual = ReadInkProfileFromUi();
            _inkModel.EnterAutoMode();
        }
        else if (prevMode == 0)
        {
            _inkModel.RenderMode = newMode;
        }
        else
        {
            var current = ReadInkProfileFromUi();
            PersistInkProfileToActiveSlot(_inkModel, current);
            _inkModel.RenderMode = newMode;
            PersistInkProfileToDisplaySlot(_inkModel, newMode, current);
        }

        _loading = true;
        SyncInkModelToUi();
        _loading = false;

        InkCapsuleLiveReload.Commit(_inkModel);
        UpdateInkDefaultStates();
        RefreshInkSliderRows();
        GammaExpander.RefreshUnsavedIndicator();
        RefreshPseudoUnsavedIndicator();
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
    }

    private void OnInkGammaResetToDefaults()
    {
        if (_loading || _inkModel.IsAutoMode || !GammaExpander.IsGateOn || !IsInkCardActive())
            return;

        var profile = _inkModel.GetActiveProfileCopy();
        var factory = _inkModel.RenderMode == 2
            ? GlassInkProfileModel.FactoryDual
            : GlassInkProfileModel.FactorySingle;
        profile.CoverageGammaThousandths = factory.CoverageGammaThousandths;
        profile.TargetBlendAlpha = factory.TargetBlendAlpha;
        profile.Clamp();
        _inkModel.SetActiveProfile(profile);
        SyncInkModelToUi();
        InkCapsuleLiveReload.Commit(_inkModel);
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
        UpdateInkDefaultStates();
        GammaExpander.RefreshUnsavedIndicator();
        RefreshPseudoUnsavedIndicator();
    }

    private void OnInkPseudoResetToDefaults()
    {
        if (_loading || _inkModel.IsAutoMode || !PseudoExpander.IsGateOn || !IsInkCardActive())
            return;

        var profile = _inkModel.GetActiveProfileCopy();
        ApplyFactoryPseudoDefaults(ref profile, _inkModel.RenderMode, keepGateOnWhenPolicyEmpty: true);
        _inkModel.SetActiveProfile(profile);
        SyncInkModelToUi();
        InkCapsuleLiveReload.Commit(_inkModel);
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
        UpdateInkDefaultStates();
        GammaExpander.RefreshUnsavedIndicator();
        RefreshPseudoUnsavedIndicator();
    }

    private async void OnVisualTuneClick()
    {
        if (_loading)
            return;

        if (App.ShellWindow is not { } window)
            return;

        if (!NativeHostInterop.CanShowTextVisualTuningDialog())
        {
            await ShowTextVisualTuningUnavailableDialogAsync(window);
            return;
        }

        SaveInkUiToActiveProfile();
        var working = _inkModel.Clone();
        var wizard = new TextWizard.TextVisualTuningWindow(
            window,
            working,
            GlassSettingsResources.Get("Text_PreviewSample"),
            ThemeRegistry.IsDarkMode());
        wizard.Activate();
        if (!await wizard.WaitForCloseAsync())
            return;

        working = wizard.ResultCapsule;

        _inkModel = working;
        SanitizeNoPseudoCapsule(_inkModel);
        _inkModel.Clamp();
        _loading = true;
        SyncInkModelToUi();
        _loading = false;

        InkCapsuleLiveReload.Commit(_inkModel);
        ReloadInkMaster();
        ApplyInkGateStateFromModel();
        UpdateInkDefaultStates();
        RefreshInkSliderRows();
        GammaExpander.RefreshUnsavedIndicator();
        RefreshPseudoUnsavedIndicator();
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
    }

    private static async Task ShowTextVisualTuningUnavailableDialogAsync(Window window)
    {
        var dialog = new ContentDialog
        {
            Title = GlassSettingsResources.Get("Text_InkVisualTuneUnavailableTitle"),
            Content = new TextBlock
            {
                Text = GlassSettingsResources.Get("Text_InkVisualTuneUnavailableBody"),
                TextWrapping = TextWrapping.Wrap
            },
            PrimaryButtonText = GlassSettingsResources.Get("Dialog_Understood"),
            DefaultButton = ContentDialogButton.Primary,
            XamlRoot = window.Content.XamlRoot
        };

        _ = await dialog.ShowAsync();
    }

    private void OnGlowEnableCommitted(bool enabled)
    {
        if (_loading || !EffectsExpander.IsGateOn)
            return;

        _model.GlowEnabled = enabled ? 1u : 0u;
        ApplyGlowDeps(enabled);
        TextGlowLiveReload.Commit(_model);
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
        UpdatePickerDefaultStates();
        EffectsExpander.RefreshUnsavedIndicator();
    }

    private void EffectsGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        if (EffectsExpander.IsGateOn)
        {
            ApplyGlowDeps(_model.GlowEnabled != 0);
            UpdatePickerDefaultStates();
            RefreshHeroPreview(TextPreviewRefreshScope.Full);
            return;
        }

        GlobalTextGlowRegistry.DeleteAll();
        SettingsSession.Current.MarkDirty();
        _loading = true;
        try
        {
            Reload();
        }
        finally
        {
            _loading = false;
        }

        TextGlowLiveReload.NotifyChanged();
    }

    private async void GammaGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || _inkModel.IsAutoMode || !IsInkCardActive())
            return;

        if (GammaExpander.IsGateOn)
        {
            _gammaGateArmed = true;
            ApplyInkAutoLockState();
            UpdateInkDefaultStates();
            return;
        }

        await DisableGammaGateAsync().ConfigureAwait(true);
    }

    private async Task DisableGammaGateAsync()
    {
        _gammaGateArmed = false;
        CancelInkRegistryDebounce();

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(async () =>
            {
                var profile = _inkModel.GetActiveProfileCopy();
                var factory = _inkModel.RenderMode == 2
                    ? GlassInkProfileModel.FactoryDual
                    : GlassInkProfileModel.FactorySingle;
                profile.CoverageGammaThousandths = factory.CoverageGammaThousandths;
                profile.TargetBlendAlpha = factory.TargetBlendAlpha;
                profile.Clamp();
                _inkModel.SetActiveProfile(profile);
                InkCapsuleLiveReload.Commit(_inkModel);
                SettingsSession.Current.MarkDirty();

                await Task.CompletedTask.ConfigureAwait(false);
            }).ConfigureAwait(true);

            await seq.CosmeticAsync(() =>
            {
                _loading = true;
                try
                {
                    SyncInkModelToUi();
                }
                finally
                {
                    _loading = false;
                }

                ApplyInkAutoLockState();
                UpdateInkDefaultStates();
                RefreshInkSliderRows();
                GammaExpander.RefreshUnsavedIndicator();
                RefreshPseudoUnsavedIndicator();
                RefreshHeroPreview(TextPreviewRefreshScope.Full);
            }).ConfigureAwait(true);

            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    private void PseudoGate_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading || _inkModel.IsAutoMode || !IsInkCardActive())
            return;

        if (PseudoExpander.IsGateOn)
        {
            var profile = _inkModel.GetActiveProfileCopy();
            ApplyPolicyPseudoOnGateEnable(ref profile, _inkModel.RenderMode);
            _inkModel.SetActiveProfile(profile);

            _loading = true;
            SyncInkModelToUi();
            _loading = false;

            InkCapsuleLiveReload.Commit(_inkModel);
            SettingsSession.Current.MarkDirty();
            UpdateInkDefaultStates();
            RefreshPseudoUnsavedIndicator();
            RefreshHeroPreview(TextPreviewRefreshScope.Full);
            return;
        }

        var offProfile = ReadInkProfileFromUi();
        offProfile.PseudoGlowEnabled = 0u;
        offProfile.PseudoGlowRadiusThousandths = 0u;
        offProfile.PseudoGlowIntensity = 0u;
        offProfile.WriteNeutralDeprecatedSlots();
        _inkModel.SetActiveProfile(offProfile);

        _loading = true;
        SyncInkModelToUi();
        _loading = false;

        InkCapsuleLiveReload.Commit(_inkModel);
        SettingsSession.Current.MarkDirty();
        UpdateInkDefaultStates();
        RefreshPseudoUnsavedIndicator();
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
    }

    private void OnGlowResetToDefaults()
    {
        if (_loading || !EffectsExpander.IsGateOn)
            return;

        var glowEnabled = _model.GlowEnabled;
        _model = GlobalTextGlowModel.Factory;
        _model.GlowEnabled = glowEnabled;
        _model.Clamp();
        SyncModelToUi();
        TextGlowLiveReload.Commit(_model);
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
        UpdatePickerDefaultStates();
        EffectsExpander.RefreshUnsavedIndicator();
    }

    private void OnGlowColorCommitted(uint argb)
    {
        if (_loading || !EffectsExpander.IsGateOn)
            return;

        LiveReloadScheduler.CancelGlobalTextGlowPending();
        TextGlowColorHelper.ApplyPickerArgb(ref _model, ThemeRegistry.IsDarkMode(), argb);
        SyncGlowEnableFromModel();
        GlowPicker.SetColor(TextGlowColorHelper.ToPickerArgb(_model));
        TextGlowLiveReload.Commit(_model);
        RefreshHeroPreview(TextPreviewRefreshScope.Full);
        UpdatePickerDefaultStates();
    }

    private void BuildSystemFontsSection()
    {
        SystemFontsRow.IconGlyph = SettingsSectionGlyphs.SystemFonts;

        var secondaryStyle = Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"] as Style;
        _systemFontNameText.Style = secondaryStyle;
        _systemFontNameText.MinWidth = 80;
        _systemFontNameText.MaxWidth = 180;
        _systemFontNameText.TextTrimming = TextTrimming.CharacterEllipsis;
        _systemFontNameText.VerticalAlignment = VerticalAlignment.Center;
        _systemFontSizeText.Style = secondaryStyle;
        _systemFontSizeText.MinWidth = 28;
        _systemFontSizeText.TextAlignment = TextAlignment.Right;
        _systemFontSizeText.VerticalAlignment = VerticalAlignment.Center;
        _systemFontChooseButton.MinWidth = 88;
        _systemFontChooseButton.VerticalAlignment = VerticalAlignment.Center;
        _systemFontChooseButton.Click += (_, _) => _ = OnSystemFontChooseClickAsync();

        var headerActions = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 12,
            VerticalAlignment = VerticalAlignment.Center,
            Children =
            {
                _systemFontNameText,
                _systemFontSizeText,
                _systemFontChooseButton
            }
        };
        SystemFontsRow.HeaderActions = headerActions;

        var tabLabels = GlassSettingsResources.GetSemicolonList("Text_SystemFontsPivotTabs");
        _systemFontsPivotTabLabels = tabLabels;
        for (var i = 0; i < SystemFontsCatalog.SlotCount && i < SystemFontsPivot.Items.Count; i++)
        {
            var label = i < tabLabels.Length ? tabLabels[i] : $"#{i}";
            _systemFontsPivotHeaders[i] = SettingsPivotTabHeader.Create(label);
            SystemFontsPivot.Items[i] = new PivotItem { Header = _systemFontsPivotHeaders[i].Root };
        }

        _lastSystemFontsPivotIndex = 0;
        _systemFontsPivotIndex = 0;
        if (SystemFontsPivot.SelectedIndex < 0)
            SystemFontsPivot.SelectedIndex = 0;
    }

    private void StartWindowMetricsWatch()
    {
        if (_windowMetricsWatch is not null)
            return;

        _windowMetricsWatch = new WindowMetricsRegistryWatchService(DispatcherQueue, SettingsSession.Current.RegistrySnapshot);
        _windowMetricsWatchHandler = (_, _) =>
        {
            if (SettingsSession.Current.HasUnsavedInScope(TextScopeCatalog.SystemFontsScope))
                return;
            ReloadFromExternalRegistrySync();
        };
        _windowMetricsWatch.ExternalRegistryChanged += _windowMetricsWatchHandler;
        _windowMetricsWatch.Start();
        _windowMetricsWatch.SyncFingerprintFromSnapshot(SettingsSession.Current.RegistrySnapshot);
    }

    private void StopWindowMetricsWatch()
    {
        if (_windowMetricsWatch is null)
            return;

        if (_windowMetricsWatchHandler is not null)
            _windowMetricsWatch.ExternalRegistryChanged -= _windowMetricsWatchHandler;

        _windowMetricsWatch.Dispose();
        _windowMetricsWatch = null;
        _windowMetricsWatchHandler = null;
    }

    private void ReloadSystemFontsFromModel()
    {
        _systemFontsModel.LoadFromRegistryAndTheme();
        _systemFontsModel.CurrentIndex = _systemFontsPivotIndex;
        InitializeSystemFontsScope();
        RefreshSystemFontsUi();
    }

    private void InitializeSystemFontsScope()
    {
        _loading = true;
        try
        {
            _isSelectiveSystemFontsScope = !_systemFontsModel.AllWorkFontsEqual();
            SystemFontsScopeBar.IsSeparateScope = _isSelectiveSystemFontsScope;
            ApplySystemFontsScopeUi();
        }
        finally
        {
            _loading = false;
        }
    }

    private void ApplySystemFontsScopeUi() =>
        SystemFontsPivot.Visibility = _isSelectiveSystemFontsScope
            ? Visibility.Visible
            : Visibility.Collapsed;

    private int GetSystemFontsDisplayIndex() =>
        _isSelectiveSystemFontsScope ? _systemFontsPivotIndex : 0;

    private void UpdateSystemFontsRowTitle()
    {
        var baseTitle = GlassSettingsResources.Get("Text_SystemFontsTitle");
        if (!_isSelectiveSystemFontsScope)
        {
            SystemFontsRow.Title = baseTitle;
            return;
        }

        var index = _systemFontsPivotIndex;
        var tabLabel = index >= 0 && index < _systemFontsPivotTabLabels.Length
            ? _systemFontsPivotTabLabels[index]
            : string.Empty;
        SystemFontsRow.Title = string.IsNullOrEmpty(tabLabel)
            ? baseTitle
            : $"{baseTitle} ({tabLabel})";
    }

    private async void SystemFontsScopeBar_OnScopeChanged(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        var newSelective = SystemFontsScopeBar.IsSeparateScope;
        if (newSelective == _isSelectiveSystemFontsScope)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.AnimatingAsync(async () =>
            {
                await PivotTabContentTransition.AnimateVerticalChangeAsync(
                    SystemFontsScopeContentHost,
                    toSeparate: newSelective,
                    () =>
                    {
                        _isSelectiveSystemFontsScope = newSelective;
                        ApplySystemFontsScopeUi();
                        RefreshSystemFontsUi();
                    }).ConfigureAwait(true);
            }).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    private void RefreshSystemFontsUi()
    {
        var index = GetSystemFontsDisplayIndex();
        if (index is < 0 or >= SystemFontsCatalog.SlotCount)
            index = 0;

        _systemFontsModel.CurrentIndex = index;
        var lf = _systemFontsModel.WorkLf[index];
        var face = string.IsNullOrWhiteSpace(lf.lfFaceName) ? "—" : lf.lfFaceName.Trim();
        _systemFontNameText.Text = face;
        ToolTipService.SetToolTip(_systemFontNameText, face);
        _systemFontSizeText.Text = SystemFontsModel.FormatPointSize(lf);

        UpdateSystemFontsRowTitle();

        var gateOn = _isSelectiveSystemFontsScope
            ? _systemFontsModel.WorkCustom[_systemFontsPivotIndex]
            : _systemFontsModel.AnyWorkCustom();
        _loading = true;
        try
        {
            SystemFontsRow.IsGateOn = gateOn;
        }
        finally
        {
            _loading = false;
        }

        _systemFontChooseButton.IsEnabled = gateOn;
        SystemFontsRow.UnsavedScope = _isSelectiveSystemFontsScope
            ? TextScopeCatalog.SystemFontScope(_systemFontsPivotIndex)
            : TextScopeCatalog.SystemFontsScope;
        SystemFontsRow.RefreshUnsavedIndicator();
        SystemFontsRow.RefreshHeaderLayout();
        if (_isSelectiveSystemFontsScope)
            RefreshSystemFontsPivotIndicators();
    }

    private void RefreshSystemFontsPivotIndicators()
    {
        for (var i = 0; i < _systemFontsPivotHeaders.Length; i++)
            _systemFontsPivotHeaders[i].RefreshUnsaved(TextScopeCatalog.SystemFontScope(i));
    }

    private void RefreshSystemFontsRowIndicator() =>
        SystemFontsRow.RefreshUnsavedIndicator();

    public void ReloadFromExternalRegistrySync()
    {
        if (_loading)
            return;

        _loading = true;
        try
        {
            _systemFontsModel.ReloadThemeRefs();
            _systemFontsModel.ReloadWorkFromRegistry();
            InitializeSystemFontsScope();
            RefreshSystemFontsUi();
        }
        finally
        {
            _loading = false;
        }
    }

    private async void SystemFontsPivot_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || !_isSelectiveSystemFontsScope)
            return;

        var newIndex = SystemFontsPivot.SelectedIndex;
        if (newIndex is < 0 or >= SystemFontsCatalog.SlotCount)
            return;

        var previousIndex = _lastSystemFontsPivotIndex;
        if (previousIndex == newIndex)
            return;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.AnimatingAsync(async () =>
            {
                await PivotTabContentTransition.AnimateTabChangeAsync(
                    SystemFontsContentHost,
                    previousIndex,
                    newIndex,
                    () =>
                    {
                        _systemFontsPivotIndex = newIndex;
                        RefreshSystemFontsUi();
                    }).ConfigureAwait(true);
            }).ConfigureAwait(true);
        }).ConfigureAwait(true);

        _lastSystemFontsPivotIndex = newIndex;
    }

    private async void SystemFontsRow_OnGateToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        var gateOn = SystemFontsRow.IsGateOn;
        var registryWriteOk = false;

        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(async () =>
            {
                WindowMetricsRegistryWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(800));

                if (_isSelectiveSystemFontsScope)
                {
                    registryWriteOk = ApplySelectiveSystemFontsGateToggle(gateOn);
                }
                else
                {
                    registryWriteOk = ApplyGlobalSystemFontsGateToggle(gateOn);
                }

                await Task.CompletedTask.ConfigureAwait(true);
            }).ConfigureAwait(true);

            await seq.CosmeticAsync(() =>
            {
                RefreshSystemFontsUi();
                if (registryWriteOk)
                {
                    RefreshSystemFontsRowIndicator();
                    RefreshSystemFontsPivotIndicators();
                }
            }).ConfigureAwait(true);

            if (registryWriteOk)
                await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    private bool ApplySelectiveSystemFontsGateToggle(bool gateOn)
    {
        var index = _systemFontsPivotIndex;
        if (index is < 0 or >= SystemFontsCatalog.SlotCount)
            return false;

        if (gateOn)
        {
            _systemFontsModel.WorkCustom[index] = true;
            return false;
        }

        if (SystemFontsLogFont.EqualsForCompare(
                _systemFontsModel.RefLf[index],
                _systemFontsModel.WorkLf[index]))
        {
            _systemFontsModel.WorkCustom[index] = false;
            return false;
        }

        var previousWork = _systemFontsModel.WorkLf[index];
        var previousCustom = _systemFontsModel.WorkCustom[index];
        _systemFontsModel.ResetSlotToTheme(index);
        if (!WindowMetricsFontRegistry.WriteSlot(index, _systemFontsModel.WorkLf[index]))
        {
            _systemFontsModel.SetWorkFont(index, previousWork);
            _systemFontsModel.WorkCustom[index] = previousCustom;
            return false;
        }

        SettingsSession.Current.MarkDirty();
        WindowMetricsRegistryWatchService.Current?.SyncFingerprintFromSnapshot(
            SettingsSession.Current.RegistrySnapshot);
        return true;
    }

    private bool ApplyGlobalSystemFontsGateToggle(bool gateOn)
    {
        if (gateOn)
        {
            for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
                _systemFontsModel.WorkCustom[i] = true;
            return false;
        }

        var allMatchRef = true;
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
        {
            if (!SystemFontsLogFont.EqualsForCompare(
                    _systemFontsModel.RefLf[i],
                    _systemFontsModel.WorkLf[i]))
            {
                allMatchRef = false;
                break;
            }
        }

        if (allMatchRef)
        {
            for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
                _systemFontsModel.WorkCustom[i] = false;
            return false;
        }

        var previousWork = CloneWorkFonts();
        var previousCustom = (bool[])_systemFontsModel.WorkCustom.Clone();
        _systemFontsModel.ResetAllSlotsToTheme();
        if (!WindowMetricsFontRegistry.WriteAll(_systemFontsModel.RefLf))
        {
            RestoreWorkFonts(previousWork, previousCustom);
            return false;
        }

        SettingsSession.Current.MarkDirty();
        WindowMetricsRegistryWatchService.Current?.SyncFingerprintFromSnapshot(
            SettingsSession.Current.RegistrySnapshot);
        return true;
    }

    private SystemFontsLogFont[] CloneWorkFonts()
    {
        var snapshot = new SystemFontsLogFont[SystemFontsCatalog.SlotCount];
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
            snapshot[i] = _systemFontsModel.WorkLf[i];
        return snapshot;
    }

    private void RestoreWorkFonts(SystemFontsLogFont[] work, bool[] custom)
    {
        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
        {
            _systemFontsModel.SetWorkFont(i, work[i]);
            _systemFontsModel.WorkCustom[i] = custom[i];
        }
    }

    private async Task OnSystemFontChooseClickAsync()
    {
        if (_loading || !SystemFontsRow.IsGateOn)
            return;

        var index = GetSystemFontsDisplayIndex();
        var initial = _systemFontsModel.WorkLf[index];
        var picked = await SystemFontPickerDialogService.PickAsync(initial).ConfigureAwait(true);

        if (picked is null)
            return;

        var registryWriteOk = false;
        await PageShell.Operations.RunSequenceAsync(async seq =>
        {
            await seq.RegistryMutationAsync(async () =>
            {
                WindowMetricsRegistryWatchService.Current?.SuppressExternalDirty(TimeSpan.FromMilliseconds(800));

                if (_isSelectiveSystemFontsScope)
                {
                    registryWriteOk = ApplySelectiveSystemFontPick(index, picked.Value);
                }
                else
                {
                    registryWriteOk = ApplyGlobalSystemFontPick(picked.Value);
                }

                await Task.CompletedTask.ConfigureAwait(true);
            }).ConfigureAwait(true);

            if (!registryWriteOk)
                return;

            await seq.CosmeticAsync(() =>
            {
                if (!_isSelectiveSystemFontsScope)
                    InitializeSystemFontsScope();

                RefreshSystemFontsUi();
                RefreshSystemFontsRowIndicator();
                RefreshSystemFontsPivotIndicators();
            }).ConfigureAwait(true);

            await seq.NoticeSyncAsync(() => PageShell.FlushSessionNotice()).ConfigureAwait(true);
        }).ConfigureAwait(true);
    }

    private bool ApplySelectiveSystemFontPick(int index, in SystemFontsLogFont picked)
    {
        if (!WindowMetricsFontRegistry.WriteSlot(index, picked))
            return false;

        _systemFontsModel.SetWorkFont(index, picked);
        _systemFontsModel.WorkCustom[index] = !_systemFontsModel.EqualsRef(index, picked);
        SettingsSession.Current.MarkDirty();
        WindowMetricsRegistryWatchService.Current?.SyncFingerprintFromSnapshot(
            SettingsSession.Current.RegistrySnapshot);
        return true;
    }

    private bool ApplyGlobalSystemFontPick(in SystemFontsLogFont picked)
    {
        var previousWork = CloneWorkFonts();
        var previousCustom = (bool[])_systemFontsModel.WorkCustom.Clone();
        _systemFontsModel.ApplyFontToAllSlots(picked);
        if (!WindowMetricsFontRegistry.WriteAll(_systemFontsModel.WorkLf))
        {
            RestoreWorkFonts(previousWork, previousCustom);
            return false;
        }

        SettingsSession.Current.MarkDirty();
        WindowMetricsRegistryWatchService.Current?.SyncFingerprintFromSnapshot(
            SettingsSession.Current.RegistrySnapshot);
        return true;
    }
}
