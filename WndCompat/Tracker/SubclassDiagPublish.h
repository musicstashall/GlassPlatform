#pragma once

#include <GlassControlKind.h>
#include <cstddef>

namespace WndCompat::SubclassDiag
{
    /// Публикует size существующего tracker-списка kind → InjectMap.
    /// subclassWindowTotal = сумма по kind = число установленных сабклассов (каждый Attach).
    void PublishKindCount(GlassShared::GlassControlKind kind, std::size_t count) noexcept;

    /// Обнуляет статистику сабклассов в InjectMap (после DetachAll).
    void ClearAll() noexcept;
}
