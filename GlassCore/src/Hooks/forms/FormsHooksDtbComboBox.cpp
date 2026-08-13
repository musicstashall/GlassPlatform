// WinForms DrawThemeBackground policy: ComboBox.

#include "CorePch.h"

#include <Hooks/forms/FormsHooksDtbPolicies.h>
#include <Hooks/forms/ComboFormsNiDtbAccess.h>
#include <Helpers/ThemeHelper.h>
#include <DebugLog.h>

namespace GlassCore::FormsHooks::DrawThemeBackground::ComboBox
{
    bool TryPaint(
        Shared::WinApiUtils::DrawThemeBackground_pfn orig,
        HMODULE callerMod,
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCRECT pRect,
        LPCRECT pClipRect,
        HRESULT& outHr) noexcept
    {
        if (!GlassCore::ComboBoxControl::FormsNiDtbAccess::IsWinFormsComboDrawThemeSessionActive())
            return false;

        WCHAR themeClass[MAX_PATH + 1]{};
        if (!SUCCEEDED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            || _wcsicmp(themeClass, L"Combobox") != 0)
            return false;

        DebugLog(DBG_NORMAL,
            L"[GlassCore][FormsHooks][ComboBox] DrawThemeBackground: caller=%p part=%d state=%d",
            callerMod, iPartId, iStateId);

        outHr = GlassCore::ComboBoxControl::FormsNiDtbAccess::TryDrawThemeBackground(
            orig,
            callerMod,
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pRect,
            pClipRect);
        return true;
    }
}
