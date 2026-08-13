using GlassSettings.Controls;
using GlassSettings.Core.Layout;
using GlassSettings.Core.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.TextWizard;

/// <summary>Строит UI карточек из кэша ArN[6][3] (рекурсивные ярусы).</summary>
public sealed class WizardPreviewMapRenderer
{
    private const double CardSpacing = HeroPreviewLayoutMetrics.WizardCardSpacing;
    private const double RowShadowSpillTop = HeroPreviewLayoutMetrics.WizardCardRowShadowSpillVerticalTop;
    private const double RowShadowSpillBottom = HeroPreviewLayoutMetrics.WizardCardRowInteractiveShadowSpillBottom;
    private const int BottomPresetColumns = HeroPreviewLayoutMetrics.WizardBottomPresetColumns;

    private static double ListHostWidth => HeroPreviewLayoutMetrics.WizardCardListHostWidth;
    private static double ListRowHorizontalSpill => HeroPreviewLayoutMetrics.WizardCardListRowHorizontalSpill;

    private static double CardFootprintWidth => HeroPreviewLayoutMetrics.TextLayerFootprintWidth;
    private static double CardFootprintHeight => HeroPreviewLayoutMetrics.CardFootprintHeight;

    private static Thickness TopRowContentMargin =>
        new(ListRowHorizontalSpill, RowShadowSpillTop, ListRowHorizontalSpill, 0);

    private static Thickness BottomRowContentMargin =>
        new(ListRowHorizontalSpill, 0, ListRowHorizontalSpill, 0);

    private static Thickness SingleTierGridMargin =>
        new(ListRowHorizontalSpill, RowShadowSpillTop, ListRowHorizontalSpill, 0);

    private static double TopRowHostHeight =>
        RowShadowSpillTop + CardFootprintHeight + RowShadowSpillBottom;

    private static double BottomRowHostHeight =>
        CardFootprintHeight + RowShadowSpillBottom;

    private readonly TextVisualTuningWizardEngine _engine;
    private readonly string _sampleText;
    private bool _isDarkTheme;

    private readonly List<WizardInkPreviewCard> _cards = [];
    private readonly List<WizardInkPreviewCard> _topRowCards = [];
    private readonly List<WizardInkPreviewCard> _descendantCards = [];
    private ScrollViewer? _descendantScrollHost;
    private WizardPreviewPageSlot? _currentSlot;

    public WizardPreviewMapRenderer(
        TextVisualTuningWizardEngine engine,
        string sampleText,
        bool isDarkTheme)
    {
        _engine = engine;
        _sampleText = sampleText;
        _isDarkTheme = isDarkTheme;
    }

    public void SetIsDarkTheme(bool isDarkTheme)
    {
        _isDarkTheme = isDarkTheme;
        foreach (var card in _cards)
            card.IsDarkTheme = isDarkTheme;
    }

    public IReadOnlyList<WizardInkPreviewCard> Cards => _cards;

    public void Reset()
    {
        _cards.Clear();
        _topRowCards.Clear();
        _descendantCards.Clear();
        _descendantScrollHost = null;
        _currentSlot = null;
    }

    public void Populate(StackPanel cardHost, WizardPreviewPageSlot slot)
    {
        Reset();
        _currentSlot = slot;
        cardHost.Children.Clear();

        if (slot.LayoutKind == WizardPreviewLayoutKind.SingleTier)
            BuildSingleTier(cardHost, slot.Root);
        else
            BuildTiered(cardHost, slot.Root);

        SyncSelectionChrome();
    }

    private void BuildSingleTier(StackPanel cardHost, WizardPreviewTierNode tier)
    {
        var leaves = WizardPreviewTierTraversal.CollectTierPresets(tier);
        var columns = tier.FlatGridColumns;
        var rows = (leaves.Count + columns - 1) / columns;

        var gridHost = new Grid
        {
            Width = ListHostWidth,
            MinHeight = rows * CardFootprintHeight
                + Math.Max(0, rows - 1) * CardSpacing
                + RowShadowSpillTop,
            HorizontalAlignment = HorizontalAlignment.Center,
        };

        var grid = new Grid
        {
            Margin = SingleTierGridMargin,
            ColumnSpacing = CardSpacing,
            RowSpacing = CardSpacing,
            HorizontalAlignment = HorizontalAlignment.Center,
        };

        for (var c = 0; c < columns; c++)
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        for (var r = 0; r < rows; r++)
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        for (var i = 0; i < leaves.Count; i++)
        {
            var leaf = leaves[i];
            var card = CreateCard(leaf);
            card.CardPressed += (_, _) => _engine.ApplySelection(leaf.Path, toggle: false);
            Grid.SetColumn(card, i % columns);
            Grid.SetRow(card, i / columns);
            grid.Children.Add(card);
            _cards.Add(card);
        }

        gridHost.Children.Add(grid);
        cardHost.Children.Add(gridHost);
    }

    private void BuildTiered(StackPanel cardHost, WizardPreviewTierNode root)
    {
        var topLeaves = WizardPreviewTierTraversal.CollectTierPresets(root);
        var topRowHost = new Grid
        {
            Width = ListHostWidth,
            Height = TopRowHostHeight,
            MinHeight = TopRowHostHeight,
            HorizontalAlignment = HorizontalAlignment.Center,
        };

        var topPanel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = CardSpacing,
            Margin = TopRowContentMargin,
            VerticalAlignment = VerticalAlignment.Top,
        };

        for (var i = 0; i < topLeaves.Count; i++)
        {
            var leaf = topLeaves[i];
            var card = CreateCard(leaf);
            card.CardPressed += (_, _) =>
            {
                _engine.ApplySelection(leaf.Path, toggle: false);
                ScrollBottomRowToTopColumn();
            };
            topPanel.Children.Add(card);
            _topRowCards.Add(card);
            _cards.Add(card);
        }

        topRowHost.Children.Add(topPanel);
        cardHost.Children.Add(topRowHost);

        if (!root.HasNestedTier)
            return;

        var descendantLeaves = WizardPreviewTierTraversal.CollectDescendantPresets(root);
        if (descendantLeaves.Count == 0)
            return;

        var descendantPanel = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = CardSpacing,
            Margin = BottomRowContentMargin,
        };

        foreach (var leaf in descendantLeaves)
        {
            var card = CreateCard(leaf);
            card.PressedCallback = () => _engine.ApplySelection(leaf.Path, toggle: true);
            descendantPanel.Children.Add(card);
            _descendantCards.Add(card);
            _cards.Add(card);
        }

        // HorizontalScrollMode=Disabled: только ChangeView; иначе ScrollViewer перехватывает pointer.
        _descendantScrollHost = new ScrollViewer
        {
            HorizontalScrollBarVisibility = ScrollBarVisibility.Hidden,
            VerticalScrollBarVisibility = ScrollBarVisibility.Disabled,
            HorizontalScrollMode = ScrollMode.Disabled,
            ZoomMode = ZoomMode.Disabled,
            IsScrollInertiaEnabled = false,
            IsHorizontalRailEnabled = false,
            IsVerticalRailEnabled = false,
            HorizontalAlignment = HorizontalAlignment.Center,
            Width = ListHostWidth,
            Height = BottomRowHostHeight,
            MinHeight = BottomRowHostHeight,
            Content = descendantPanel,
        };

        cardHost.Children.Add(_descendantScrollHost);
    }

    private WizardInkPreviewCard CreateCard(WizardPreviewPresetLeaf leaf)
    {
        var card = new WizardInkPreviewCard();
        card.SuppressLoadedRepaint = true;
        card.SuppressPaintInputRepaint = true;
        card.SampleText = _sampleText;
        card.IsDarkTheme = _isDarkTheme;
        card.HostInListView = true;

        card.EnableCardPressed();
        card.RenderMode = leaf.Block.RenderMode;
        card.Profile = leaf.Block.Profile;

        if (leaf.BitmapBuffer is { } buffer)
        {
            var bitmap = WizardPreviewBitmapFactory.CreateBitmapFromBuffer(buffer);
            card.SetPreviewBitmap(bitmap);
        }

        return card;
    }

    public void SyncSelectionChrome()
    {
        if (_currentSlot is null)
            return;

        WizardPreviewSelectionSync.ApplySlotMarkersToCards(
            _currentSlot,
            _topRowCards,
            _descendantCards,
            _cards,
            _engine.IsPairedPage(_engine.StepIndex));
    }

    public void ScrollBottomRowToTopColumn()
    {
        if (_descendantScrollHost is null || _currentSlot is null)
            return;

        var nestedPerRow = WizardPreviewTierTraversal.NestedPresetsPerRow(_currentSlot.Root);
        if (nestedPerRow <= 0)
            return;

        var topMarked = WizardPreviewSelectionMarkers.FindMarkedRow(_currentSlot.Root);
        if (topMarked < 0)
            topMarked = 1;

        _descendantScrollHost.UpdateLayout();
        var firstPreset = topMarked * nestedPerRow;
        var step = CardFootprintWidth + CardSpacing;
        _descendantScrollHost.ChangeView(firstPreset * step, null, null, disableAnimation: false);
    }

    public void ScheduleScrollBottomRowToTopColumn()
    {
        if (_descendantScrollHost is null)
            return;

        void TryScroll() => ScrollBottomRowToTopColumn();

        if (_descendantScrollHost.IsLoaded)
        {
            TryScroll();
            return;
        }

        var scrollHost = _descendantScrollHost;
        void OnLoaded(object sender, RoutedEventArgs e)
        {
            scrollHost.Loaded -= OnLoaded;
            TryScroll();
        }

        scrollHost.Loaded += OnLoaded;
    }
}
