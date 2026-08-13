using GlassSettings.Core.Text;

namespace GlassSettings.TextWizard;

/// <summary>Корневой массив Ar[pageIndex] = ArN[6][3].</summary>
public sealed class WizardPreviewCacheStore
{
    private readonly List<WizardPreviewPageSlot?> _pages = [];

    public WizardPreviewPageSlot? GetPage(int pageIndex)
    {
        EnsureCapacity(pageIndex);
        return _pages[pageIndex];
    }

    public void SetPage(int pageIndex, WizardPreviewPageSlot slot)
    {
        EnsureCapacity(pageIndex);
        _pages[pageIndex] = slot;
    }

    public bool TryGetCompletePage(int pageIndex, out WizardPreviewPageSlot slot)
    {
        slot = null!;
        if (pageIndex < 0 || pageIndex >= _pages.Count)
            return false;

        var page = _pages[pageIndex];
        if (page is null || !page.IsComplete)
            return false;

        slot = page;
        return true;
    }

    public void InvalidateFrom(int pageIndex)
    {
        if (pageIndex < 0)
            return;

        while (_pages.Count <= pageIndex)
            _pages.Add(null);

        for (var i = pageIndex; i < _pages.Count; i++)
            _pages[i] = null;
    }

    public IReadOnlyList<WizardPreviewMarkerTierSnapshot?> CaptureAllPageMarkers()
    {
        var snapshots = new WizardPreviewMarkerTierSnapshot?[_pages.Count];
        for (var i = 0; i < _pages.Count; i++)
        {
            if (_pages[i] is { } page)
                snapshots[i] = WizardPreviewMarkerTierSnapshot.Capture(page.Root);
        }

        return snapshots;
    }

    public void ClearAll() => InvalidateFrom(0);

    public static Task<WizardPreviewPageSlot> RenderAllCellsAsync(
        WizardPreviewPageSlot slot,
        string sampleText,
        bool isDarkTheme)
    {
        return WizardPreviewRenderScheduler.RunOnStaThread(() =>
        {
            RenderAllCells(slot, sampleText, isDarkTheme);
            return slot;
        });
    }

    public static void RenderAllCells(
        WizardPreviewPageSlot slot,
        string sampleText,
        bool isDarkTheme)
    {
        RenderTier(slot.Root, sampleText, isDarkTheme);
    }

    private static void RenderTier(
        WizardPreviewTierNode tier,
        string sampleText,
        bool isDarkTheme)
    {
        for (var row = 0; row < WizardPreviewTierNode.MaxRows; row++)
        {
            var mapRow = tier.Rows[row];
            if (mapRow.Preset is { } leaf)
            {
                leaf.BitmapBuffer = WizardPreviewBitmapFactory.TryRenderBlockBuffer(
                    leaf.Block,
                    sampleText,
                    isDarkTheme);
            }

            if (mapRow.Nested is { } nested)
                RenderTier(nested, sampleText, isDarkTheme);

            if (mapRow.Preset is null && mapRow.Nested is null)
                break;
        }
    }

    private void EnsureCapacity(int pageIndex)
    {
        while (_pages.Count <= pageIndex)
            _pages.Add(null);
    }
}
