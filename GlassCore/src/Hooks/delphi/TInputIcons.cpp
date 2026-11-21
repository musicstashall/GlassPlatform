// Native VCL ImageList icons: TButtonedEdit/TGlyph and TSearchBox's window DC.
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

        HWND IconOwner(HDC dc) noexcept
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
            // VCL's TEditButton.TGlyph is a real child TCustomControl, not a Button.
            if (HasClass(window, L"TGlyph"))
            {
                owner = GetParent(window);
                if (!HasClass(owner, L"TButtonedEdit")) return nullptr;
            }
            else if (!HasClass(window, L"TButtonedEdit") && !HasClass(window, L"TSearchBox"))
                return nullptr;
            return GlassShared::GlassGetWindowKind(owner) == GlassShared::GlassControlKind::Edit
                && GlassShared::GlassIsBackdropAllowedForHwnd(owner)
                && GlassShared::GlassIsBackdropAllowedForHwnd(window) ? owner : nullptr;
        }

        template<typename Paint> std::optional<BOOL> TryDraw(
            HIMAGELIST images, HDC dc, int x, int y, int width, int height, UINT style, void* nativeApi, Paint paint)
        {
            const HWND owner = IconOwner(dc);
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
            if (drawn)
            {
                RGBQUAD* pixels{};
                int stride{};
                if (FAILED(GetBufferedPaintBits(buffer, &pixels, &stride)) || !pixels || stride < width
                    || !GdiFlush()) return std::nullopt;
                bool hasAlpha = false;
                for (int row = 0; row < height && !hasAlpha; ++row)
                    for (int col = 0; col < width; ++col)
                        if (pixels[static_cast<size_t>(row) * stride + col].rgbReserved)
                        {
                            hasAlpha = true;
                            break;
                        }
                // A legacy renderer may ignore PRESERVEALPHA. Do not report a
                // successful invisible icon; discard it and use the native call.
                if (!hasAlpha) return std::nullopt;
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
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiInputIcon] hwnd=0x%p dc=0x%p rect=[%ld,%ld,%ld,%ld] style=0x%X draw=%d commit=0x%08lX",
                    owner, dc, bounds.left, bounds.top, bounds.right, bounds.bottom, style, drawn, committed);
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
            if (const auto result = TryDraw(images, dc, x, y, 0, 0, style, reinterpret_cast<void*>(orig),
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
            if (const auto result = TryDraw(images, dc, x, y, width, height, style, reinterpret_cast<void*>(orig),
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
            if (const auto result = TryDraw(params->himl, params->hdcDst, params->x, params->y, params->cx, params->cy, params->fStyle, reinterpret_cast<void*>(orig),
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
        g_shutdown.store(false, std::memory_order_release);
    }
    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.DisableAllHooks();
    }
}
