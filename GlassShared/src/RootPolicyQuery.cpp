#include "SharedPch.h"
#include <RootPolicyQuery.h>
#include <PlatformPresenceState.h>
#include <ProcessBlockList.h>
#include <Helpers/RegHelper.h>
#include <OpenGlassDwmPolicy.h>

#include <atomic>

namespace Shared::RootPolicyQuery
{
    namespace
    {
        std::atomic<DWORD> g_cachedRootBackdropMode{ 0xFFFFFFFFu };

        /// LogViewer: стабильный SWCA-backdrop; не читать snapshot платформы / pipeline OpenGlass.
        constexpr DWORD kLogViewerRootBackdropMode = 2u;

        [[nodiscard]] bool IsLogViewerIsolatedHost() noexcept
        {
            return ProcessBlockList::IsCurrentProcessLogViewerHost();
        }

        [[nodiscard]] DWORD ReadNoBackdropRaw() noexcept
        {
            constexpr std::wstring_view kRoot{L"Root"};
            constexpr std::wstring_view kEmpty{L""};

            if (const auto noBackdrop = Shared::RegHelper::TryGet<DWORD>(
                    { kRoot, kEmpty }, L"NoBackdrop", 1))
            {
                return noBackdrop.value() != 0 ? 1u : 0u;
            }

            return 0u;
        }

        [[nodiscard]] DWORD ReadModeFromMapping(bool ignoreNoBackdrop = false) noexcept
        {
            if (!ignoreNoBackdrop && ReadNoBackdropRaw() != 0)
                return 0xFFFFFFFFu;

            PlatformPresenceState::Snapshot snap{};
            if (!PlatformPresenceState::TryReadSnapshot(snap) || snap.revision == 0)
                return 0xFFFFFFFFu;
            return snap.rootBackdropMode;
        }

        [[nodiscard]] DWORD ComputeFallbackRootBackdropMode(bool ignoreNoBackdrop = false) noexcept
        {
            if (!ignoreNoBackdrop && ReadNoBackdropRaw() != 0)
                return 0u;

            if (Shared::OpenGlassDwmPolicy::IsDwmGlassInjectedPipelinePresent())
                return 1u;

            return 2u;
        }
    } // namespace

    DWORD ReadNoBackdropFromRegistry() noexcept
    {
        if (IsLogViewerIsolatedHost())
            return 0u;
        return ReadNoBackdropRaw();
    }

    void SetRootBackdropMode(DWORD mode) noexcept
    {
        g_cachedRootBackdropMode.store(mode, std::memory_order_release);
    }

    void RefreshRootBackdropPolicyCache() noexcept
    {
        if (IsLogViewerIsolatedHost())
        {
            g_cachedRootBackdropMode.store(kLogViewerRootBackdropMode, std::memory_order_release);
            return;
        }

        const bool toolHost = ProcessBlockList::IsCurrentProcessGlassToolHost();
        if (!toolHost && ReadNoBackdropRaw() != 0)
        {
            g_cachedRootBackdropMode.store(0u, std::memory_order_release);
            return;
        }

        DWORD mode = ReadModeFromMapping(toolHost);
        if (mode == 0xFFFFFFFFu)
            mode = ComputeFallbackRootBackdropMode(toolHost);

        g_cachedRootBackdropMode.store(mode, std::memory_order_release);
    }

    DWORD ReadRootBackdropMode() noexcept
    {
        if (IsLogViewerIsolatedHost())
        {
            g_cachedRootBackdropMode.store(kLogViewerRootBackdropMode, std::memory_order_release);
            return kLogViewerRootBackdropMode;
        }

        // Paint зовёт это тысячи раз в секунду. Реестр и snapshot — только пока кэш пуст;
        // смена политики приходит через SetRootBackdropMode / RefreshRootBackdropPolicyCache.
        const DWORD cached = g_cachedRootBackdropMode.load(std::memory_order_acquire);
        if (cached != 0xFFFFFFFFu)
            return cached;

        const bool toolHost = ProcessBlockList::IsCurrentProcessGlassToolHost();
        if (!toolHost && ReadNoBackdropRaw() != 0)
        {
            g_cachedRootBackdropMode.store(0u, std::memory_order_release);
            return 0u;
        }

        const DWORD fromMapping = ReadModeFromMapping(toolHost);
        if (fromMapping != 0xFFFFFFFFu)
        {
            g_cachedRootBackdropMode.store(fromMapping, std::memory_order_release);
            return fromMapping;
        }

        const DWORD cachedAgain = g_cachedRootBackdropMode.load(std::memory_order_acquire);
        if (cachedAgain != 0xFFFFFFFFu)
            return cachedAgain;

        const DWORD mode = ComputeFallbackRootBackdropMode(toolHost);
        g_cachedRootBackdropMode.store(mode, std::memory_order_release);
        return mode;
    }

    bool IsRootGlassRenderingEnabled() noexcept
    {
        if (IsLogViewerIsolatedHost())
            return true;
        if (ProcessBlockList::IsCurrentProcessGlassToolHost())
            return true;
        return ReadRootBackdropMode() != 0u;
    }

    bool UsesSwcaBackdrop() noexcept
    {
        if (IsLogViewerIsolatedHost())
            return true;
        return ReadRootBackdropMode() == 2u;
    }
}
