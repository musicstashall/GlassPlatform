// Standalone C++17 test; no Windows SDK or installer execution is required.
#include "../src/WizardProgress.h"
#include <cassert>
#include <initializer_list>
#include <iostream>

int main() {
    using namespace WizardProgress;

    // Installation grows from the left; the label describes the same width.
    int lastRight = 13;
    for (int visible = 0; visible <= kMaxUnits; visible += 100) {
        const Span span = DeterminateSpan(13, 1013, visible);
        assert(span.left == 13 && span.right >= lastRight);
        assert(span.right == 13 + visible / 10);
        assert(Percent(visible) == visible / 100);
        lastRight = span.right;
    }

    // Cancel at various *visible* values, including a smoothed value that is
    // below the engine's target. Neither confirmation nor an unknown phase may
    // replace it with 100%, a right-anchored bar, or a moving segment.
    for (int start : {0, 1, 999, 2500, 4317, 8350, 10000}) {
        RollbackTracker tracker;
        tracker.Begin(start);
        assert(tracker.Start() == start && tracker.Target() == start);
        tracker.ReportRemaining(0, false); // MSI is still acknowledging cancel.
        assert(tracker.Target() == start);
        int previous = start;
        for (int remaining : {10000, 9000, 5000, 7500, 1000, 0}) {
            tracker.ReportRemaining(remaining, true);
            assert(tracker.Target() <= previous);
            assert(tracker.Target() == 0 || tracker.Target() <= start);
            assert(start == 0 || tracker.Target() > 0); // later work still remains
            const Span bar = DeterminateSpan(19, 1019, tracker.Target());
            assert(bar.left == 19 && bar.right <= DeterminateSpan(19, 1019, previous).right);
            assert(Percent(tracker.Target()) == tracker.Target() / 100);
            previous = tracker.Target();
        }
        tracker.ReportRemaining(10000, false); // unknown MSI/registry/host work
        assert(tracker.Target() == previous);
        tracker.Finish(); // only after success
        assert(tracker.Target() == 0);
        assert(DeterminateSpan(19, 1019, tracker.Target()).right == 19);
    }

    // A timer can advance between paints, and more values can arrive while the
    // confirmation is open. Keep the last painted value from the cancel click.
    VisibleProgress visible;
    assert(visible.PaintedOr(1200) == 1200); // before the first paint
    visible.RecordPaint(3175);
    assert(visible.PaintedOr(3300) == 3175);
    visible.Capture(visible.PaintedOr(3300));
    visible.RecordPaint(6500);
    assert(visible.RollbackStart(8000) == 3175);
    visible.Resume();
    assert(visible.RollbackStart(8000) == 6500);
    visible.Reset();
    assert(visible.PaintedOr(20000) == 0); // cancel before the first paint
    visible.RecordPaint(20000);
    assert(visible.PaintedOr(8000) == kMaxUnits);
    visible.RecordPaint(-1);
    assert(visible.PaintedOr(8000) == 0);
    visible.Capture(3175);

    // Many start widths and small increments must remain monotonic even when
    // rounding to units and then to pixels occurs at different boundaries.
    for (int start = 0; start <= kMaxUnits; start += 73) {
        RollbackTracker tracker;
        tracker.Begin(start);
        int previous = start;
        for (int completed = 0; completed <= kMaxUnits; completed += 125) {
            tracker.ReportRemaining(RollbackRemaining(completed), true);
            assert(tracker.Target() <= previous);
            assert(start == 0 || tracker.Target() > 0);
            assert(DeterminateSpan(7, 1007, tracker.Target()).right <=
                   DeterminateSpan(7, 1007, previous).right);
            previous = tracker.Target();
        }
    }

    RollbackTracker smoothed;
    smoothed.Begin(visible.RollbackStart(8000));
    smoothed.ReportRemaining(10000, true);
    assert(smoothed.Target() == 3175);
    smoothed.ReportRemaining(5000, true);
    assert(smoothed.Target() == 1588); // half of the visible, not the engine target
    smoothed.ReportRemaining(8000, true);
    assert(smoothed.Target() == 1588); // stale/out-of-order callback

    // Exercise the same smoothing used by the UI, including a zero-time tick
    // immediately after cancellation and a delayed frame with alpha > 1.
    for (int start : {0, 1, 1234, 5000, 9999, 10000}) {
        for (int target : {0, 1, 4321, 9999, 10000}) {
            for (double alpha : {0.0, 0.001, 0.1, 0.5, 1.0, 2.0}) {
                const int next = SmoothTowards(start, target, alpha);
                assert(next >= (start < target ? start : target));
                assert(next <= (start > target ? start : target));
                if (alpha == 0.0) assert(next == start);
                if (alpha >= 1.0) assert(next == target);
            }
        }
    }
    int displayed = 3175;
    for (int i = 0; i < 1000 && displayed != smoothed.Target(); ++i) {
        const int next = SmoothTowards(displayed, smoothed.Target(), 0.1);
        assert(next <= displayed && next >= smoothed.Target());
        displayed = next;
    }
    assert(displayed == smoothed.Target());

    // Short or offset rectangles, partial pixels, and misbehaving inputs.
    assert(ClampUnits(-1) == 0 && ClampUnits(10001) == kMaxUnits);
    assert(RollbackRemaining(-1) == kMaxUnits && RollbackRemaining(10001) == 0);
    assert(DeterminateSpan(20, 20, 5000).right == 20);
    assert(DeterminateSpan(20, 10, 5000).right == 20);
    const Span fractional = DeterminateSpan(10, 113, 2500);
    assert(fractional.left == 10 && fractional.right == 36);
    for (int width : {0, 1, 2, 7, 103, 999, 1000, 1500}) {
        int previousRight = 19 + width;
        for (int remaining = kMaxUnits; remaining >= 0; remaining -= 100) {
            const Span bar = DeterminateSpan(19, 19 + width, remaining);
            assert(bar.left == 19 && bar.right <= previousRight);
            previousRight = bar.right;
        }
    }

    // Once success is known, animate the last visible value down to zero.
    for (int start : {0, 1, 3175, 10000}) {
        int previous = start;
        for (unsigned long long ms = 0; ms <= 1000; ms += 10) {
            const int visible = FadeToZero(start, ms, 1000);
            assert(visible <= previous && visible >= 0);
            if (!ms) assert(visible == start);
            if (ms == 1000) assert(visible == 0);
            previous = visible;
        }
    }
    assert(FadeToZero(5000, 0, 0) == 0);
    std::cout << "PASS: left-anchored installation and rollback, capture, unknown phases, and completion\n";
}
