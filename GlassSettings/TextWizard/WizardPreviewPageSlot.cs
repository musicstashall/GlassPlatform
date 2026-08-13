using GlassSettings.Core.Text;

namespace GlassSettings.TextWizard;

public sealed class WizardPreviewPresetLeaf
{
    public required WizardPreviewBlock Block { get; init; }

    public byte[]? BitmapBuffer { get; set; }

    public required WizardSelectionPath Path { get; init; }
}

/// <summary>Строка ArN[row]: col0=preset, col1=nested, col2=marker.</summary>
public sealed class WizardPreviewMapRow
{
    /// <summary>Col 0 — лист пресета.</summary>
    public WizardPreviewPresetLeaf? Preset { get; set; }

    /// <summary>Col 1 — дочерний ярус (рекурсивная ArN[6][3]).</summary>
    public WizardPreviewTierNode? Nested { get; set; }

    /// <summary>Col 2 — метка выбора (1 = выбранная строка яруса).</summary>
    public int Marker { get; set; }
}

/// <summary>Узел яруса: до 6 строк ArN[row][3].</summary>
public sealed class WizardPreviewTierNode
{
    public const int MaxRows = 6;
    public const int PresetColumn = 0;
    public const int NestedColumn = 1;
    public const int MarkerColumn = 2;
    public const int ColumnCount = 3;

    public WizardPreviewMapRow[] Rows { get; } = CreateEmptyRows();

    /// <summary>Для SingleTier: число колонок плоской сетки.</summary>
    public int FlatGridColumns { get; set; } = 3;

    public bool HasNestedTier
    {
        get
        {
            for (var row = 0; row < MaxRows; row++)
            {
                if (Rows[row].Nested is not null)
                    return true;
            }

            return false;
        }
    }

    public int CountActiveRows()
    {
        var count = 0;
        for (var row = 0; row < MaxRows; row++)
        {
            if (Rows[row].Preset is not null)
                count++;
            else
                break;
        }

        return count;
    }

    public static WizardPreviewMapRow[] CreateEmptyRows()
    {
        var rows = new WizardPreviewMapRow[MaxRows];
        for (var i = 0; i < MaxRows; i++)
            rows[i] = new WizardPreviewMapRow();
        return rows;
    }

    public static WizardPreviewTierNode CreateEmpty() => new();
}

/// <summary>Режим компоновки UI, определяется структурой ArN.</summary>
public enum WizardPreviewLayoutKind
{
    SingleTier,
    Tiered,
}

/// <summary>Слот страницы: корневой ярус ArN[6][3].</summary>
public sealed class WizardPreviewPageSlot
{
    public const int MaxRows = WizardPreviewTierNode.MaxRows;
    public const int ColumnCount = WizardPreviewTierNode.ColumnCount;

    public required WizardPreviewTierNode Root { get; init; }

    public WizardPreviewLayoutKind LayoutKind =>
        Root.HasNestedTier ? WizardPreviewLayoutKind.Tiered : WizardPreviewLayoutKind.SingleTier;

    public int SingleTierColumns => Root.FlatGridColumns;

    public bool IsComplete
    {
        get
        {
            var any = false;
            foreach (var leaf in WizardPreviewTierTraversal.AllPresetLeaves(Root))
            {
                any = true;
                if (leaf.BitmapBuffer is null)
                    return false;
            }

            return any;
        }
    }
}

/// <summary>Обход рекурсивной ArN[6][3].</summary>
public static class WizardPreviewTierTraversal
{
    public static IEnumerable<WizardPreviewPresetLeaf> AllPresetLeaves(WizardPreviewTierNode tier)
    {
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            var mapRow = tier.Rows[row];
            if (mapRow.Preset is { } preset)
                yield return preset;

            if (mapRow.Nested is { } nested)
            {
                foreach (var leaf in AllPresetLeaves(nested))
                    yield return leaf;
            }

            if (mapRow.Preset is null && mapRow.Nested is null)
                break;
        }
    }

    public static IReadOnlyList<WizardPreviewPresetLeaf> CollectTierPresets(WizardPreviewTierNode tier)
    {
        var leaves = new List<WizardPreviewPresetLeaf>();
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            if (tier.Rows[row].Preset is { } preset)
                leaves.Add(preset);
            else
                break;
        }

        return leaves;
    }

    public static IReadOnlyList<WizardPreviewPresetLeaf> CollectDescendantPresets(WizardPreviewTierNode tier)
    {
        var leaves = new List<WizardPreviewPresetLeaf>();
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            var mapRow = tier.Rows[row];
            if (mapRow.Nested is { } nested)
                leaves.AddRange(CollectTierPresets(nested));
        }

        return leaves;
    }

    public static int NestedPresetsPerRow(WizardPreviewTierNode tier)
    {
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            if (tier.Rows[row].Nested is { } nested)
                return nested.CountActiveRows();
        }

        return 0;
    }
}

/// <summary>Метки выбора в col2 ArN[row][3].</summary>
public static class WizardPreviewSelectionMarkers
{
    public static int FindMarkedRow(WizardPreviewTierNode tier)
    {
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            if (tier.Rows[row].Marker == 1)
                return row;
        }

        return -1;
    }

    public static void SetMarkedRow(WizardPreviewTierNode tier, int row, bool exclusive = true)
    {
        if (exclusive)
            ClearMarkers(tier);

        if (row >= 0 && row < WizardPreviewTierNode.MaxRows)
            tier.Rows[row].Marker = 1;
    }

    public static void ClearMarkers(WizardPreviewTierNode tier)
    {
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
            tier.Rows[row].Marker = 0;
    }

    public static void ClearNestedMarkers(WizardPreviewTierNode tier)
    {
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            if (tier.Rows[row].Nested is { } nested)
                ClearMarkers(nested);
        }
    }
}

/// <summary>Снимок col2 (меток) рекурсивного яруса ArN — для rebuild кэша без потери выбора.</summary>
public sealed class WizardPreviewMarkerTierSnapshot
{
    private readonly int[] _rowMarkers = new int[WizardPreviewTierNode.MaxRows];
    private readonly WizardPreviewMarkerTierSnapshot?[] _nested = new WizardPreviewMarkerTierSnapshot?[WizardPreviewTierNode.MaxRows];

    public static WizardPreviewMarkerTierSnapshot Capture(WizardPreviewTierNode tier)
    {
        var snapshot = new WizardPreviewMarkerTierSnapshot();
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            var mapRow = tier.Rows[row];
            snapshot._rowMarkers[row] = mapRow.Marker;
            if (mapRow.Nested is { } nested)
                snapshot._nested[row] = Capture(nested);

            if (mapRow.Preset is null && mapRow.Nested is null)
                break;
        }

        return snapshot;
    }

    public void ApplyTo(WizardPreviewTierNode tier)
    {
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            var mapRow = tier.Rows[row];
            mapRow.Marker = _rowMarkers[row];
            if (mapRow.Nested is { } nested && _nested[row] is { } nestedSnapshot)
                nestedSnapshot.ApplyTo(nested);

            if (mapRow.Preset is null && mapRow.Nested is null)
                break;
        }
    }
}
