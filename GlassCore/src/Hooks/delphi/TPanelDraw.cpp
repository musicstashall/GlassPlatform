// VCL panels/frames: preserve native erase so the actual background brush is
// available. Only client-background fills are replaced; bevels and other
// controls' DCs are not recoloured. The root themed-text slot also shares this
// session/DC gate and the existing transparent compositor. No alpha pattern brushes.
// What the graphic controls of a TScrollBox draw inside its paint is the EXE's
// own too: a bitmap copied as it is lands opaque, with everything it was
// composed on; an icon or a cursor is blended with its transparency; a label
// is the ANSI text of a panel.
#include "CorePch.h"
#include <Hooks/delphi/TPanelDraw.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Core/Intercept/HookHelper.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/OpaqueBitmapSource.h>
#include <Subsystems/PictureAlpha.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassDelphiPanel.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
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
        enum : size_t { FillSlot, PatSlot, ExtSlot, StretchSlot, Count };
        constexpr unsigned kAnsiTextMask = 1u << Count; // separate export detour, not an IAT slot
        constexpr unsigned kPageAnsiTextMask = 1u << (Count + 1);
        constexpr unsigned kIconMask = 1u << (Count + 2); // its own EXE import, not a panel slot
        enum class AnsiTextRoute { PanelExport, PageIat };
        // Whose pictures the EXE draws itself: the donor's TPanel inside a TPanel
        // (the picture as it is, any ROP) and a TScrollBox (pictures and icons of
        // graphic controls laid out in it).
        enum class ImageHost { None, NestedPanel, ScrollBox };
        int WINAPI PanelFill(HDC, const RECT*, HBRUSH);
        BOOL WINAPI PanelPat(HDC, int, int, int, int, DWORD);
        BOOL WINAPI PanelExt(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
        BOOL WINAPI PanelStretch(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
        int WINAPI PanelDrawTextA(HDC, LPCSTR, int, LPRECT, UINT);
        int WINAPI PageDrawTextA(HDC, LPCSTR, int, LPRECT, UINT);
        BOOL WINAPI PanelIcon(HDC, int, int, HICON, int, int, UINT, HBRUSH, UINT);
        const HookDispatcherDependency<Count, 3> g_dep{{
            std::tuple{std::array{"user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv},
                "FillRect", reinterpret_cast<PVOID>(PanelFill)},
            std::tuple{std::array{"gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv},
                "PatBlt", reinterpret_cast<PVOID>(PanelPat)},
            std::tuple{std::array{"gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv},
                "ExtTextOutW", reinterpret_cast<PVOID>(PanelExt)},
            std::tuple{std::array{"gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv},
                "StretchBlt", reinterpret_cast<PVOID>(PanelStretch)},
        }};
        // Background/image imports retain their EXE dispatcher and registry.
        // TPanel DrawTextA uses BWM's user32 export detour: Delphi need not
        // have a named EXE import (it may call via a thunk or another module).
        HookDispatcher<Count, 3> g_dispatcher{g_dep, nullptr};
        // The Restorator TPage experiment uses only its named EXE import. Keep
        // it separate from both the panel background slots and TPanel's BWM
        // export lease; all slot ownership/originals remain with IATRegistry.
        const HookDispatcherDependency<1, 3> g_pageTextDep{{
            std::tuple{std::array{"user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv},
                "DrawTextA", reinterpret_cast<PVOID>(PageDrawTextA)},
        }};
        HookDispatcher<1, 3> g_pageTextDispatcher{g_pageTextDep, nullptr};
        // Icons and cursors a TScrollBox shows are drawn by the EXE with
        // DrawIconEx. Again a named import of its own, apart from the panel slots.
        const HookDispatcherDependency<1, 3> g_iconDep{{
            std::tuple{std::array{"user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv},
                "DrawIconEx", reinterpret_cast<PVOID>(PanelIcon)},
        }};
        HookDispatcher<1, 3> g_iconDispatcher{g_iconDep, nullptr};
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        decltype(&::DrawTextA) g_actualDrawTextA{};
        size_t g_ansiTextRefs{};
        bool g_ansiTextAttached = false;
        struct Session
        {
            HWND hwnd{};
            UINT message{};
            HDC suppliedDc{};
            std::array<bool, Count> acquired{};
            bool ansiTextPaint = false; // Class/message checked once at BeginPaint, for either hook.
            bool ansiTextAcquired = false;
            bool pageAnsiTextAcquired = false;
            bool iconAcquired = false;
            bool panelBackground = false;
        };
        thread_local std::vector<Session> g_stack;
        thread_local bool g_inHook = false;
        constexpr BYTE kCustomBackgroundAlpha = 100;

        bool TakeDiag(size_t channel) noexcept
        {
            static thread_local ULONGLONG epoch[7]{};
            static thread_local unsigned budget[7]{};
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
        // Caller holds g_lock exclusively. Logical session refs are separate
        // from attachment state so a failed final Detach keeps its trampoline
        // valid even after the last logical ref was released.
        bool DetachAnsiText() noexcept
        {
            if (!g_ansiTextAttached) return true;
            const HRESULT hr = Detours::Write([] {
                Detours::Detach(reinterpret_cast<void**>(&g_actualDrawTextA),
                    reinterpret_cast<void*>(&PanelDrawTextA));
            });
            if (SUCCEEDED(hr)) g_ansiTextAttached = false;
            DebugLog(SUCCEEDED(hr) ? DBG_NORMAL : DBG_WARNING,
                L"[GlassCore][DelphiPanel] text-detour api=DrawTextA enable=0 refs=%zu attached=%d hr=0x%08X",
                g_ansiTextRefs, g_ansiTextAttached ? 1 : 0, static_cast<unsigned>(hr));
            return SUCCEEDED(hr);
        }
        bool AcquireAnsiText() noexcept
        {
            if (!g_actualDrawTextA) return false;
            if (!g_ansiTextRefs && !g_ansiTextAttached)
            {
                const HRESULT hr = Detours::Write([] {
                    Detours::Attach(reinterpret_cast<void**>(&g_actualDrawTextA),
                        reinterpret_cast<void*>(&PanelDrawTextA));
                });
                if (SUCCEEDED(hr)) g_ansiTextAttached = true;
                DebugLog(SUCCEEDED(hr) ? DBG_NORMAL : DBG_WARNING,
                    L"[GlassCore][DelphiPanel] text-detour api=DrawTextA enable=1 refs=%zu attached=%d hr=0x%08X",
                    g_ansiTextRefs + (SUCCEEDED(hr) ? 1u : 0u), g_ansiTextAttached ? 1 : 0, static_cast<unsigned>(hr));
                if (FAILED(hr)) return false; // No phantom lease after an aborted transaction.
            }
            ++g_ansiTextRefs;
            return true;
        }
        // Caller holds g_lock. A false EnableHook return may still acquire a
        // nested ref; only the ref delta says whether this session owns a lease.
        bool AcquirePageAnsiText(HWND hwnd) noexcept
        {
            const int before = g_pageTextDispatcher.GetRef(0);
            g_pageTextDispatcher.EnableHook(0, true);
            const int refs = g_pageTextDispatcher.GetRef(0);
            const bool acquired = refs > before;
            if (TakeDiag(5)) DebugLog(acquired ? DBG_NORMAL : DBG_WARNING,
                L"[GlassCore][DelphiPanel] text-iat api=DrawTextA class=TPage enable=1 hwnd=0x%p exe=0x%p acquired=%d refs=%d",
                hwnd, g_pageTextDispatcher.moduleAddress, acquired ? 1 : 0, refs);
            return acquired;
        }
        // Caller holds g_lock. Only the ref delta says whether this session owns a lease.
        bool AcquireIcon() noexcept
        {
            if (!g_iconDispatcher.moduleAddress) return false;
            const int before = g_iconDispatcher.GetRef(0);
            g_iconDispatcher.EnableHook(0, true);
            return g_iconDispatcher.GetRef(0) > before;
        }
        void ReleaseSession(const Session& s) noexcept
        {
            if (s.iconAcquired) g_iconDispatcher.EnableHook(0, false);
            if (s.pageAnsiTextAcquired)
            {
                g_pageTextDispatcher.EnableHook(0, false);
                if (TakeDiag(5)) DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiPanel] text-iat api=DrawTextA class=TPage enable=0 hwnd=0x%p exe=0x%p refs=%d",
                    s.hwnd, g_pageTextDispatcher.moduleAddress, g_pageTextDispatcher.GetRef(0));
            }
            if (s.ansiTextAcquired && g_ansiTextRefs && --g_ansiTextRefs == 0) DetachAnsiText();
            for (size_t i = Count; i > 0; --i)
                if (s.acquired[i-1]) g_dispatcher.EnableHook(i-1, false);
        }
        decltype(&::DrawTextA) AnsiTextOriginal() noexcept
        {
            auto lock = g_lock.lock_shared();
            return g_actualDrawTextA; // Detours trampoline while attached, export after Detach.
        }
        decltype(&::DrawTextA) PageAnsiTextOriginal() noexcept
        {
            auto lock = g_lock.lock_shared();
            return g_pageTextDispatcher.GetOrg<0, decltype(&::DrawTextA)>();
        }
        decltype(&::DrawIconEx) IconOriginal() noexcept
        {
            auto lock = g_lock.lock_shared();
            return g_iconDispatcher.GetOrg<0, decltype(&::DrawIconEx)>();
        }
        bool IsNestedImagePanel(HWND hwnd) noexcept
        {
            wchar_t name[64]{}, parentName[64]{};
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            return exe && IsWindow(hwnd)
                && reinterpret_cast<HMODULE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe
                && GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))
                && _wcsicmp(name, L"TPanel") == 0
                && GetClassNameW(GetParent(hwnd), parentName, static_cast<int>(_countof(parentName)))
                && _wcsicmp(parentName, L"TPanel") == 0;
        }
        // The exact VCL class, registered by the EXE itself: VCL is linked into
        // it, and only then do its graphic controls draw through the EXE's imports.
        bool IsImageScrollBox(HWND hwnd) noexcept
        {
            wchar_t name[64]{};
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            return exe && IsWindow(hwnd)
                && reinterpret_cast<HMODULE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe
                && GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))
                && _wcsicmp(name, L"TScrollBox") == 0;
        }
        ImageHost ImageHostOf(HWND hwnd) noexcept
        {
            return IsNestedImagePanel(hwnd) ? ImageHost::NestedPanel
                : IsImageScrollBox(hwnd) ? ImageHost::ScrollBox : ImageHost::None;
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
            if (!r || IsRectEmpty(r) || !IsPanelDc(dc) || !g_stack.back().panelBackground) return false;
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
            // Оба системных фона — прозрачная база, в том числе exact TPanel
            // (BWM). Произвольный RGB, включая чёрный, остаётся tint A=100.
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
        // BWM window + caller EXE: exact TPanel внутри TPanel и TScrollBox. Это
        // подготовка отдельного изображения, не общий финальный blit DoubleBuffered.
        BOOL WINAPI PanelStretch(HDC dc, int x, int y, int w, int h,
            HDC source, int sx, int sy, int sw, int sh, DWORD rop)
        {
            const DWORD entryError = GetLastError();
            const auto orig = Original<StretchSlot, decltype(&::StretchBlt)>();
            const auto native = [&] {
                SetLastError(entryError);
                return orig ? orig(dc, x, y, w, h, source, sx, sy, sw, sh, rop)
                    : ::StretchBlt(dc, x, y, w, h, source, sx, sy, sw, sh, rop);
            };
            ImageHost host = ImageHost::None;
            if (g_inHook || !IsPanelDc(dc) || !g_stack.back().acquired[StretchSlot]
                || g_stack.back().message != WM_PAINT
                || (host = ImageHostOf(g_stack.back().hwnd)) == ImageHost::None
                // В TScrollBox рисует любой графический контрол: изображение «как
                // есть» — только прямая копия; маски и прочие ROP остаются native.
                || (host == ImageHost::ScrollBox && rop != SRCCOPY)
                || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(g_stack.back().hwnd)
                || !GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(g_stack.back().hwnd, dc)
                || !source || source == dc || !w || !h || !sw || !sh
                || GetMapMode(dc) != MM_TEXT || GetMapMode(source) != MM_TEXT
                || GetCurrentObject(source, OBJ_BITMAP) == GetCurrentObject(dc, OBJ_BITMAP))
                return native();
            const HWND hwnd = g_stack.back().hwnd;
            BITMAP bitmap{}; RECT client{};
            if (GetObjectW(GetCurrentObject(source, OBJ_BITMAP), sizeof(bitmap), &bitmap) != sizeof(bitmap)
                || !GetClientRect(hwnd, &client)) return native();
            // Даже partial final-copy может читать полноразмерный буфер.
            // Его назначение неоднозначно: не делать всё окно A=255.
            if (bitmap.bmWidth == client.right-client.left && bitmap.bmHeight == client.bottom-client.top)
            {
                if (TakeDiag(3))
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][DelphiPanel] image-native api=StretchBlt hwnd=0x%p dc=0x%p src=0x%p size=[%ld,%ld] rop=0x%08X reason=client-sized-source",
                        hwnd, dc, source, bitmap.bmWidth, bitmap.bmHeight, rop);
                return native();
            }
            g_inHook = true;
            auto leave = wil::scope_exit([]() noexcept { g_inHook = false; });
            BOOL result{}; DWORD error{}; bool prepared{};
            {
                // Копия A=255 рисуется как есть — вместе с тем, на чём приложение
                // собрало изображение. По цвету из неё ничего не вырезается.
                GlassCore::GdiUtils::OpaqueBitmapSource image{source};
                prepared = static_cast<bool>(image);
                result = native(); // mask/ROP, масштаб и знаки extents сохраняются
                error = GetLastError();
            }
            if (TakeDiag(3))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiPanel] image api=StretchBlt hwnd=0x%p dc=0x%p src=0x%p bpp=%u dst=[%d,%d,%d,%d] srcRect=[%d,%d,%d,%d] rop=0x%08X opaqueCopy=%d result=%d",
                    hwnd, dc, source, unsigned(bitmap.bmBitsPixel),
                    x, y, w, h, sx, sy, sw, sh, rop, prepared ? 1 : 0, result);
            SetLastError(error);
            return result;
        }
        // Значок или курсор, который EXE рисует сам (графический контрол в
        // TScrollBox): GDI оставляет A=0 под значком без собственной альфы. Значок
        // рисуется оригиналом в свои буферы и смешивается с DC уже с прозрачностью.
        BOOL WINAPI PanelIcon(HDC dc, int x, int y, HICON icon, int cx, int cy, UINT step, HBRUSH flicker, UINT flags)
        {
            const DWORD entryError = GetLastError();
            const auto orig = IconOriginal();
            const auto native = [&] {
                SetLastError(entryError);
                return orig ? orig(dc, x, y, icon, cx, cy, step, flicker, flags)
                    : ::DrawIconEx(dc, x, y, icon, cx, cy, step, flicker, flags);
            };
            int width{}, height{};
            // Кисть «без мерцания» — это заливка под значком, а отдельные проходы
            // маски и изображения приложение смешивает само: такие вызовы его.
            // The lease itself says whose paint this is: only the paint of a scroll box takes it.
            if (!orig || g_inHook || flicker || (flags & DI_NORMAL) != DI_NORMAL
                || !IsPanelDc(dc) || !g_stack.back().iconAcquired
                || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(g_stack.back().hwnd)
                || !GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(g_stack.back().hwnd, dc)
                || GetMapMode(dc) != MM_TEXT || GetLayout(dc) != 0
                || !GlassCore::GdiUtils::IconDrawSize(icon, cx, cy, flags, width, height))
                return native();
            const HWND hwnd = g_stack.back().hwnd;
            bool drawn = false;
            {
                g_inHook = true;
                auto leave = wil::scope_exit([]() noexcept { g_inHook = false; });
                drawn = GlassCore::GdiUtils::DrawIconAlpha(orig, dc, x, y, icon, width, height, step, flags);
            }
            if (TakeDiag(6))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiPanel] icon api=DrawIconEx hwnd=0x%p dc=0x%p icon=0x%p at=[%d,%d] size=[%d,%d] flags=0x%X alpha=%d",
                    hwnd, dc, icon, x, y, width, height, flags, drawn ? 1 : 0);
            if (!drawn) return native();
            SetLastError(entryError);
            return TRUE;
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

        // Export acquisition is exact TPanel only; TPage acquires its EXE IAT.
        // Both feed the same ANSI paint context, not the wider background set.
        bool IsAnsiTextPanel(HWND hwnd, UINT message) noexcept
        {
            wchar_t name[64]{};
            // TScrollBox: подписи графических контролов внутри него — тот же
            // DrawTextA приложения, что и у подписей TPanel.
            return (message == WM_PAINT || message == WM_PRINTCLIENT)
                && GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))
                && (_wcsicmp(name, L"TPanel") == 0 || _wcsicmp(name, L"TScrollBox") == 0);
        }
        bool IsAnsiTextRect(const RECT* rect) noexcept
        {
            if (!rect) return false;
            const auto width = std::int64_t(rect->right) - rect->left;
            const auto height = std::int64_t(rect->bottom) - rect->top;
            return width > 0 && height > 0 && width <= (std::numeric_limits<int>::max)()
                && height <= (std::numeric_limits<int>::max)();
        }
        bool DecodeAnsiPanelText(HDC dc, LPCSTR text, int count, std::wstring& wide, UINT& cp)
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
            if (count == -1) wide.pop_back(); // -1 includes the terminating NUL, explicit byte counts do not.
            return !wide.empty();
        }
        // One Glass text path for both TPanel/TPage. The interception route
        // selects only its original and diagnostic label, not drawing permission.
        int DrawAnsiText(AnsiTextRoute route, HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
        {
            const DWORD entryError = GetLastError();
            DWORD exitError = entryError;
            // Run last, after DC, string and logger cleanup. Native failures (or
            // originals that leave LastError untouched) retain their own value.
            auto preserveError = wil::scope_exit([&]() noexcept { SetLastError(exitError); });
            const bool pageIat = route == AnsiTextRoute::PageIat;
            const LPCWSTR routeName = pageIat ? L"page-iat" : L"panel-export";
            const auto original = pageIat ? PageAnsiTextOriginal() : AnsiTextOriginal();
            if (!original && !pageIat) return 0; // No export address means no panel attachment.
            const auto native = [&] {
                const bool previous = g_inHook;
                // The IAT original may reach the export hook. Once falling
                // back, stay native instead of retrying the same compositor.
                g_inHook = true;
                auto leave = wil::scope_exit([&]() noexcept { g_inHook = previous; });
                SetLastError(entryError);
                // Prefer the registry-provided original, never a peer bypass.
                // A stale/missing page original uses the API export, not its IAT.
                // For TPanel, original is always the resolved Detours trampoline.
                const auto target = original ? original : &::DrawTextA;
                const int result = target(dc, text, count, rect, flags);
                exitError = GetLastError();
                return result;
            };
            if (!original || g_inHook || !IsPanelDc(dc) || !g_stack.back().ansiTextPaint) return native();
            const HWND hwnd = g_stack.back().hwnd;
            const auto nativeReason = [&](LPCWSTR reason) {
                if (TakeDiag(4)) DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiPanel] text-native api=DrawTextA hwnd=0x%p dc=0x%p ansiCount=%d flags=0x%X reason=%s route=%s",
                    hwnd, dc, count, flags, reason, routeName);
                return native();
            };
            // Buffered bounds cannot reproduce DT_NOCLIP or non-MM_TEXT maps.
            // Measurements and mutable ANSI strings must reach the ANSI original.
            if (!text || count == 0 || count < -1 || !IsAnsiTextRect(rect)
                || (flags & (DT_CALCRECT | DT_MODIFYSTRING | DT_NOCLIP)) || GetMapMode(dc) != MM_TEXT)
                return nativeReason(L"arguments-flags-or-mapping");
            std::wstring wide; UINT cp{}; bool decoded = false;
            try { decoded = DecodeAnsiPanelText(dc, text, count, wide, cp); }
            catch (...) { return nativeReason(L"conversion-exception"); }
            if (!decoded) return nativeReason(L"conversion");
            const int saved = SaveDC(dc);
            if (!saved) return nativeReason(L"SaveDC");
            RECT drawRect = *rect; int result = 0; HRESULT hr = E_FAIL;
            {
                g_inHook = true;
                auto leave = wil::scope_exit([]() noexcept { g_inHook = false; });
                auto restore = wil::scope_exit([&]() noexcept { RestoreDC(dc, saved); });
                if (SetBkMode(dc, TRANSPARENT))
                {
                    // Glyphs only, over the accepted panel tint/transparent base.
                    // No BWM BLACKNESS/opaque text-rect fill and no shared A/W policy.
                    try { hr = GlassCore::Text::DrawTextWithAlpha(dc, wide.data(),
                        static_cast<int>(wide.size()), &drawRect, flags, result, true); }
                    catch (...) { hr = E_FAIL; }
                }
            }
            if (TakeDiag(4)) DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiPanel] text api=DrawTextA hwnd=0x%p dc=0x%p ansiCount=%d wideCount=%d cp=%u flags=0x%X rect=[%ld,%ld,%ld,%ld] transparent=1 hr=0x%08X result=%d route=%s",
                hwnd, dc, count, static_cast<int>(wide.size()), cp, flags,
                rect->left, rect->top, rect->right, rect->bottom, static_cast<unsigned>(hr), result, routeName);
            // Restore the original DC before any native fallback; caller RECT
            // is never replaced by the compositor's private working copy.
            return SUCCEEDED(hr) ? result : native();
        }
        int WINAPI PanelDrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
        { return DrawAnsiText(AnsiTextRoute::PanelExport, dc, text, count, rect, flags); }
        int WINAPI PageDrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
        { return DrawAnsiText(AnsiTextRoute::PageIat, dc, text, count, rect, flags); }
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
            || (message != WM_PAINT && message != WM_ERASEBKGND && message != WM_PRINTCLIENT)) return false;
        const bool background = GlassShared::IsDelphiPanelWindow(hwnd);
        const bool pageText = GlassShared::IsDelphiPageAnsiPaintWindow(hwnd, message);
        if (!background && !pageText) return false;
        const bool panelText = IsAnsiTextPanel(hwnd, message);
        Session s{}; s.hwnd = hwnd; s.message = message; s.suppliedDc = suppliedDc;
        s.panelBackground = background;
        s.ansiTextPaint = panelText || pageText;
        unsigned mask = 0;
        // Только paint-путь изображения: донорский TPanel в TPanel и TScrollBox.
        // Erase/PrintClient, frames и остальные panel-классы его не приобретают.
        const ImageHost images = message == WM_PAINT ? ImageHostOf(hwnd) : ImageHost::None;
        {
            auto lock = g_lock.lock_exclusive();
            if (g_shutdown.load(std::memory_order_acquire)) return false;
            for (size_t i = 0; background && g_dispatcher.moduleAddress && i < Count; ++i)
            {
                if (i == StretchSlot && images == ImageHost::None) continue;
                const int before = g_dispatcher.GetRef(i);
                g_dispatcher.EnableHook(i, true);
                s.acquired[i] = g_dispatcher.GetRef(i) > before;
                if (s.acquired[i]) mask |= 1u << i;
            }
            s.ansiTextAcquired = panelText && AcquireAnsiText();
            if (s.ansiTextAcquired) mask |= kAnsiTextMask;
            s.pageAnsiTextAcquired = pageText && AcquirePageAnsiText(hwnd);
            if (s.pageAnsiTextAcquired) mask |= kPageAnsiTextMask;
            s.iconAcquired = images == ImageHost::ScrollBox && AcquireIcon();
            if (s.iconAcquired) mask |= kIconMask;
            if (!mask) return false; // TPanel export OR TPage IAT text can own a text-only session.
            try { g_stack.push_back(s); }
            catch (...) { ReleaseSession(s); return false; }
        }
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
        if (!g_shutdown.load(std::memory_order_acquire)) ReleaseSession(s);
    }
    void Startup() noexcept
    {
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire)) return;
        g_dispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule;
        g_dispatcher.CacheHookData();
        g_pageTextDispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule;
        g_pageTextDispatcher.CacheHookData();
        g_iconDispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule;
        g_iconDispatcher.CacheHookData();
        // Do not replace a retained trampoline if an earlier Detach failed.
        if (!g_ansiTextAttached)
        {
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            g_actualDrawTextA = user32
                ? reinterpret_cast<decltype(g_actualDrawTextA)>(GetProcAddress(user32, "DrawTextA")) : nullptr;
            DebugLog(g_actualDrawTextA ? DBG_NORMAL : DBG_WARNING,
                L"[GlassCore][DelphiPanel] text-resolve api=DrawTextA user32=0x%p target=0x%p",
                user32, reinterpret_cast<void*>(g_actualDrawTextA));
        }
        g_shutdown.store(false, std::memory_order_release);
    }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.DisableAllHooks();
        g_pageTextDispatcher.DisableAllHooks();
        g_iconDispatcher.DisableAllHooks();
        g_ansiTextRefs = 0;
        DetachAnsiText(); // Also retries a failed last-session Detach.
        g_stack.clear();
    }
}
