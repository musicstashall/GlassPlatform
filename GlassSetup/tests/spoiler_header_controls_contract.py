"""Portable header policy/wiring checks, not a WinUI layout execution."""
from pathlib import Path
import re
root = Path(__file__).resolve().parents[2]
read = lambda p: (root / p).read_text()
table = read('GlassSettings/Controls/SpoilerHeaderControlsTable.xaml.cs')
engine = read('GlassSettings/Controls/SpoilerHeaderLayoutEngine.cs')
host = read('GlassSettings/Controls/SpoilerHeaderLayoutHost.cs')
page = read('GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs')
metrics = read('GlassSettings/Controls/GlassSettingsSliderMetrics.cs')
assert 'StackFixedActionBelowSlider = true' in page
assert 'ApplyInlineWidths(availableWidth)' in table
assert 'availableWidth < minimum + (_cellsStacked ? 1 : 0)' in table
assert 'Grid.SetRow(sliderHost, 0)' in table and 'Grid.SetRow(actionHost, 1)' in table
assert 'Grid.SetColumnSpan(sliderHost, 2)' in table
assert 'ResetCellRows();' in table and 'CellsGrid.RowDefinitions.Clear()' in table
assert 'context.BelowHost.ActualWidth' in engine
assert '_belowHost.SizeChanged += BelowHost_OnSizeChanged' in host
assert '_belowHost.SizeChanged -= BelowHost_OnSizeChanged' in host
minimum = float(re.search(r'DefaultTrackMinWidth = (\d+)', metrics)[1])
maximum = float(re.search(r'DefaultTrackMaxWidth = (\d+)', metrics)[1])
# Reference budget sweep for the two-cell policy. Real text/button metrics are
# measured by WinUI; vary their stand-ins to cover localization and suffix sizes.
for fixed in (72, 108, 156):
    for label_suffix in (120, 160, 200):
        for gap in (8, 25):
            threshold = fixed + gap + label_suffix + minimum
            for widths in (range(900, int(label_suffix + minimum), -1),
                           range(int(label_suffix + minimum), 901)):
                stacked = False
                for available in widths:
                    stacked = available < threshold + (1 if stacked else 0)
                    budget = available - label_suffix - (0 if stacked else fixed + gap)
                    track = max(minimum, min(maximum, budget))
                    used = label_suffix + track + (0 if stacked else fixed + gap)
                    assert minimum <= track <= maximum
                    assert used <= available
print('PASS: shared stacked budget, slider-first/action-below split, reversible measured thresholds and policy track bounds')
print('NOTE: reference budget sweep and source checks only; not a Windows visual test')
