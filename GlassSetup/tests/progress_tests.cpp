// Standalone C++17 test; no Windows SDK or installer execution is required.
#include "../src/WizardProgress.h"
#include <cassert>
#include <initializer_list>
#include <iostream>

int main() {
    using namespace WizardProgress;

    // Ordinary installation grows from the left without changing its anchor.
    int lastRight = 13;
    for (int completed = 0; completed <= kMaxUnits; completed += 1000) {
        const Span span = DeterminateSpan(13, 1013, completed, false);
        assert(span.left == 13);
        assert(span.right >= lastRight);
        assert(span.right == 13 + completed / 10);
        assert(Percent(completed, false) == completed / 100);
        lastRight = span.right;
    }

    // Rollback reports remaining units: its completed region starts at the
    // right and grows towards the left, while its percentage increases.
    int lastLeft = 1013;
    for (int completed = 0; completed <= kMaxUnits; completed += 1000) {
        const int remaining = RollbackRemaining(completed);
        const Span span = DeterminateSpan(13, 1013, remaining, true);
        assert(span.right == 1013);
        assert(span.left <= lastLeft);
        assert(span.left == 1013 - completed / 10);
        assert(Percent(remaining, true) == completed / 100);
        lastLeft = span.left;
    }

    // Clamp bad data, handle empty/offset bounds, and round partial pixels.
    assert(CompletedUnits(-1, false) == 0);
    assert(CompletedUnits(10001, false) == kMaxUnits);
    assert(RollbackRemaining(-1) == kMaxUnits);
    assert(RollbackRemaining(10001) == 0);
    assert(DeterminateSpan(20, 20, 5000, true).left == 20);
    assert(DeterminateSpan(20, 10, 5000, false).right == 20);
    const Span fractional = DeterminateSpan(10, 113, 2500, false);
    assert(fractional.left == 10 && fractional.right == 36);
    for (int width : {0, 1, 2, 7, 103, 999, 1000, 1500}) {
        int previousRight = 19, previousLeft = 19 + width;
        for (int completed = 0; completed <= kMaxUnits; completed += 100) {
            const Span forward = DeterminateSpan(19, 19 + width, completed, false);
            const Span backward = DeterminateSpan(19, 19 + width, RollbackRemaining(completed), true);
            assert(forward.right >= previousRight && backward.left <= previousLeft);
            assert(forward.right - forward.left == backward.right - backward.left);
            assert(Percent(completed, false) == Percent(RollbackRemaining(completed), true));
            previousRight = forward.right;
            previousLeft = backward.left;
        }
    }

    // The marquee also travels right-to-left throughout cancellation, even
    // before the MSI call returns and the engine begins its own rollback.
    const Mode install = ResolveMode(false, false, false);
    assert(!install.reverse && !install.marquee);
    const Mode installMarquee = ResolveMode(false, false, true);
    assert(!installMarquee.reverse && installMarquee.marquee);
    const Mode pending = ResolveMode(true, false, false);
    assert(pending.reverse && pending.marquee);
    const Mode rollbackWait = ResolveMode(true, true, true);
    assert(rollbackWait.reverse && rollbackWait.marquee);
    const Mode rollbackKnown = ResolveMode(true, true, false);
    assert(rollbackKnown.reverse && !rollbackKnown.marquee);
    const Mode otherRollback = ResolveMode(false, true, false);
    assert(otherRollback.reverse && !otherRollback.marquee);
    assert(MarqueePhase(0.0, true) == 1.0);
    assert(MarqueePhase(0.25, true) > MarqueePhase(0.75, true));
    assert(MarqueePhase(0.25, false) < MarqueePhase(0.75, false));

    std::cout << "PASS: forward and rollback progress direction, percentages, and cancellation modes\n";
}
