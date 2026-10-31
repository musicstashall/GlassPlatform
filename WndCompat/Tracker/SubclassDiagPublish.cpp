#include "WndPch.h"
#include "Tracker/SubclassDiagPublish.h"
#include <InjectProcessMap.h>

#include <array>
#include <atomic>

namespace WndCompat::SubclassDiag
{
    namespace
    {
        constexpr std::size_t kMaxKind = 64;
        std::array<std::atomic<std::uint32_t>, kMaxKind> g_counts{};

        void FlushToMap() noexcept
        {
            // total = число установленных сабклассов (сумма size tracker-списков по kind).
            std::uint32_t total = 0;
            std::uint64_t mask = 0;
            for (std::size_t i = 1; i < kMaxKind; ++i)
            {
                const std::uint32_t c = g_counts[i].load(std::memory_order_relaxed);
                if (c == 0)
                    continue;
                total += c;
                mask |= (1ull << i);
            }
            (void)Shared::InjectProcessMap::TryPublishSubclassStats(
                GetCurrentProcessId(),
                total,
                mask);
        }
    }

    void PublishKindCount(GlassShared::GlassControlKind kind, std::size_t count) noexcept
    {
        const auto idx = static_cast<std::size_t>(kind);
        if (idx == 0 || idx >= kMaxKind)
            return;
        g_counts[idx].store(static_cast<std::uint32_t>(count), std::memory_order_relaxed);
        FlushToMap();
    }

    // #region agent log
    // GLASS-ULTRACE (H91)
    std::uint32_t Total() noexcept
    {
        std::uint32_t total = 0;
        for (std::size_t i = 1; i < kMaxKind; ++i)
            total += g_counts[i].load(std::memory_order_relaxed);
        return total;
    }
    // #endregion

    void ClearAll() noexcept
    {
        for (auto& c : g_counts)
            c.store(0, std::memory_order_relaxed);
        (void)Shared::InjectProcessMap::TryPublishSubclassStats(GetCurrentProcessId(), 0, 0);
    }
}
