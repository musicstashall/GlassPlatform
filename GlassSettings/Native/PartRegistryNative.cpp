#include <PartRegistryQuery.h>
#include <EffectTypeQuery.h>
#include <InkCapsuleQuery.h>

#include <Windows.h>
#include <cstring>

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadGlassLocalDword(
    const wchar_t* GlassSubKey,
    const wchar_t* valueName,
    UINT* outValue) noexcept
{
    if (!outValue || !valueName)
        return FALSE;

    const auto value = Shared::PartRegistryQuery::TryReadLocalDword(
        GlassSubKey ? GlassSubKey : L"",
        valueName);
    if (!value.has_value())
        return FALSE;

    *outValue = value.value();
    return TRUE;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_ReadGlassEffectiveDword(
    const wchar_t* GlassSubKey,
    const wchar_t* valueName,
    UINT defaultValue) noexcept
{
    if (!valueName)
        return defaultValue;

    return Shared::PartRegistryQuery::ReadEffectiveDword(
        GlassSubKey ? GlassSubKey : L"",
        valueName,
        defaultValue);
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_ReadGlassUiGradientArgb(
    const wchar_t* GlassSubKey,
    BOOL darkMode) noexcept
{
    return Shared::PartRegistryQuery::ReadUiGradientArgb(
        GlassSubKey ? GlassSubKey : L"",
        darkMode != FALSE);
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_ReadGlassUiBorderArgb(
    const wchar_t* GlassSubKey,
    BOOL darkMode) noexcept
{
    return Shared::PartRegistryQuery::ReadUiBorderArgb(
        GlassSubKey ? GlassSubKey : L"",
        darkMode != FALSE);
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_ReadGlassUiCornerType(
    const wchar_t* GlassSubKey) noexcept
{
    return Shared::PartRegistryQuery::ReadUiCornerType(GlassSubKey ? GlassSubKey : L"");
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_ReadGlassUiNoBorderColor(
    const wchar_t* GlassSubKey) noexcept
{
    return Shared::PartRegistryQuery::ReadUiNoBorderColor(GlassSubKey ? GlassSubKey : L"")
        ? TRUE
        : FALSE;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetDefaultFlyoutEffectType() noexcept
{
    return Shared::EffectTypeQuery::GetDefaultFlyoutEffectType();
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetDefaultRootEffectType() noexcept
{
    return Shared::EffectTypeQuery::GetDefaultRootEffectType();
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_ReadGlassUiEffectType(
    const wchar_t* GlassSubKey) noexcept
{
    return Shared::EffectTypeQuery::ReadUiEffectType(GlassSubKey ? GlassSubKey : L"");
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_TryReadGlassLocalEffectType(
    const wchar_t* GlassSubKey,
    UINT* outValue) noexcept
{
    if (!outValue)
        return FALSE;

    const auto value = Shared::EffectTypeQuery::TryReadLocalEffectType(
        GlassSubKey ? GlassSubKey : L"");
    if (!value.has_value())
        return FALSE;

    *outValue = value.value();
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_ReadUiInkCapsule(
    BYTE* buffer, SIZE_T byteCount) noexcept
{
    if (!buffer || byteCount < sizeof(Shared::InkCapsule::GlassInkAlphaCapsule))
        return FALSE;

    Shared::InkCapsule::GlassInkAlphaCapsule cap{};
    const bool fromRegistry = Shared::InkCapsuleQuery::ReadUiCapsule(cap);
    std::memcpy(buffer, &cap, sizeof(cap));
    return fromRegistry ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void WINAPI GlassSettings_ReadFactoryInkCapsule(
    BYTE* buffer, SIZE_T byteCount) noexcept
{
    if (!buffer || byteCount < sizeof(Shared::InkCapsule::GlassInkAlphaCapsule))
        return;

    const auto cap = Shared::InkCapsuleQuery::MakeFactoryCapsule();
    std::memcpy(buffer, &cap, sizeof(cap));
}

extern "C" __declspec(dllexport) BOOL WINAPI GlassSettings_IsInkCapsulePresent() noexcept
{
    return Shared::InkCapsuleQuery::IsLocalCapsulePresent() ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetInkUiGammaMinThousandths() noexcept
{
    return Shared::InkCapsuleQuery::kUiGammaMinThousandths;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetInkUiGammaMaxThousandths() noexcept
{
    return Shared::InkCapsuleQuery::kUiGammaMaxThousandths;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetInkUiBlendMin() noexcept
{
    return Shared::InkCapsuleQuery::kUiBlendMin;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetInkUiBlendMax() noexcept
{
    return Shared::InkCapsuleQuery::kUiBlendMax;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetInkUiPseudoRadiusMaxThousandths() noexcept
{
    return Shared::InkCapsuleQuery::kUiPseudoRadiusMaxThousandths;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetInkUiPseudoRadiusMinThousandths() noexcept
{
    return Shared::InkCapsuleQuery::kUiPseudoRadiusMinThousandths;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetInkUiPseudoIntensityMax() noexcept
{
    return Shared::InkCapsuleQuery::kUiPseudoIntensityMax;
}

extern "C" __declspec(dllexport) UINT WINAPI GlassSettings_GetInkUiPseudoIntensityMin() noexcept
{
    return Shared::InkCapsuleQuery::kUiPseudoIntensityMin;
}
