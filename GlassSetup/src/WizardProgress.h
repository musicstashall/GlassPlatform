#pragma once
#include <cmath>

// The visible value is always the length of a bar anchored at the left edge.
// Installation grows it; cancellation retracts the *currently displayed* bar.
namespace WizardProgress {
constexpr int kMaxUnits = 10000;
// A snapshot copy is followed by MSI, registry and host restoration. Its last
// callback must not visually claim that the entire rollback has finished.
constexpr int kRemainingWhileWorking = 1000;

constexpr int ClampUnits(int value) noexcept {
    return value < 0 ? 0 : value > kMaxUnits ? kMaxUnits : value;
}

constexpr int RollbackRemaining(int completed) noexcept {
    return kMaxUnits - ClampUnits(completed);
}

constexpr int Percent(int visible) noexcept {
    return ClampUnits(visible) / 100;
}

struct Span {
    int left;
    int right;
};

constexpr Span DeterminateSpan(int left, int right, int visible) noexcept {
    if (right < left) right = left;
    const auto width = static_cast<long long>(right) - left;
    const int pixels = static_cast<int>((width * ClampUnits(visible) + kMaxUnits / 2) / kMaxUnits);
    return {left, left + pixels};
}

constexpr int ScaleRemaining(int start, int remaining) noexcept {
    const auto product = static_cast<long long>(ClampUnits(start)) * ClampUnits(remaining);
    // Round up: a nonempty bar stays nonempty until completion is confirmed.
    return static_cast<int>((product + kMaxUnits - 1) / kMaxUnits);
}

constexpr int FadeToZero(int start, unsigned long long elapsed,
                         unsigned long long duration) noexcept {
    if (!duration || elapsed >= duration) return 0;
    return static_cast<int>(static_cast<unsigned long long>(ClampUnits(start)) *
                            (duration - elapsed) / duration);
}

inline int SmoothTowards(int current, int target, double alpha) noexcept {
    current = ClampUnits(current);
    target = ClampUnits(target);
    if (alpha <= 0.0 || current == target) return current;
    if (alpha > 1.0) alpha = 1.0;
    int step = static_cast<int>(std::lround((target - current) * alpha));
    if (!step) step = target > current ? 1 : -1;
    return ClampUnits(current + step);
}

// Timer values can be newer than the last strip actually painted to the user.
// Preserve the painted value across the (possibly long) confirmation page.
class VisibleProgress {
public:
    constexpr void Reset() noexcept {
        painted_ = captured_ = 0;
        // A new installation starts with an empty strip, even if cancellation
        // arrives before the first owner-draw message has been processed.
        havePainted_ = true;
        haveCapture_ = false;
    }
    constexpr void RecordPaint(int value) noexcept {
        painted_ = ClampUnits(value);
        havePainted_ = true;
    }
    [[nodiscard]] constexpr int PaintedOr(int current) const noexcept {
        return havePainted_ ? painted_ : ClampUnits(current);
    }
    constexpr void Capture(int visibleAtRequest) noexcept {
        captured_ = ClampUnits(visibleAtRequest);
        haveCapture_ = true;
    }
    constexpr void Resume() noexcept { haveCapture_ = false; }
    [[nodiscard]] constexpr int RollbackStart(int current) const noexcept {
        return haveCapture_ ? captured_ : PaintedOr(current);
    }
private:
    int painted_ = 0, captured_ = 0;
    bool havePainted_ = false, haveCapture_ = false;
};

// The engine's rollback callbacks describe only measured work. Unknown phases
// hold the previous target, and out-of-order callbacks can never grow the bar.
class RollbackTracker {
public:
    constexpr void Begin(int visible) noexcept {
        start_ = target_ = ClampUnits(visible);
    }
    constexpr void ReportRemaining(int remaining, bool known) noexcept {
        if (!known) return;
        const int bounded = ClampUnits(remaining);
        const int reserve = bounded < kRemainingWhileWorking ? kRemainingWhileWorking : bounded;
        const int candidate = ScaleRemaining(start_, reserve);
        if (candidate < target_) target_ = candidate;
    }
    constexpr void Finish() noexcept { target_ = 0; }
    [[nodiscard]] constexpr int Start() const noexcept { return start_; }
    [[nodiscard]] constexpr int Target() const noexcept { return target_; }
private:
    int start_ = 0;
    int target_ = 0;
};
} // namespace WizardProgress
