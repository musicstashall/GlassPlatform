#pragma once

#include <GlassInkAlphaCapsule.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace Shared::InkCapsuleQuery
{
    inline constexpr wchar_t kValueName[] = L"GlassInkAlphaCapsule";

    /// Единый диапазон слайдера / REG_BINARY (‰); dual-pass умножает при рендере — см. kDualPassCoverageGammaMultiplier.
    inline constexpr std::uint32_t kUiGammaMinThousandths = 250u;
    inline constexpr std::uint32_t kUiGammaMaxThousandths = 5000u;
    /// Множитель effective coverage gamma для dual-pass (не отображается в UI).
    inline constexpr std::uint32_t kDualPassCoverageGammaMultiplier = 2u;
    inline constexpr std::uint32_t kUiBlendMin = 100u;
    inline constexpr std::uint32_t kUiBlendMax = 255u;
    inline constexpr std::uint32_t kUiPseudoRadiusMaxThousandths = 1000u;
    inline constexpr std::uint32_t kUiPseudoRadiusMinThousandths = 150u;
    inline constexpr std::uint32_t kUiPseudoIntensityMax = 255u;
    /// При включённом pseudoGlow — inten=0 не даёт эффекта в PostProcessPremulInkBuffer.
    inline constexpr std::uint32_t kUiPseudoIntensityMin = 1u;

    [[nodiscard]] InkCapsule::GlassInkAlphaCapsule MakeFactoryCapsule() noexcept;

    /// UI-значение из капсулы → effective ‰ для LUT (dual-pass ×2, cap 10000‰).
    [[nodiscard]] inline std::uint32_t EffectiveCoverageGammaThousandths(
        std::uint32_t storedUiGammaThousandths,
        bool applyDualPassMultiplier) noexcept
    {
        const std::uint64_t scaled = applyDualPassMultiplier
            ? static_cast<std::uint64_t>(storedUiGammaThousandths) * kDualPassCoverageGammaMultiplier
            : storedUiGammaThousandths;
        constexpr std::uint64_t kMaxEffective =
            static_cast<std::uint64_t>(kUiGammaMaxThousandths) * kDualPassCoverageGammaMultiplier;
        return static_cast<std::uint32_t>(scaled < kMaxEffective ? scaled : kMaxEffective);
    }

    void ClampProfileGamma(InkCapsule::ProfilePayload& profile) noexcept;

    void ClampProfile(InkCapsule::ProfilePayload& profile) noexcept;

    [[nodiscard]] bool TryDeserializeCapsuleBytes(
        const void* data, std::size_t byteCount, InkCapsule::GlassInkAlphaCapsule& out) noexcept;

    [[nodiscard]] bool TryReadLocalCapsuleFromHive(
        HKEY hive, InkCapsule::GlassInkAlphaCapsule& out) noexcept;

    /// HKCU → HKLM → factory; при renderMode==0 UI-профили = factory.
    [[nodiscard]] bool ReadUiCapsule(InkCapsule::GlassInkAlphaCapsule& out) noexcept;

    [[nodiscard]] bool IsLocalCapsulePresent() noexcept;

    [[nodiscard]] const InkCapsule::ProfilePayload& ResolveUiActiveProfile(
        const InkCapsule::GlassInkAlphaCapsule& capsule) noexcept;
}
