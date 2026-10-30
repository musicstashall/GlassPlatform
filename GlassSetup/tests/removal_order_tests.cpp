#include "../src/InstallSupport/GlassRemovalOrder.h"
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

// Portable failure injection checks the SAME ordering function used by Setup.
// Real MSI, COM task registration, service deletion and ACLs require Windows VM tests.
int main() {
    for (int failure = -1; failure < 5; ++failure) {
        bool shortcut = true, service = true;
        bool tasks[] = {true, true, true};
        std::vector<std::string> calls;
        const bool committed = GlassRemovalOrder::Commit(3,
            [&] {
                calls.push_back("shortcut");
                if (failure == 0) return false;
                shortcut = false;
                return true;
            },
            [&](std::size_t i) {
                calls.push_back("task" + std::to_string(i));
                if (failure == static_cast<int>(i) + 1) return false;
                tasks[i] = false;
                return true;
            },
            [&] {
                calls.push_back("service");
                if (failure == 4) return false;
                service = false;
                return true;
            });
        if (failure < 0) {
            assert(committed && !shortcut && !tasks[0] && !tasks[1] && !tasks[2] && !service);
            assert(calls == std::vector<std::string>({"shortcut", "task0", "task1", "task2", "service"}));
        } else {
            assert(!committed && service); // service deletion cannot be followed by another failure
            for (int i = 0; i < 3; ++i) {
                if (failure <= i + 1) assert(tasks[i]); // this task was never deleted
            }
            if (failure < 4) assert(calls.back() != "service");
            // Model restoration of previously removed reversible artifacts.
            for (int i = 2; i >= 0; --i) if (!tasks[i]) tasks[i] = true;
            if (!shortcut) shortcut = true;
            assert(shortcut && tasks[0] && tasks[1] && tasks[2] && service);
        }
    }
    std::vector<std::string> calls;
    assert(GlassRemovalOrder::Commit(0, [&]{ calls.push_back("shortcut"); return true; },
        [&](std::size_t){ assert(false); return false; }, [&]{ calls.push_back("service"); return true; }));
    assert(calls == std::vector<std::string>({"shortcut", "service"}));
    std::cout << "PASS: failure injection keeps irreversible service deletion last\n";
}
