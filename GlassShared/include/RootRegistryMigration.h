#pragma once

namespace Shared::RootRegistryMigration
{
    /// Однократная миграция Root\BackDropType → Root\NoBackdrop (идемпотентно).
    void MigrateLegacyRootBackDropType() noexcept;
}
