#include "NativeHost.h"

#include <Helpers/ThemeHelper.h>
#include <OpenGlassDwmPolicy.h>

extern "C" HRESULT WINAPI GlassSettings_ApplyVisualStyleAndDwmPolicy(
    const wchar_t* msstylesPath,
    const wchar_t* colorStyle,
    const wchar_t* size,
    int flags,
    BOOL applyVisualStyle,
    BOOL applyDwmPolicy) noexcept
{
    if (applyVisualStyle)
    {
        if (!msstylesPath || !*msstylesPath)
            return E_INVALIDARG;

        const HRESULT hr = Shared::ThemeHelper::SetSystemVisualStyle(
            msstylesPath,
            colorStyle && *colorStyle ? colorStyle : L"NormalColor",
            size && *size ? size : L"NormalSize",
            flags);
        if (FAILED(hr))
            return hr;
    }

    if (applyDwmPolicy)
        Shared::OpenGlassDwmPolicy::ApplyDwmPolicyOnVisualStyleChange();

    return S_OK;
}

extern "C" void WINAPI GlassSettings_ApplyDwmPolicyOnVisualStyleChange() noexcept
{
    Shared::OpenGlassDwmPolicy::ApplyDwmPolicyOnVisualStyleChange();
}
