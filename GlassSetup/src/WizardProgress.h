#pragma once

// The engine reports completed units during installation and remaining units
// during rollback. Convert both to a completed amount for drawing and the
// percentage label, so rollback fills from the right edge towards the left.
namespace WizardProgress {
constexpr int kMaxUnits = 10000;

constexpr int ClampUnits(int value) noexcept {
    return value < 0 ? 0 : value > kMaxUnits ? kMaxUnits : value;
}

constexpr int RollbackRemaining(int completed) noexcept {
    return kMaxUnits - ClampUnits(completed);
}

constexpr int CompletedUnits(int value, bool reverse) noexcept {
    const int bounded = ClampUnits(value);
    return reverse ? kMaxUnits - bounded : bounded;
}

constexpr int Percent(int value, bool reverse) noexcept {
    return CompletedUnits(value, reverse) / 100;
}

struct Span {
    int left;
    int right;
};

constexpr Span DeterminateSpan(int left, int right, int value, bool reverse) noexcept {
    if (right < left) right = left;
    const auto width = static_cast<long long>(right) - left;
    const int pixels = static_cast<int>((width * CompletedUnits(value, reverse) + kMaxUnits / 2) / kMaxUnits);
    return reverse ? Span{right - pixels, right} : Span{left, left + pixels};
}

constexpr double MarqueePhase(double phase, bool reverse) noexcept {
    return reverse ? 1.0 - phase : phase;
}

struct Mode {
    bool reverse;
    bool marquee;
};

// MSI may still report forward installation progress while cancellation is
// pending. Show an indeterminate reverse animation until rollback is ready to
// provide its own progress rather than presenting those values as rollback.
constexpr Mode ResolveMode(bool cancelling, bool engineReverse, bool engineMarquee) noexcept {
    return {cancelling || engineReverse, engineMarquee || (cancelling && !engineReverse)};
}
} // namespace WizardProgress
