// GlassCore/src/Subsystems/MenuRendering.cpp
#include "CorePch.h"

#include <Subsystems/MenuRendering.h>
#include <Subsystems/MenuHandler.h>
#include <Helpers/ThemeHelper.h>
#include <DXHelper.h>    // Shared::DXHelper::LazyD2D, MakeColorF
#include <MenuUtils.h>

#include <cwchar>

namespace
{
    constexpr int kMenuPopupBorderOutlinePx = 1;
}

namespace GlassCore::Menu::Rendering
{
    using WndCompat::Menu::MenuContext;

    static const MenuContext& Ctx()
    {
        return MenuHandler::GetContext();
    }

    // -----------------------------------------------------------------------
    // HandleCustomRendering — D2D рендеринг кастомных элементов меню.
    //
    // Зеркальная копия BWM::MenuRendering::HandleCustomRendering.
    // Всегда начинает с PatBlt(BLACKNESS) чтобы открыть DWM blur под элементом,
    // затем рисует D2D поверх.
    // -----------------------------------------------------------------------
    bool HandleCustomRendering(
        HDC hdc,
        int iPartId,
        int iStateId,
        const RECT& clipRect,
        const RECT& paintRect)
    {
        auto& lazyD2D = Shared::DXHelper::LazyD2D::GetInstance();
        lazyD2D.EnsureResources();
        auto renderTarget = lazyD2D.GetRenderTarget();
        if (!renderTarget)
            return false;

        const auto& cr = Ctx().customRendering;

        // Ограничиваем clip и черним фон — открываем DWM blur
        int savedDC = SaveDC(hdc);
        IntersectClipRect(hdc, clipRect.left, clipRect.top, clipRect.right, clipRect.bottom);
        PatBlt(hdc, paintRect.left, paintRect.top,
            paintRect.right - paintRect.left, paintRect.bottom - paintRect.top, BLACKNESS);

        auto restoreDC = wil::scope_exit([&] { RestoreDC(hdc, savedDC); });

        // ------ Separator: горизонтальная линия D2D ------
        if (iPartId == MENU_POPUPSEPARATOR)
        {
            DebugLog(DBG_IMPORTANT, L"[GlassCore][MenuRendering] Separator: disabled=%d width=%u color=0x%08X cornerRadius=%u rect=(%d,%d,%d,%d)",
                cr.separator_disabled, cr.separator_width, cr.separator_color, cr.separator_cornerRadius,
                paintRect.left, paintRect.top, paintRect.right, paintRect.bottom);

            if (cr.separator_disabled)
                return false;   // BLACKNESS уже залит; родитель блокирует system DTB

            try
            {
                wil::com_ptr<ID2D1SolidColorBrush> brush;
                THROW_IF_FAILED(renderTarget->CreateSolidColorBrush(
                    Shared::DXHelper::MakeColorF(cr.separator_color), &brush));
                THROW_IF_FAILED(renderTarget->BindDC(hdc, &paintRect));

                renderTarget->BeginDraw();
                float cy = static_cast<float>(paintRect.bottom - paintRect.top) / 2.f;
                float w = static_cast<float>(paintRect.right - paintRect.left);
                renderTarget->DrawLine(
                    D2D1::Point2F(0.f, cy), D2D1::Point2F(w, cy),
                    brush.get(),
                    static_cast<float>(cr.separator_width) / 1000.f);
                THROW_IF_FAILED(renderTarget->EndDraw());
                return true;
            }
            catch (...) { LOG_CAUGHT_EXCEPTION(); return false; }
        }

        // ------ Keyboard focus rect: скруглённый контур ------
        if (iPartId == MENU_POPUPITEMKBFOCUS)
        {
            if (cr.focusing_disabled)
                return false;

            try
            {
                wil::com_ptr<ID2D1SolidColorBrush> brush;
                THROW_IF_FAILED(renderTarget->CreateSolidColorBrush(
                    Shared::DXHelper::MakeColorF(cr.focusing_color), &brush));
                THROW_IF_FAILED(renderTarget->BindDC(hdc, &paintRect));

                renderTarget->BeginDraw();
                float r = static_cast<float>(cr.focusing_cornerRadius);
                float cx = static_cast<float>(paintRect.right - paintRect.left);
                float cy = static_cast<float>(paintRect.bottom - paintRect.top);
                renderTarget->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(0.f, 0.f, cx, cy), r, r),
                    brush.get(),
                    static_cast<float>(cr.focusing_width) / 1000.f);
                THROW_IF_FAILED(renderTarget->EndDraw());
                return true;
            }
            catch (...) { LOG_CAUGHT_EXCEPTION(); return false; }
        }

        // ------ PopupItem: HOT и DISABLEDHOT подсветка ------
        if (iPartId == MENU_POPUPITEM || iPartId == MENU_POPUPITEMFOCUSABLE)
        {
            // Disabled+Hot: заливка скруглённым прямоугольником
            if (iStateId == MPI_DISABLEDHOT)
            {
                if (cr.disabledHot_disabled)
                    return false;

                try
                {
                    wil::com_ptr<ID2D1SolidColorBrush> brush;
                    THROW_IF_FAILED(renderTarget->CreateSolidColorBrush(
                        Shared::DXHelper::MakeColorF(cr.disabledHot_color), &brush));
                    THROW_IF_FAILED(renderTarget->BindDC(hdc, &paintRect));

                    renderTarget->BeginDraw();
                    float rr = static_cast<float>(cr.disabledHot_cornerRadius);
                    float cx = static_cast<float>(paintRect.right - paintRect.left);
                    float cy = static_cast<float>(paintRect.bottom - paintRect.top);
                    renderTarget->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(0.f, 0.f, cx, cy), rr, rr),
                        brush.get());
                    THROW_IF_FAILED(renderTarget->EndDraw());
                    return true;
                }
                catch (...) { LOG_CAUGHT_EXCEPTION(); return false; }
            }

            // Hot: заливка подсветкой при наведении
            if (iStateId == MPI_HOT)
            {
                if (cr.hot_disabled)
                    return false;

                try
                {
                    wil::com_ptr<ID2D1SolidColorBrush> brush;
                    THROW_IF_FAILED(renderTarget->CreateSolidColorBrush(
                        Shared::DXHelper::MakeColorF(cr.hot_color), &brush));
                    THROW_IF_FAILED(renderTarget->BindDC(hdc, &paintRect));

                    renderTarget->BeginDraw();
                    float rr = static_cast<float>(cr.hot_cornerRadius);
                    float cx = static_cast<float>(paintRect.right - paintRect.left);
                    float cy = static_cast<float>(paintRect.bottom - paintRect.top);
                    renderTarget->FillRoundedRectangle(
                        D2D1::RoundedRect(D2D1::RectF(0.f, 0.f, cx, cy), rr, rr),
                        brush.get());
                    THROW_IF_FAILED(renderTarget->EndDraw());
                    return true;
                }
                catch (...) { LOG_CAUGHT_EXCEPTION(); return false; }
            }
        }

        return false;
    }

    // -----------------------------------------------------------------------
    // HandleDrawThemeBackground — основная точка перехвата DTB для всех типов
    // меню (Immersive, TraverseLog, NativeTheme через E8-патчи).
    //
    // Зеркальная копия BWM::MenuRendering::HandleDrawThemeBackground.
    // -----------------------------------------------------------------------
    bool HandleDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect,
        decltype(&DrawThemeBackground) actualDTB)
    {
        if (!hdc || !pRect || IsRectEmpty(pRect))
            return false;

        // Проверяем что это тема Menu
        wchar_t cls[128]{};
        if (FAILED(Shared::ThemeHelper::GetThemeClass(hTheme, cls, _countof(cls) - 1)))
            return false;
        if (std::wcsstr(cls, L"Menu") == nullptr)
            return false;

        const auto& ctx = MenuHandler::GetContext();

        RECT clipRect = *pRect;
        if (pClipRect)
            IntersectRect(&clipRect, &clipRect, pClipRect);

        // Separator — паритет BWM: custom D2D при успехе; иначе system DTB (return false).
        // Исключение: Separator\Disabled=1 — оставляем BLACKNESS и блокируем систему.
        if (iPartId == MENU_POPUPSEPARATOR)
        {
            if (ctx.customRendering.enable)
            {
                if (HandleCustomRendering(hdc, iPartId, iStateId, clipRect, *pRect))
                    return true;
                if (ctx.customRendering.separator_disabled)
                    return true;
            }
            return false;
        }

        // Keyboard focus rect
        if (iPartId == MENU_POPUPITEMKBFOCUS)
        {
            if (ctx.customRendering.enable)
                HandleCustomRendering(hdc, iPartId, iStateId, clipRect, *pRect);
            return ctx.customRendering.enable;
        }

        // Popup items: HOT и DISABLEDHOT
        if (iPartId == MENU_POPUPITEM || iPartId == MENU_POPUPITEMFOCUSABLE)
        {
            if (iStateId == MPI_DISABLEDHOT)
            {
                if (ctx.customRendering.enable)
                {
                    if (HandleCustomRendering(hdc, iPartId, iStateId, clipRect, *pRect))
                        return true;
                }
            }
            if (iStateId == MPI_HOT)
            {
                if (ctx.customRendering.enable)
                {
                    if (HandleCustomRendering(hdc, iPartId, iStateId, clipRect, *pRect))
                        return true;
                }

                if (actualDTB)
                    actualDTB(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
                return true;
            }
        }

        // Рамка popup (Native Theme DTB) — паритет BWM UxThemeHooks MENU_POPUPBORDERS
        if (iPartId == MENU_POPUPBORDERS)
        {
            PatBlt(hdc, clipRect.left, clipRect.top,
                clipRect.right - clipRect.left, clipRect.bottom - clipRect.top, BLACKNESS);

            const auto& border = ctx.border;
            // Паритет TF UxThemeHooks: theme-outline после black fill для любого cornerType.
            if (!Shared::MenuUtils::HandlePopupMenuNonClientBorderColors(hdc, clipRect, border))
            {
                const int savedDC = SaveDC(hdc);
                ExcludeClipRect(hdc,
                    clipRect.left + kMenuPopupBorderOutlinePx,
                    clipRect.top + kMenuPopupBorderOutlinePx,
                    clipRect.right - kMenuPopupBorderOutlinePx,
                    clipRect.bottom - kMenuPopupBorderOutlinePx);
                if (actualDTB)
                    actualDTB(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
                RestoreDC(hdc, savedDC);
            }
            return true;
        }

        // Всё остальное (BACKGROUND, BORDERS, GUTTER, обычные ITEM) — черним
        if (iPartId != MENU_POPUPBACKGROUND &&
            iPartId != MENU_POPUPBORDERS &&
            iPartId != MENU_POPUPGUTTER &&
            iPartId != MENU_POPUPITEM &&
            iPartId != MENU_POPUPITEMFOCUSABLE)
        {
            return false;
        }

        PatBlt(hdc, clipRect.left, clipRect.top,
            clipRect.right - clipRect.left, clipRect.bottom - clipRect.top, BLACKNESS);
        DebugLog(L"[GlassCore][Menu] DTB: BLACKNESS partId=%d stateId=%d", iPartId, iStateId);
        return true;
    }

    // -----------------------------------------------------------------------
    // Прочие функции (NC-border, bitmap, internal E8-хуки)
    // -----------------------------------------------------------------------

    bool HandlePopupDrawItem(void*, HDC, RECT*, int) { return false; }
    bool HandlePopupDrawItemCheck(void*, HDC, RECT*, int) { return false; }
    bool HandlePopupDrawClientArea(void*, HDC, RECT*) { return false; }
    bool HandlePopupDrawNonClientArea(void*, HDC, RECT*) { return false; }
}