#pragma once

#include <GlassControlKind.h>
#include <cstddef>
#include <cstdint>

namespace WndCompat::SubclassDiag
{
    /// Публикует size существующего tracker-списка kind → InjectMap.
    /// subclassWindowTotal = сумма по kind = число установленных сабклассов (каждый Attach).
    void PublishKindCount(GlassShared::GlassControlKind kind, std::size_t count) noexcept;

    /// Обнуляет статистику сабклассов в InjectMap (после DetachAll).
    void ClearAll() noexcept;

    /// #region agent log
    /// GLASS-ULTRACE (R2, H91): суммарное число «живых» сабклассов по всем kind.
    /// После DetachAll должно быть 0; иначе FreeLibrary(WndCompat) = stale proc → CFG fail-fast.
    std::uint32_t Total() noexcept;
    /// #endregion
}
