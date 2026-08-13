// WinForms DrawThemeBackground policy: Button / ListBox.

#include "CorePch.h"

#include <Hooks/forms/FormsHooksDtbPolicies.h>
#include <Hooks/forms/ButtonFormsNiDtbAccess.h>
#include <Helpers/ThemeHelper.h>
#include <Subsystems/PaintDispatcher.h>
#include <DebugLog.h>

namespace GlassCore::FormsHooks::DrawThemeBackground::Button
{
    namespace
    {
        thread_local uint32_t g_seq{};
    }

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
        if (!GlassCore::Button::FormsNiDtbAccess::IsDrawThemeBackgroundDetourSessionActive())
            return false;

        const uint32_t seq = ++g_seq;
        WCHAR themeClass[MAX_PATH + 1]{ L"?" };
        if (hTheme)
            Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][FormsHooks][Button] DrawThemeBackground: seq=%u hdc=%p part=%d state=%d theme=%s "
            L"rect=[%d,%d,%d,%d] hostHwnd=%p caller=%p",
            seq, hdc, iPartId, iStateId, themeClass,
            pRect ? pRect->left : -1, pRect ? pRect->top : -1,
            pRect ? pRect->right : -1, pRect ? pRect->bottom : -1,
            GlassCore::PaintDispatcher::GetPaintSessionHostHwnd(), callerMod);

        outHr = GlassCore::Button::FormsNiDtbAccess::TryDrawThemeBackground(
            orig,
            callerMod,
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pRect,
            pClipRect,
            seq);
        return true;
    }
}
