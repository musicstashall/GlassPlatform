using GlassSettings.Controls.Processes;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Pages;

internal enum ProcessesHeaderLayoutMode
{
    Wide,
    Medium,
    Narrow
}

internal sealed class ProcessesPageHeaderLayout
{
    private const double FilterSortSpacing = 18;

    private readonly Grid _headerGrid;
    private readonly Grid _searchRowGrid;
    private readonly AutoSuggestBox _searchBox;
    private readonly ProcessViewSwitcher _viewSwitcher;
    private readonly TextBlock _foundCountText;
    private readonly StackPanel _filterGroup;
    private readonly StackPanel _sortGroup;

    public ProcessesPageHeaderLayout(
        Grid headerGrid,
        Grid searchRowGrid,
        AutoSuggestBox searchBox,
        ProcessViewSwitcher viewSwitcher,
        TextBlock foundCountText,
        StackPanel filterGroup,
        StackPanel sortGroup)
    {
        _headerGrid = headerGrid;
        _searchRowGrid = searchRowGrid;
        _searchBox = searchBox;
        _viewSwitcher = viewSwitcher;
        _foundCountText = foundCountText;
        _filterGroup = filterGroup;
        _sortGroup = sortGroup;
    }

    public ProcessesHeaderLayoutMode Mode { get; private set; }

    public void Update(double contentWidth)
    {
        if (contentWidth <= 0)
            return;

        _searchBox.Width = ProcessesHeaderLayoutMetrics.ResolveSearchWidth(contentWidth);
        _searchBox.MinHeight = ProcessesHeaderLayoutMetrics.SearchHeight;
        _searchBox.HorizontalAlignment = HorizontalAlignment.Left;

        _viewSwitcher.Width = ProcessesHeaderLayoutMetrics.ViewSwitcherWidth;
        _viewSwitcher.Height = ProcessesHeaderLayoutMetrics.ViewSwitcherHeight;
        _viewSwitcher.HorizontalAlignment = HorizontalAlignment.Right;
        _viewSwitcher.VerticalAlignment = VerticalAlignment.Center;

        _filterGroup.Visibility = Visibility.Visible;
        _sortGroup.Visibility = Visibility.Visible;
        _foundCountText.Visibility = Visibility.Visible;

        if (contentWidth >= ProcessesHeaderLayoutMetrics.ContentBreakpointThreeRows)
        {
            Mode = ProcessesHeaderLayoutMode.Wide;
            ApplyWideLayout();
            return;
        }

        if (contentWidth >= ProcessesHeaderLayoutMetrics.ContentBreakpointFourRows)
        {
            Mode = ProcessesHeaderLayoutMode.Medium;
            ApplyMediumLayout();
            return;
        }

        Mode = ProcessesHeaderLayoutMode.Narrow;
        ApplyNarrowLayout();
    }

    private void ApplyWideLayout()
    {
        SetToolbarColumns();
        SetHeaderRows(3);
        PlaceSearchRow(0, 4);

        Grid.SetRow(_filterGroup, 1);
        Grid.SetColumn(_filterGroup, 1);
        Grid.SetRow(_sortGroup, 1);
        Grid.SetColumn(_sortGroup, 2);
        _filterGroup.HorizontalAlignment = HorizontalAlignment.Right;
        _sortGroup.HorizontalAlignment = HorizontalAlignment.Right;
        _sortGroup.Margin = new Thickness(FilterSortSpacing, 0, 0, 0);

        PlaceFoundCountRow(2, 4);
    }

    private void ApplyMediumLayout()
    {
        SetLeftToolbarColumns();
        SetHeaderRows(3);
        PlaceSearchRow(0, 3);

        Grid.SetRow(_filterGroup, 1);
        Grid.SetColumn(_filterGroup, 0);
        Grid.SetRow(_sortGroup, 1);
        Grid.SetColumn(_sortGroup, 1);
        _filterGroup.HorizontalAlignment = HorizontalAlignment.Left;
        _sortGroup.HorizontalAlignment = HorizontalAlignment.Left;
        _sortGroup.Margin = new Thickness(FilterSortSpacing, 0, 0, 0);

        PlaceFoundCountRow(2, 3);
    }

    private void ApplyNarrowLayout()
    {
        SetSingleColumn();
        SetHeaderRows(4);
        PlaceSearchRow(0, 1);

        Grid.SetRow(_filterGroup, 1);
        Grid.SetColumn(_filterGroup, 0);
        _filterGroup.HorizontalAlignment = HorizontalAlignment.Left;

        Grid.SetRow(_sortGroup, 2);
        Grid.SetColumn(_sortGroup, 0);
        _sortGroup.HorizontalAlignment = HorizontalAlignment.Left;
        _sortGroup.Margin = new Thickness(0);

        PlaceFoundCountRow(3, 1);
    }

    private void PlaceFoundCountRow(int row, int columnSpan)
    {
        Grid.SetRow(_foundCountText, row);
        Grid.SetColumn(_foundCountText, 0);
        Grid.SetColumnSpan(_foundCountText, columnSpan);
        _foundCountText.HorizontalAlignment = HorizontalAlignment.Left;
    }

    private void PlaceSearchRow(int row, int columnSpan)
    {
        Grid.SetRow(_searchRowGrid, row);
        Grid.SetColumn(_searchRowGrid, 0);
        Grid.SetColumnSpan(_searchRowGrid, columnSpan);
    }

    private void SetLeftToolbarColumns()
    {
        _headerGrid.ColumnDefinitions.Clear();
        _headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
    }

    private void SetToolbarColumns()
    {
        _headerGrid.ColumnDefinitions.Clear();
        _headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        _headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
    }

    private void SetSingleColumn()
    {
        _headerGrid.ColumnDefinitions.Clear();
        _headerGrid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
    }

    private void SetHeaderRows(int count)
    {
        _headerGrid.RowDefinitions.Clear();
        for (var i = 0; i < count; i++)
            _headerGrid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        _headerGrid.RowSpacing = ProcessesHeaderLayoutMetrics.HeaderRowSpacing;
    }
}
