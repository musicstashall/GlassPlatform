// Restorator pictures. BWM RestoratorPaintHandler::Paint, caller EXE: exact
// TResTreeView -> BitBltAttach, exact TImgViewBox -> StretchBltAttach. The EXE
// copies a bitmap of its own into the paint DC - the two startup placeholders
// and every picture resource the viewer shows; on glass it lands with A=0 and
// only brightens the backdrop.
//
// While such a paint session is open both gdi32 exports are detoured. A SRCCOPY
// blit of a bitmap into that window's own DC gets an owned 32bpp copy with
// A=255, and the caller's blit then runs unchanged with that copy selected:
// the picture is drawn as it is, with everything the EXE composed it on - the
// black of the placeholders, the checkerboard RestoratorCheckerboard lays
// under a picture that has transparency. Other ROPs, DCs and callers stay
// native. Nothing is masked by brightness, no colour is keyed or cut out of
// the picture, and there is no AlphaBlend.
#include "CorePch.h"
#include <Hooks/delphi/RestoratorImages.h>
#include <Hooks/delphi/RestoratorCheckerboard.h>
#include <Core/Intercept/HookHelper.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/OpaqueBitmapSource.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassBackdropMark.h>
#include <DebugLog.h>
#include <wil.h>
#include <CommCtrl.h>
#include <atomic>
#include <cstdint>
#include <vector>

namespace GlassCore::RestoratorImages
{
    namespace
    {
        using namespace GlassCore::Hooks;

        // The picture, or a client-sized buffer holding it, up to 2560x1600.
        // This bounds the copy made per paint; a larger source stays native.
        constexpr std::uint64_t kMaxPixels = 1u << 22;

        BOOL WINAPI PictureBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
        BOOL WINAPI PictureStretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);

        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        decltype(&::BitBlt) g_actualBitBlt{};         // export, or its trampoline while attached
        decltype(&::StretchBlt) g_actualStretchBlt{};
        size_t g_refs{};                              // open leased paint sessions, all threads
        unsigned g_generation{};                      // bumped by Shutdown: older leases are void
        bool g_attached = false;                      // both detours, one transaction
        struct Session { HWND hwnd{}; bool leased{}; unsigned generation{}; };
        thread_local std::vector<Session> g_sessions;
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

        struct Blit
        {
            LPCWSTR api{};
            HWND hwnd{};
            HDC dc{}, source{};
            DWORD rop{};
            int x{}, y{}, w{}, h{}, sx{}, sy{}, sw{}, sh{};
        };

        // nullptr: the selected bitmap may be copied. Otherwise the reason it stays native.
        LPCWSTR RejectBitmap(HDC source, BITMAP& info) noexcept
        {
            const auto bitmap = static_cast<HBITMAP>(GetCurrentObject(source, OBJ_BITMAP));
            DIBSECTION dib{};
            const int size = bitmap ? GetObjectW(bitmap, sizeof(dib), &dib) : 0;
            if (size != sizeof(dib) && size != sizeof(BITMAP)) return L"source";
            info = dib.dsBm;
            if (info.bmWidth <= 0 || info.bmHeight <= 0
                || static_cast<std::uint64_t>(info.bmWidth) * static_cast<std::uint64_t>(info.bmHeight) > kMaxPixels)
                return L"size";
            // A 32bpp DIB that already carries alpha is copied exactly by the native
            // blit. A selected DDB cannot be read here; its fourth byte is ignored.
            if (size == sizeof(dib) && info.bmBitsPixel == 32 && info.bmBits && GdiFlush())
            {
                const auto* bytes = static_cast<const BYTE*>(info.bmBits);
                const size_t count = static_cast<size_t>(info.bmWidth) * static_cast<size_t>(info.bmHeight);
                for (size_t i = 0; i < count; ++i)
                    if (bytes[i * 4 + 3]) return L"alpha";
            }
            return nullptr;
        }
        LPCWSTR Reject(const Blit& blit, BITMAP& info) noexcept
        {
            if (blit.rop != SRCCOPY) return L"rop";
            if (!GlassCore::Root::IsFullInjectionMode() || !IsWindow(blit.hwnd)
                || !GlassShared::GlassIsBackdropAllowedForHwnd(blit.hwnd)) return L"mode";
            // A DC of this very window (BeginPaint's, or one a VCL canvas took with
            // GetDC) that the dispatcher tracks, with no drawing session above ours.
            // Never a memory DC: WindowFromDC has no owner for it.
            if (!blit.dc || WindowFromDC(blit.dc) != blit.hwnd
                || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(blit.hwnd)
                || !GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(blit.hwnd, blit.dc)) return L"dc";
            if (!blit.source || blit.source == blit.dc) return L"source";
            return RejectBitmap(blit.source, info);
        }
        // The donor's gate: a leased paint session on this thread and an EXE call site.
        HWND SessionWindow(void* returnAddress) noexcept
        {
            if (g_inside || g_sessions.empty() || !g_sessions.back().leased
                || g_shutdown.load(std::memory_order_acquire)) return nullptr;
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            return exe && DetourGetContainingModule(returnAddress) == exe ? g_sessions.back().hwnd : nullptr;
        }
        // `native` restores the entry LastError and calls the original once, with
        // the caller's arguments. It runs exactly once on every path.
        template <typename Native>
        BOOL DrawPicture(const Blit& blit, bool extents, const Native& native)
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            BITMAP info{};
            LPCWSTR reason = extents ? Reject(blit, info) : L"extents";
            BOOL result{}; DWORD error{};
            if (!reason)
            {
                // The A=255 copy is drawn as it is: the picture keeps what the
                // EXE composed it on, and nothing of it is cut out by colour.
                GlassCore::GdiUtils::OpaqueBitmapSource picture{blit.source};
                if (!picture) reason = L"copy";
                else { result = native(); error = GetLastError(); }
            } // The copy is deleted and the caller's bitmap selected again.
            if (reason) { result = native(); error = GetLastError(); }
            if (TakeDiag(1))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][RestoratorImages] image api=%s hwnd=0x%p dc=0x%p dcWnd=0x%p dcType=%lu src=0x%p bpp=%u size=[%ld,%ld] dst=[%d,%d,%d,%d] srcRect=[%d,%d,%d,%d] rop=0x%08X opaque=%d reason=%s result=%d",
                    blit.api, blit.hwnd, blit.dc, blit.dc ? WindowFromDC(blit.dc) : nullptr,
                    blit.dc ? GetObjectType(blit.dc) : 0ul, blit.source,
                    unsigned(info.bmBitsPixel), info.bmWidth, info.bmHeight,
                    blit.x, blit.y, blit.w, blit.h, blit.sx, blit.sy, blit.sw, blit.sh, blit.rop,
                    reason ? 0 : 1, reason ? reason : L"none", result);
            SetLastError(error);
            return result;
        }
        template <typename Fn> Fn Actual(const Fn& slot) noexcept
        {
            auto lock = g_lock.lock_shared();
            return slot;
        }
        BOOL WINAPI PictureBitBlt(HDC dc, int x, int y, int w, int h, HDC source, int sx, int sy, DWORD rop)
        {
            const DWORD entryError = GetLastError();
            void* const returnAddress = _ReturnAddress();
            const auto actual = Actual(g_actualBitBlt);
            if (!actual) return FALSE; // Never attached without it; the export would re-enter.
            const auto native = [&] {
                SetLastError(entryError);
                return actual(dc, x, y, w, h, source, sx, sy, rop);
            };
            const HWND hwnd = SessionWindow(returnAddress);
            if (!hwnd) return native();
            return DrawPicture(Blit{L"BitBlt", hwnd, dc, source, rop, x, y, w, h, sx, sy, w, h}, w && h, native);
        }
        BOOL WINAPI PictureStretchBlt(HDC dc, int x, int y, int w, int h,
            HDC source, int sx, int sy, int sw, int sh, DWORD rop)
        {
            const DWORD entryError = GetLastError();
            void* const returnAddress = _ReturnAddress();
            const auto actual = Actual(g_actualStretchBlt);
            if (!actual) return FALSE;
            const auto native = [&] {
                SetLastError(entryError);
                return actual(dc, x, y, w, h, source, sx, sy, sw, sh, rop); // scale and mirroring stay native
            };
            const HWND hwnd = SessionWindow(returnAddress);
            if (!hwnd) return native();
            return DrawPicture(Blit{L"StretchBlt", hwnd, dc, source, rop, x, y, w, h, sx, sy, sw, sh},
                w && h && sw && sh, native);
        }

        // Caller holds g_lock exclusively. Session refs are separate from the
        // attachment: a failed last Detach keeps both trampolines valid.
        bool Detach() noexcept
        {
            if (!g_attached) return true;
            const HRESULT hr = Detours::Write([] {
                Detours::Detach(reinterpret_cast<void**>(&g_actualBitBlt), reinterpret_cast<void*>(&PictureBitBlt));
                Detours::Detach(reinterpret_cast<void**>(&g_actualStretchBlt), reinterpret_cast<void*>(&PictureStretchBlt));
            });
            if (SUCCEEDED(hr)) g_attached = false;
            if (TakeDiag(2)) DebugLog(SUCCEEDED(hr) ? DBG_NORMAL : DBG_WARNING,
                L"[GlassCore][RestoratorImages] detour api=BitBlt+StretchBlt enable=0 refs=%zu attached=%d hr=0x%08X",
                g_refs, g_attached ? 1 : 0, static_cast<unsigned>(hr));
            return SUCCEEDED(hr);
        }
        bool Acquire(unsigned& generation) noexcept
        {
            auto lock = g_lock.lock_exclusive();
            if (g_shutdown.load(std::memory_order_acquire) || !g_actualBitBlt || !g_actualStretchBlt) return false;
            if (!g_attached)
            {
                const HRESULT hr = Detours::Write([] {
                    Detours::Attach(reinterpret_cast<void**>(&g_actualBitBlt), reinterpret_cast<void*>(&PictureBitBlt));
                    Detours::Attach(reinterpret_cast<void**>(&g_actualStretchBlt), reinterpret_cast<void*>(&PictureStretchBlt));
                });
                if (SUCCEEDED(hr)) g_attached = true;
                if (TakeDiag(2)) DebugLog(SUCCEEDED(hr) ? DBG_NORMAL : DBG_WARNING,
                    L"[GlassCore][RestoratorImages] detour api=BitBlt+StretchBlt enable=1 refs=%zu attached=%d hr=0x%08X",
                    g_refs + (SUCCEEDED(hr) ? 1u : 0u), g_attached ? 1 : 0, static_cast<unsigned>(hr));
                if (FAILED(hr)) return false; // No phantom lease after an aborted transaction.
            }
            ++g_refs; generation = g_generation;
            return true;
        }
        void Release(unsigned generation) noexcept
        {
            auto lock = g_lock.lock_exclusive();
            // A session of another thread may outlive Shutdown, and even a restart.
            if (generation != g_generation) return;
            if (g_refs && --g_refs == 0) Detach();
        }

        enum class Host { None, Tree, Viewer };
        Host HostOf(HWND hwnd) noexcept
        {
            wchar_t name[32]{};
            if (!hwnd || !GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))) return Host::None;
            if (_wcsicmp(name, L"TResTreeView") == 0) return Host::Tree;
            if (_wcsicmp(name, L"TImgViewBox") == 0) return Host::Viewer;
            return Host::None;
        }
        void OnBegin(HWND hwnd, HMODULE caller) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire)) return;
            const Host host = HostOf(hwnd);
            if (host == Host::None) return;
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            // A window of the EXE's own class, on glass - whoever begins its paint.
            const bool own = exe && GlassCore::Root::IsFullInjectionMode()
                && GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
                && reinterpret_cast<HMODULE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe;
            // This EXE shows its pictures itself, on glass: from now on what it
            // lays under a picture it composes is watched. A populated tree,
            // which comctl32 paints, says so as well - the picture a viewer
            // shows next is composed before that viewer paints.
            if (own) GlassCore::RestoratorCheckerboard::Arm(hwnd);
            const bool eligible = own && caller == exe;
            // comctl32 draws a populated tree; only the empty one shows a picture.
            const unsigned items = eligible && host == Host::Tree ? static_cast<unsigned>(TreeView_GetCount(hwnd)) : 0;
            // Every BeginPaint of these classes is recorded, leased or not, so that
            // OnEnd pairs with it by LIFO and never releases an outer session.
            Session session{hwnd};
            session.leased = eligible && !items && Acquire(session.generation);
            try { g_sessions.push_back(session); }
            catch (...) { if (session.leased) Release(session.generation); return; }
            if (TakeDiag(0))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][RestoratorImages] Begin hwnd=0x%p class=%s caller=0x%p eligible=%d items=%u leased=%d depth=%zu",
                    hwnd, host == Host::Tree ? L"TResTreeView" : L"TImgViewBox", caller,
                    eligible ? 1 : 0, items, session.leased ? 1 : 0, g_sessions.size());
        }
        void OnEnd(HWND hwnd, HMODULE) noexcept
        {
            for (auto it = g_sessions.rbegin(); it != g_sessions.rend(); ++it)
            {
                if (it->hwnd != hwnd) continue;
                const Session session = *it;
                g_sessions.erase((it + 1).base());
                if (session.leased) Release(session.generation);
                return;
            }
        }
    }

    void Startup() noexcept
    {
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire)) return;
        // Do not replace retained trampolines if an earlier Detach failed.
        if (!g_attached)
        {
            const HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
            g_actualBitBlt = gdi32
                ? reinterpret_cast<decltype(g_actualBitBlt)>(GetProcAddress(gdi32, "BitBlt")) : nullptr;
            g_actualStretchBlt = gdi32
                ? reinterpret_cast<decltype(g_actualStretchBlt)>(GetProcAddress(gdi32, "StretchBlt")) : nullptr;
            DebugLog(g_actualBitBlt && g_actualStretchBlt ? DBG_NORMAL : DBG_WARNING,
                L"[GlassCore][RestoratorImages] resolve gdi32=0x%p BitBlt=0x%p StretchBlt=0x%p",
                gdi32, reinterpret_cast<void*>(g_actualBitBlt), reinterpret_cast<void*>(g_actualStretchBlt));
        }
        GlassCore::PaintDispatcher::RegisterPaintProvider({OnBegin, OnEnd});
        g_shutdown.store(false, std::memory_order_release);
    }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_refs = 0; ++g_generation;
        Detach(); // Also retries a failed last-session Detach.
        g_sessions.clear();
    }
}
