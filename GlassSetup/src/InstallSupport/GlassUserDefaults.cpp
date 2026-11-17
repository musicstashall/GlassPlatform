#include "GlassUserDefaults.h"

#include <DefaultProcessListsSeed.h>

namespace GlassUserDefaults {

bool SeedProcessLists(SeedReport& report) noexcept {
    report = SeedReport{};
    GlassShared::DefaultProcessLists::SeedResult seeded{};
    const bool ok = GlassShared::DefaultProcessLists::SeedMissing(seeded);
    report.attachListCreated = seeded.attachCreated;
    report.blockListCreated = seeded.blockCreated;
    report.attachWritten = seeded.attachWritten;
    report.blockWritten = seeded.blockWritten;
    report.error = seeded.error;
    report.detail = seeded.detail;
    return ok;
}

void UndoSeededProcessLists(const SeedReport& report) noexcept {
    // Only lists this run filled are removed; a list the user already had is
    // never touched, exactly like during seeding.
    GlassShared::DefaultProcessLists::SeedResult seeded{};
    seeded.attachCreated = report.attachListCreated;
    seeded.blockCreated = report.blockListCreated;
    GlassShared::DefaultProcessLists::UndoSeed(seeded);
}

} // namespace GlassUserDefaults
