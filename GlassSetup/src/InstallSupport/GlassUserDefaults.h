#pragma once
#include <windows.h>
#include <string>

// Per-user defaults written by Setup into the installing user's hive
// (HKCU\Software\StanislavKa\GlassPlatform). The data lives in
// GlassShared/include/DefaultProcessLists.h and is shared with the platform.
namespace GlassUserDefaults {

struct SeedReport {
    bool attachListCreated = false;   // key did not exist (or held no entries) and was filled
    bool blockListCreated = false;
    unsigned attachWritten = 0;
    unsigned blockWritten = 0;
    LSTATUS error = ERROR_SUCCESS;
    std::wstring detail;              // technical reason of a failure, English, for the log/notes
};

// Fills AttachList and BlockList with the defaults when a list does not exist
// yet or holds no entries. Lists that already have entries are never changed,
// so an update keeps the user's edits. Returns false when a registry write
// failed; whatever was written stays and is described in the report.
bool SeedProcessLists(SeedReport& report) noexcept;

// Deletes the lists that SeedProcessLists created in this run. Used when the
// installation is rolled back before the commit point.
void UndoSeededProcessLists(const SeedReport& report) noexcept;

} // namespace GlassUserDefaults
