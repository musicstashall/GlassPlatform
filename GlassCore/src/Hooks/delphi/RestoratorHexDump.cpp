// Restorator hex viewer. BWM RestoratorPaintHandler::Paint (window, caller EXE):
// exact THexDump -> TabbedTextOutAttach + ExtTextOutAttach. The EXE writes every
// row with ANSI GDI text straight into its paint DC: the address and the
// character column through ExtTextOutA, the bytes through TabbedTextOutA. On
// glass those glyphs land with A=0 and are not visible.
//
// While such a paint session is open both exports are detoured. A call from the
// EXE into that window's own DC keeps the caller's position, clipping, tab stops
// and colours; only the glyphs go through the shared alpha text renderer. An
// opaque text background is cleared when it is the window's own (black or a
// system colour) and filled opaque when it is any other colour. Other DCs and
// callers, and layouts this module cannot place exactly, stay native.
#include "CorePch.h"
#include <Hooks/delphi/RestoratorHexDump.h>
#include <Core/Intercept/HookHelper.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassBackdropMark.h>
#include <WinApiUtils.h>
#include <DebugLog.h>
#include <wil.h>
#include <atomic>
#include <climits>
#include <string>
#include <vector>

namespace GlassCore::RestoratorHexDump
{
    namespace
    {
        using namespace GlassCore::Hooks;

        BOOL WINAPI RowExtTextOutA(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
        LONG WINAPI RowTabbedTextOutA(HDC, int, int, LPCSTR, int, int, const INT*, int);

        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        decltype(&::ExtTextOutA) g_actualExtTextOut{};       // export, or its trampoline while attached
        decltype(&::TabbedTextOutA) g_actualTabbedTextOut{};
        size_t g_refs{};                                     // open leased paint sessions, all threads
        unsigned g_generation{};                             // bumped by Shutdown: older leases are void
        bool g_attached = false;                             // both detours, one transaction
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

        // nullptr: text for this DC is drawn here. Otherwise the reason it stays native.
        LPCWSTR RejectTarget(HWND hwnd, HDC dc) noexcept
        {
            if (!GlassCore::Root::IsFullInjectionMode() || !IsWindow(hwnd)
                || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)) return L"mode";
            // The window's own DC (BeginPaint's, or one its canvas took with GetDC),
            // or a memory DC the dispatcher saw this window create while painting,
            // with no drawing session above ours. An open paint session alone
            // never authorizes a DC.
            const HWND owner = dc ? WindowFromDC(dc) : nullptr;
            const bool own = dc && (owner == hwnd || (!owner && GetObjectType(dc) == OBJ_MEMDC
                && GlassCore::PaintDispatcher::GetStackOwnerForDc(dc) == hwnd));
            if (!own || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(hwnd)
                || !GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(hwnd, dc)) return L"dc";
            // The renderer places its buffer in device units.
            if (GetMapMode(dc) != MM_TEXT) return L"map";
            return nullptr;
        }

        // GDI converts ANSI text with the code page of the selected font's charset.
        bool Decode(HDC dc, LPCSTR text, int count, std::wstring& wide, UINT& codepage)
        {
            const int charset = GetTextCharset(dc);
            if (charset < 0 || charset == SYMBOL_CHARSET) return false;
            codepage = CP_ACP;
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
            return MultiByteToWideChar(codepage, 0, text, count, wide.data(), size) == size;
        }

        // What an opaque text background becomes on glass, as in BWM's ANSI text
        // hooks: black, COLOR_WINDOW and COLOR_BTNFACE are the window's own
        // background, which is transparent here; any other colour is a highlight
        // and is filled fully opaque. This is the background of one text call,
        // not a colour key over pixels.
        bool Background(HDC dc, const RECT& r, COLORREF color)
        {
            const long long width = static_cast<long long>(r.right) - r.left;
            const long long height = static_cast<long long>(r.bottom) - r.top;
            if (width <= 0 || height <= 0) return true; // nothing to fill
            if (width > INT_MAX || height > INT_MAX) return false;
            const COLORREF rgb = color & 0xFFFFFFu;
            if (!rgb || rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu)
                || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu))
                return PatBlt(dc, r.left, r.top, static_cast<int>(width), static_cast<int>(height), BLACKNESS) != FALSE;
            wil::unique_hbrush brush{Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(rgb, 255)};
            return brush && FillRect(dc, &r, brush.get()) != 0;
        }

        // user32 writes a tabbed string run by run: a run of text starts where the
        // previous one ended or, after n tabs, at the n-th tab stop beyond that
        // point. With a single distance the stops are origin + k * step. The runs
        // become one glyph string with explicit advances, so that one renderer
        // call places every glyph where the native call would.
        bool Layout(HDC dc, const std::wstring& text, int x, int step, int origin,
            std::wstring& glyphs, std::vector<INT>& advance, int& first, int& width)
        {
            long long cursor = x; // where the next run starts unless tabs move it
            std::vector<INT> partial;
            first = x;
            for (size_t i = 0; i < text.size();)
            {
                long long tabs = 0;
                while (i < text.size() && text[i] == L'\t') { ++tabs; ++i; }
                const size_t begin = i;
                while (i < text.size() && text[i] != L'\t') ++i;
                const size_t length = i - begin;
                if (cursor < origin || length > INT_MAX) return false;
                const long long start = tabs ? origin + ((cursor - origin) / step + tabs) * step : cursor;
                if (start > INT_MAX) return false;
                long long run = 0;
                if (length)
                {
                    SIZE size{};
                    partial.assign(length, 0);
                    if (!GetTextExtentExPointW(dc, text.data() + begin, static_cast<int>(length), 0, nullptr,
                        partial.data(), &size)) return false;
                    if (advance.empty()) first = static_cast<int>(start);
                    else
                    {
                        // The last glyph of the previous run reaches up to this run.
                        const long long reach = advance.back() + (start - cursor);
                        if (reach > INT_MAX) return false;
                        advance.back() = static_cast<INT>(reach);
                    }
                    for (size_t k = 0; k < length; ++k)
                    {
                        // The run is as wide as GDI measures it; the glyphs inside
                        // it stand at their partial extents.
                        const long long after = k + 1 < length ? partial[k] : size.cx;
                        if (after < run) return false;
                        advance.push_back(static_cast<INT>(after - run));
                        run = after;
                    }
                    glyphs.append(text, begin, length);
                }
                cursor = start + run;
                if (cursor > INT_MAX) return false;
            }
            width = static_cast<int>(cursor - x);
            return true;
        }

        struct Row
        {
            LPCWSTR api{};
            HWND hwnd{};
            HDC dc{};
            int x{}, y{};
            UINT options{};
            RECT rect{};          // the caller's rectangle, or the extent of a tabbed row
            int ansiCount{};
            size_t wideCount{};
            UINT codepage{};
            int tabCount{}, step{}, origin{};
        };
        // Metadata only: the text of a row is file content and is never logged.
        void Report(const Row& row, LPCWSTR reason, long result) noexcept
        {
            if (!TakeDiag(1)) return;
            const HDC dc = row.dc;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][RestoratorHexDump] text api=%s hwnd=0x%p dc=0x%p dcWnd=0x%p dcType=%lu pos=[%d,%d] options=0x%X rect=[%ld,%ld,%ld,%ld] ansiCount=%d wideCount=%zu cp=%u align=0x%X bkMode=%d bk=0x%06X color=0x%06X tabs=%d step=%d origin=%d drawn=%d reason=%s result=%ld",
                row.api, row.hwnd, dc, dc ? WindowFromDC(dc) : nullptr, dc ? GetObjectType(dc) : 0ul,
                row.x, row.y, row.options, row.rect.left, row.rect.top, row.rect.right, row.rect.bottom,
                row.ansiCount, row.wideCount, row.codepage,
                dc ? GetTextAlign(dc) : 0u, dc ? GetBkMode(dc) : 0,
                dc ? GetBkColor(dc) & 0xFFFFFFu : 0u, dc ? GetTextColor(dc) & 0xFFFFFFu : 0u,
                row.tabCount, row.step, row.origin, reason ? 0 : 1, reason ? reason : L"none", result);
        }

        HWND SessionWindow(void* returnAddress) noexcept
        {
            if (g_inside || g_sessions.empty() || !g_sessions.back().leased
                || g_shutdown.load(std::memory_order_acquire)) return nullptr;
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            return exe && DetourGetContainingModule(returnAddress) == exe ? g_sessions.back().hwnd : nullptr;
        }

        // `native` restores the entry LastError and calls the original once, with
        // the caller's arguments. A rejected call is exactly that one call; after a
        // background fill it is a repaint by the original, not a pixel rollback.
        template <typename Native>
        BOOL DrawExtText(HWND hwnd, HDC dc, int x, int y, UINT options, const RECT* rect,
            LPCSTR text, UINT count, const INT* dx, DWORD entryError, const Native& native)
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            std::wstring wide;
            UINT codepage{};
            LPCWSTR reason = RejectTarget(hwnd, dc);
            // Glyph indices and paired offsets are not ANSI text.
            if (!reason && (options & (ETO_GLYPH_INDEX | ETO_PDY))) reason = L"options";
            if (!reason && (options & ETO_OPAQUE) && !rect) reason = L"rect";
            if (!reason && !count && !(options & ETO_OPAQUE)) reason = L"empty";
            // The renderer draws in a buffer DC of its own, which carries the font
            // and colour but not the caller's alignment, current position or
            // character spacing: text that depends on them would be misplaced.
            if (!reason && count && GetTextAlign(dc) != (TA_LEFT | TA_TOP | TA_NOUPDATECP)) reason = L"align";
            if (!reason && count && !dx && GetTextCharacterExtra(dc) != 0) reason = L"spacing";
            if (!reason && count)
            {
                bool decoded = false;
                try { decoded = text && count <= INT_MAX && Decode(dc, text, static_cast<int>(count), wide, codepage); }
                catch (...) { decoded = false; }
                if (!decoded) reason = L"conversion";
                // Advances given per byte do not map onto fewer characters.
                else if (dx && wide.size() != count) reason = L"dx";
            }
            if (!reason && (options & ETO_OPAQUE))
            {
                bool filled = false;
                try { filled = Background(dc, *rect, GetBkColor(dc)); }
                catch (...) { filled = false; }
                if (!filled) reason = L"background";
            }
            if (!reason && count)
            {
                HRESULT hr = E_FAIL;
                try
                {
                    // Position, clipping rectangle and remaining options are the caller's.
                    hr = GlassCore::Text::ExtTextOutWithAlpha(dc, x, y, options & ~static_cast<UINT>(ETO_OPAQUE),
                        rect, wide.data(), static_cast<UINT>(wide.size()), dx, &::ExtTextOutW);
                }
                catch (...) { hr = E_FAIL; }
                if (FAILED(hr)) reason = L"renderer";
            }
            BOOL result = TRUE;
            DWORD error = entryError;
            if (reason) { result = native(); error = GetLastError(); }
            Row row{L"ExtTextOutA", hwnd, dc, x, y, options};
            if (rect) row.rect = *rect;
            row.ansiCount = static_cast<int>(count); row.wideCount = wide.size(); row.codepage = codepage;
            Report(row, reason, result);
            SetLastError(error);
            return result;
        }

        template <typename Native>
        LONG DrawTabbedText(HWND hwnd, HDC dc, int x, int y, LPCSTR text, int count,
            int tabCount, const INT* tabs, int tabOrigin, DWORD entryError, const Native& native)
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            std::wstring wide, glyphs;
            std::vector<INT> advance;
            UINT codepage{};
            TEXTMETRICW metrics{};
            RECT extent{};
            int first = x, width = 0;
            LPCWSTR reason = RejectTarget(hwnd, dc);
            // One positive tab-stop distance, expanded from an origin at or left of
            // the text: the layout of a hex row, and the only one reproduced here.
            // BWM handles the call only with a tab-stop array as well.
            if (!reason && (tabCount != 1 || !tabs || tabs[0] <= 0 || x < tabOrigin)) reason = L"tabs";
            // Runs are placed left to right from (x, y); any other alignment or the
            // current position would move them. Character spacing is part of the
            // measured advances and needs no check here.
            if (!reason && GetTextAlign(dc) != (TA_LEFT | TA_TOP | TA_NOUPDATECP)) reason = L"align";
            if (!reason)
            {
                bool decoded = false;
                try { decoded = text && count > 0 && Decode(dc, text, count, wide, codepage); }
                catch (...) { decoded = false; }
                if (!decoded) reason = L"conversion";
            }
            // One glyph string would draw an underline across the tab gaps.
            if (!reason && (!GetTextMetricsW(dc, &metrics) || metrics.tmHeight <= 0
                || metrics.tmUnderlined || metrics.tmStruckOut)) reason = L"font";
            if (!reason)
            {
                bool placed = false;
                try { placed = Layout(dc, wide, x, tabs[0], tabOrigin, glyphs, advance, first, width); }
                catch (...) { placed = false; }
                const long long bottom = static_cast<long long>(y) + metrics.tmHeight;
                if (!placed || bottom > INT_MAX) reason = L"layout";
                else extent = RECT{x, y, x + width, static_cast<LONG>(bottom)};
            }
            // In OPAQUE mode user32 fills behind every run and the tab gap before it.
            if (!reason && GetBkMode(dc) == OPAQUE)
            {
                bool filled = false;
                try { filled = Background(dc, extent, GetBkColor(dc)); }
                catch (...) { filled = false; }
                if (!filled) reason = L"background";
            }
            if (!reason && !glyphs.empty())
            {
                HRESULT hr = E_FAIL;
                try
                {
                    // The rectangle only bounds the renderer's buffer: without
                    // ETO_CLIPPED and ETO_OPAQUE GDI ignores it.
                    hr = GlassCore::Text::ExtTextOutWithAlpha(dc, first, y, 0, &extent, glyphs.data(),
                        static_cast<UINT>(glyphs.size()), advance.data(), &::ExtTextOutW);
                }
                catch (...) { hr = E_FAIL; }
                if (FAILED(hr)) reason = L"renderer";
            }
            // user32 returns the extent of the string: width low, height high.
            LONG result = MAKELONG(width, metrics.tmHeight);
            DWORD error = entryError;
            if (reason) { result = native(); error = GetLastError(); }
            Row row{L"TabbedTextOutA", hwnd, dc, x, y, 0, extent};
            row.ansiCount = count; row.wideCount = wide.size(); row.codepage = codepage;
            row.tabCount = tabCount; row.step = tabs && tabCount > 0 ? tabs[0] : 0; row.origin = tabOrigin;
            Report(row, reason, result);
            SetLastError(error);
            return result;
        }

        template <typename Fn> Fn Actual(const Fn& slot) noexcept
        {
            auto lock = g_lock.lock_shared();
            return slot;
        }
        BOOL WINAPI RowExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,
            LPCSTR text, UINT count, const INT* dx)
        {
            const DWORD entryError = GetLastError();
            void* const returnAddress = _ReturnAddress();
            const auto actual = Actual(g_actualExtTextOut);
            if (!actual) return FALSE; // Never attached without it; the export would re-enter.
            const auto native = [&] {
                SetLastError(entryError);
                return actual(dc, x, y, options, rect, text, count, dx);
            };
            const HWND hwnd = SessionWindow(returnAddress);
            if (!hwnd) return native();
            return DrawExtText(hwnd, dc, x, y, options, rect, text, count, dx, entryError, native);
        }
        LONG WINAPI RowTabbedTextOutA(HDC dc, int x, int y, LPCSTR text, int count,
            int tabCount, const INT* tabs, int tabOrigin)
        {
            const DWORD entryError = GetLastError();
            void* const returnAddress = _ReturnAddress();
            const auto actual = Actual(g_actualTabbedTextOut);
            if (!actual) return 0;
            const auto native = [&] {
                SetLastError(entryError);
                return actual(dc, x, y, text, count, tabCount, tabs, tabOrigin);
            };
            const HWND hwnd = SessionWindow(returnAddress);
            if (!hwnd) return native();
            return DrawTabbedText(hwnd, dc, x, y, text, count, tabCount, tabs, tabOrigin, entryError, native);
        }

        // Caller holds g_lock exclusively. Session refs are separate from the
        // attachment: a failed last Detach keeps both trampolines valid.
        bool Detach() noexcept
        {
            if (!g_attached) return true;
            const HRESULT hr = Detours::Write([] {
                Detours::Detach(reinterpret_cast<void**>(&g_actualExtTextOut), reinterpret_cast<void*>(&RowExtTextOutA));
                Detours::Detach(reinterpret_cast<void**>(&g_actualTabbedTextOut), reinterpret_cast<void*>(&RowTabbedTextOutA));
            });
            if (SUCCEEDED(hr)) g_attached = false;
            if (TakeDiag(2)) DebugLog(SUCCEEDED(hr) ? DBG_NORMAL : DBG_WARNING,
                L"[GlassCore][RestoratorHexDump] detour api=ExtTextOutA+TabbedTextOutA enable=0 refs=%zu attached=%d hr=0x%08X",
                g_refs, g_attached ? 1 : 0, static_cast<unsigned>(hr));
            return SUCCEEDED(hr);
        }
        bool Acquire(unsigned& generation) noexcept
        {
            auto lock = g_lock.lock_exclusive();
            if (g_shutdown.load(std::memory_order_acquire) || !g_actualExtTextOut || !g_actualTabbedTextOut) return false;
            if (!g_attached)
            {
                const HRESULT hr = Detours::Write([] {
                    Detours::Attach(reinterpret_cast<void**>(&g_actualExtTextOut), reinterpret_cast<void*>(&RowExtTextOutA));
                    Detours::Attach(reinterpret_cast<void**>(&g_actualTabbedTextOut), reinterpret_cast<void*>(&RowTabbedTextOutA));
                });
                if (SUCCEEDED(hr)) g_attached = true;
                if (TakeDiag(2)) DebugLog(SUCCEEDED(hr) ? DBG_NORMAL : DBG_WARNING,
                    L"[GlassCore][RestoratorHexDump] detour api=ExtTextOutA+TabbedTextOutA enable=1 refs=%zu attached=%d hr=0x%08X",
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

        bool IsHexDump(HWND hwnd) noexcept
        {
            wchar_t name[32]{};
            return hwnd && GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))
                && _wcsicmp(name, L"THexDump") == 0;
        }
        void OnBegin(HWND hwnd, HMODULE caller) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || !IsHexDump(hwnd)) return;
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            const bool eligible = exe && caller == exe && GlassCore::Root::IsFullInjectionMode()
                && GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)
                && reinterpret_cast<HMODULE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)) == exe;
            // Every BeginPaint of this class is recorded, leased or not, so that
            // OnEnd pairs with it by LIFO and never releases an outer session.
            Session session{hwnd};
            session.leased = eligible && Acquire(session.generation);
            try { g_sessions.push_back(session); }
            catch (...) { if (session.leased) Release(session.generation); return; }
            if (TakeDiag(0))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][RestoratorHexDump] Begin hwnd=0x%p caller=0x%p eligible=%d leased=%d depth=%zu",
                    hwnd, caller, eligible ? 1 : 0, session.leased ? 1 : 0, g_sessions.size());
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
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            g_actualExtTextOut = gdi32
                ? reinterpret_cast<decltype(g_actualExtTextOut)>(GetProcAddress(gdi32, "ExtTextOutA")) : nullptr;
            g_actualTabbedTextOut = user32
                ? reinterpret_cast<decltype(g_actualTabbedTextOut)>(GetProcAddress(user32, "TabbedTextOutA")) : nullptr;
            DebugLog(g_actualExtTextOut && g_actualTabbedTextOut ? DBG_NORMAL : DBG_WARNING,
                L"[GlassCore][RestoratorHexDump] resolve gdi32=0x%p ExtTextOutA=0x%p user32=0x%p TabbedTextOutA=0x%p",
                gdi32, reinterpret_cast<void*>(g_actualExtTextOut),
                user32, reinterpret_cast<void*>(g_actualTabbedTextOut));
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
