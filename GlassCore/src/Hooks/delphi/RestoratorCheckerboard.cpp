// The background of a picture the EXE composes. BWM RestoratorWinHooks, caller
// EXE: MyCreateDIBSection keeps the size of the last DIB section the EXE
// created - the picture it has just read, or the bitmap it is about to draw it
// on - and MyFillRect replaces the solid fill of exactly that rectangle in a DC
// without a window by white under a GDI+ large checker board. There GetSysColor
// gives the EXE black for its window, so the squares (the brush's colour at
// alpha 50 over white) come out RGB(205,205,205).
//
// Here the EXE keeps the colours of the system, and that is one more gate: the
// fill must be a solid brush of the button face - the colour of the window,
// which a viewer lays under a picture where the window should show through.
// The fill is then made by the same call with a pattern brush: RGB(205,205,205)
// and white in squares of four pixels. The EXE draws its picture over it, and
// RestoratorImages / TPanelDraw later show that bitmap opaque, as it is.
//
// Both hooks are the EXE's own imports - no other module's call is seen - and
// both are held from the first picture viewer that paints on glass (Arm) until
// Shutdown: a picture is composed when its resource is chosen, outside every
// paint session and with no window to ask. The top-level window of the viewer
// that painted last answers for the glass instead.
#include "CorePch.h"
#include <Hooks/delphi/RestoratorCheckerboard.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>

namespace GlassCore::RestoratorCheckerboard
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;
        enum : size_t { FillSlot, DibSlot, Count };
        int WINAPI BoardFill(HDC, const RECT*, HBRUSH);
        HBITMAP WINAPI NoteDib(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD);
        const HookDispatcherDependency<Count, 3> g_dep{{
            std::tuple{std::array{"user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv},
                "FillRect", reinterpret_cast<PVOID>(BoardFill)},
            std::tuple{std::array{"gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv},
                "CreateDIBSection", reinterpret_cast<PVOID>(NoteDib)},
        }};
        HookDispatcher<Count, 3> g_dispatcher{g_dep, nullptr}; // the EXE's import table
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        std::atomic<bool> g_armed{false};          // the imports were asked for: once per start, held or not
        // The top-level window of the viewer that painted on glass last.
        std::atomic<HWND> g_root{nullptr};
        // The donor's imgRect: width (low half) and height of the last DIB
        // section the EXE asked for, as it asked - a top-down one has a negative
        // height and matches no fill.
        std::atomic<std::uint64_t> g_lastDib{0};

        constexpr std::uint64_t Pack(LONG width, LONG height) noexcept
        {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(height)) << 32)
                | static_cast<std::uint32_t>(width);
        }
        constexpr LONG Width(std::uint64_t size) noexcept { return static_cast<LONG>(static_cast<std::uint32_t>(size)); }
        constexpr LONG Height(std::uint64_t size) noexcept { return static_cast<LONG>(static_cast<std::uint32_t>(size >> 32)); }

        // 0 a board, 1 the window's colour in another rectangle, 2 another brush
        // in the picture's rectangle, 3 a refusal or a failure after both matched.
        bool TakeDiag(size_t channel) noexcept
        {
            static thread_local ULONGLONG epoch[4]{};
            static thread_local unsigned budget[4]{};
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

        // The donor's HatchStyleLargeCheckerBoard: squares of four pixels. As a
        // pattern it is one 8x8 tile - a packed 24bpp DIB, bottom-up, its rows
        // 24 bytes each. No alpha is written with it: the bitmap is made opaque
        // when it is shown.
        constexpr int kSquare = 4, kTile = 2 * kSquare;
        constexpr BYTE kLight = 255, kDark = 205;  // white, and black at alpha 50 over it
        struct Tile { BITMAPINFOHEADER header; BYTE bits[kTile * kTile * 3]; };
        static_assert(offsetof(Tile, bits) == sizeof(BITMAPINFOHEADER) && (kTile * 3) % 4 == 0,
            "a packed DIB: the pixels follow the header, in rows of whole DWORDs");
        [[nodiscard]] HBRUSH CreateBoardBrush() noexcept
        {
            Tile tile{};
            tile.header.biSize = sizeof(tile.header);
            tile.header.biWidth = kTile;
            tile.header.biHeight = kTile;
            tile.header.biPlanes = 1;
            tile.header.biBitCount = 24;
            tile.header.biCompression = BI_RGB;
            for (int y = 0; y < kTile; ++y)
                for (int x = 0; x < kTile; ++x)
                {
                    // The square at the origin is the dark one; the first row in memory is the lowest.
                    const BYTE level = (x / kSquare + y / kSquare) % 2 == 0 ? kDark : kLight;
                    BYTE* const pixel = tile.bits + static_cast<size_t>(((kTile - 1 - y) * kTile + x) * 3);
                    pixel[0] = pixel[1] = pixel[2] = level;
                }
            return CreateDIBPatternBrushPt(&tile, DIB_RGB_COLORS);
        }

        // What a fill is made with. FillRect also takes a system colour index,
        // plus one, in place of a brush: that is not an object, and not a brush
        // the EXE made.
        struct BrushInfo { bool solid; COLORREF colour; int style; };
        [[nodiscard]] BrushInfo BrushOf(HBRUSH brush) noexcept
        {
            LOGBRUSH lb{};
            if (reinterpret_cast<ULONG_PTR>(brush) <= COLOR_MENUBAR + 1u
                || GetObjectW(brush, sizeof(lb), &lb) != sizeof(lb)) return {false, 0, -1};
            return {lb.lbStyle == BS_SOLID, lb.lbColor & 0xFFFFFFu, static_cast<int>(lb.lbStyle)};
        }
        // Nothing changes in a window that is not on glass. No window is known
        // where a picture is composed, so the top-level window of the viewer
        // that painted last answers: its own backdrop mark while it lives, the
        // policy of the process once it is gone.
        [[nodiscard]] bool OnGlass(HWND root) noexcept
        {
            return GlassShared::GlassIsRootBackdropAllowed(root);
        }

        int WINAPI BoardFill(HDC dc, const RECT* rect, HBRUSH brush)
        {
            const DWORD entryError = GetLastError();
            const auto original = Original<FillSlot, decltype(&::FillRect)>();
            // `native` restores the entry LastError and makes the caller's own call.
            const auto native = [&] {
                SetLastError(entryError);
                return original ? original(dc, rect, brush) : ::FillRect(dc, rect, brush);
            };
            if (!original || !rect || g_shutdown.load(std::memory_order_acquire)
                || !GlassCore::Root::IsFullInjectionMode() || GetObjectType(dc) != OBJ_MEMDC)
                return native();
            const std::uint64_t last = g_lastDib.load(std::memory_order_acquire);
            const LONG width = Width(last), height = Height(last);
            // The donor's EqualRect(lprc, &imgRect): the whole of the bitmap the
            // EXE has just asked for, from its origin.
            const bool whole = width > 0 && height > 0 && rect->left == 0 && rect->top == 0
                && rect->right == width && rect->bottom == height;
            const BrushInfo made = BrushOf(brush);
            const bool window = made.solid && made.colour == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
            // Neither that rectangle nor the window's colour: every such fill of
            // the EXE's passes without a word.
            if (!whole && !window) return native();
            const HWND root = g_root.load(std::memory_order_acquire);
            LPCWSTR reason = nullptr;
            int result = 0; DWORD error = entryError;
            if (!whole) reason = L"size";
            else if (!window) reason = L"colour";
            else if (!OnGlass(root)) reason = L"glass";
            else if (const HBRUSH board = CreateBoardBrush(); !board) reason = L"pattern";
            else
            {
                SetLastError(entryError);
                result = original(dc, rect, board); // the same call, clip and transform: only the brush differs
                error = GetLastError();
                DeleteObject(board);
                if (!result) reason = L"fill";
            }
            // Whatever was refused, or failed, is the caller's own fill, once.
            if (reason) { result = native(); error = GetLastError(); }
            const size_t channel = !reason ? 0u : !whole ? 1u : !window ? 2u : 3u;
            if (TakeDiag(channel))
            {
                BITMAP bitmap{};
                const HGDIOBJ selected = GetCurrentObject(dc, OBJ_BITMAP);
                const bool known = selected && GetObjectW(selected, sizeof(bitmap), &bitmap) == sizeof(bitmap);
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][RestoratorCheckerboard] fill dc=0x%p rect=[%ld,%ld,%ld,%ld] dib=[%ld,%ld] brush=0x%p style=%d colour=0x%06X bitmap=0x%p bpp=%u size=[%ld,%ld] section=%d root=0x%p board=%d reason=%s result=%d",
                    dc, rect->left, rect->top, rect->right, rect->bottom, width, height,
                    brush, made.style, static_cast<unsigned>(made.colour), selected,
                    known ? unsigned(bitmap.bmBitsPixel) : 0u, known ? bitmap.bmWidth : 0, known ? bitmap.bmHeight : 0,
                    known && bitmap.bmBits ? 1 : 0, root, reason ? 0 : 1, reason ? reason : L"none", result);
            }
            SetLastError(error);
            return result;
        }
        HBITMAP WINAPI NoteDib(HDC dc, const BITMAPINFO* info, UINT usage, void** bits, HANDLE section, DWORD offset)
        {
            const DWORD entryError = GetLastError();
            const auto original = Original<DibSlot, decltype(&::CreateDIBSection)>();
            if (info && !g_shutdown.load(std::memory_order_acquire))
                g_lastDib.store(Pack(info->bmiHeader.biWidth, info->bmiHeader.biHeight), std::memory_order_release);
            SetLastError(entryError);
            return original ? original(dc, info, usage, bits, section, offset)
                : ::CreateDIBSection(dc, info, usage, bits, section, offset);
        }
        // A detour stands in its import's slot: the same signature, the calling convention included.
        static_assert(std::is_same_v<decltype(&BoardFill), decltype(&::FillRect)>
            && std::is_same_v<decltype(&NoteDib), decltype(&::CreateDIBSection)>,
            "a detour must have the signature of the function it replaces");

        // Caller holds g_lock exclusively. Both imports, or none: one without
        // the other has no size to compare, or no fill to compare it with.
        bool Take(std::array<bool, Count>& taken) noexcept
        {
            for (size_t i = 0; i < Count; ++i)
            {
                // A false EnableHook may still have taken a ref: only the delta says what is held.
                const int before = g_dispatcher.GetRef(i);
                g_dispatcher.EnableHook(i, true);
                taken[i] = g_dispatcher.GetRef(i) > before;
            }
            const bool held = taken[FillSlot] && taken[DibSlot];
            if (!held)
                for (size_t i = Count; i > 0; --i)
                    if (taken[i - 1]) g_dispatcher.EnableHook(i - 1, false);
            return held;
        }
    }

    void Arm(HWND viewer) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;
        if (const HWND root = GetAncestor(viewer, GA_ROOT))
            g_root.store(root, std::memory_order_release);
        if (g_armed.load(std::memory_order_acquire)) return;
        const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
        std::array<bool, Count> taken{};
        bool held = false;
        {
            auto lock = g_lock.lock_exclusive();
            if (!exe || g_armed.load(std::memory_order_acquire) || g_shutdown.load(std::memory_order_acquire)) return;
            g_dispatcher.moduleAddress = exe;
            held = Take(taken);
            // An import that is not there will not appear: no second attempt in this start.
            g_armed.store(true, std::memory_order_release);
        }
        DebugLog(held ? DBG_NORMAL : DBG_WARNING,
            L"[GlassCore][RestoratorCheckerboard] arm exe=0x%p root=0x%p FillRect=%d CreateDIBSection=%d held=%d",
            exe, g_root.load(std::memory_order_acquire), taken[FillSlot] ? 1 : 0, taken[DibSlot] ? 1 : 0, held ? 1 : 0);
    }
    void Startup() noexcept
    {
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire)) return;
        // A start begins with no window and no size, whatever a paint that
        // raced with the last Shutdown may have left.
        g_root.store(nullptr, std::memory_order_release);
        g_lastDib.store(0, std::memory_order_release);
        g_shutdown.store(false, std::memory_order_release);
    }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.DisableAllHooks();
        g_armed.store(false, std::memory_order_release);
    }
}
