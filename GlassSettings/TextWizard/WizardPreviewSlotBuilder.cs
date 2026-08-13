using GlassSettings.Core.Text;

namespace GlassSettings.TextWizard;

/// <summary>Сборка ArN[6][3] из конфигурации engine.</summary>
internal static class WizardPreviewSlotBuilder
{
    private const int PairedTopRowBlocks = 3;
    private const int PairedBottomBlockBase = 3;

    public static WizardPreviewPageSlot Build(TextVisualTuningWizardEngine engine, int pageIndex)
    {
        engine.EnsurePageConfigured(pageIndex);
        var snapshot = engine.GetPageSnapshot(pageIndex);
        var root = WizardPreviewTierNode.CreateEmpty();

        if (snapshot.IsPaired)
            BuildTieredMap(root, snapshot);
        else if (snapshot.Blocks.Count == 2)
            BuildFlatMap(root, snapshot, gridColumns: 2);
        else
            BuildFlatMap(root, snapshot, gridColumns: 3);

        var slot = new WizardPreviewPageSlot { Root = root };
        WizardPreviewSelectionSync.ApplyEngineToSlot(slot, engine, pageIndex);
        return slot;
    }

    private static void BuildFlatMap(
        WizardPreviewTierNode root,
        WizardPageSnapshot snapshot,
        int gridColumns)
    {
        for (var row = 0; row < snapshot.Blocks.Count; row++)
        {
            root.Rows[row].Preset = CreateLeaf(
                snapshot.Blocks[row],
                new WizardSelectionPath(row, WizardPreviewTierNode.PresetColumn, 0, WizardSelectionRole.FlatBlock));
        }

        root.FlatGridColumns = gridColumns;
    }

    private static void BuildTieredMap(WizardPreviewTierNode root, WizardPageSnapshot snapshot)
    {
        for (var row = 0; row < PairedTopRowBlocks; row++)
        {
            var mapRow = root.Rows[row];
            mapRow.Preset = CreateLeaf(
                snapshot.Blocks[row],
                new WizardSelectionPath(row, WizardPreviewTierNode.PresetColumn, 0, WizardSelectionRole.TopRow));

            var child = WizardPreviewTierNode.CreateEmpty();
            for (var j = 0; j < PairedTopRowBlocks; j++)
            {
                child.Rows[j].Preset = CreateLeaf(
                    snapshot.Blocks[BottomPresetIndex(row, j)],
                    new WizardSelectionPath(
                        row,
                        WizardPreviewTierNode.NestedColumn,
                        j,
                        WizardSelectionRole.BottomRow));
            }

            mapRow.Nested = child;
            // Метки во вложенном ярусе не ставим — только при явном выборе пользователя.
        }
    }

    private static WizardPreviewPresetLeaf CreateLeaf(WizardPreviewBlock block, WizardSelectionPath path) =>
        new()
        {
            Block = block,
            Path = path,
        };

    private static int BottomPresetIndex(int topColumn, int contourColumn) =>
        PairedBottomBlockBase + topColumn * PairedTopRowBlocks + contourColumn;
}
