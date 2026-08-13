// WinForms DrawThemeBackground policy: DateTimePicker.

#include "CorePch.h"

#include <Hooks/forms/FormsHooksDtbPolicies.h>
#include <Hooks/forms/DateTimePickerFormsNiDtbAccess.h>

namespace GlassCore::FormsHooks::DrawThemeBackground::DateTimePicker
{
    bool TryPaint(
        Shared::WinApiUtils::DrawThemeBackground_pfn orig,
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCRECT pRect,
        LPCRECT pClipRect,
        HRESULT& outHr) noexcept
    {
        if (!GlassCore::DateTimePickerControl::FormsNiDtbAccess::IsWinFormsDateTimePickerDrawThemeSessionActive())
            return false;

        outHr = GlassCore::DateTimePickerControl::FormsNiDtbAccess::TryDrawThemeBackground(
            orig, hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        return true;
    }
}
