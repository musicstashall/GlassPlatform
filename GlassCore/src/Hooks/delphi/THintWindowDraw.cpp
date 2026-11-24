// Delphi THintWindow: the window VCL shows a control's Hint in (TApplication
// creates one and reuses it). The tooltip subsystem attaches to it as to a
// tooltip and gives it the backdrop, but the window is painted by VCL, with
// code of the EXE and not of comctl32, so the tooltip hooks never see it:
//   - WM_ERASEBKGND: TWinControl.WMEraseBkgnd fills the client with the hint
//     colour (FillRect, the brush of Color);
//   - WM_NCPAINT: THintWindow.NCPaint draws an edge around the whole window
//     into a window DC;
//   - WM_PAINT: THintWindow.Paint draws the themed tooltip face where themes
//     are on (a gradient under a VCL style, nothing in old versions) and then
//     the caption with one DrawText, wide or ANSI by the version of Delphi.
// On glass the fill and the face are opaque blocks over the backdrop, and the
// glyphs land with A=0.
//
// Here the erase and the edge are answered instead of VCL with the transparent
// base. In WM_PAINT VCL paints as it does; when its caption reaches the EXE's
// DrawText import for the window's own DC, the client is cleared once - of
// whatever was laid under the caption in that paint - and the caption is drawn
// through the shared alpha text renderer in the tooltip text colour.
//
// VCL shows a hint with AnimateWindow when the system animates tooltips
// (THintWindow.ActivateHint asks SPI_GETTOOLTIPANIMATION first). That
// animation plays a picture the window printed with WM_PRINT, and VCL paints
// that one inside itself (PaintTo), past the subclass and past the paint
// session: the opaque hint would be shown first and the glass one after it.
// So while the thread has a hint window that is painted here, the EXE is told
// that tooltips are not animated, and VCL just shows the window.
//
// The imports of the EXE are taken when the first hint window is attached and
// held until Shutdown: what answers the erase must be able to draw the caption
// later, and the answer about the animation is asked before the window is
// shown. The gate is the exact window class registered by the EXE itself; no
// application is named.
#include "CorePch.h"
#include <Hooks/delphi/THintWindowDraw.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/Settings.h>
#include <Subsystems/TooltipContext.h>
#include <DebugLog.h>
#include <wil.h>
#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace GlassCore::DelphiHint
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;

        enum : size_t { TextW, TextA, ParamW, ParamA, Count };
        int WINAPI HintDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
        int WINAPI HintDrawTextA(HDC, LPCSTR, int, LPRECT, UINT);
        BOOL WINAPI HintParametersW(UINT, UINT, PVOID, UINT);
        BOOL WINAPI HintParametersA(UINT, UINT, PVOID, UINT);
        const HookDispatcherDependency<Count, 3> g_dep{{
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "DrawTextW", reinterpret_cast<void*>(HintDrawTextW) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "DrawTextA", reinterpret_cast<void*>(HintDrawTextA) },
            // A Delphi EXE imports these two from user32.dll by name.
            std::tuple{ std::array{ "user32.dll"sv, ""sv, ""sv },
                "SystemParametersInfoW", reinterpret_cast<void*>(HintParametersW) },
            std::tuple{ std::array{ "user32.dll"sv, ""sv, ""sv },
                "SystemParametersInfoA", reinterpret_cast<void*>(HintParametersA) },
        }};
        HookDispatcher<Count, 3> g_dispatcher{g_dep, nullptr}; // the EXE's import table
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        std::atomic<bool> g_armed{false};   // the imports were asked for
        std::atomic<bool> g_text{false};    // ...and a DrawText of the EXE is held: a caption can be drawn
        struct Session
        {
            HWND hwnd{};
            UINT message{};
            bool cleared{};                 // the client was cleared in this paint
            unsigned texts{};               // captions drawn in it
        };
        thread_local std::vector<Session> g_sessions;
        thread_local HWND g_hint{};         // the hint window of this thread that was attached last
        thread_local bool g_inside = false;

        // The exact VCL class, registered by the EXE itself - VCL is linked into
        // it, and only then does its drawing go through the EXE's imports - of
        // this thread, laid out left to right.
        bool IsHint(HWND hwnd) noexcept
        {
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            wchar_t name[32]{};
            return exe && hwnd && GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId()
                && GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))
                && _wcsicmp(name, L"THintWindow") == 0
                && reinterpret_cast<HMODULE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe
                && !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL);
        }

        // Once. True: a DrawText of the EXE is held, a caption can be drawn.
        // The answer about the animation is taken only together with it.
        bool Arm() noexcept
        {
            if (g_armed.load(std::memory_order_acquire)) return g_text.load(std::memory_order_acquire);
            auto lock = g_lock.lock_exclusive();
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            if (g_shutdown.load(std::memory_order_acquire) || !exe) return false;
            if (!g_armed.load(std::memory_order_acquire))
            {
                g_dispatcher.moduleAddress = exe;
                // A reference is counted only when the dispatcher really took it.
                const auto take = [](size_t slot) {
                    const int before = g_dispatcher.GetRef(slot);
                    g_dispatcher.EnableHook(slot, true);
                    return g_dispatcher.GetRef(slot) > before;
                };
                const bool wide = take(TextW), ansi = take(TextA);
                const bool text = wide || ansi;
                [[maybe_unused]] bool parameters = false;
                if (text)
                {
                    const bool wideParameters = take(ParamW), ansiParameters = take(ParamA);
                    parameters = wideParameters || ansiParameters;
                }
                g_text.store(text, std::memory_order_release);
                g_armed.store(true, std::memory_order_release);
#if defined(_DEBUG)
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiHint] armed exe=0x%p textW=%d textA=%d parameters=%d",
                    exe, wide ? 1 : 0, ansi ? 1 : 0, parameters ? 1 : 0);
#endif
            }
            return g_text.load(std::memory_order_acquire);
        }

        // A hint window whose caption can be drawn here.
        bool Served(HWND hwnd) noexcept
        {
            return !g_shutdown.load(std::memory_order_acquire) && IsHint(hwnd)
                && !GlassCore::Settings::IsTooltipCustomizationDisabled() && Arm();
        }

        // The clear and the renderer work in device units of the window.
        bool DeviceUnits(HDC dc) noexcept
        {
            POINT viewport{}, window{};
            return GetMapMode(dc) == MM_TEXT && GetViewportOrgEx(dc, &viewport) && GetWindowOrgEx(dc, &window)
                && !viewport.x && !viewport.y && !window.x && !window.y;
        }
        bool OwnDc(HWND hwnd, HDC dc) noexcept
        {
            return dc && GetObjectType(dc) == OBJ_DC && WindowFromDC(dc) == hwnd;
        }

        // The erase Windows sent with a DC of the window: the transparent base
        // over the client, where VCL would fill it with the hint colour.
        bool ClearClient(HWND hwnd, HDC dc) noexcept
        {
            RECT client{};
            return OwnDc(hwnd, dc) && DeviceUnits(dc) && GetClientRect(hwnd, &client) && !IsRectEmpty(&client)
                && PatBlt(dc, client.left, client.top, client.right - client.left, client.bottom - client.top, BLACKNESS);
        }

        // The frame, where VCL would draw an edge around the whole window: the
        // base around the client. A window without a frame has nothing drawn.
        bool ClearFrame(HWND hwnd) noexcept
        {
            RECT window{}, client{};
            POINT origin{};
            if (!GetWindowRect(hwnd, &window) || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
                return false;
            const int width = window.right - window.left, height = window.bottom - window.top;
            const int left = origin.x - window.left, top = origin.y - window.top;
            if (width <= 0 || height <= 0) return false;
            const HDC dc = GetWindowDC(hwnd);
            if (!dc) return false;
            bool cleared = false;
            if (const int saved = SaveDC(dc))
            {
                cleared = ExcludeClipRect(dc, left, top, left + client.right, top + client.bottom) != ERROR
                    && PatBlt(dc, 0, 0, width, height, BLACKNESS);
                RestoreDC(dc, saved);
            }
            ReleaseDC(hwnd, dc);
            return cleared;
        }

        // The paint session on top of this thread's, when `dc` is its window's
        // own DC: -1 otherwise. A session answered here (erase, frame) runs no
        // code of VCL, and a session below the top one is another message.
        int Target(HDC dc) noexcept
        {
            if (g_inside || g_sessions.empty() || g_shutdown.load(std::memory_order_acquire)) return -1;
            const Session& top = g_sessions.back();
            if (top.message != WM_PAINT || !OwnDc(top.hwnd, dc)) return -1;
            return static_cast<int>(g_sessions.size() - 1);
        }

        // GDI converts ANSI text with the code page of the selected font's charset.
        bool Decode(HDC dc, LPCSTR text, int count, std::wstring& wide)
        {
            const int charset = GetTextCharset(dc);
            if (charset < 0 || charset == SYMBOL_CHARSET) return false;
            UINT codepage = CP_ACP;
            if (charset != DEFAULT_CHARSET)
            {
                CHARSETINFO info{};
                if (!TranslateCharsetInfo(reinterpret_cast<DWORD*>(static_cast<ULONG_PTR>(charset)),
                    &info, TCI_SRCCHARSET)) return false;
                codepage = info.ciACP;
            }
            const int size = MultiByteToWideChar(codepage, 0, text, count, nullptr, 0);
            if (size <= 0) return false;
            wide.resize(static_cast<size_t>(size));
            if (MultiByteToWideChar(codepage, 0, text, count, wide.data(), size) != size) return false;
            if (count == -1) wide.pop_back(); // -1 includes the terminating NUL, explicit byte counts do not.
            return !wide.empty();
        }

        // `native` restores the entry LastError and calls the original once, with
        // the caller's arguments: a caption that is not drawn here is exactly
        // that one call.
        template <typename Native>
        int DrawCaption(size_t slot, HDC dc, LPCWSTR text, int count, const RECT* rect, UINT flags,
            DWORD entryError, const Native& native)
        {
            const HWND hwnd = g_sessions[slot].hwnd;
            RECT client{};
            // A measurement, a string the call may rewrite, a mapping of another
            // kind: the application's own.
            if (!text || !rect || count == 0 || count < -1 || (flags & (DT_CALCRECT | DT_MODIFYSTRING))
                || rect->right <= rect->left || rect->bottom <= rect->top || !DeviceUnits(dc)
                || !GetClientRect(hwnd, &client) || IsRectEmpty(&client))
                return native();
            const int saved = SaveDC(dc);
            if (!saved) return native();
            int result = 0;
            bool drawn = false;
            {
                g_inside = true;
                auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
                auto restore = wil::scope_exit([&]() noexcept { RestoreDC(dc, saved); });
                // Whatever VCL laid under the caption in this paint - the themed
                // tooltip face, a gradient, the hint colour of an erase that
                // came with a DC of another kind - is not glass: once per paint
                // the client becomes the base, inside the clip of the paint.
                if (!g_sessions[slot].cleared)
                    g_sessions[slot].cleared = PatBlt(dc, client.left, client.top,
                        client.right - client.left, client.bottom - client.top, BLACKNESS) != FALSE;
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, GlassCore::Tooltip::Detail::Context().rendering.color);
                // The caller's rectangle is not the compositor's working copy.
                // The rectangle of a hint is its client: DT_NOCLIP shows nothing more.
                RECT where = *rect;
                try
                {
                    drawn = SUCCEEDED(GlassCore::Text::DrawTextWithAlpha(dc, text, count, &where,
                        flags & ~static_cast<UINT>(DT_NOCLIP), result));
                }
                catch (...) { drawn = false; }
            }
            if (!drawn) return native();
            if (slot < g_sessions.size()) ++g_sessions[slot].texts;
            SetLastError(entryError);
            return result;
        }

        int WINAPI HintDrawTextW(HDC dc, LPCWSTR text, int count, LPRECT rect, UINT flags)
        {
            const DWORD entryError = GetLastError();
            decltype(&::DrawTextW) original{};
            { auto lock = g_lock.lock_shared(); original = g_dispatcher.GetOrg<TextW, decltype(&::DrawTextW)>(); }
            const auto native = [&] {
                SetLastError(entryError);
                return original ? original(dc, text, count, rect, flags) : ::DrawTextW(dc, text, count, rect, flags);
            };
            const int slot = Target(dc);
            if (slot < 0) return native();
            return DrawCaption(static_cast<size_t>(slot), dc, text, count, rect, flags, entryError, native);
        }
        int WINAPI HintDrawTextA(HDC dc, LPCSTR text, int count, LPRECT rect, UINT flags)
        {
            const DWORD entryError = GetLastError();
            decltype(&::DrawTextA) original{};
            { auto lock = g_lock.lock_shared(); original = g_dispatcher.GetOrg<TextA, decltype(&::DrawTextA)>(); }
            const auto native = [&] {
                SetLastError(entryError);
                return original ? original(dc, text, count, rect, flags) : ::DrawTextA(dc, text, count, rect, flags);
            };
            const int slot = Target(dc);
            if (slot < 0 || !text || count == 0 || count < -1) return native();
            std::wstring wide;
            bool decoded = false;
            try { decoded = Decode(dc, text, count, wide); }
            catch (...) { decoded = false; }
            if (!decoded) return native();
            return DrawCaption(static_cast<size_t>(slot), dc, wide.c_str(), static_cast<int>(wide.size()),
                rect, flags, entryError, native);
        }

        // The answer about the animation (see the header of this file). Only
        // SPI_GETTOOLTIPANIMATION, only a successful call, only for a thread
        // that has a hint window painted here; the caller's LastError is kept.
        template <size_t slot>
        BOOL Parameters(UINT action, UINT param, PVOID data, UINT flags)
        {
            decltype(&::SystemParametersInfoW) original{};
            { auto lock = g_lock.lock_shared(); original = g_dispatcher.GetOrg<slot, decltype(&::SystemParametersInfoW)>(); }
            const BOOL result = original ? original(action, param, data, flags)
                : slot == ParamW ? ::SystemParametersInfoW(action, param, data, flags)
                : ::SystemParametersInfoA(action, param, data, flags);
            if (!result || action != SPI_GETTOOLTIPANIMATION || !data) return result;
            const DWORD error = GetLastError();
            if (Served(g_hint)) *static_cast<BOOL*>(data) = FALSE;
            SetLastError(error);
            return result;
        }
        BOOL WINAPI HintParametersW(UINT action, UINT param, PVOID data, UINT flags)
        { return Parameters<ParamW>(action, param, data, flags); }
        BOOL WINAPI HintParametersA(UINT action, UINT param, PVOID data, UINT flags)
        { return Parameters<ParamA>(action, param, data, flags); }
    }

    void Startup() noexcept
    {
        auto lock = g_lock.lock_exclusive();
        if (!g_shutdown.load(std::memory_order_acquire)) return;
        // Find the imports now: the scan of the import table stays out of the first hint.
        if (const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule)
        {
            g_dispatcher.moduleAddress = exe;
            g_dispatcher.CacheHookData();
        }
        g_shutdown.store(false, std::memory_order_release);
    }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_dispatcher.DisableAllHooks();
        g_text.store(false, std::memory_order_release);
        g_armed.store(false, std::memory_order_release);
        g_sessions.clear();
        g_hint = nullptr;
    }

    void OnAttached(HWND hwnd) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !IsHint(hwnd)) return;
        g_hint = hwnd;
        Arm();
    }

    bool BeginPaint(HWND hwnd, UINT message, HDC dc) noexcept
    {
        if (message != WM_PAINT && message != WM_ERASEBKGND && message != WM_NCPAINT) return false;
        // WM_PAINT into a DC someone supplied is a print: the application's own.
        if ((message == WM_PAINT && dc) || !Served(hwnd)) return false;
        if (message == WM_ERASEBKGND && !ClearClient(hwnd, dc)) return false;
        if (message == WM_NCPAINT && !ClearFrame(hwnd)) return false;
        try { g_sessions.push_back(Session{hwnd, message}); }
        catch (...) { return false; }
        return true;
    }
    void EndPaint() noexcept
    {
        if (g_sessions.empty()) return;
        const Session session = g_sessions.back();
        g_sessions.pop_back();
#if defined(_DEBUG)
        if (session.message != WM_PAINT) return;
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 8; }
        if (!budget) return;
        --budget;
        DebugLog(DBG_NORMAL, L"[GlassCore][DelphiHint] paint hwnd=0x%p cleared=%d texts=%u",
            session.hwnd, session.cleared ? 1 : 0, session.texts);
#else
        (void)session;
#endif
    }
}
