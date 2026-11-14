#pragma once
#include <cstddef>

// Platform-neutral commit ordering. Each reversible operation is recorded by
// its callback. The potentially irreversible service deletion is last: if it
// succeeds there is no later fallible operation which could require recreating
// service credentials. Before this boundary Setup still supports cancellation.
namespace GlassRemovalOrder {
template<class RemoveShortcut, class RemoveTask, class DeleteService>
bool Commit(std::size_t taskCount, RemoveShortcut removeShortcut,
    RemoveTask removeTask, DeleteService deleteService) {
    if (!removeShortcut()) return false;
    for (std::size_t i = 0; i < taskCount; ++i)
        if (!removeTask(i)) return false;
    return deleteService();
}
} // namespace GlassRemovalOrder
