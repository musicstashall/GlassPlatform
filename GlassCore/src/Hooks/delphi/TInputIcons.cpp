// Native VCL ImageList icons: TButtonedEdit/TEditButton.TGlyph and TSearchBox.
// Do not redirect Edit text or reinterpret arbitrary BitBlt/MaskBlt operations.
#include "CorePch.h"
#include <Hooks/delphi/TInputIcons.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <CommCtrl.h>
#include <uxtheme.h>
#include <wil.h>
#include <array>
#include <atomic>
#include <limits>
#include <optional>
#include <DebugLog.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")

namespace GlassCore::DelphiInputIcons
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;
        BOOL WINAPI Draw(HIMAGELIST, int, HDC, int, int, UINT);
        BOOL WINAPI DrawEx(HIMAGELIST, int, HDC, int, int, int, int, COLORREF, COLORREF, UINT);
        BOOL WINAPI DrawIndirect(IMAGELISTDRAWPARAMS*);
        enum : size_t { DrawSlot, DrawExSlot, IndirectSlot, HookCount };
        const HookDispatcherDependency<HookCount, 1> g_dep{{
            std::tuple{std::array{"comctl32.dll"sv}, "ImageList_Draw", reinterpret_cast<void*>(Draw)},
            std::tuple{std::array{"comctl32.dll"sv}, "ImageList_DrawEx", reinterpret_cast<void*>(DrawEx)},
            std::tuple{std::array{"comctl32.dll"sv}, "ImageList_DrawIndirect", reinterpret_cast<void*>(DrawIndirect)},
        }};
        HookDispatcher<HookCount, 1> g_dispatcher{g_dep, nullptr};
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        thread_local bool g_inside = false;

        template<size_t Slot, typename Fn> Fn Original()
        {
            auto lock = g_lock.lock_shared();
            return g_dispatcher.GetOrg<Slot, Fn>();
        }

        bool HasClass(HWND hwnd, const wchar_t* name) noexcept
        {
            wchar_t cls[64]{};
            return hwnd && GetClassNameW(hwnd, cls, _countof(cls)) && _wcsicmp(cls, name) == 0;
        }

        HWND IconOwner(HDC dc, HWND* target = nullptr) noexcept
        {
            if (!dc || g_inside || g_shutdown.load(std::memory_order_acquire)
                || !GlassCore::Root::IsFullInjectionMode() || GetMapMode(dc) != MM_TEXT) return nullptr;
            HWND window = WindowFromDC(dc);
            if (!window)
            {
                if (GetObjectType(dc) != OBJ_MEMDC) return nullptr;
                window = GlassCore::PaintDispatcher::GetStackOwnerForDc(dc);
                if (!window || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(window)) return nullptr;
                BITMAP bitmap{};
                if (GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof(bitmap), &bitmap) != sizeof(bitmap)
                    || bitmap.bmBitsPixel <= 1) return nullptr;
            }
            HWND owner = window;
            // Exact container classes, independent of the assigned image/index.
            // Both glyph spellings require an immediate TButtonedEdit parent.
            if (HasClass(window, L"TEditButton.TGlyph") || HasClass(window, L"TGlyph"))
            {
                owner = GetParent(window);
                if (!HasClass(owner, L"TButtonedEdit")) return nullptr;
            }
            else if (!HasClass(window, L"TButtonedEdit") && !HasClass(window, L"TSearchBox"))
                return nullptr;
            if (GlassShared::GlassGetWindowKind(owner) != GlassShared::GlassControlKind::Edit
                || !GlassShared::GlassIsBackdropAllowedForHwnd(owner)
                || !GlassShared::GlassIsBackdropAllowedForHwnd(window)) return nullptr;
            if (target) *target = window;
            return owner;
        }

        struct BackgroundPatches
        {
            std::array<RECT, 2> rects{};
            size_t count{};
        };

        bool ReadSolidBrush(HBRUSH brush, COLORREF& color) noexcept
        {
            const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(brush);
            if (value && value <= COLOR_MENUBAR + 1u)
            { color = GetSysColor(static_cast<int>(value - 1)); return true; }
            LOGBRUSH lb{};
            if (!brush || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID) return false;
            color = lb.lbColor;
            return true;
        }

        bool ButtonRect(HWND child, HWND owner, const RECT& parentClient, POINT parentOrigin, RECT& result) noexcept
        {
            if (GetParent(child) != owner || !GlassShared::GlassIsBackdropAllowedForHwnd(child)) return false;
            RECT screen{}, client{};
            POINT origin{};
            if (!GetWindowRect(child, &screen) || !GetClientRect(child, &client) || !ClientToScreen(child, &origin)
                || client.left || client.top || client.right <= 0 || client.bottom <= 0
                || client.right > 1024 || client.bottom > 1024
                || origin.x != screen.left || origin.y != screen.top
                || static_cast<long long>(screen.right) - screen.left != client.right
                || static_cast<long long>(screen.bottom) - screen.top != client.bottom) return false;
            // Checked wide subtraction; no MapWindowPoints ambiguity or mutation
            // of a DC's origins/clip. Bordered/oversized/foreign children stay native.
            const long long left = static_cast<long long>(screen.left) - parentOrigin.x;
            const long long top = static_cast<long long>(screen.top) - parentOrigin.y;
            const long long right = static_cast<long long>(screen.right) - parentOrigin.x;
            const long long bottom = static_cast<long long>(screen.bottom) - parentOrigin.y;
            if (left < parentClient.left || top < parentClient.top || right > parentClient.right
                || bottom > parentClient.bottom) return false;
            result = {static_cast<LONG>(left), static_cast<LONG>(top), static_cast<LONG>(right), static_cast<LONG>(bottom)};
            return !EqualRect(&result, &parentClient); // never blacken the whole field
        }

        BackgroundPatches PlanButtonBackground(HWND owner, HWND target, HDC dc, const RECT* rect, HBRUSH brush) noexcept
        {
            BackgroundPatches patches{};
            // Only the window-client DCs actually observed in LogViewer-21. Do
            // not infer a client coordinate space for a registered memory DC.
            if (!owner || !HasClass(owner, L"TButtonedEdit") || !rect || WindowFromDC(dc) != target
                || GetGraphicsMode(dc) != GM_COMPATIBLE || GetLayout(dc) != 0) return patches;
            COLORREF color{};
            if (!ReadSolidBrush(brush, color) || (color & 0xFFFFFFu) != (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)) return patches;
            RECT client{}, parentClient{};
            POINT origin{}, dcOrigin{}, viewport{}, windowOrigin{}, parentOrigin{};
            if (!GetClientRect(target, &client) || IsRectEmpty(&client) || !EqualRect(rect, &client)
                || !ClientToScreen(target, &origin) || !GetDCOrgEx(dc, &dcOrigin)
                || origin.x != dcOrigin.x || origin.y != dcOrigin.y
                || !GetViewportOrgEx(dc, &viewport) || !GetWindowOrgEx(dc, &windowOrigin)
                || viewport.x || viewport.y || windowOrigin.x || windowOrigin.y
                || !GetClientRect(owner, &parentClient) || IsRectEmpty(&parentClient)
                || !ClientToScreen(owner, &parentOrigin)) return patches;
            if (target != owner)
            {
                RECT inParent{};
                if (ButtonRect(target, owner, parentClient, parentOrigin, inParent))
                { patches.rects[0] = client; patches.count = 1; }
                return patches;
            }
            unsigned visited = 0;
            // GetWindow walks direct siblings, NOT descendants. Hidden glyphs
            // deliberately remain eligible so the exposed parent area is cleared.
            for (HWND child = GetWindow(owner, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            {
                if (++visited > 64) return {};
                if (!HasClass(child, L"TEditButton.TGlyph") && !HasClass(child, L"TGlyph")) continue;
                RECT button{};
                if (patches.count == patches.rects.size() || !ButtonRect(child, owner, parentClient, parentOrigin, button)) return {};
                patches.rects[patches.count++] = button;
            }
            return patches;
        }

        BackgroundPatches PlanSearchBackground(HWND owner, HWND target, HDC dc, const RECT* rect, HBRUSH brush) noexcept
        {
            BackgroundPatches patches{};
            // SearchBox owns a reserved NC strip, not a TGlyph child. Prove the
            // WINDOW coordinate DC; a client/registered memory DC is not enough.
            if (!owner || owner != target || !HasClass(owner, L"TSearchBox") || !rect
                || WindowFromDC(dc) != owner || GetGraphicsMode(dc) != GM_COMPATIBLE || GetLayout(dc) != 0) return patches;
            COLORREF color{};
            if (!ReadSolidBrush(brush, color) || (color & 0xFFFFFFu) != (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)) return patches;
            RECT window{}, client{};
            POINT clientOrigin{}, dcOrigin{}, viewport{}, windowOrigin{};
            if (!GetWindowRect(owner, &window) || !GetClientRect(owner, &client)
                || client.left || client.top || IsRectEmpty(&client)
                || !ClientToScreen(owner, &clientOrigin) || !GetDCOrgEx(dc, &dcOrigin)
                || dcOrigin.x != window.left || dcOrigin.y != window.top
                || !GetViewportOrgEx(dc, &viewport) || !GetWindowOrgEx(dc, &windowOrigin)
                || viewport.x || viewport.y || windowOrigin.x || windowOrigin.y) return patches;
            const long long width = static_cast<long long>(window.right) - window.left;
            const long long height = static_cast<long long>(window.bottom) - window.top;
            const long long left = static_cast<long long>(clientOrigin.x) - window.left;
            const long long top = static_cast<long long>(clientOrigin.y) - window.top;
            const long long right = left + client.right;
            const long long bottom = top + client.bottom;
            if (width <= 0 || height <= 0 || width > (std::numeric_limits<LONG>::max)()
                || height > (std::numeric_limits<LONG>::max)() || left < 0 || top < 0
                || right > width || bottom > height) return patches;
            const long long rightInset = width - right;
            const long long border = left < rightInset ? left : rightInset;
            // Require a symmetric frame plus ONE extra horizontal reservation.
            // Infer its side/width from current geometry, not a guessed DPI width,
            // clip bounding box, image rectangle, or private VCL field offset.
            if (border <= 0 || top != border || height - bottom != border || left == rightInset) return patches;
            const long long buttonLeft = left > rightInset ? border : right;
            const long long buttonRight = left > rightInset ? left : width - border;
            if (buttonRight <= buttonLeft || buttonRight - buttonLeft > 1024 || bottom - top > 1024) return patches;
            const RECT button{static_cast<LONG>(buttonLeft), static_cast<LONG>(top),
                static_cast<LONG>(buttonRight), static_cast<LONG>(bottom)};
            // Only the complete background fill of that strip. Partial NC repaint
            // remains controlled by the untouched caller clip, as for ButtonedEdit.
            // Native coloured hot/pressed/custom brushes were rejected above.
            if (!EqualRect(rect, &button)) return patches;
            patches.rects[0] = button;
            patches.count = 1;
            return patches;
        }

        int WINAPI Fill(HDC, const RECT*, HBRUSH);
        const HookDispatcherDependency<1, 3> g_fillDep{{
            std::tuple{std::array{"user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv}, "FillRect", reinterpret_cast<void*>(Fill)},
        }};
        HookDispatcher<1, 3> g_fillDispatcher{g_fillDep, nullptr};
        thread_local bool g_inFillHook = false;

        int WINAPI Fill(HDC dc, const RECT* rect, HBRUSH brush)
        {
            const DWORD entryError = GetLastError();
            decltype(&::FillRect) orig{};
            { auto lock = g_lock.lock_shared(); orig = g_fillDispatcher.GetOrg<0, decltype(&::FillRect)>(); }
            if (!orig) orig = ::FillRect;
            if (g_inFillHook) { SetLastError(entryError); return orig(dc, rect, brush); }
            g_inFillHook = true;
            auto leave = wil::scope_exit([]() noexcept { g_inFillHook = false; });
            HWND target{};
            const HWND owner = rect && brush ? IconOwner(dc, &target) : nullptr;
            BackgroundPatches patches = PlanButtonBackground(owner, target, dc, rect, brush);
            if (!patches.count) patches = PlanSearchBackground(owner, target, dc, rect, brush);
            SetLastError(entryError);
            const int result = orig(dc, rect, brush); // original arguments/result exactly once
            const DWORD error = GetLastError();
            unsigned patched = 0;
            if (result)
                for (size_t i = 0; i < patches.count; ++i)
                {
                    const RECT& p = patches.rects[i];
                    // Keep the caller's actual (possibly complex) clip intact.
                    // BLACKNESS clears RGB/alpha only in the proven button area.
                    if (PatBlt(dc, p.left, p.top, p.right - p.left, p.bottom - p.top, BLACKNESS)) ++patched;
                }
            SetLastError(error);
            return result;
        }

        template<typename Paint> bool RestoreLegacyMaskAlpha(HDC dc, const RECT& bounds,
            RGBQUAD* black, int blackStride, int width, int height, UINT style, Paint& paint)
        {
            // The first native RGB-only pass was drawn on transparent black.
            // Replay that SAME operation on white: the native mask (including
            // holes, overlay and state) determines which destination pixels survive.
            // This is coverage recovery, never a black/white RGB colour key and
            // never a request for ILD_MASK that would bypass native image states.
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            BP_PAINTPARAMS params{sizeof(params), 0, nullptr, &blend};
            HDC memory{};
            HPAINTBUFFER matte = BeginBufferedPaint(dc, &bounds, BPBF_TOPDOWNDIB, &params, &memory);
            if (!matte || !memory)
            {
                if (matte) EndBufferedPaint(matte, FALSE);
                return false;
            }
            auto discardMatte = wil::scope_exit([matte] { EndBufferedPaint(matte, FALSE); });
            RGBQUAD* white{};
            int whiteStride{};
            if (FAILED(BufferedPaintClear(matte, nullptr))
                || FAILED(GetBufferedPaintBits(matte, &white, &whiteStride))
                || !white || whiteStride < width || !GdiFlush()) return false;
            for (int row = 0; row < height; ++row)
                for (int col = 0; col < width; ++col)
                    white[static_cast<size_t>(row) * whiteStride + col] = RGBQUAD{255, 255, 255, 0};
            if (!paint(memory, style | ILD_TRANSPARENT | ILD_PRESERVEALPHA) || !GdiFlush()) return false;
            for (int row = 0; row < height; ++row)
                for (int col = 0; col < width; ++col)
                {
                    RGBQUAD& dark = black[static_cast<size_t>(row) * blackStride + col];
                    const RGBQUAD& light = white[static_cast<size_t>(row) * whiteStride + col];
                    const int delta = light.rgbBlue - dark.rgbBlue;
                    // A valid source-over RGB/mask operation has the same
                    // nonnegative white-minus-black contribution in all channels.
                    // Unsupported XOR/colour-dependent results stay native.
                    if (delta < 0 || light.rgbGreen - dark.rgbGreen != delta
                        || light.rgbRed - dark.rgbRed != delta) return false;
                    const int alpha = 255 - delta;
                    if (dark.rgbBlue > alpha || dark.rgbGreen > alpha || dark.rgbRed > alpha) return false;
                    // RGB over black is already premultiplied. Both opaque black
                    // and opaque white survive; untouched transparent holes get A0.
                    dark.rgbReserved = static_cast<BYTE>(alpha);
                }
            return true;
        }

        template<typename Paint> std::optional<BOOL> TryDraw(
            HIMAGELIST images, HDC dc, int x, int y, int width, int height, UINT style, void* nativeApi,
            [[maybe_unused]] const wchar_t* api, [[maybe_unused]] int index, Paint paint)
        {
            HWND target{};
            const HWND owner = IconOwner(dc, &target);
            // Mask/ROP requests are not visible icons; DPI-sized bounds are not inferred.
            if (!owner || !images || (style & (ILD_MASK | ILD_ROP | ILD_DPISCALE))) return std::nullopt;
            // Use the same comctl32 generation as the intercepted import (v5/v6
            // ImageList handles must not be sent to another DLL's size API).
            HMODULE module{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(nativeApi), &module)) return std::nullopt;
            const auto getSize = reinterpret_cast<decltype(&::ImageList_GetIconSize)>(GetProcAddress(module, "ImageList_GetIconSize"));
            int naturalWidth{}, naturalHeight{};
            if (!getSize || !getSize(images, &naturalWidth, &naturalHeight)) return std::nullopt;
            if (!width) width = naturalWidth;
            if (!height) height = naturalHeight;
            if (width <= 0 || height <= 0 || width > 1024 || height > 1024
                || x > (std::numeric_limits<int>::max)() - width
                || y > (std::numeric_limits<int>::max)() - height) return std::nullopt;
            const RECT bounds{x, y, x + width, y + height};
            g_inside = true;
            auto leave = wil::scope_exit([] { g_inside = false; });
            BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
            BP_PAINTPARAMS params{sizeof(params), 0, nullptr, &blend};
            HDC memory{};
            HPAINTBUFFER buffer = BeginBufferedPaint(dc, &bounds, BPBF_TOPDOWNDIB, &params, &memory);
            if (!buffer || !memory)
            {
                if (buffer) EndBufferedPaint(buffer, FALSE);
                return std::nullopt;
            }
            auto discard = wil::scope_exit([&] { if (buffer) EndBufferedPaint(buffer, FALSE); });
            if (FAILED(BufferedPaintClear(buffer, nullptr))) return std::nullopt;
            // Let comctl32 retain its alpha/mask, image index, overlay and state.
            // No black-key: a black magnifier is part of the icon, not a colour key.
            const BOOL drawn = paint(memory, style | ILD_TRANSPARENT | ILD_PRESERVEALPHA);
            const DWORD error = GetLastError();
            RGBQUAD* pixels{};
            int stride{};
            HRESULT bitsResult = E_FAIL;
            BOOL flushed = FALSE;
            if (drawn)
            {
                bitsResult = GetBufferedPaintBits(buffer, &pixels, &stride);
                if (SUCCEEDED(bitsResult) && pixels && stride >= width) flushed = GdiFlush();
            }
            [[maybe_unused]] bool legacyMask = false;
            if (drawn)
            {
                if (!flushed) return std::nullopt;
                bool hasAlpha = false;
                for (int row = 0; row < height && !hasAlpha; ++row)
                    for (int col = 0; col < width; ++col)
                        if (pixels[static_cast<size_t>(row) * stride + col].rgbReserved)
                        {
                            hasAlpha = true;
                            break;
                        }
                // Keep all existing nonzero-alpha bytes and SearchBox behaviour.
                // Only a bounded TButtonedEdit RGB-only result gets mask recovery.
                if (!hasAlpha)
                {
                    if (!HasClass(owner, L"TButtonedEdit")
                        || !RestoreLegacyMaskAlpha(dc, bounds, pixels, stride, width, height, style, paint)) return std::nullopt;
                    legacyMask = true;
                }
            }
            const HRESULT committed = EndBufferedPaint(buffer, drawn);
            buffer = nullptr;
#if defined(_DEBUG)
            static thread_local ULONGLONG epoch{};
            static thread_local unsigned budget{};
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 12; }
            if (budget)
            {
                --budget;
                wchar_t cls[64]{};
                GetClassNameW(target, cls, _countof(cls));
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiInputIcon] stage=result api=%s hwnd=0x%p target=0x%p class=\"%s\" dc=0x%p images=0x%p index=%d rect=[%ld,%ld,%ld,%ld] style=0x%X draw=%d commit=0x%08lX legacyMask=%d",
                    api, owner, target, cls, dc, images, index, bounds.left, bounds.top, bounds.right, bounds.bottom, style, drawn, committed, legacyMask ? 1 : 0);
            }
#endif
            SetLastError(error);
            if (!drawn) return FALSE; // preserve native failure; never commit a partial icon
            return SUCCEEDED(committed) ? std::optional<BOOL>{TRUE} : std::nullopt;
        }

        BOOL WINAPI Draw(HIMAGELIST images, int index, HDC dc, int x, int y, UINT style)
        {
            const auto orig = Original<DrawSlot, decltype(&::ImageList_Draw)>();
            if (!orig) return ImageList_Draw(images, index, dc, x, y, style);
            const DWORD error = GetLastError();
            if (const auto result = TryDraw(images, dc, x, y, 0, 0, style, reinterpret_cast<void*>(orig), L"ImageList_Draw", index,
                [&](HDC memory, UINT flags) { return orig(images, index, memory, x, y, flags); })) return *result;
            SetLastError(error);
            return orig(images, index, dc, x, y, style);
        }
        BOOL WINAPI DrawEx(HIMAGELIST images, int index, HDC dc, int x, int y, int width, int height,
            COLORREF background, COLORREF foreground, UINT style)
        {
            const auto orig = Original<DrawExSlot, decltype(&::ImageList_DrawEx)>();
            if (!orig) return ImageList_DrawEx(images, index, dc, x, y, width, height, background, foreground, style);
            const DWORD error = GetLastError();
            if (const auto result = TryDraw(images, dc, x, y, width, height, style, reinterpret_cast<void*>(orig), L"ImageList_DrawEx", index,
                [&](HDC memory, UINT flags) { return orig(images, index, memory, x, y, width, height, background, foreground, flags); })) return *result;
            SetLastError(error);
            return orig(images, index, dc, x, y, width, height, background, foreground, style);
        }
        BOOL WINAPI DrawIndirect(IMAGELISTDRAWPARAMS* params)
        {
            const auto orig = Original<IndirectSlot, decltype(&::ImageList_DrawIndirect)>();
            if (!orig) return ImageList_DrawIndirect(params);
            if (!params || params->cbSize != sizeof(*params)) return orig(params);
            const DWORD error = GetLastError();
            if (const auto result = TryDraw(params->himl, params->hdcDst, params->x, params->y, params->cx, params->cy, params->fStyle,
                reinterpret_cast<void*>(orig), L"ImageList_DrawIndirect", params->i,
                [&](HDC memory, UINT flags) {
                    auto work = *params;
                    work.hdcDst = memory; work.fStyle = flags;
                    return orig(&work);
                })) return *result;
            SetLastError(error);
            return orig(params);
        }
    }

    void Startup()
    {
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire)) return;
        // The EXE outlives these hooks. Do not retain slots into unloadable BPLs.
        g_dispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule;
        g_dispatcher.CacheHookData();
        for (size_t i = 0; i < HookCount; ++i) g_dispatcher.EnableHook(i, true);
        // Register before paint-session hooks; keep their effective originals.
        g_fillDispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule;
        g_fillDispatcher.CacheHookData();
        g_fillDispatcher.EnableHook(0, true);
        g_shutdown.store(false, std::memory_order_release);
    }
    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_fillDispatcher.DisableAllHooks();
        g_dispatcher.DisableAllHooks();
    }
}
