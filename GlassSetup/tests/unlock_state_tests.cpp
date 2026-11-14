#include "../src/WizardUnlockState.h"
#include <atomic>
#include <cassert>
#include <iostream>
#include <string_view>
#include <thread>

using WizardUnlock::Activity;
using WizardUnlock::InstallingPage;
using WizardUnlock::Phase;

static bool IsPage(const Activity& activity, const wchar_t* name) {
    return std::wstring_view(InstallingPage(activity.Current())) == name;
}

// Simulate a worker leaving via either a successful or a failing/cancelled path.
static bool FinishAttempt(Activity& activity, bool success) {
    WizardUnlock::EndOnExit finish(activity);
    if (!success) return false;
    return true;
}

int main() {
    Activity activity;
    assert(IsPage(activity,L"installing"));

    // Retry succeeds: keep the unlock animation while work is running, then
    // return to the normal installation carousel as soon as work has finished.
    activity.Begin(Phase::Modules);
    assert(IsPage(activity,L"unlock-modules"));
    assert(FinishAttempt(activity,true));
    assert(IsPage(activity,L"installing"));

    // Retry cannot release everything: a new prompt ends the previous activity;
    // after the user proceeds, the second unlock phase runs and then also ends.
    activity.Begin(Phase::Modules);
    activity.End(); // new processes-still-holding prompt
    assert(IsPage(activity,L"installing"));
    activity.Begin(Phase::Processes);
    assert(IsPage(activity,L"unlock-processes"));
    assert(FinishAttempt(activity,true));
    assert(IsPage(activity,L"installing"));

    // Failure, cancellation and a cancelled confirmation must never leave a
    // stale unlock page when the user resumes normal installation.
    for (Phase phase : {Phase::Modules,Phase::Processes}) {
        activity.Begin(phase);
        assert(!FinishAttempt(activity,false));
        assert(IsPage(activity,L"installing"));
        activity.Begin(phase);
        activity.End(); // cancel while the worker is still in the unlock stage
        assert(IsPage(activity,L"installing"));
    }

    // UI and worker use the same activity safely from different threads.
    std::atomic_bool ready{false},release{false};
    activity.Begin(Phase::Processes);
    std::thread worker([&] {
        WizardUnlock::EndOnExit finish(activity);
        ready.store(true,std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    assert(IsPage(activity,L"unlock-processes"));
    release.store(true,std::memory_order_release);
    worker.join();
    assert(IsPage(activity,L"installing"));
    std::cout << "PASS: unlock prompts/operations return to the installation slideshow on completion or cancellation\n";
}
