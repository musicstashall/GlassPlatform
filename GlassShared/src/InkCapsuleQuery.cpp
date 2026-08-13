#include "SharedPch.h"
#include <InkCapsuleQuery.h>
#include <Helpers/RegHelper.h>

#include <algorithm>
#include <cstring>

namespace Shared::InkCapsuleQuery
{
    namespace
    {
        using Capsule = InkCapsule::GlassInkAlphaCapsule;
        using Profile = InkCapsule::ProfilePayload;

        void ApplyUiProfilesFromCapsule(const Capsule& loaded, Capsule& out) noexcept
        {
            out.renderMode = std::min(loaded.renderMode, 2u);
            out.magic = loaded.magic;
            out.reservedPad = loaded.reservedPad;
            if (out.renderMode == 0u)
            {
                out.single = InkCapsule::kFactoryUserProfileSingle;
                out.dual = InkCapsule::kFactoryUserProfileDual;
                return;
            }

            out.single = loaded.single;
            out.dual = loaded.dual;
            ClampProfile(out.single);
            ClampProfile(out.dual);
        }
    } // namespace

    Capsule MakeFactoryCapsule() noexcept
    {
        Capsule cap{};
        cap.magic = InkCapsule::kMagic;
        cap.renderMode = 0u;
        cap.reservedPad = 0u;
        cap.single = InkCapsule::kFactoryUserProfileSingle;
        cap.dual = InkCapsule::kFactoryUserProfileDual;
        return cap;
    }

    void ClampProfileGamma(Profile& profile) noexcept
    {
        profile.coverageGammaThousandths = static_cast<std::uint32_t>(std::clamp(
            static_cast<int>(profile.coverageGammaThousandths),
            static_cast<int>(kUiGammaMinThousandths),
            static_cast<int>(kUiGammaMaxThousandths)));
    }

    void ClampProfile(Profile& profile) noexcept
    {
        ClampProfileGamma(profile);
        profile.targetBlendAlpha = std::clamp(profile.targetBlendAlpha, kUiBlendMin, kUiBlendMax);
        if (profile.pseudoGlowEnabled)
        {
            profile.pseudoGlowRadiusThousandths = std::clamp(
                profile.pseudoGlowRadiusThousandths,
                kUiPseudoRadiusMinThousandths,
                kUiPseudoRadiusMaxThousandths);
            profile.pseudoGlowIntensity = std::clamp(
                profile.pseudoGlowIntensity,
                kUiPseudoIntensityMin,
                kUiPseudoIntensityMax);
        }
        else
        {
            profile.pseudoGlowRadiusThousandths =
                std::min(profile.pseudoGlowRadiusThousandths, kUiPseudoRadiusMaxThousandths);
            profile.pseudoGlowIntensity =
                std::clamp(profile.pseudoGlowIntensity, 0u, kUiPseudoIntensityMax);
        }
        profile.pseudoGlowEnabled = profile.pseudoGlowEnabled ? 1u : 0u;
        InkCapsule::WriteNeutralDeprecatedInkSlots(profile);
    }

    bool TryDeserializeCapsuleBytes(const void* data, std::size_t byteCount, Capsule& out) noexcept
    {
        if (!data || byteCount < sizeof(Capsule))
            return false;

        std::memcpy(&out, data, sizeof(Capsule));
        if (out.magic != InkCapsule::kMagic)
            return false;

        out.renderMode = std::min(out.renderMode, 2u);
        ClampProfile(out.single);
        ClampProfile(out.dual);
        return true;
    }

    bool TryReadLocalCapsuleFromHive(HKEY hive, Capsule& out) noexcept
    {
        std::uint8_t buffer[sizeof(Capsule)]{};
        DWORD type = 0;
        DWORD size = static_cast<DWORD>(sizeof(buffer));
        const LSTATUS status = RegGetValueW(
            hive,
            RegHelper::g_regPath.data(),
            kValueName,
            RRF_RT_REG_BINARY,
            &type,
            buffer,
            &size);
        if (status != ERROR_SUCCESS || type != REG_BINARY)
            return false;

        return TryDeserializeCapsuleBytes(buffer, size, out);
    }

    bool ReadUiCapsule(Capsule& out) noexcept
    {
        out = MakeFactoryCapsule();

        Capsule loaded{};
        if (TryReadLocalCapsuleFromHive(HKEY_CURRENT_USER, loaded)
            || TryReadLocalCapsuleFromHive(HKEY_LOCAL_MACHINE, loaded))
        {
            ApplyUiProfilesFromCapsule(loaded, out);
            return true;
        }

        return false;
    }

    bool IsLocalCapsulePresent() noexcept
    {
        Capsule capsule{};
        return TryReadLocalCapsuleFromHive(HKEY_CURRENT_USER, capsule);
    }

    const Profile& ResolveUiActiveProfile(const Capsule& capsule) noexcept
    {
        return capsule.renderMode == 2u ? capsule.dual : capsule.single;
    }
}
