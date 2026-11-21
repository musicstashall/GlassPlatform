// TBomeMemo/TBMGutter (Restorator API-Monitor-6): EXE DrawTextA + row FillRect.
// The memo uses a compatible bitmap and native StretchBlt; the gutter paints
// directly. Do not alias either class to Edit or change the shared text renderer.
#include "CorePch.h"
#include <Hooks/delphi/TBomeMemoDraw.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Subsystems/RenderText.h>
#include <GlassBomeMemo.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
#include <cstdint>
#include <limits>
#include <string>

namespace GlassCore::DelphiBomeMemo
{
    namespace
    {
        thread_local bool g_inside = false;

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
        HWND PaintWindow(HDC dc) noexcept
        {
            if (g_inside || !dc || !GlassCore::Root::IsFullInjectionMode()
                || GetMapMode(dc) != MM_TEXT || GetGraphicsMode(dc) != GM_COMPATIBLE
                || GetLayout(dc) != 0) return nullptr;
            POINT viewport{}, window{};
            if (!GetViewportOrgEx(dc, &viewport) || !GetWindowOrgEx(dc, &window)
                || viewport.x || viewport.y || window.x || window.y) return nullptr;

            HWND hwnd = WindowFromDC(dc);
            const bool memory = !hwnd && GetObjectType(dc) == OBJ_MEMDC;
            if (memory) hwnd = GlassCore::PaintDispatcher::GetStackOwnerForDc(dc);
            if (!IsWindow(hwnd) || !GlassShared::IsBomeMemoWindow(hwnd)
                || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
                || !GlassCore::PaintDispatcher::HasOpenPaintSessionForHwnd(hwnd)
                || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(hwnd)) return nullptr;
            // Exclude NC/GetWindowDC and unrelated memory DCs even for this HWND.
            if (!memory)
                return GlassCore::PaintDispatcher::IsCurrentPaintDcForHwnd(hwnd, dc) ? hwnd : nullptr;
            if (!GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(hwnd, dc)) return nullptr;
            BITMAP bitmap{};
            const HGDIOBJ selected = GetCurrentObject(dc, OBJ_BITMAP);
            if (!selected || GetObjectW(selected, sizeof(bitmap), &bitmap) != sizeof(bitmap)
                || bitmap.bmPlanes != 1 || bitmap.bmBitsPixel != 32
                || bitmap.bmWidth <= 0 || bitmap.bmHeight <= 0)
            {
                if (TakeDiag(2))
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][BomeMemo] UnsupportedBitmap hwnd=0x%p dc=0x%p bitmap=0x%p size=%ldx%ld planes=%u bpp=%u",
                        hwnd, dc, selected, bitmap.bmWidth, bitmap.bmHeight,
                        unsigned(bitmap.bmPlanes), unsigned(bitmap.bmBitsPixel));
                return nullptr;
            }
            return hwnd;
        }
        bool DefaultBackground(HWND hwnd, COLORREF color) noexcept
        {
            const COLORREF rgb = color & 0xFFFFFFu;
            if (rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)) return true;
            wchar_t name[64]{};
            return GetClassNameW(hwnd, name, static_cast<int>(_countof(name))) > 0
                && _wcsicmp(name, L"TBMGutter") == 0
                && rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
        }
        bool ValidRect(const RECT* rect) noexcept
        {
            if (!rect) return false;
            const auto w = std::int64_t(rect->right) - rect->left;
            const auto h = std::int64_t(rect->bottom) - rect->top;
            return w > 0 && h > 0 && w <= (std::numeric_limits<int>::max)()
                && h <= (std::numeric_limits<int>::max)();
        }
        bool Blacken(HDC dc, const RECT& rect) noexcept
        {
            return PatBlt(dc, rect.left, rect.top,
                rect.right - rect.left, rect.bottom - rect.top, BLACKNESS) != FALSE;
        }
        bool SolidColor(HBRUSH brush, COLORREF& color) noexcept
        {
            const ULONG_PTR encoded = reinterpret_cast<ULONG_PTR>(brush);
            if (encoded && encoded <= COLOR_MENUBAR + 1u)
            { color = GetSysColor(static_cast<int>(encoded - 1)); return true; }
            LOGBRUSH lb{};
            if (!brush || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID) return false;
            color = lb.lbColor & 0xFFFFFFu; return true;
        }
        bool TextCodePage(HDC dc, UINT& codePage) noexcept
        {
            const int charset = GetTextCharset(dc);
            // Symbol-font ANSI mappings are not equivalent to Unicode DrawText.
            if (charset == SYMBOL_CHARSET || charset < 0) return false;
            if (charset == DEFAULT_CHARSET) { codePage = CP_ACP; return true; }
            CHARSETINFO info{};
            if (!TranslateCharsetInfo(reinterpret_cast<DWORD*>(static_cast<ULONG_PTR>(charset)),
                &info, TCI_SRCCHARSET)) return false;
            codePage = info.ciACP; return true;
        }
    }

    int DrawAnsiText(decltype(&::DrawTextA) original, HDC dc, LPCSTR text,
        int count, LPRECT rect, UINT flags)
    {
        if (!original) return ::DrawTextA(dc, text, count, rect, flags);
        const auto native = [&] { return original(dc, text, count, rect, flags); };
        // Only captured single-line rows. Measurements, ANSI mutations and
        // unclipped/multiline layout keep their native buffer/rectangle semantics.
        if (!text || count == 0 || count < -1 || !ValidRect(rect)
            || !(flags & DT_SINGLELINE)
            || (flags & (DT_CALCRECT | DT_MODIFYSTRING | DT_NOCLIP))) return native();
        const HWND hwnd = PaintWindow(dc);
        if (!hwnd) return native();
        const bool opaque = GetBkMode(dc) == OPAQUE;
        const COLORREF background = GetBkColor(dc);
        // Unknown custom/selection backgrounds stay native; no selection classifier
        // or new A100 policy is inferred from this normal-state capture.
        if (opaque && !DefaultBackground(hwnd, background)) return native();
        UINT codePage{};
        if (!TextCodePage(dc, codePage)) return native();

        std::wstring wide;
        int wideCount = 0;
        try
        {
            const int size = MultiByteToWideChar(codePage, 0, text, count, nullptr, 0);
            if (size > 0)
            {
                wide.resize(static_cast<size_t>(size));
                if (MultiByteToWideChar(codePage, 0, text, count, wide.data(), size) == size)
                    wideCount = count == -1 ? size - 1 : size;
            }
        }
        catch (...) { return native(); }
        if (!wideCount) return native();

        const int saved = SaveDC(dc);
        if (!saved) return native();
        auto restore = wil::scope_exit([&]() noexcept { RestoreDC(dc, saved); });
        g_inside = true;
        auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
        if (!SetBkMode(dc, TRANSPARENT)) return native();
        if (opaque && !Blacken(dc, *rect))
        {
            SetBkMode(dc, OPAQUE);
            return native();
        }
        RECT drawRect = *rect;
        int result = 0;
        HRESULT hr = E_FAIL;
        try
        {
            // The renderer's last-resort plain GDI path must also be transparent.
            hr = GlassCore::Text::DrawTextWithAlpha(
                dc, wide.data(), wideCount, &drawRect, flags, result, true);
        }
        catch (...) { hr = E_FAIL; }
        if (TakeDiag(0))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][BomeMemo] text hwnd=0x%p dc=0x%p ansiCount=%d wideCount=%d codePage=%u flags=0x%X opaque=%d bk=0x%06X hr=0x%08X result=%d",
                hwnd, dc, count, wideCount, codePage, flags, opaque ? 1 : 0,
                background & 0xFFFFFFu, static_cast<unsigned>(hr), result);
        // A cleared system-default row is already the intended Glass background.
        // On a render failure draw native ANSI glyphs over it, not an opaque box.
        // This does not promise transactional pixel rollback after publication.
        return SUCCEEDED(hr) ? result : native();
    }

    int FillBackground(decltype(&::FillRect) original, HDC dc, const RECT* rect, HBRUSH brush)
    {
        if (!original) return ::FillRect(dc, rect, brush);
        const HWND hwnd = PaintWindow(dc);
        COLORREF color{};
        if (!hwnd || !ValidRect(rect) || !SolidColor(brush, color) || !DefaultBackground(hwnd, color))
            return original(dc, rect, brush);
        bool ok = false;
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            ok = Blacken(dc, *rect);
        }
        if (TakeDiag(1))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][BomeMemo] fill hwnd=0x%p dc=0x%p rgb=0x%06X alpha=0 rect=[%ld,%ld,%ld,%ld] ok=%d",
                hwnd, dc, color & 0xFFFFFFu, rect->left, rect->top, rect->right, rect->bottom, ok ? 1 : 0);
        return ok ? 1 : original(dc, rect, brush);
    }
}
