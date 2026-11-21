// VCL panels/frames: preserve native erase so the actual background brush is
// available. Only client-background fills are replaced; bevels and other
// controls' DCs are not recoloured. The root themed-text slot also shares this
// session/DC gate and the existing transparent compositor. No alpha pattern brushes.
#include "CorePch.h"
#include <Hooks/delphi/TPanelDraw.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassDelphiPanel.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
#include <array>
#include <atomic>
#include <tuple>
#include <vector>
#include <limits>

#pragma comment(lib, "msimg32.lib")

namespace GlassCore::DelphiPanel
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;
        enum : size_t { FillSlot, PatSlot, ExtSlot, Count };
        int WINAPI PanelFill(HDC, const RECT*, HBRUSH);
        BOOL WINAPI PanelPat(HDC, int, int, int, int, DWORD);
        BOOL WINAPI PanelExt(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        const HookDispatcherDependency<Count, 3> g_dep{{
            std::tuple{std::array{"user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv},
                "FillRect", reinterpret_cast<PVOID>(PanelFill)},
            std::tuple{std::array{"gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv},
                "PatBlt", reinterpret_cast<PVOID>(PanelPat)},
            std::tuple{std::array{"gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv},
                "ExtTextOutW", reinterpret_cast<PVOID>(PanelExt)},
        }};
        // One known source, EXE (as in LogViewer-16). The dispatcher, not a
        // return address in an IAT trampoline, supplies the correct original.
        HookDispatcher<Count, 3> g_dispatcher{g_dep, nullptr};
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        struct Session
        {
            HWND hwnd{};
            UINT message{};
            HDC suppliedDc{};
            std::array<bool, Count> acquired{};
        };
        thread_local std::vector<Session> g_stack;
        thread_local bool g_inHook = false;
        constexpr BYTE kCustomBackgroundAlpha = 100;

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
        template <size_t slot, typename Fn> Fn Original() noexcept
        {
            auto lock = g_lock.lock_shared();
            return g_dispatcher.template GetOrg<slot, Fn>();
        }
        bool IsPanelDc(HDC dc) noexcept
        {
            if (!dc || g_stack.empty() || g_shutdown.load(std::memory_order_acquire)
                || !GlassCore::Root::IsFullInjectionMode()) return false;
            const auto& s = g_stack.back();
            if (!IsWindow(s.hwnd) || !GlassShared::GlassIsBackdropAllowedForHwnd(s.hwnd)) return false;
            if (s.suppliedDc && dc == s.suppliedDc) return true;
            if (const HWND owner = WindowFromDC(dc)) return owner == s.hwnd;
            return GetObjectType(dc) == OBJ_MEMDC
                && GlassCore::PaintDispatcher::IsTopWindowDrawingSession(s.hwnd)
                && GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(s.hwnd, dc);
        }
        bool IsBackgroundRect(HDC dc, const RECT* r) noexcept
        {
            if (!r || IsRectEmpty(r) || !IsPanelDc(dc)) return false;
            RECT client{};
            if (!GetClientRect(g_stack.back().hwnd, &client) || IsRectEmpty(&client)) return false;
            // TPanel fills inside its bevel; tiny chrome strips must keep their
            // original colours/opacity. A partial repaint clips the *full* fill.
            const UINT dpi = GetDpiForWindow(g_stack.back().hwnd);
            const int ex = GetSystemMetricsForDpi(SM_CXEDGE, dpi ? dpi : 96);
            const int ey = GetSystemMetricsForDpi(SM_CYEDGE, dpi ? dpi : 96);
            return r->left >= client.left && r->top >= client.top
                && r->right <= client.right && r->bottom <= client.bottom
                && r->left - client.left <= ex && r->top - client.top <= ey
                && client.right - r->right <= ex && client.bottom - r->bottom <= ey;
        }
        bool SolidBrushColor(HBRUSH brush, COLORREF& color) noexcept
        {
            // FillRect also accepts (COLOR_* + 1), which is not an HBRUSH.
            const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(brush);
            if (value && value <= COLOR_MENUBAR + 1u)
            { color = GetSysColor(static_cast<int>(value - 1)); return true; }
            LOGBRUSH lb{};
            if (!brush || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID) return false;
            color = lb.lbColor & 0xFFFFFFu; return true;
        }
        bool Blacken(HDC dc, const RECT& r) noexcept
        { return PatBlt(dc, r.left, r.top, r.right-r.left, r.bottom-r.top, BLACKNESS) != FALSE; }

        // Same GDI recipe as the existing grid colour fills: 1x1 premultiplied
        // BGRA DIB + AC_SRC_ALPHA. Clear first on every repaint, so alpha does
        // not accumulate and an old opaque client background cannot survive.
        bool ApplyBackground(HDC dc, const RECT& r, COLORREF color, const wchar_t* api) noexcept
        {
            const COLORREF rgb = color & 0xFFFFFFu;
            const bool system = rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
                || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
            bool ok = false;
            if (system) ok = Blacken(dc, r);
            else
            {
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = 1; bi.bmiHeader.biHeight = -1;
                bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                HDC mem = CreateCompatibleDC(dc);
                if (!mem) return false;
                void* bits = nullptr;
                HBITMAP bitmap = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (bitmap && bits)
                {
                    const HGDIOBJ old = SelectObject(mem, bitmap);
                    if (old && old != HGDI_ERROR)
                    {
                        const auto premul = [](BYTE c) noexcept {
                            return static_cast<BYTE>((static_cast<unsigned>(c) * kCustomBackgroundAlpha + 127) / 255);
                        };
                        auto* pixel = static_cast<BYTE*>(bits);
                        pixel[0] = premul(GetBValue(rgb)); pixel[1] = premul(GetGValue(rgb));
                        pixel[2] = premul(GetRValue(rgb)); pixel[3] = kCustomBackgroundAlpha;
                        const BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                        ok = Blacken(dc, r) && GdiAlphaBlend(dc, r.left, r.top, r.right-r.left, r.bottom-r.top,
                            mem, 0, 0, 1, 1, blend) != FALSE;
                        SelectObject(mem, old);
                    }
                }
                if (bitmap) DeleteObject(bitmap);
                DeleteDC(mem);
            }
            if (TakeDiag(1))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiPanel] background api=%s hwnd=0x%p dc=0x%p color=0x%06X alpha=%u rect=[%ld,%ld,%ld,%ld] ok=%d",
                    api, g_stack.back().hwnd, dc, rgb, system ? 0u : unsigned(kCustomBackgroundAlpha),
                    r.left, r.top, r.right, r.bottom, ok ? 1 : 0);
            return ok;
        }
        int WINAPI PanelFill(HDC dc, const RECT* r, HBRUSH brush)
        {
            const auto orig = Original<FillSlot, decltype(&::FillRect)>();
            if (!orig) return FillRect(dc, r, brush);
            COLORREF color{};
            if (g_inHook || !IsBackgroundRect(dc, r) || !SolidBrushColor(brush, color)) return orig(dc, r, brush);
            g_inHook = true;
            auto leave = wil::scope_exit([]() noexcept { g_inHook = false; });
            return ApplyBackground(dc, *r, color, L"FillRect") ? 1 : orig(dc, r, brush);
        }
        BOOL WINAPI PanelPat(HDC dc, int x, int y, int w, int h, DWORD rop)
        {
            const auto orig = Original<PatSlot, decltype(&::PatBlt)>();
            if (!orig) return PatBlt(dc, x, y, w, h, rop);
            if (g_inHook || rop != PATCOPY || x < 0 || y < 0 || w <= 0 || h <= 0
                || w > (std::numeric_limits<int>::max)() - x
                || h > (std::numeric_limits<int>::max)() - y)
                return orig(dc, x, y, w, h, rop);
            const RECT r{x, y, x+w, y+h};
            COLORREF color{};
            if (!IsBackgroundRect(dc, &r)
                || !SolidBrushColor(static_cast<HBRUSH>(GetCurrentObject(dc, OBJ_BRUSH)), color))
                return orig(dc, x, y, w, h, rop);
            g_inHook = true;
            auto leave = wil::scope_exit([]() noexcept { g_inHook = false; });
            return ApplyBackground(dc, r, color, L"PatBlt") ? TRUE : orig(dc, x, y, w, h, rop);
        }
        BOOL WINAPI PanelExt(HDC dc, int x, int y, UINT options, const RECT* r,
            LPCWSTR text, UINT count, const INT* dx)
        {
            const auto orig = Original<ExtSlot, decltype(&::ExtTextOutW)>();
            if (!orig) return ExtTextOutW(dc, x, y, options, r, text, count, dx);
            if (g_inHook || !(options & ETO_OPAQUE) || count || !IsBackgroundRect(dc, r))
                return orig(dc, x, y, options, r, text, count, dx);
            g_inHook = true;
            auto leave = wil::scope_exit([]() noexcept { g_inHook = false; });
            return ApplyBackground(dc, *r, GetBkColor(dc), L"ExtTextOutW") ? TRUE
                : orig(dc, x, y, options, r, text, count, dx);
        }
    }

    HRESULT DrawContainerThemeTextEx(decltype(&::DrawThemeTextEx) original,
        HTHEME theme, HDC dc, int part, int state, LPCWSTR text, int count,
        DWORD flags, LPRECT rect, const DTTOPTS* options)
    {
        if (!original) return ::DrawThemeTextEx(theme, dc, part, state, text, count, flags, rect, options);
        const HWND dcWindow = dc ? WindowFromDC(dc) : nullptr;
        const HWND owner = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
        const bool panelDc = IsPanelDc(dc);
        const HWND hwnd = panelDc ? g_stack.back().hwnd : dcWindow;
        wchar_t name[64]{};
        if (hwnd) GetClassNameW(hwnd, name, static_cast<int>(_countof(name)));
        const bool container = GlassShared::IsDelphiPanelClassName(name)
            || _wcsicmp(name, L"TGroupBox") == 0 || _wcsicmp(name, L"TTabSheet") == 0
            || _wcsicmp(name, L"TCard") == 0 || _wcsicmp(name, L"TRadioGroup") == 0;
        // Probe: EXE!DrawThemeTextEx(Edit, 1, 1) for TCard/frames; Button,
        // BP_GROUPBOX for TRadioGroup. Neither needs a new background policy.
        // Keep measurements, callbacks, arbitrary DCs and all caller options native.
        if (g_shutdown.load(std::memory_order_acquire) || g_inHook
            || !GlassCore::Root::IsFullInjectionMode() || !hwnd || !container
            || (!panelDc && hwnd != owner) || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
            || !rect || IsRectEmpty(rect) || !text || count == 0
            || (flags & (DT_CALCRECT | DT_MODIFYSTRING))
            || (options && (options->dwSize != sizeof(DTTOPTS)
                || (options->dwFlags & (DTT_CALCRECT | DTT_CALLBACK)))))
            return original(theme, dc, part, state, text, count, flags, rect, options);

        DTTOPTS work{};
        if (options) work = *options;
        else work.dwSize = sizeof(DTTOPTS);
        work.dwFlags |= DTT_COMPOSITED;
        RECT drawRect = *rect;
        HRESULT drawn = E_FAIL;
        HRESULT buffered = E_FAIL;
        {
            g_inHook = true;
            auto leave = wil::scope_exit([]() noexcept { g_inHook = false; });
            // Reuse the existing VCL compositor: clear transparent buffer, let
            // uxtheme render the exact font/colour/glow, then alpha-compose it.
            // No ETO/glyph interception inside gdi32full and no background erase.
            buffered = GlassCore::Text::DrawThemeContent(dc, *rect, nullptr, nullptr, 0,
                [&](HDC memoryDc, HPAINTBUFFER, RGBQUAD*, int) {
                    drawn = original(theme, memoryDc, part, state, text, count, flags, &drawRect, &work);
                }, 0xFF, false, true, false);
        }
        if (TakeDiag(2))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiPanel] text api=DrawThemeTextEx hwnd=0x%p class=\"%s\" dc=0x%p dcWnd=0x%p panelDc=%d theme=0x%p part=%d state=%d flags=0x%X options=0x%X bufferHr=0x%08X drawHr=0x%08X",
                hwnd, name, dc, dcWindow, panelDc ? 1 : 0, theme, part, state, flags, options ? options->dwFlags : 0u,
                static_cast<unsigned>(buffered), static_cast<unsigned>(drawn));
        if (!SUCCEEDED(buffered) || !SUCCEEDED(drawn))
            return original(theme, dc, part, state, text, count, flags, rect, options);
        return drawn;
    }

    bool BeginPaint(HWND hwnd, UINT message, HDC suppliedDc) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !GlassCore::Root::IsFullInjectionMode()
            || !IsWindow(hwnd) || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
            || !GlassShared::IsDelphiPanelWindow(hwnd)
            || (message != WM_PAINT && message != WM_ERASEBKGND && message != WM_PRINTCLIENT)) return false;
        Session s{}; s.hwnd = hwnd; s.message = message; s.suppliedDc = suppliedDc;
        unsigned mask = 0;
        {
            auto lock = g_lock.lock_exclusive();
            if (g_shutdown.load(std::memory_order_acquire) || !g_dispatcher.moduleAddress) return false;
            for (size_t i = 0; i < Count; ++i)
            {
                const int before = g_dispatcher.GetRef(i);
                g_dispatcher.EnableHook(i, true);
                s.acquired[i] = g_dispatcher.GetRef(i) > before;
                if (s.acquired[i]) mask |= 1u << i;
            }
        }
        if (!mask) return false;
        g_stack.push_back(s);
        if (TakeDiag(0))
        {
            wchar_t name[64]{}; GetClassNameW(hwnd, name, static_cast<int>(_countof(name)));
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiPanel] Begin hwnd=0x%p class=\"%s\" msg=0x%X dc=0x%p acquired=0x%X depth=%zu",
                hwnd, name, message, suppliedDc, mask, g_stack.size());
        }
        return true;
    }
    void EndPaint() noexcept
    {
        if (g_stack.empty()) return;
        const Session s = g_stack.back(); g_stack.pop_back();
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire))
            for (size_t i = Count; i > 0; --i)
                if (s.acquired[i-1]) g_dispatcher.EnableHook(i-1, false);
    }
    void Startup() noexcept
    {
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule;
        g_dispatcher.CacheHookData();
        g_shutdown.store(false, std::memory_order_release);
    }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.DisableAllHooks();
        g_stack.clear();
    }
}
