// Delphi TResVersionEditor: a custom VCL control (the editor of a version
// resource) that an ANSI Delphi EXE paints itself in WM_PAINT. VCL writes text
// with ExtTextOutA (TCanvas.TextRect / TextOut) and fills with FillRect and
// the brushes of the control's colours. On glass the glyphs land with A=0 and
// are not visible, and a fill of the default colour is an opaque block.
//
// BWM attaches ExtTextOutA for this class, and for the whole process it
// returns black for COLOR_WINDOW and COLOR_BTNFACE, so that every brush VCL
// makes of the control's own colours is black. Here nothing is changed for the
// process. While the EXE paints such a window, two imports of the EXE are
// taken together:
//   - ExtTextOutA into the window's own DC keeps the caller's position,
//     clipping and colours; only the glyphs go through the shared alpha text
//     renderer. An opaque text background is cleared when it is the window's
//     own (black or a system colour) and filled opaque when it is any other;
//   - FillRect into that DC with a solid brush of the default window or
//     button-face colour is cleared. VCL caches its brushes, so remapping a
//     colour for one paint would not reach them: the fill itself is answered.
// Other DCs, other brushes and calls this module cannot place exactly stay
// native. The gate is the window class and the EXE that paints it; no
// application is named and no process name is read.
#include "CorePch.h"
#include <Hooks/delphi/TResVersionEditorDraw.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassBackdropMark.h>
#include <WinApiUtils.h>
#include <DebugLog.h>
#include <wil.h>
#include <array>
#include <atomic>
#include <climits>
#include <string>
#include <vector>

namespace GlassCore::DelphiResVersionEditor
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;

        enum : size_t { TextSlot, FillSlot, Count };
        BOOL WINAPI CellExtTextOutA(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
        int WINAPI CellFillRect(HDC, const RECT*, HBRUSH);
        const HookDispatcherDependency<Count, 3> g_dep{{
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
                "ExtTextOutA", reinterpret_cast<void*>(CellExtTextOutA) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "FillRect", reinterpret_cast<void*>(CellFillRect) },
        }};
        HookDispatcher<Count, 3> g_dispatcher{g_dep, nullptr}; // the EXE's import table
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{true};
        std::atomic<unsigned> g_generation{};                 // bumped by Shutdown: older leases are void
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

        // nullptr: this DC carries the window's own drawing. Otherwise the reason
        // a call into it stays native.
        LPCWSTR RejectTarget(HWND hwnd, HDC dc) noexcept
        {
            if (!GlassCore::Root::IsFullInjectionMode() || !IsWindow(hwnd)
                || !GlassShared::GlassIsBackdropAllowedForHwnd(hwnd)) return L"mode";
            // A window DC is its window's own: it counts while an open session of
            // this window holds it (BeginPaint's, or one its canvas took with
            // GetDC), whichever session of the thread was opened last: VCL keeps
            // the canvas DCs of other controls until the message ends. A memory DC
            // is a buffer the dispatcher saw this window create while painting.
            // An open paint session alone never authorizes a DC.
            const HWND owner = dc ? WindowFromDC(dc) : nullptr;
            const bool own = owner
                ? owner == hwnd && GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(hwnd, dc)
                : GetObjectType(dc) == OBJ_MEMDC && GlassCore::PaintDispatcher::GetStackOwnerForDc(dc) == hwnd;
            if (!own) return L"dc";
            // The renderer and the clear work in device units.
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

        bool SystemBackground(COLORREF color) noexcept
        {
            const COLORREF rgb = color & 0xFFFFFFu;
            return rgb == (GetSysColor(COLOR_WINDOW) & 0xFFFFFFu) || rgb == (GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu);
        }
        bool Extent(const RECT& r, int& width, int& height) noexcept
        {
            const long long w = static_cast<long long>(r.right) - r.left;
            const long long h = static_cast<long long>(r.bottom) - r.top;
            if (w <= 0 || h <= 0 || w > INT_MAX || h > INT_MAX) return false;
            width = static_cast<int>(w); height = static_cast<int>(h);
            return true;
        }
        // What an opaque text background becomes on glass, as in BWM's ANSI text
        // hooks: black, COLOR_WINDOW and COLOR_BTNFACE are the window's own
        // background, which is transparent here; any other colour is a highlight
        // and is filled fully opaque. This is the background of one text call,
        // not a colour key over pixels.
        bool Background(HDC dc, const RECT& r, COLORREF color)
        {
            int width{}, height{};
            if (!Extent(r, width, height))
                return r.right <= r.left || r.bottom <= r.top; // nothing to fill; an overflow is a failure
            const COLORREF rgb = color & 0xFFFFFFu;
            if (!rgb || SystemBackground(rgb))
                return PatBlt(dc, r.left, r.top, width, height, BLACKNESS) != FALSE;
            wil::unique_hbrush brush{Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(rgb, 255)};
            return brush && FillRect(dc, &r, brush.get()) != 0;
        }

        // Metadata only: the text of a cell is the content of a resource and is never logged.
        void ReportText(HWND hwnd, HDC dc, int x, int y, UINT options, const RECT* rect,
            UINT ansiCount, size_t wideCount, UINT codepage, LPCWSTR reason, BOOL result) noexcept
        {
            if (!TakeDiag(1)) return;
            const RECT r = rect ? *rect : RECT{};
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiResVersionEditor] text hwnd=0x%p dc=0x%p dcWnd=0x%p dcType=%lu pos=[%d,%d] options=0x%X rect=%d[%ld,%ld,%ld,%ld] ansiCount=%u wideCount=%zu cp=%u align=0x%X bkMode=%d bk=0x%06X color=0x%06X drawn=%d reason=%s result=%d",
                hwnd, dc, dc ? WindowFromDC(dc) : nullptr, dc ? GetObjectType(dc) : 0ul,
                x, y, options, rect ? 1 : 0, r.left, r.top, r.right, r.bottom,
                ansiCount, wideCount, codepage,
                dc ? GetTextAlign(dc) : 0u, dc ? GetBkMode(dc) : 0,
                dc ? GetBkColor(dc) & 0xFFFFFFu : 0u, dc ? GetTextColor(dc) & 0xFFFFFFu : 0u,
                reason ? 0 : 1, reason ? reason : L"none", result);
        }
        void ReportFill(HWND hwnd, HDC dc, const RECT* rect, COLORREF color, LPCWSTR reason) noexcept
        {
            if (!TakeDiag(2)) return;
            const RECT r = rect ? *rect : RECT{};
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiResVersionEditor] fill hwnd=0x%p dc=0x%p dcWnd=0x%p rect=%d[%ld,%ld,%ld,%ld] rgb=0x%06X cleared=%d reason=%s",
                hwnd, dc, dc ? WindowFromDC(dc) : nullptr, rect ? 1 : 0, r.left, r.top, r.right, r.bottom,
                color & 0xFFFFFFu, reason ? 0 : 1, reason ? reason : L"none");
        }

        // The window whose paint session is the newest one of this thread, when
        // that session holds the imports. A session that could not take them is a
        // native barrier over an outer one. The hooks sit in the EXE's import
        // table, so whoever reaches them is the EXE.
        HWND SessionWindow() noexcept
        {
            if (g_inside || g_sessions.empty() || g_shutdown.load(std::memory_order_acquire)) return nullptr;
            const Session& top = g_sessions.back();
            return top.leased && top.generation == g_generation.load(std::memory_order_acquire) ? top.hwnd : nullptr;
        }

        // `native` restores the entry LastError and calls the original once, with
        // the caller's arguments. A rejected call is exactly that one call; after a
        // background fill it is a repaint by the original, not a pixel rollback.
        template <typename Native>
        BOOL DrawCellText(HWND hwnd, HDC dc, int x, int y, UINT options, const RECT* rect,
            LPCSTR text, UINT count, const INT* dx, DWORD entryError, const Native& native)
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            std::wstring wide;
            UINT codepage{};
            LPCWSTR reason = RejectTarget(hwnd, dc);
            // Glyph indices and paired offsets are not ANSI text.
            if (!reason && (options & (ETO_GLYPH_INDEX | ETO_PDY))) reason = L"options";
            if (!reason && (options & (ETO_OPAQUE | ETO_CLIPPED)) && !rect) reason = L"rect";
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
            ReportText(hwnd, dc, x, y, options, rect, count, wide.size(), codepage, reason, result);
            SetLastError(error);
            return result;
        }

        // The fill of the control's own background colour. Black and every other
        // colour, patterns and brushes without metadata are the application's.
        template <typename Native>
        int ClearOwnFill(HWND hwnd, HDC dc, const RECT* rect, HBRUSH brush, DWORD entryError, const Native& native)
        {
            g_inside = true;
            auto leave = wil::scope_exit([]() noexcept { g_inside = false; });
            COLORREF color = 0;
            int width{}, height{};
            LPCWSTR reason = RejectTarget(hwnd, dc);
            if (!reason && (!rect || !Extent(*rect, width, height))) reason = L"rect";
            if (!reason)
            {
                // VCL always passes a brush of its own; anything GetObject does not
                // describe as a solid brush is left to the original.
                LOGBRUSH lb{};
                if (brush && GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb) && lb.lbStyle == BS_SOLID) color = lb.lbColor;
                else reason = L"brush";
                if (!reason && !SystemBackground(color)) reason = L"colour";
            }
            if (!reason && !PatBlt(dc, rect->left, rect->top, width, height, BLACKNESS)) reason = L"clear";
            int result = 1; // FillRect: nonzero on success
            DWORD error = entryError;
            if (reason) { result = native(); error = GetLastError(); }
            ReportFill(hwnd, dc, rect, color, reason);
            SetLastError(error);
            return result;
        }

        BOOL WINAPI CellExtTextOutA(HDC dc, int x, int y, UINT options, const RECT* rect,
            LPCSTR text, UINT count, const INT* dx)
        {
            const DWORD entryError = GetLastError();
            decltype(&::ExtTextOutA) original{};
            { auto lock = g_lock.lock_shared(); original = g_dispatcher.GetOrg<TextSlot, decltype(&::ExtTextOutA)>(); }
            const auto native = [&] {
                SetLastError(entryError);
                return original ? original(dc, x, y, options, rect, text, count, dx)
                    : ::ExtTextOutA(dc, x, y, options, rect, text, count, dx);
            };
            const HWND hwnd = SessionWindow();
            if (!hwnd) return native();
            return DrawCellText(hwnd, dc, x, y, options, rect, text, count, dx, entryError, native);
        }
        int WINAPI CellFillRect(HDC dc, const RECT* rect, HBRUSH brush)
        {
            const DWORD entryError = GetLastError();
            decltype(&::FillRect) original{};
            { auto lock = g_lock.lock_shared(); original = g_dispatcher.GetOrg<FillSlot, decltype(&::FillRect)>(); }
            const auto native = [&] {
                SetLastError(entryError);
                return original ? original(dc, rect, brush) : ::FillRect(dc, rect, brush);
            };
            const HWND hwnd = SessionWindow();
            if (!hwnd) return native();
            return ClearOwnFill(hwnd, dc, rect, brush, entryError, native);
        }

        // Both imports or neither: cleared fills under text that stays native
        // would leave the control empty. A reference is counted only when the
        // dispatcher really took it.
        bool Acquire(unsigned& generation) noexcept
        {
            auto lock = g_lock.lock_exclusive();
            const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule;
            if (g_shutdown.load(std::memory_order_acquire) || !exe) return false;
            g_dispatcher.moduleAddress = exe;
            size_t taken = 0;
            for (; taken < Count; ++taken)
            {
                const int before = g_dispatcher.GetRef(taken);
                g_dispatcher.EnableHook(taken, true);
                if (g_dispatcher.GetRef(taken) <= before) break;
            }
            if (taken == Count) { generation = g_generation.load(std::memory_order_acquire); return true; }
            while (taken--) g_dispatcher.EnableHook(taken, false);
            return false;
        }
        void Release(unsigned generation) noexcept
        {
            auto lock = g_lock.lock_exclusive();
            // A session of another thread may outlive Shutdown, and even a restart:
            // Shutdown has already dropped every reference of its generation.
            if (generation != g_generation.load(std::memory_order_acquire)) return;
            for (size_t i = 0; i < Count; ++i) g_dispatcher.EnableHook(i, false);
        }

        bool IsVersionEditor(HWND hwnd) noexcept
        {
            wchar_t name[32]{};
            return hwnd && GetClassNameW(hwnd, name, static_cast<int>(_countof(name)))
                && _wcsicmp(name, L"TResVersionEditor") == 0;
        }
        void OnBegin(HWND hwnd, HMODULE caller) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || !IsVersionEditor(hwnd)) return;
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
                    L"[GlassCore][DelphiResVersionEditor] Begin hwnd=0x%p caller=0x%p eligible=%d leased=%d depth=%zu",
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
        // Find the two imports now: the scan of the import table stays out of the first paint.
        if (const HMODULE exe = GlassCore::ModuleHandler::g_entryPointModule)
        {
            g_dispatcher.moduleAddress = exe;
            g_dispatcher.CacheHookData();
        }
        GlassCore::PaintDispatcher::RegisterPaintProvider({OnBegin, OnEnd});
        g_shutdown.store(false, std::memory_order_release);
    }
    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        auto lock = g_lock.lock_exclusive();
        g_generation.fetch_add(1, std::memory_order_acq_rel);
        g_dispatcher.DisableAllHooks();
        g_sessions.clear();
    }
}
