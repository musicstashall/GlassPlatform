using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace GlassSettings.Controls;

/// <summary>Таблица контролов шапки спойлера: до двух ячеек label+control.</summary>
public sealed partial class SpoilerHeaderControlsTable : UserControl
{
    private readonly SpoilerHeaderControlCell?[] _cells = new SpoilerHeaderControlCell?[2];

    public SpoilerHeaderControlsTable()
    {
        InitializeComponent();
    }

    public int CellCount
    {
        get
        {
            var count = 0;
            if (_cells[0]?.HasContent == true)
                count++;
            if (_cells[1]?.HasContent == true)
                count++;
            return count;
        }
    }

    public bool IsEmpty => CellCount == 0;

    public SpoilerHeaderControlCell? GetCell(int index) =>
        index is >= 0 and < 2 ? _cells[index] : null;

    public void SetCell(int index, string? label, UIElement? control)
    {
        if (index is not (0 or 1))
            return;

        if (control is null)
        {
            ClearCell(index);
            return;
        }

        var cell = _cells[index] ?? new SpoilerHeaderControlCell();
        cell.Label = label ?? string.Empty;
        cell.Control = control;
        _cells[index] = cell;
        AssignCellHost(index, cell);
        UpdateVisibility();
    }

    public void SetCell(int index, SpoilerHeaderControlCell cell)
    {
        if (index is not (0 or 1))
            return;

        _cells[index] = cell;
        AssignCellHost(index, cell);
        UpdateVisibility();
    }
    public void ClearCell(int index)
    {
        if (index is not (0 or 1))
            return;

        _cells[index] = null;
        GetHost(index).Content = null;
        GetHost(index).Visibility = Visibility.Collapsed;
        UpdateVisibility();
    }

    public void ClearAll()
    {
        ClearCell(0);
        ClearCell(1);
    }

    /// <summary>Минимальная inline-ширина таблицы (все adaptive на min).</summary>
    public double MeasureMinWidth()
    {
        SyncVisibleLabelWidths();
        var width = 0d;
        var gap = ReadColumnSpacing();
        var first = true;
        for (var i = 0; i < 2; i++)
        {
            if (_cells[i]?.HasContent != true)
                continue;

            if (!first)
                width += gap;
            width += _cells[i]!.MeasureMinWidth();
            first = false;
        }

        return width;
    }

    /// <summary>Максимальная inline-ширина таблицы (adaptive на max).</summary>
    public double MeasureMaxWidth()
    {
        SyncVisibleLabelWidths();
        var width = 0d;
        var gap = ReadColumnSpacing();
        var first = true;
        for (var i = 0; i < 2; i++)
        {
            if (_cells[i]?.HasContent != true)
                continue;

            if (!first)
                width += gap;
            width += _cells[i]!.MeasureMaxWidth();
            first = false;
        }

        return width;
    }

    /// <summary>Фактическая ширина таблицы после ApplyInlineWidths.</summary>
    public double MeasureAppliedWidth()
    {
        SyncVisibleLabelWidths();
        var width = 0d;
        var gap = ReadColumnSpacing();
        var first = true;
        for (var i = 0; i < 2; i++)
        {
            if (_cells[i]?.HasContent != true)
                continue;

            if (!first)
                width += gap;

            _cells[i]!.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
            width += Math.Ceiling(_cells[i]!.DesiredSize.Width);
            first = false;
        }

        return width;
    }

    /// <summary>Распределение budget между ячейками: fixed first, adaptive slider — остаток.</summary>
    public void ApplyInlineWidths(double available)
    {
        SyncVisibleLabelWidths();
        if (available <= 0 || IsEmpty)
            return;

        var indices = GetVisibleIndices();
        if (indices.Count == 0)
            return;

        var gap = ReadColumnSpacing();
        var fixedTotal = 0d;
        var adaptiveIndices = new List<int>();
        foreach (var i in indices)
        {
            var cell = _cells[i]!;
            if (cell.IsAdaptiveControl)
                adaptiveIndices.Add(i);
            else
                fixedTotal += cell.MeasureMinWidth();
        }

        var gaps = Math.Max(0, indices.Count - 1) * gap;
        var remaining = Math.Max(0, available - fixedTotal - gaps);

        foreach (var i in indices)
        {
            var cell = _cells[i]!;
            if (!cell.IsAdaptiveControl)
                cell.ApplyInlineWidth(cell.MeasureMinWidth());
        }

        if (adaptiveIndices.Count == 0)
            return;

        var perAdaptive = remaining / adaptiveIndices.Count;
        foreach (var i in adaptiveIndices)
            _cells[i]!.ApplyInlineWidth(perAdaptive);
    }

    public void ApplyStackedChrome(double availableWidth)
    {
        HorizontalAlignment = HorizontalAlignment.Right;
        foreach (var cell in _cells)
            cell?.ApplyStackedChrome(availableWidth);
    }

    /// <summary>Выравнивание подписей между ячейками одной таблицы.</summary>
    public void SyncLabelColumnWidths()
    {
        var visible = new List<SpoilerHeaderControlCell>();
        foreach (var cell in _cells)
        {
            if (cell?.HasContent == true)
                visible.Add(cell);
        }

        SpoilerHeaderControlCell.SyncLabelColumnWidths(visible);
    }

    /// <summary>Выравнивание подписей между несколькими таблицами (Theme expanders).</summary>
    public static void SyncLabelColumnWidthsAcross(IEnumerable<SpoilerHeaderControlsTable> tables)
    {
        var cells = new List<SpoilerHeaderControlCell>();
        foreach (var table in tables)
        {
            foreach (var cell in table._cells)
            {
                if (cell?.HasContent == true && !string.IsNullOrEmpty(cell.Label))
                    cells.Add(cell);
            }
        }

        SpoilerHeaderControlCell.SyncLabelColumnWidths(cells);
    }

    /// <summary>Legacy: HubInlineLabel, bare control, labeled slider grid.</summary>
    public static SpoilerHeaderControlCell? FromLegacyContent(object? content)
    {
        if (content is null)
            return null;

        if (content is SpoilerHeaderControlCell cell)
            return cell;

        if (content is HubInlineLabel hub)
        {
            var result = new SpoilerHeaderControlCell();
            ApplyHubInlineLabelToCell(result, hub);
            return result;
        }

        if (content is UIElement element)
        {
            var result = new SpoilerHeaderControlCell();
            result.Control = element;
            return result;
        }

        return null;
    }

    public void SetCellFromLegacy(int index, object? content)
    {
        if (index is not (0 or 1))
            return;

        if (content is null)
        {
            ClearCell(index);
            return;
        }

        if (content is SpoilerHeaderControlCell cell)
        {
            SetCell(index, cell);
            return;
        }

        var target = _cells[index] ?? new SpoilerHeaderControlCell();

        if (content is HubInlineLabel hub)
        {
            ApplyHubInlineLabelToCell(target, hub);
        }
        else if (content is UIElement element)
        {
            target.Label = string.Empty;
            target.Control = element;
        }
        else
        {
            ClearCell(index);
            return;
        }

        SetCell(index, target);
    }

    internal IEnumerable<SpoilerHeaderControlCell> GetVisibleCells()
    {
        foreach (var cell in _cells)
        {
            if (cell?.HasContent == true)
                yield return cell;
        }
    }

    private void SyncVisibleLabelWidths() => SyncLabelColumnWidths();

    private List<int> GetVisibleIndices()
    {
        var list = new List<int>();
        for (var i = 0; i < 2; i++)
        {
            if (_cells[i]?.HasContent == true)
                list.Add(i);
        }

        return list;
    }

    private void AssignCellHost(int index, SpoilerHeaderControlCell cell)
    {
        var host = GetHost(index);
        if (!ReferenceEquals(host.Content, cell))
        {
            SpoilerHeaderLayoutMetrics.DetachFromVisualParent(cell);
            host.Content = cell;
        }

        host.Visibility = Visibility.Visible;
    }

    private ContentPresenter GetHost(int index) => index == 0 ? Cell0Host : Cell1Host;

    private void UpdateVisibility()
    {
        Visibility = IsEmpty ? Visibility.Collapsed : Visibility.Visible;
    }

    private double ReadColumnSpacing() =>
        Application.Current.Resources["GlassSettingsRowControlSpacing"] is double value ? value : 25;

    /// <summary>CheckBox: подпись в Content, ячейка без левой колонки; иначе label|control.</summary>
    private static void ApplyHubInlineLabelToCell(SpoilerHeaderControlCell cell, HubInlineLabel hub)
    {
        if (hub.Control is CheckBox check)
        {
            if (check.Content is null or string { Length: 0 })
                check.Content = hub.Label;

            cell.Label = string.Empty;
            cell.Control = check;
            return;
        }

        cell.Label = hub.Label;
        cell.Control = hub.Control as UIElement;
    }
}
