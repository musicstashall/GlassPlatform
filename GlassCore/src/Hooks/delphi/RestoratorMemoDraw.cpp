// BWM RestoratorPaintHandler::Paint/Dc (window): erase + ANSI text + brush
// creation. Keep cached default FillRect brushes covered too; never change the
// application's DoubleBuffered/final StretchBlt or the shared text renderer.
#include "CorePch.h"
#include <Hooks/delphi/RestoratorMemoDraw.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Subsystems/RenderText.h>
#include <Module/ModuleHandler.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace GlassCore::RestoratorMemo
{
    namespace
    {
        thread_local bool g_inside = false;
        struct Session { HWND hwnd{}; bool paint{}, ready{}; };
        thread_local std::vector<Session> g_sessions;
        bool Diag(unsigned channel) noexcept
        {
            static thread_local ULONGLONG epoch[4]{};
            static thread_local unsigned budget[4]{};
            const auto now = GetTickCount64();
            if (!epoch[channel] || now - epoch[channel] >= 1000)
            { epoch[channel] = now; budget[channel] = 16; }
            if (!budget[channel]) return false;
            --budget[channel]; return true;
        }
        HWND ActiveHost() noexcept
        {
            if (g_inside || g_sessions.empty() || !g_sessions.back().ready
                || !GlassCore::Root::IsFullInjectionMode()) return nullptr;
            const HWND host = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            return host == g_sessions.back().hwnd && GlassCore::RestoratorMemo::IsWindow(host)
                && GlassShared::GlassIsBackdropAllowedForHwnd(host)
                && GlassCore::PaintDispatcher::IsTopWindowDrawingSession(host) ? host : nullptr;
        }
        HWND TargetDc(HDC dc) noexcept
        {
            const HWND host = ActiveHost();
            if (!host || !dc || GetMapMode(dc) != MM_TEXT) return nullptr;
            const HWND owner = WindowFromDC(dc);
            if (owner && owner != host) return nullptr;
            // An ambient HWND alone cannot authorize an unrelated/cached DC.
            return GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(host, dc)
                && (owner == host || (GetObjectType(dc) == OBJ_MEMDC
                    && GlassCore::PaintDispatcher::GetStackOwnerForDc(dc) == host)) ? host : nullptr;
        }
        bool SystemBackground(COLORREF color) noexcept
        {
            const auto rgb = color & 0xFFFFFFu;
            return rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
                || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
        }
        bool ValidRect(const RECT* rect) noexcept
        {
            if (!rect) return false;
            const auto w = std::int64_t(rect->right) - rect->left;
            const auto h = std::int64_t(rect->bottom) - rect->top;
            return w > 0 && h > 0 && w <= (std::numeric_limits<int>::max)()
                && h <= (std::numeric_limits<int>::max)();
        }
        bool Clear(HDC dc, const RECT& r) noexcept
        { return PatBlt(dc, r.left, r.top, r.right-r.left, r.bottom-r.top, BLACKNESS) != FALSE; }
        bool OpaqueColor(HDC dc, const RECT& r, COLORREF color)
        {
            wil::unique_hbrush brush{CreateSolidBrush(color)};
            if (!brush) return false;
            // The donor uses A255 for a non-default opaque text background.
            // Compose only this operation's rect, without a GDI pattern brush.
            return SUCCEEDED(GlassCore::Text::DrawThemeContent(dc, r, nullptr, nullptr, 0,
                [&](HDC memory, HPAINTBUFFER buffer, RGBQUAD*, int) {
                    THROW_HR_IF(E_FAIL, !::FillRect(memory, &r, brush.get()));
                    THROW_HR_IF(E_FAIL, !GdiFlush());
                    THROW_IF_FAILED(BufferedPaintSetAlpha(buffer, nullptr, 255));
                }, 255, false, true, false));
        }
        bool Decode(HDC dc, LPCSTR text, int count, std::wstring& wide, UINT& cp)
        {
            const int charset = GetTextCharset(dc);
            if (charset < 0 || charset == SYMBOL_CHARSET) return false;
            cp = CP_ACP;
            if (charset != DEFAULT_CHARSET)
            {
                CHARSETINFO info{};
                if (!TranslateCharsetInfo(reinterpret_cast<DWORD*>(static_cast<ULONG_PTR>(charset)),
                    &info, TCI_SRCCHARSET)) return false;
                cp = info.ciACP;
            }
            const int size = MultiByteToWideChar(cp, 0, text, count, nullptr, 0);
            if (size <= 0) return false;
            wide.resize(static_cast<size_t>(size));
            if (MultiByteToWideChar(cp, 0, text, count, wide.data(), size) != size) return false;
            if (count == -1) wide.pop_back();
            return !wide.empty();
        }
    }

    bool IsClassName(LPCWSTR name) noexcept
    { return name && (!_wcsicmp(name, L"TBomeMemo") || !_wcsicmp(name, L"TBMGutter")); }
    bool IsWindow(HWND hwnd) noexcept
    {
        wchar_t name[64]{}, path[MAX_PATH]{};
        if (!hwnd || !::IsWindow(hwnd) || !GetClassNameW(hwnd, name, _countof(name))
            || !IsClassName(name) || !GlassCore::ModuleHandler::g_entryPointModule) return false;
        const DWORD length = GetModuleFileNameW(GlassCore::ModuleHandler::g_entryPointModule,
            path, _countof(path));
        if (!length || length >= _countof(path)) return false;
        const wchar_t* base = wcsrchr(path, L'\\');
        return _wcsicmp(base ? base + 1 : path, L"Restorator.exe") == 0;
    }
    void BeginSession(HWND hwnd, bool paint, unsigned mask) noexcept
    {
        g_sessions.push_back({hwnd, paint, mask == kReadyMask});
        if (Diag(0))
        {
            wchar_t cls[64]{}; GetClassNameW(hwnd, cls, _countof(cls));
            DebugLog(DBG_NORMAL,
                L"[GlassCore][RestoratorMemo] hooks route=2 hwnd=0x%p class=%s channel=%s mask=0x%X ready=%d (Text=1 BrushIndirect=2 SysColor=4 Fill=8)",
                hwnd, cls, paint ? L"Paint" : L"GetDC", mask, mask == kReadyMask ? 1 : 0);
        }
    }
    bool EndSession(HWND hwnd, bool paint) noexcept
    {
        for (auto it = g_sessions.rbegin(); it != g_sessions.rend(); ++it)
        {
            if (it->hwnd != hwnd || it->paint != paint) continue;
            const bool ready = it->ready;
            g_sessions.erase((it + 1).base());
            return ready;
        }
        return false;
    }
    void ClearSessions() noexcept { g_sessions.clear(); }
    bool AfterBeginPaint(HWND hwnd, HDC dc, LPPAINTSTRUCT ps, HMODULE caller) noexcept
    {
        // Post providers run BEFORE PaintDispatcher pushes the returned DC.
        // Authenticate the pre-BeginPaint session here, not an ambient paint host.
        if (g_inside || g_sessions.empty() || !g_sessions.back().ready
            || !g_sessions.back().paint || g_sessions.back().hwnd != hwnd
            || !GlassCore::Root::IsFullInjectionMode() || !GlassCore::RestoratorMemo::IsWindow(hwnd)
            || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
            || caller != GlassCore::ModuleHandler::g_entryPointModule
            || !dc || !ps || !ValidRect(&ps->rcPaint)) return false;
        const bool ok = Clear(dc, ps->rcPaint);
        if (Diag(1)) DebugLog(DBG_NORMAL,
            L"[GlassCore][RestoratorMemo] erase hwnd=0x%p dc=0x%p rect=[%ld,%ld,%ld,%ld] ok=%d",
            hwnd, dc, ps->rcPaint.left, ps->rcPaint.top, ps->rcPaint.right, ps->rcPaint.bottom, ok ? 1 : 0);
        return ok; // PaintDispatcher resets fErase only on success.
    }
    DWORD SystemColor(decltype(&::GetSysColor) original, int index)
    {
        if (!original) return ::GetSysColor(index);
        if (ActiveHost() && (index == COLOR_WINDOW || index == COLOR_BTNFACE))
        {
            if (Diag(3)) DebugLog(DBG_NORMAL,
                L"[GlassCore][RestoratorMemo] syscolor index=%d default-to-black=1", index);
            return 0;
        }
        return original(index);
    }
    HBRUSH CreateBrush(decltype(&::CreateBrushIndirect) original, const LOGBRUSH* brush)
    {
        if (!original) return ::CreateBrushIndirect(brush);
        if (!ActiveHost() || !brush || brush->lbStyle != BS_SOLID || !SystemBackground(brush->lbColor))
            return original(brush);
        LOGBRUSH copy = *brush; copy.lbColor = 0;
        // Return a real caller-owned brush, not the donor's borrowed stock brush.
        const auto result = original(&copy);
        if (Diag(1)) DebugLog(DBG_NORMAL,
            L"[GlassCore][RestoratorMemo] brush rgb=0x%06X default=1 result=0x%p",
            brush->lbColor & 0xFFFFFFu, result);
        return result;
    }
    int Fill(decltype(&::FillRect) original, HDC dc, const RECT* rect, HBRUSH brush)
    {
        if (!original) return ::FillRect(dc, rect, brush);
        const HWND host = TargetDc(dc);
        if (!host || !ValidRect(rect)) return original(dc, rect, brush);
        LOGBRUSH lb{};
        const auto encoded = reinterpret_cast<ULONG_PTR>(brush);
        const bool systemBrush = encoded && encoded <= COLOR_MENUBAR + 1u;
        if (systemBrush && encoded != COLOR_WINDOW + 1u && encoded != COLOR_BTNFACE + 1u)
            return original(dc, rect, brush);
        const bool solid = systemBrush || (brush && GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb)
            && lb.lbStyle == BS_SOLID);
        const auto color = systemBrush ? GetSysColor(static_cast<int>(encoded - 1)) : lb.lbColor;
        // Black/custom fills stay native; this is not a black-pixel colour key.
        if (!solid || !SystemBackground(color)) return original(dc, rect, brush);
        g_inside = true; auto leave = wil::scope_exit([] { g_inside = false; });
        const bool ok = Clear(dc, *rect);
        if (Diag(1)) DebugLog(DBG_NORMAL,
            L"[GlassCore][RestoratorMemo] fill hwnd=0x%p dc=0x%p rgb=0x%06X ok=%d",
            host, dc, color & 0xFFFFFFu, ok ? 1 : 0);
        return ok ? 1 : original(dc, rect, brush);
    }
    int DrawAnsi(decltype(&::DrawTextA) original, HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
    {
        if (!original) return ::DrawTextA(dc, text, count, rect, flags);
        const auto native = [&] { return original(dc, text, count, rect, flags); };
        const HWND host = TargetDc(dc);
        if (!host)
        {
            if (ActiveHost() && Diag(2)) DebugLog(DBG_NORMAL,
                L"[GlassCore][RestoratorMemo] text-skip dc=0x%p dcWnd=0x%p stackOwner=0x%p reason=unowned-or-mapped-dc",
                dc, WindowFromDC(dc), GlassCore::PaintDispatcher::GetStackOwnerForDc(dc));
            return native();
        }
        const auto nativeReason = [&](LPCWSTR reason) {
            if (Diag(2)) DebugLog(DBG_NORMAL,
                L"[GlassCore][RestoratorMemo] text-native hwnd=0x%p dc=0x%p ansiCount=%d flags=0x%X reason=%s",
                host, dc, count, flags, reason);
            return native();
        };
        if (!text || count == 0 || count < -1 || !ValidRect(rect)
            || (flags & (DT_CALCRECT | DT_MODIFYSTRING | DT_NOCLIP))) return nativeReason(L"arguments-or-flags");
        std::wstring wide; UINT cp{}; bool decoded = false;
        try { decoded = Decode(dc, text, count, wide, cp); }
        catch (...) { return nativeReason(L"conversion-exception"); }
        if (!decoded) return nativeReason(L"conversion");
        const int saved = SaveDC(dc);
        if (!saved) return nativeReason(L"SaveDC");
        // Restore state before invoking the ANSI original. This cannot undo
        // preparation pixels: native fallback is a repaint, not a pixel rollback.
        bool restored = false;
        const auto restoreNow = [&] { if (!restored) { RestoreDC(dc, saved); restored = true; } };
        auto restore = wil::scope_exit(restoreNow);
        const auto fallback = [&](LPCWSTR reason) { restoreNow(); return nativeReason(reason); };
        const auto background = GetBkColor(dc);
        const bool opaque = GetBkMode(dc) == OPAQUE;
        g_inside = true; auto leave = wil::scope_exit([] { g_inside = false; });
        bool prepared = !opaque;
        // BWM's special zero/WINDOW/BTNFACE rule belongs only to this opaque
        // text operation, never to arbitrary pixels, icons or a final bitmap.
        try { if (opaque) prepared = !(background & 0xFFFFFFu) || SystemBackground(background)
            ? Clear(dc, *rect) : OpaqueColor(dc, *rect, background); }
        catch (...) { return fallback(L"background-exception"); }
        if (!prepared || !SetBkMode(dc, TRANSPARENT)) return fallback(L"background-or-mode");
        RECT drawRect = *rect; int result = 0; HRESULT hr = E_FAIL;
        try { hr = GlassCore::Text::DrawTextWithAlpha(dc, wide.data(), static_cast<int>(wide.size()), &drawRect, flags, result, true); }
        catch (...) { hr = E_FAIL; }
        if (Diag(2))
        {
            BITMAP bitmap{};
            const HGDIOBJ selected = GetCurrentObject(dc, OBJ_BITMAP);
            const bool known = selected && GetObjectW(selected, sizeof(bitmap), &bitmap) == sizeof(bitmap);
            // Observational only: no guessed 32bpp prerequisite or pixel colour key.
            DebugLog(DBG_NORMAL,
                L"[GlassCore][RestoratorMemo] text hwnd=0x%p dc=0x%p dcType=%lu bitmap=0x%p bpp=%u size=[%ld,%ld] ansiCount=%d wideCount=%d cp=%u flags=0x%X rect=[%ld,%ld,%ld,%ld] bk=0x%06X opaque=%d hr=0x%08X result=%d",
                host, dc, GetObjectType(dc), selected,
                known ? unsigned(bitmap.bmBitsPixel) * bitmap.bmPlanes : 0u,
                bitmap.bmWidth, bitmap.bmHeight, count, static_cast<int>(wide.size()), cp, flags,
                rect->left, rect->top, rect->right, rect->bottom,
                background & 0xFFFFFFu, opaque ? 1 : 0, static_cast<unsigned>(hr), result);
        }
        return SUCCEEDED(hr) ? result : fallback(L"renderer");
    }
}
