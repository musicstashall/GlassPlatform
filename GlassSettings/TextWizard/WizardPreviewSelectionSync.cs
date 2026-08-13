using GlassSettings.Controls;
using GlassSettings.Core.Text;

namespace GlassSettings.TextWizard;

/// <summary>Синхронизация меток ArN[col2] с состоянием engine и UI.</summary>
internal static class WizardPreviewSelectionSync
{
    public static void ApplyEngineToSlot(WizardPreviewPageSlot slot, TextVisualTuningWizardEngine engine, int pageIndex)
    {
        if (engine.IsPairedPage(pageIndex))
            ApplyPairedEngineToSlot(slot.Root, engine);
        else
            WizardPreviewSelectionMarkers.SetMarkedRow(slot.Root, engine.SelectedBlockIndex);
    }

    public static void RestoreEngineFromSlot(TextVisualTuningWizardEngine engine, WizardPreviewPageSlot slot, int pageIndex)
    {
        if (engine.IsPairedPage(pageIndex))
            RestorePairedEngineFromSlot(slot.Root, engine);
        else
            engine.RestoreFlatSelection(WizardPreviewSelectionMarkers.FindMarkedRow(slot.Root));
    }

    public static void ApplySlotMarkersToCards(
        WizardPreviewPageSlot slot,
        IReadOnlyList<WizardInkPreviewCard> topRowCards,
        IReadOnlyList<WizardInkPreviewCard> descendantCards,
        IReadOnlyList<WizardInkPreviewCard> flatCards,
        bool isPairedPage)
    {
        if (isPairedPage)
            ApplyPairedSlotToCards(slot.Root, topRowCards, descendantCards);
        else
            ApplyFlatSlotToCards(slot.Root, flatCards);
    }

    private static void ApplyPairedEngineToSlot(WizardPreviewTierNode root, TextVisualTuningWizardEngine engine)
    {
        WizardPreviewSelectionMarkers.SetMarkedRow(root, engine.TopColumn);
        WizardPreviewSelectionMarkers.ClearNestedMarkers(root);

        if (!engine.BottomRowActive)
            return;

        var child = root.Rows[engine.TopColumn].Nested;
        if (child is null)
            return;

        WizardPreviewSelectionMarkers.SetMarkedRow(child, engine.BottomColumn);
    }

    private static void RestorePairedEngineFromSlot(WizardPreviewTierNode root, TextVisualTuningWizardEngine engine)
    {
        var topRow = WizardPreviewSelectionMarkers.FindMarkedRow(root);
        if (topRow < 0)
            topRow = 1;

        var child = root.Rows[topRow].Nested;
        var bottomRow = child is null ? -1 : WizardPreviewSelectionMarkers.FindMarkedRow(child);
        engine.RestorePairedSelection(topRow, bottomRow);
    }

    private static void ApplyPairedSlotToCards(
        WizardPreviewTierNode root,
        IReadOnlyList<WizardInkPreviewCard> topRowCards,
        IReadOnlyList<WizardInkPreviewCard> descendantCards)
    {
        var topMarked = WizardPreviewSelectionMarkers.FindMarkedRow(root);
        for (var i = 0; i < topRowCards.Count; i++)
            topRowCards[i].IsCardSelected = i == topMarked;

        var nestedPerRow = WizardPreviewTierTraversal.NestedPresetsPerRow(root);
        if (nestedPerRow <= 0)
            return;

        var childMarked = topMarked >= 0 && root.Rows[topMarked].Nested is { } child
            ? WizardPreviewSelectionMarkers.FindMarkedRow(child)
            : -1;

        for (var i = 0; i < descendantCards.Count; i++)
        {
            var parentRow = i / nestedPerRow;
            var childCol = i % nestedPerRow;
            descendantCards[i].IsCardSelected = parentRow == topMarked && childCol == childMarked;
        }
    }

    private static void ApplyFlatSlotToCards(WizardPreviewTierNode root, IReadOnlyList<WizardInkPreviewCard> flatCards)
    {
        var marked = WizardPreviewSelectionMarkers.FindMarkedRow(root);
        for (var i = 0; i < flatCards.Count; i++)
            flatCards[i].IsCardSelected = i == marked;
    }
}
