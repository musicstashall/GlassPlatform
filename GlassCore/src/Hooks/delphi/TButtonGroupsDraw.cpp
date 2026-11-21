// Exact VCL TButtonGroup/TCategoryButtons. Keep native layout, brushes and
// gradient interpolation; only compose their client fills/text with real alpha.
#include "CorePch.h"
#include <Hooks/delphi/TButtonGroupsDraw.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Subsystems/RenderText.h>
#include <GlassDelphiButtonGroups.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
#include <uxtheme.h>

namespace GlassCore::DelphiButtonGroups
{
    namespace
    {
        thread_local bool g_inside = false;
        constexpr BYTE kBackgroundAlpha = 100;

        bool TakeDiag(size_t channel) noexcept
        {
            static thread_local ULONGLONG epoch[3]{};
            static thread_local unsigned budget[3]{};
            const ULONGLONG now = GetTickCount64();
            if (!epoch[channel] || now - epoch[channel] >= 1000)
            { epoch[channel] = now; budget[channel] = 16; }
            if (!budget[channel]) return false;
            --budget[channel]; return true;
        }
        HWND TargetWindow(HDC dc) noexcept
        {
            if (g_inside || !dc || !GlassCore::Root::IsFullInjectionMode() || GetMapMode(dc) != MM_TEXT)
                return nullptr;
            HWND hwnd = WindowFromDC(dc);
            if (!hwnd && GetObjectType(dc) == OBJ_MEMDC)
                hwnd = GlassCore::PaintDispatcher::GetStackOwnerForDc(dc);
            // A thread's ambient paint host alone does not establish DC ownership.
            if (!IsWindow(hwnd) || !GlassShared::IsDelphiButtonGroupsWindow(hwnd)
                || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
                || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(hwnd)) return nullptr;
            return hwnd;
        }
        bool IsClientFill(HWND hwnd, const RECT* rect) noexcept
        {
            RECT client{};
            if (!rect || IsRectEmpty(rect) || !GetClientRect(hwnd, &client)) return false;
            const UINT dpi = GetDpiForWindow(hwnd);
            const int ex = GetSystemMetricsForDpi(SM_CXEDGE, dpi ? dpi : 96);
            const int ey = GetSystemMetricsForDpi(SM_CYEDGE, dpi ? dpi : 96);
            // Do not recolour native outline strips or custom insertion markers.
            // Compare full operation bounds, not the potentially tiny repaint clip.
            return rect->left >= client.left && rect->top >= client.top
                && rect->right <= client.right && rect->bottom <= client.bottom
                && rect->right - rect->left > ex && rect->bottom - rect->top > ey;
        }
        bool SolidColor(HBRUSH brush, COLORREF& rgb) noexcept
        {
            const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(brush);
            if (value && value <= COLOR_MENUBAR + 1u)
            { rgb = GetSysColor(static_cast<int>(value - 1)) & 0xFFFFFFu; return true; }
            LOGBRUSH lb{};
            if (!brush || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID) return false;
            rgb = lb.lbColor & 0xFFFFFFu; return true;
        }
        BOOL Blacken(HDC dc, const RECT& rect) noexcept
        {
            return PatBlt(dc, rect.left, rect.top, rect.right - rect.left,
                rect.bottom - rect.top, BLACKNESS);
        }
        template <typename Draw>
        bool ComposeBackground(HDC dc, const RECT& rect, Draw draw)
        {
            // GradientFill ignores TRIVERTEX.Alpha. Render its actual RGB using
            // native interpolation, then establish opaque premultiplied pixels
            // and compose with constant A100. Black is a valid gradient colour,
            // not a colour key. No reconstruction/guessing of category colours.
            const HRESULT hr = GlassCore::Text::DrawThemeContent(dc, rect, nullptr, nullptr, 0,
                [&](HDC memory, HPAINTBUFFER buffer, RGBQUAD*, int) {
                    THROW_HR_IF(E_FAIL, !draw(memory));
                    THROW_HR_IF(E_FAIL, !GdiFlush());
                    THROW_IF_FAILED(BufferedPaintSetAlpha(buffer, nullptr, 255));
                    // Only clear after the buffer is ready; every repaint starts
                    // from zero rather than accumulating alpha over the last one.
                    THROW_HR_IF(E_FAIL, !Blacken(dc, rect));
                }, kBackgroundAlpha, false, true, false);
            return SUCCEEDED(hr);
        }
    }

    int DrawCaptionText(decltype(&::DrawTextExW) original, HDC dc, LPWSTR text, int count,
        LPRECT rect, UINT flags, LPDRAWTEXTPARAMS params)
    {
        if (!original) return ::DrawTextExW(dc, text, count, rect, flags, params);
        const HWND hwnd = TargetWindow(dc);
        if (!hwnd || !rect || IsRectEmpty(rect) || !text || count == 0 || params
            || (flags & (DT_CALCRECT | DT_MODIFYSTRING)))
            return original(dc, text, count, rect, flags, params);
        int result = 0;
        HRESULT hr = E_FAIL;
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            // Transparent buffer keeps the category gradient beneath the caption.
            hr = GlassCore::Text::DrawTextWithAlpha(dc, text, count, rect, flags, result, true);
        }
        if (TakeDiag(0))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiButtonGroups] text hwnd=0x%p dc=0x%p format=0x%X count=%d hr=0x%08X",
                hwnd, dc, flags, count, static_cast<unsigned>(hr));
        return SUCCEEDED(hr) ? result : original(dc, text, count, rect, flags, params);
    }

    int Fill(decltype(&::FillRect) original, HDC dc, const RECT* rect, HBRUSH brush)
    {
        if (!original) return ::FillRect(dc, rect, brush);
        const HWND hwnd = TargetWindow(dc);
        COLORREF rgb{};
        if (!hwnd || !IsClientFill(hwnd, rect) || !SolidColor(brush, rgb)) return original(dc, rect, brush);
        const bool system = rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
            || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
        bool drawn = false;
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            drawn = system ? Blacken(dc, *rect) != FALSE
                : ComposeBackground(dc, *rect, [&](HDC memory) { return original(memory, rect, brush); });
        }
        if (TakeDiag(1))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiButtonGroups] fill hwnd=0x%p dc=0x%p rgb=0x%06X alpha=%u rect=[%ld,%ld,%ld,%ld] ok=%d",
                hwnd, dc, rgb, system ? 0u : unsigned(kBackgroundAlpha),
                rect->left, rect->top, rect->right, rect->bottom, drawn ? 1 : 0);
        return drawn ? 1 : original(dc, rect, brush);
    }

    BOOL Gradient(decltype(&::GdiGradientFill) original, HDC dc, PTRIVERTEX vertices,
        ULONG vertexCount, PVOID mesh, ULONG meshCount, ULONG mode, LPCWSTR api)
    {
        if (!original) return ::GdiGradientFill(dc, vertices, vertexCount, mesh, meshCount, mode);
        const auto native = [&] { return original(dc, vertices, vertexCount, mesh, meshCount, mode); };
        const HWND hwnd = TargetWindow(dc);
        // VCL GraphUtil.GradientFillCanvas: exactly two vertices and one rectangle.
        // Leave triangles, arbitrary meshes and invalid/unknown API input native.
        if (!hwnd || !vertices || !mesh || vertexCount != 2 || meshCount != 1
            || (mode != GRADIENT_FILL_RECT_H && mode != GRADIENT_FILL_RECT_V)) return native();
        const auto& pair = *static_cast<const GRADIENT_RECT*>(mesh);
        if (pair.UpperLeft >= vertexCount || pair.LowerRight >= vertexCount
            || pair.UpperLeft == pair.LowerRight) return native();
        const auto& first = vertices[pair.UpperLeft];
        const auto& last = vertices[pair.LowerRight];
        const RECT rect{first.x, first.y, last.x, last.y};
        if (!IsClientFill(hwnd, &rect)) return native();
        bool drawn = false;
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            drawn = ComposeBackground(dc, rect, [&](HDC memory) {
                return original(memory, vertices, vertexCount, mesh, meshCount, mode);
            });
        }
        if (TakeDiag(2))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiButtonGroups] gradient api=%s hwnd=0x%p dc=0x%p mode=%lu rgb=[0x%06X,0x%06X] alpha=%u rect=[%ld,%ld,%ld,%ld] ok=%d",
                api, hwnd, dc, mode,
                RGB(first.Red >> 8, first.Green >> 8, first.Blue >> 8),
                RGB(last.Red >> 8, last.Green >> 8, last.Blue >> 8), unsigned(kBackgroundAlpha),
                rect.left, rect.top, rect.right, rect.bottom, drawn ? 1 : 0);
        return drawn ? TRUE : native();
    }
}
