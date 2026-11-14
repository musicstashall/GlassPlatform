#pragma once
#include <atomic>

// The active unlock operation is separate from the prompt waiting for the
// user's choice. The UI must leave the unlock page when the operation ends,
// even when no further status-text callback is delivered.
namespace WizardUnlock {
enum class Phase { None, Modules, Processes };

inline constexpr const wchar_t* InstallingPage(Phase phase) noexcept {
    switch (phase) {
    case Phase::Modules: return L"unlock-modules";
    case Phase::Processes: return L"unlock-processes";
    default: return L"installing";
    }
}

class Activity {
public:
    void Begin(Phase phase) noexcept { phase_.store(phase,std::memory_order_release); }
    void End() noexcept { phase_.store(Phase::None,std::memory_order_release); }
    Phase Current() const noexcept { return phase_.load(std::memory_order_acquire); }

private:
    std::atomic<Phase> phase_{Phase::None};
};

// Covers every success, failure and cancellation return from the worker.
class EndOnExit {
public:
    explicit EndOnExit(Activity& activity) noexcept : activity_(activity) {}
    ~EndOnExit() noexcept { activity_.End(); }
    EndOnExit(const EndOnExit&) = delete;
    EndOnExit& operator=(const EndOnExit&) = delete;

private:
    Activity& activity_;
};
} // namespace WizardUnlock
