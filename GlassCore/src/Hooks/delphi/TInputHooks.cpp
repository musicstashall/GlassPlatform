// Narrow VCL-specific policies; common Edit paint/session infrastructure stays shared.
#include "CorePch.h"
#include <Hooks/delphi/TInputHooks.h>
#include <Subsystems/PaintDispatcher.h>
#include <GlassDelphiInputBackground.h>
#include <DebugLog.h>

namespace GlassCore::DelphiInput
{
    static bool HasClass(HWND hwnd, const wchar_t* name) noexcept
    {
        WCHAR cls[64]{};
        return hwnd && GetClassNameW(hwnd, cls, _countof(cls)) && _wcsicmp(cls, name) == 0;
    }
    bool IsSpinEdit(HWND hwnd) noexcept { return HasClass(hwnd, L"TSpinEdit"); }
    bool IsNumberBox(HWND hwnd) noexcept { return HasClass(hwnd, L"TNumberBox"); }
    bool IsHotKey(HWND hwnd) noexcept { return HasClass(hwnd, L"THotKey"); }

    bool ClearNumberBoxGlyphBackground(HWND hwnd, HDC dc, const RECT* rect, UINT options) noexcept
    {
        if (!IsNumberBox(hwnd) || !GlassShared::HasDelphiInputDefaultBackground(hwnd)
            || GetFocus() == hwnd || !dc || !rect || (GetWindowLongW(hwnd, GWL_STYLE) & ES_NOHIDESEL)
            || (options & (ETO_GLYPH_INDEX | ETO_CLIPPED | ETO_OPAQUE)) != (ETO_GLYPH_INDEX | ETO_CLIPPED)
            || !GlassCore::PaintDispatcher::IsCurrentPaintDcForHwnd(hwnd, dc)) return false;
        RECT client{}, clip{};
        if (!GetClientRect(hwnd, &client) || IsRectEmpty(&client) || !EqualRect(&client, rect)
            || GetClipBox(dc, &clip) <= NULLREGION) return false;
        const BOOL result = PatBlt(dc, client.left, client.top,
            client.right - client.left, client.bottom - client.top, BLACKNESS);
#if defined(_DEBUG)
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
        if (budget) { --budget; DebugLog(DBG_NORMAL, L"[GlassCore][DelphiInput] NumberBox glyph background hwnd=0x%p dc=0x%p result=%d", hwnd, dc, result); }
#endif
        return result != FALSE;
    }
    bool ClearHotKeyBackground(HWND hwnd, HDC dc, const RECT* rect, HBRUSH brush) noexcept
    {
        if (!IsHotKey(hwnd) || !dc || !rect || !brush
            || !GlassCore::PaintDispatcher::IsCurrentPaintDcForHwnd(hwnd, dc)) return false;
        RECT client{}, overlap{};
        LOGBRUSH lb{};
        if (!GetClientRect(hwnd, &client) || IsRectEmpty(&client)
            || !IntersectRect(&overlap, &client, rect) || !EqualRect(&overlap, rect)
            || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID
            || ((lb.lbColor & 0xFFFFFFu) != (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
                && (lb.lbColor & 0xFFFFFFu) != (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu))) return false;
        const BOOL result = PatBlt(dc, rect->left, rect->top,
            rect->right - rect->left, rect->bottom - rect->top, BLACKNESS);
#if defined(_DEBUG)
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
        if (budget) { --budget; DebugLog(DBG_NORMAL, L"[GlassCore][DelphiInput] HotKey FillRect hwnd=0x%p dc=0x%p rect=[%ld,%ld,%ld,%ld] result=%d", hwnd, dc, rect->left, rect->top, rect->right, rect->bottom, result); }
#endif
        return result != FALSE;
    }
}
