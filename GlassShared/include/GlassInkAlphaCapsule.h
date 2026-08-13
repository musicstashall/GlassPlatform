#pragma once
#include <Windows.h>
#include <cstring>
#include <cstdint>
#include <Helpers/ThemeHelper.h>

// Единый бинарный формат REG_BINARY GlassInkAlphaCapsule (GlassSettings + GlassCore).
// Один layout на весь репозиторий; поле version не используется.
namespace Shared::InkCapsule
{
    inline constexpr uint32_t kMagic =
        (static_cast<uint32_t>('G') | (static_cast<uint32_t>('V') << 8) |
            (static_cast<uint32_t>('T') << 16) | (static_cast<uint32_t>('1') << 24));

    // Слоты бывш. ClearType / preserve / fringe soft — фиксированные нейтральные значения.
    inline constexpr uint32_t kInkCapsuleNeutralPreserveClearTypeTint = 0u;
    inline constexpr uint32_t kInkCapsuleNeutralCovInAlphaThousandths = 1000u;
    inline constexpr uint32_t kInkCapsuleNeutralFringeGainThousandths = 1000u;
    inline constexpr uint32_t kInkCapsuleNeutralAlphaPostAdd = 0u;
    inline constexpr uint32_t kInkCapsuleNeutralFringeSofteningThousandths = 1000u;

    inline constexpr wchar_t kValLightGlobalTextGlow[] = L"LightMode_GlobalTextGlow";
    inline constexpr wchar_t kValDarkGlobalTextGlow[] = L"DarkMode_GlobalTextGlow";

#pragma pack(push, 1)
    /// Глобальный glow на одну тему ОС (отдельный REG_BINARY на light/dark).
    /// glowColor: ARGB 0xAARRGGBB; интенсивность = alpha (ResolveGlowIntensityThousandths).
    /// glowSpreadMode: 0 = минимально, 1 = умеренно, 2 = глобально (использование в рендере — отдельно).
    struct GlobalTextGlow
    {
        uint32_t glowEnabled;
        uint32_t glowRadiusThousandths;
        uint32_t glowColor;
        uint32_t glowSpreadMode;
    };

    /// Профиль ink для одного прохода (single или dual): без glow.
    struct ProfilePayload
    {
        uint32_t coverageGammaThousandths;
        uint32_t preserveClearTypeTint;
        uint32_t covInAlphaScaleThousandths;
        uint32_t fringeGainThousandths;
        uint32_t alphaPostAdd;
        uint32_t targetBlendAlpha;
        uint32_t grayBgLevel;
        uint32_t fringeSofteningThousandths;
        uint32_t pseudoGlowEnabled;
        uint32_t pseudoGlowRadiusThousandths;
        uint32_t pseudoGlowIntensity; // 0…255 (альфа силы контура, как targetBlendAlpha)
    };

    /// Реестр: LightMode_/DarkMode_GlobalTextGlow (см. GlassCore / GlassSettings).
    struct GlassInkAlphaCapsule
    {
        uint32_t magic;
        uint32_t renderMode;
        uint32_t reservedPad;
        ProfilePayload single;
        ProfilePayload dual;
    };
#pragma pack(pop)

    static_assert(sizeof(GlobalTextGlow) == 16);
    static_assert(sizeof(ProfilePayload) == 44);
    static_assert(sizeof(GlassInkAlphaCapsule) == 100);

    inline void WriteNeutralDeprecatedInkSlots(ProfilePayload& p) noexcept
    {
        p.preserveClearTypeTint = kInkCapsuleNeutralPreserveClearTypeTint;
        p.covInAlphaScaleThousandths = kInkCapsuleNeutralCovInAlphaThousandths;
        p.fringeGainThousandths = kInkCapsuleNeutralFringeGainThousandths;
        p.alphaPostAdd = kInkCapsuleNeutralAlphaPostAdd;
        p.grayBgLevel = 0u;
        p.fringeSofteningThousandths = kInkCapsuleNeutralFringeSofteningThousandths;
    }

    /// Alpha factory glow (бывш. 14900‰); RGB — runtime COLOR_WINDOW.
    inline constexpr uint32_t kFactoryGlowAlpha = 149u;

    inline uint32_t PackGlowColorArgb(uint32_t a, uint32_t r, uint32_t g, uint32_t b) noexcept
    {
        return ((a & 0xFFu) << 24) | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
    }

    inline void UnpackGlowColorArgb(
        uint32_t argb, uint32_t& a, uint32_t& r, uint32_t& g, uint32_t& b) noexcept
    {
        a = (argb >> 24) & 0xFFu;
        r = (argb >> 16) & 0xFFu;
        g = (argb >> 8) & 0xFFu;
        b = argb & 0xFFu;
    }

    inline uint32_t MakeFactoryGlowColorArgb() noexcept
    {
        const COLORREF win = GetSysColor(COLOR_WINDOW);
        return PackGlowColorArgb(
            kFactoryGlowAlpha,
            static_cast<uint32_t>(GetRValue(win)),
            static_cast<uint32_t>(GetGValue(win)),
            static_cast<uint32_t>(GetBValue(win)));
    }

    inline GlobalTextGlow MakeFactoryGlobalGlow() noexcept
    {
        GlobalTextGlow g{};
        g.glowEnabled = 0u;
        g.glowRadiusThousandths = 300u;
        g.glowColor = MakeFactoryGlowColorArgb();
        g.glowSpreadMode = 0u;
        return g;
    }

    inline bool IsFactoryGlobalGlow(const GlobalTextGlow& g) noexcept
    {
        const GlobalTextGlow f = MakeFactoryGlobalGlow();
        return g.glowEnabled == f.glowEnabled
            && g.glowRadiusThousandths == f.glowRadiusThousandths
            && g.glowSpreadMode == f.glowSpreadMode
            && g.glowColor == f.glowColor;
    }

    inline uint32_t ResolveGlowIntensityThousandths(const GlobalTextGlow& g) noexcept
    {
        return ((g.glowColor >> 24) & 0xFFu) * 100u;
    }

    inline uint32_t ResolveGlowColorRgb(const GlobalTextGlow& g) noexcept
    {
        return g.glowColor & 0x00FFFFFFu;
    }

    inline COLORREF ResolveGlowColorRef(const GlobalTextGlow& g) noexcept
    {
        uint32_t a = 0;
        uint32_t r = 0;
        uint32_t gCh = 0;
        uint32_t b = 0;
        UnpackGlowColorArgb(g.glowColor, a, r, gCh, b);
        return RGB(static_cast<BYTE>(r), static_cast<BYTE>(gCh), static_cast<BYTE>(b));
    }

    inline COLORREF ResolveGlowColorRefForRegistryTheme(const GlobalTextGlow& g) noexcept
    {
        return ResolveGlowColorRef(g);
    }

    /// REG_BINARY theme capsule (16 byte); иначе factory.
    inline void DeserializeGlobalTextGlow(GlobalTextGlow& out, const void* data, size_t byteCount) noexcept
    {
        out = MakeFactoryGlobalGlow();
        if (!data || byteCount < sizeof(GlobalTextGlow))
            return;
        std::memcpy(&out, data, sizeof(GlobalTextGlow));
    }

    // «Авто» / factory: single-pass и dual-pass различаются по гамме / tblend / псевдо; глобальный glow — в отдельном ключе реестра.
    inline constexpr ProfilePayload kAutoProfileSingle{
        506u, kInkCapsuleNeutralPreserveClearTypeTint,
        kInkCapsuleNeutralCovInAlphaThousandths, kInkCapsuleNeutralFringeGainThousandths,
        kInkCapsuleNeutralAlphaPostAdd,
        194u, 0u,
        kInkCapsuleNeutralFringeSofteningThousandths,
        0u, 0u, 0u,
    };
    inline constexpr ProfilePayload kAutoProfileDual{
        1775u, kInkCapsuleNeutralPreserveClearTypeTint,
        kInkCapsuleNeutralCovInAlphaThousandths, kInkCapsuleNeutralFringeGainThousandths,
        kInkCapsuleNeutralAlphaPostAdd,
        190u, 0u,
        kInkCapsuleNeutralFringeSofteningThousandths,
        1u, 610u, 113u,
    };

    inline constexpr ProfilePayload kFactoryUserProfileSingle{
        506u, kInkCapsuleNeutralPreserveClearTypeTint,
        kInkCapsuleNeutralCovInAlphaThousandths, kInkCapsuleNeutralFringeGainThousandths,
        kInkCapsuleNeutralAlphaPostAdd,
        194u, 0u,
        kInkCapsuleNeutralFringeSofteningThousandths,
        0u, 0u, 0u,
    };
    inline constexpr ProfilePayload kFactoryUserProfileDual{
        1775u, kInkCapsuleNeutralPreserveClearTypeTint,
        kInkCapsuleNeutralCovInAlphaThousandths, kInkCapsuleNeutralFringeGainThousandths,
        kInkCapsuleNeutralAlphaPostAdd,
        190u, 0u,
        kInkCapsuleNeutralFringeSofteningThousandths,
        1u, 610u, 113u,
    };
}
