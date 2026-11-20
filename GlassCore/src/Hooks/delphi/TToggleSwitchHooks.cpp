// VCL toggle paint safety and debug-only DC lifecycle tracing.
#include "CorePch.h"
#include <Hooks/delphi/TToggleSwitchHooks.h>
#include <DebugLog.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RootSwcaGradient.h>
#include <GlassBackdropMark.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Subsystems/GdiHookUtils.h>
#include <WinApiUtils.h>
#include <array>
#include <vector>

namespace GlassCore::DelphiToggle
{
    namespace
    {
        using namespace GlassCore::Hooks;
        using namespace std::literals;
        enum : size_t { TrackRectangle, ThumbFillRect, CopyStretchBlt,
#if defined(_DEBUG)
            CopyBitBlt,
#endif
            HookCount };
        BOOL WINAPI ToggleStretchBlt(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
#if defined(_DEBUG)
        BOOL WINAPI TraceBitBlt(HDC, int, int, int, int, HDC, int, int, DWORD);
#endif
        BOOL WINAPI ToggleRectangle(HDC dc, int left, int top, int right, int bottom);
        int WINAPI ToggleFillRect(HDC dc, const RECT* rect, HBRUSH brush);
        const HookDispatcherDependency<HookCount, 3> g_dependency{{
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "Rectangle", reinterpret_cast<void*>(ToggleRectangle) },
            std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv,
                "ext-ms-win-ntuser-misc-l1-1-0.dll"sv }, "FillRect", reinterpret_cast<void*>(ToggleFillRect) },
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "StretchBlt", reinterpret_cast<void*>(ToggleStretchBlt) },
#if defined(_DEBUG)
            std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv,
                "ext-ms-win-gdi-desktop-l1-1-0.dll"sv }, "BitBlt", reinterpret_cast<void*>(TraceBitBlt) },
#endif
        }};
        HookDispatcher<HookCount, 3> g_dispatcher{ g_dependency, nullptr };
        wil::srwlock g_lock;
        std::atomic<bool> g_shutdown{false};
        struct Session
        {
            HWND hwnd{};
            unsigned long long serial{};
            bool isPaint{};
            bool eligible{};
            HDC windowDC{};
            bool copyInvalidated{};
            std::array<bool, HookCount> hooks{};
            HDC trackDC{};
            HBITMAP trackBitmap{};
            RECT trackRect{};
        };
        thread_local std::vector<Session> g_sessions;
        thread_local unsigned long long g_nextSerial{};

        bool HasToggleClass(HWND hwnd) noexcept
        {
            wchar_t cls[64]{};
            return hwnd && GetClassNameW(hwnd, cls, _countof(cls))
                && _wcsicmp(cls, L"TToggleSwitch") == 0;
        }
        bool IsTarget(HWND hwnd) noexcept
        {
            return HasToggleClass(hwnd) && GlassCore::Root::IsFullInjectionMode()
                && GlassShared::GlassIsBackdropAllowedForHwnd(hwnd);
        }
        void Begin(HWND hwnd, HMODULE caller, bool paint) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || !HasToggleClass(hwnd)) return;
            Session session{};
            session.hwnd = hwnd;
            session.serial = ++g_nextSerial;
            session.isPaint = paint;
            if (!paint) session.windowDC = GlassCore::PaintDispatcher::GetCurrentGetDcSessionHdc();
            session.eligible = IsTarget(hwnd) && caller
                && caller == GlassCore::ModuleHandler::g_entryPointModule;
            if (!session.eligible)
            {
                // Сохраняем даже неразрешённую вложенную сессию этого HWND,
                // чтобы её End не освободил хуки внешнего вызова.
                g_sessions.push_back(session);
                return;
            }
            {
                auto lock = g_lock.lock_exclusive();
                if (g_shutdown.load(std::memory_order_acquire)) return;
                for (size_t i = 0; i < HookCount; ++i)
                {
                    // Копирование контролируется только в сессии GetDC.
                    if (paint && i == CopyStretchBlt) continue;
#if defined(_DEBUG)
                    if (paint && i == CopyBitBlt) continue;
#endif
                    const int before = g_dispatcher.GetRef(i);
                    g_dispatcher.EnableHook(i, true);
                    session.hooks[i] = g_dispatcher.GetRef(i) > before;
                }
            }
            g_sessions.push_back(session);
#if defined(_DEBUG)
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiToggle] begin hwnd=0x%p paint=%d rectangle=%d fill=%d copyBit=%d copyStretch=%d windowDC=0x%p",
                hwnd, paint, session.hooks[TrackRectangle], session.hooks[ThumbFillRect],
                session.hooks[CopyBitBlt], session.hooks[CopyStretchBlt], session.windowDC);
#endif
        }
        void End(HWND hwnd, bool paint) noexcept
        {
            for (auto it = g_sessions.rbegin(); it != g_sessions.rend(); ++it)
            {
                if (it->hwnd != hwnd || it->isPaint != paint) continue;
                Session session = *it;
                g_sessions.erase((it + 1).base());
                auto lock = g_lock.lock_exclusive();
                if (!g_shutdown.load(std::memory_order_acquire))
                    for (size_t i = 0; i < HookCount; ++i)
                        if (session.hooks[i]) g_dispatcher.EnableHook(i, false);
                return;
            }
        }
        void BeginPaintSession(HWND hwnd, HMODULE caller) { Begin(hwnd, caller, true); }
        void EndPaintSession(HWND hwnd, HMODULE) { End(hwnd, true); }
        void BeginDcSession(HWND hwnd, HMODULE caller) { Begin(hwnd, caller, false); }
        void EndDcSession(HWND hwnd, HMODULE) { End(hwnd, false); }

        // Контекст сам по себе не доказывает владельца HDC: дополнительно
        // проверяем живой bitmap и отсутствие преобразования координат.
        bool IsCandidate(const Session& s, HDC dc, RECT& client, HBITMAP& bitmap) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire) || !s.eligible || !dc || !IsTarget(s.hwnd)
                || !GlassCore::PaintDispatcher::IsTopWindowDrawingSession(s.hwnd)
                || GlassCore::PaintDispatcher::GetStackOwnerForDc(dc) != s.hwnd
                || !GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(s.hwnd, dc)
                || GetObjectType(dc) != OBJ_MEMDC || WindowFromDC(dc)
                || !GetClientRect(s.hwnd, &client) || IsRectEmpty(&client)
                || GetMapMode(dc) != MM_TEXT) return false;
            POINT viewport{}, window{};
            if (!GetViewportOrgEx(dc, &viewport) || !GetWindowOrgEx(dc, &window)
                || viewport.x || viewport.y || window.x || window.y) return false;
            bitmap = static_cast<HBITMAP>(GetCurrentObject(dc, OBJ_BITMAP));
            BITMAP info{};
            return bitmap && GetObject(bitmap, sizeof(info), &info) == sizeof(info)
                && info.bmWidth == client.right && info.bmHeight == client.bottom
                && info.bmBitsPixel == 32;
        }

        BOOL WINAPI ToggleRectangle(HDC dc, int left, int top, int right, int bottom)
        {
            auto orig = g_dispatcher.GetOrg<TrackRectangle, decltype(&Rectangle)>();
            if (!orig) return Rectangle(dc, left, top, right, bottom);
            if (g_sessions.empty()) return orig(dc, left, top, right, bottom);
            const Session s = g_sessions.back();
            RECT client{}; HBITMAP bitmap{};
            const int height = bottom - top;
            const int width = right - left;
#if defined(_DEBUG)
            // Бюджетированная диагностика без подмены: размеры выбираемого
            // bitmap и viewport помогают увидеть, почему защитные проверки не прошли.
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const auto now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
            if (budget)
            {
                --budget;
                RECT bounds{}; GetClientRect(s.hwnd, &bounds);
                POINT vp{}, win{}; GetViewportOrgEx(dc, &vp); GetWindowOrgEx(dc, &win);
                const HGDIOBJ bmp = GetCurrentObject(dc, OBJ_BITMAP);
                BITMAP bi{}; const int got = bmp ? GetObject(bmp, sizeof(bi), &bi) : 0;
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiToggle] rectangle probe hwnd=0x%p dc=0x%p bmp=0x%p size=%ldx%ld bpp=%u obj=%d client=%ldx%ld viewport=%ld,%ld window=%ld,%ld rect=[%d,%d,%d,%d]",
                    s.hwnd, dc, bmp, bi.bmWidth, bi.bmHeight, static_cast<unsigned>(bi.bmBitsPixel), got,
                    bounds.right, bounds.bottom, vp.x, vp.y, win.x, win.y, left, top, right, bottom);
            }
#endif
            // Только похожая на VCL дорожку фигура целиком в клиенте.
            // Родительские координаты и произвольные буферы здесь не подходят.
            if (!IsCandidate(s, dc, client, bitmap) || left < 0 || top < 0
                || right > client.right || bottom > client.bottom + 1
                || top > client.bottom / 3 || bottom < client.bottom - client.bottom / 3
                || height <= 0 || width < height * 3 / 2 || width > client.right)
                return orig(dc, left, top, right, bottom);
            LOGPEN lp{}; LOGBRUSH lb{};
            HGDIOBJ oldPen = GetCurrentObject(dc, OBJ_PEN);
            HGDIOBJ oldBrush = GetCurrentObject(dc, OBJ_BRUSH);
            if (!oldPen || !oldBrush || GetObject(oldPen, sizeof(lp), &lp) != sizeof(lp)
                || GetObject(oldBrush, sizeof(lb), &lb) != sizeof(lb)
                || (lb.lbStyle != BS_SOLID && lb.lbStyle != BS_NULL))
                return orig(dc, left, top, right, bottom);
            auto pen = GlassCore::GdiUtils::LayeredStrokePen::TryCreateSolid(lp, 255);
            if (!pen) return orig(dc, left, top, right, bottom);
            wil::unique_hbrush alphaBrush;
            HBRUSH replacement = static_cast<HBRUSH>(oldBrush);
            if (lb.lbStyle == BS_SOLID)
            {
                if ((lb.lbColor & 0xFFFFFF) == (GetSysColor(COLOR_WINDOW) & 0xFFFFFF))
                    replacement = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
                else
                {
                    alphaBrush.reset(Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255));
                    if (!alphaBrush) return orig(dc, left, top, right, bottom);
                    replacement = alphaBrush.get();
                }
            }
            HGDIOBJ prevPen = SelectObject(dc, pen.get());
            if (!prevPen || prevPen == HGDI_ERROR) return orig(dc, left, top, right, bottom);
            HGDIOBJ prevBrush = SelectObject(dc, replacement);
            if (!prevBrush || prevBrush == HGDI_ERROR)
            {
                SelectObject(dc, prevPen);
                return orig(dc, left, top, right, bottom);
            }
            const HWND sessionHwnd = s.hwnd;
            const auto sessionSerial = s.serial;
            const BOOL result = orig(dc, left, top, right, bottom);
            SelectObject(dc, prevBrush);
            SelectObject(dc, prevPen);
            // Вложенные вызовы могли перевыделить вектор; повторно проверяем вершину.
            if (result && !g_sessions.empty() && g_sessions.back().hwnd == sessionHwnd
                && g_sessions.back().serial == sessionSerial)
            {
                auto& current = g_sessions.back();
                current.trackDC = dc;
                current.trackBitmap = bitmap;
                current.trackRect = RECT{left, top, right, bottom};
#if defined(_DEBUG)
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiToggle] track hwnd=0x%p dc=0x%p bitmap=0x%p rect=[%d,%d,%d,%d]",
                    current.hwnd, dc, bitmap, left, top, right, bottom);
#endif
            }
            return result;
        }

#if defined(_DEBUG)
        void LogCopy(const wchar_t* api, const Session& session, HDC dest, int x, int y,
            int width, int height, HDC source, int sx, int sy, int sw, int sh,
            DWORD rop, BOOL result) noexcept
        {
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 32; }
            if (!budget) return;
            --budget;
            HGDIOBJ bitmap = source ? GetCurrentObject(source, OBJ_BITMAP) : nullptr;
            DIBSECTION dib{};
            const int got = bitmap ? GetObject(bitmap, sizeof(dib), &dib) : 0;
            // Только доступный 32bpp DIB: небольшая безопасная выборка из 16
            // пикселей через ReadProcessMemory, без модификации или GdiFlush.
            // Это статистика, не полное доказательство альфы или принадлежности DC.
            unsigned samples = 0, alphaZero = 0, alphaFull = 0, alphaPartial = 0;
            unsigned blackZero = 0, blackFull = 0;
            const BITMAP& bm = dib.dsBm;
            if (got == sizeof(DIBSECTION) && bm.bmBits && bm.bmBitsPixel == 32
                && bm.bmWidth > 0 && bm.bmWidth <= 4096 && bm.bmHeight > 0 && bm.bmHeight <= 4096
                && bm.bmWidthBytes >= bm.bmWidth * 4 && bm.bmWidthBytes <= 1024 * 1024)
            {
                for (int row = 0; row < 4; ++row)
                    for (int col = 0; col < 4; ++col)
                    {
                        const size_t py = static_cast<size_t>((static_cast<long long>(bm.bmHeight - 1) * row) / 3);
                        const size_t px = static_cast<size_t>((static_cast<long long>(bm.bmWidth - 1) * col) / 3);
                        const auto address = reinterpret_cast<ULONG_PTR>(bm.bmBits)
                            + py * static_cast<size_t>(bm.bmWidthBytes) + px * 4;
                        DWORD pixel = 0; SIZE_T read = 0;
                        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                            &pixel, sizeof(pixel), &read) || read != sizeof(pixel)) continue;
                        ++samples;
                        const BYTE alpha = static_cast<BYTE>(pixel >> 24);
                        if (!alpha) { ++alphaZero; if (!(pixel & 0xFFFFFF)) ++blackZero; }
                        else if (alpha == 255) { ++alphaFull; if (!(pixel & 0xFFFFFF)) ++blackFull; }
                        else ++alphaPartial;
                    }
            }
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiToggle] copy api=%s hwnd=0x%p dest=0x%p src=0x%p srcType=%u bmp=0x%p bitmap=%ldx%ld bpp=%u dib=%d bits=%d samples=%u a0=%u a255=%u apart=%u blackA0=%u blackA255=%u dst=[%d,%d,%d,%d] srcRect=[%d,%d,%d,%d] rop=0x%08X result=%d",
                api, session.hwnd, dest, source, source ? static_cast<unsigned>(GetObjectType(source)) : 0,
                bitmap, bm.bmWidth, bm.bmHeight, static_cast<unsigned>(bm.bmBitsPixel),
                got == sizeof(DIBSECTION), got == sizeof(DIBSECTION) && bm.bmBits != nullptr,
                samples, alphaZero, alphaFull, alphaPartial, blackZero, blackFull,
                x, y, width, height, sx, sy, sw, sh, static_cast<unsigned>(rop), result);
        }

        BOOL WINAPI TraceBitBlt(HDC dest, int x, int y, int width, int height,
            HDC source, int sx, int sy, DWORD rop)
        {
            const DWORD entryError = GetLastError();
            auto orig = g_dispatcher.GetOrg<CopyBitBlt, decltype(&BitBlt)>();
            if (!orig) { SetLastError(entryError); return BitBlt(dest, x, y, width, height, source, sx, sy, rop); }
            const bool tracked = !g_sessions.empty() && g_sessions.back().eligible
                && !g_sessions.back().isPaint && g_sessions.back().windowDC == dest
                && WindowFromDC(dest) == g_sessions.back().hwnd
                && GlassCore::PaintDispatcher::IsTopWindowDrawingSession(g_sessions.back().hwnd);
            Session session{};
            if (tracked) session = g_sessions.back();
            SetLastError(entryError);
            const BOOL result = orig(dest, x, y, width, height, source, sx, sy, rop);
            const DWORD error = GetLastError();
            if (tracked) LogCopy(L"BitBlt", session, dest, x, y, width, height,
                source, sx, sy, width, height, rop, result);
            SetLastError(error);
            return result;
        }

#endif

        // VCL может подготовить DDB кадра до GetDC и затем скопировать его
        // целиком в окно: на таком пути Rectangle/FillRect не вызываются. Не
        // подменяем содержимое или ROP. После успешной точной копии просим VCL
        // перерисовать само окно обычным WM_PAINT (без фонового erase).
        BOOL WINAPI ToggleStretchBlt(HDC dest, int x, int y, int width, int height,
            HDC source, int sx, int sy, int sw, int sh, DWORD rop)
        {
            const DWORD entryError = GetLastError();
            auto orig = g_dispatcher.GetOrg<CopyStretchBlt, decltype(&StretchBlt)>();
            if (!orig) { SetLastError(entryError); return StretchBlt(dest, x, y, width, height, source, sx, sy, sw, sh, rop); }
            const bool tracked = !g_shutdown.load(std::memory_order_acquire)
                && !g_sessions.empty() && g_sessions.back().eligible
                && !g_sessions.back().isPaint && g_sessions.back().windowDC == dest
                && dest && WindowFromDC(dest) == g_sessions.back().hwnd
                && GlassCore::PaintDispatcher::IsTopWindowDrawingSession(g_sessions.back().hwnd);
            Session session{};
            if (tracked) session = g_sessions.back();
            bool refresh = false;
            if (tracked && !session.copyInvalidated && rop == SRCCOPY && source
                // Не создавать цикл invalidate -> paint -> вложенный GetDC -> invalidate.
                && !GlassCore::PaintDispatcher::HasOpenPaintSessionForHwnd(session.hwnd)
                && GetObjectType(source) == OBJ_MEMDC && !WindowFromDC(source)
                && IsTarget(session.hwnd) && IsWindowVisible(session.hwnd))
            {
                RECT client{};
                BITMAP bitmap{};
                const HGDIOBJ bmp = GetCurrentObject(source, OBJ_BITMAP);
                refresh = GetClientRect(session.hwnd, &client) && !IsRectEmpty(&client)
                    && x == 0 && y == 0 && sx == 0 && sy == 0
                    && width == client.right && height == client.bottom
                    && sw == width && sh == height
                    && bmp && GetObject(bmp, sizeof(bitmap), &bitmap) == sizeof(bitmap)
                    && bitmap.bmWidth == width && bitmap.bmHeight == height
                    && bitmap.bmBitsPixel == 32;
            }
            SetLastError(entryError);
            const BOOL result = orig(dest, x, y, width, height, source, sx, sy, sw, sh, rop);
            const DWORD error = GetLastError();
            if (result && refresh && !g_shutdown.load(std::memory_order_acquire)
                && !g_sessions.empty() && g_sessions.back().serial == session.serial
                && g_sessions.back().hwnd == session.hwnd
                && !g_sessions.back().copyInvalidated && IsTarget(session.hwnd)
                && !GlassCore::PaintDispatcher::HasOpenPaintSessionForHwnd(session.hwnd))
            {
                // Один запрос на GetDC-сессию; Windows объединяет invalidation.
                g_sessions.back().copyInvalidated = true;
                const BOOL invalidated = InvalidateRect(session.hwnd, nullptr, FALSE);
#if defined(_DEBUG)
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiToggle] copy refresh hwnd=0x%p dc=0x%p invalidated=%d",
                    session.hwnd, dest, invalidated);
#else
                (void)invalidated;
#endif
            }
#if defined(_DEBUG)
            if (tracked) LogCopy(L"StretchBlt", session, dest, x, y, width, height,
                source, sx, sy, sw, sh, rop, result);
#endif
            SetLastError(error);
            return result;
        }

        int WINAPI ToggleFillRect(HDC dc, const RECT* rect, HBRUSH brush)
        {
            auto orig = g_dispatcher.GetOrg<ThumbFillRect, decltype(&FillRect)>();
            if (!orig) return FillRect(dc, rect, brush);
            if (g_sessions.empty() || !rect || !brush) return orig(dc, rect, brush);
            const Session s = g_sessions.back();
            RECT client{}; HBITMAP bitmap{};
            if (!s.trackDC || s.trackDC != dc || !IsCandidate(s, dc, client, bitmap)
                || bitmap != s.trackBitmap || IsRectEmpty(rect)
                || rect->left < s.trackRect.left || rect->top < s.trackRect.top
                || rect->right > s.trackRect.right || rect->bottom > s.trackRect.bottom
                || rect->right - rect->left > s.trackRect.bottom - s.trackRect.top)
                return orig(dc, rect, brush);
            LOGBRUSH lb{};
            if (GetObject(brush, sizeof(lb), &lb) != sizeof(lb) || lb.lbStyle != BS_SOLID)
                return orig(dc, rect, brush);
            wil::unique_hbrush alpha{Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(lb.lbColor, 255)};
            if (!alpha) return orig(dc, rect, brush);
#if defined(_DEBUG)
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiToggle] thumb hwnd=0x%p dc=0x%p rect=[%ld,%ld,%ld,%ld] color=0x%06X",
                s.hwnd, dc, rect->left, rect->top, rect->right, rect->bottom, static_cast<unsigned>(lb.lbColor & 0xFFFFFF));
#endif
            return orig(dc, rect, alpha.get());
        }
    }

    void Startup() noexcept
    {
        g_shutdown.store(false, std::memory_order_release);
        { auto lock = g_lock.lock_exclusive(); g_dispatcher.moduleAddress = GlassCore::ModuleHandler::g_entryPointModule; }
        GlassCore::PaintDispatcher::RegisterPaintProvider({BeginPaintSession, EndPaintSession});
        GlassCore::PaintDispatcher::RegisterGetDCProvider({BeginDcSession, EndDcSession});
    }

    void Shutdown() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        { auto lock = g_lock.lock_exclusive(); g_dispatcher.DisableAllHooks(); }
        g_sessions.clear();
    }

#if defined(_DEBUG)
    // Контекст TToggleSwitch — только реально открытая BeginPaint-сессия,
    // а не HWND, унаследованный memory DC из ближайшей записи стека. Сам
    // факт нахождения в paint НЕ доказывает, кому принадлежит удаляемый DC.
    static thread_local bool g_loggingDelphiToggleDelete = false;
    bool IsToggleSwitch(HWND hwnd) noexcept
    {
        wchar_t cls[64]{};
        return hwnd && GetClassNameW(hwnd, cls, _countof(cls))
            && _wcsicmp(cls, L"TToggleSwitch") == 0;
    }

    DelphiToggleDeleteTrace CaptureDelphiToggleDelete(HWND paintHwnd, HDC dc) noexcept
    {
        DelphiToggleDeleteTrace trace{};
        if (!dc || !paintHwnd || g_loggingDelphiToggleDelete) return trace;
        trace.paintHwnd = paintHwnd;
        trace.dcType = GetObjectType(dc);
        if (trace.dcType != OBJ_MEMDC) { trace.paintHwnd = nullptr; return trace; }
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        static thread_local unsigned total = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 32; }
        if (!budget || total >= 128) { trace.paintHwnd = nullptr; return trace; }
        --budget;
        trace.serial = ++total;
        trace.bitmap = GetCurrentObject(dc, OBJ_BITMAP);
        trace.count = CaptureStackBackTrace(0, _countof(trace.frames), trace.frames, nullptr);
        return trace;
    }

    void LogDelphiToggleDelete(const DelphiToggleDeleteTrace& trace,
        HDC dc, HMODULE caller, BOOL result) noexcept
    {
        if (!trace.paintHwnd || g_loggingDelphiToggleDelete) return;
        g_loggingDelphiToggleDelete = true;
        auto resetLogging = wil::scope_exit([]() noexcept { g_loggingDelphiToggleDelete = false; });
        DebugLog(DBG_NORMAL,
            L"[GlassCore][DelphiToggleDC] DeleteDC seq=%u paintHwnd=0x%p dc=0x%p type=%u bitmap=0x%p caller=0x%p result=%d frames=%u (paint context, not DC ownership)",
            trace.serial, trace.paintHwnd, dc, static_cast<unsigned>(trace.dcType),
            trace.bitmap, caller, result, static_cast<unsigned>(trace.count));
        for (USHORT i = 0; i < trace.count; ++i)
        {
            HMODULE module = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(trace.frames[i]), &module);
            const auto address = reinterpret_cast<ULONG_PTR>(trace.frames[i]);
            const auto base = reinterpret_cast<ULONG_PTR>(module);
            wchar_t path[MAX_PATH]{};
            if (module) GetModuleFileNameW(module, path, _countof(path));
            const wchar_t* name = wcsrchr(path, L'\\');
            name = name ? name + 1 : path;
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiToggleDC] frame seq=%u index=%u address=0x%p module=0x%p name=%s offset=0x%llX",
                trace.serial, static_cast<unsigned>(i), trace.frames[i], module, name,
                static_cast<unsigned long long>(module ? address - base : 0));
        }
    }
#endif

}
