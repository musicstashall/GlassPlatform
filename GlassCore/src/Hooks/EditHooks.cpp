// Хуки для отрисовки Edit / RichEdit20W / RICHEDIT50W и их WinForms-вариантов.
//
// Архитектура (PaintDispatcher + per-module g_overallMap):
//
// Edit/RichEdit рисуют текст через ExtTextOutW с ETO_GLYPH_INDEX.
// Наш подход: ExtTextOutWithAlpha на HDC; для многострочного Edit внутри BeginPaint/EndPaint
// — сессионный DIB на весь клиент + один GdiAlphaBlend на EndPaint.
//
// КЛЮЧЕВЫЕ МОМЕНТЫ:
//
// 1. Сессионный DIB и bgSnapshot только в канале BeginPaint/EndPaint (WM_PAINT).
//    GetDC/ReleaseDC (выделение, каретка) — прямой ExtTextOutWithAlpha на HDC окна;
//    иначе композит «снимок + альфа-текст» оставляет прозрачные области и «призрак» выделения.
//
// 2. EnsureEditBuffer захватывает bgSnapshot ДО первого ExtTextOut в WM_PAINT.
//    FlushEditBuffer: bgSnapshot (или fallback screenDC) + BLACKNESS по ETO_OPAQUE + GdiAlphaBlend.
//
// 3. GetDC + pending WM_PAINT: ExtTextOut не глушим (раньше — пустой кадр или origW+затир перед
//    WM_PAINT). Всегда glass через multilinePlainGetDc; дублирование с BeginPaint — приемлемо.
//
// 4. EditBufferedSession: hasHScroll → single-pass; иначе dual-pass (качество).
//
// 5. ETO_OPAQUE только в ApplyEditBackgroundRect; в рендер уходит drawOptions без ETO_OPAQUE.
//
// Цена: WS_HSCROLL + выделение снова даёт много ExtTextOut на экран (медленнее, чем буфер на GetDC).
//
// Модули, из которых приходят вызовы ExtTextOutW / ExtTextOutA:
//   Edit          → user32.dll    → IAT gdi32(full).dll (ExtTextOutW)
//   RichEdit20W   → riched20.dll  → IAT gdi32: ExtTextOutW и ExtTextOutA
//   RICHEDIT50W   → msftedit.dll  → IAT gdi32(full).dll

#include "CorePch.h"

#include <Hooks/EditControl.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/TextInkClientRegistry.h>
#include <GlassControlKind.h>
#include <GlassDelphiMemoBackground.h>
#include <Hooks/delphi/TInputHooks.h>
#include <WinApiUtils.h>
#include <wil.h>

#include <string>
#include <vector>
#include <cmath>

#ifndef EM_GETBKGNDCOLOR
#define EM_GETBKGNDCOLOR (WM_USER + 109)
#endif
#ifndef ECO_AUTOBKGND
#define ECO_AUTOBKGND 0x80000000
#endif

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::EditControl
{
    namespace
    {
        constexpr DWORD kEditStyleInsetTextRect = 0x50231004u;
    }

    // =========================================================================
    //  LIFO сессий paint
    // =========================================================================
    struct EditPaintSession
    {
        HWND    hwnd;
        HMODULE mod;
        GlassShared::GlassControlKind kind;
        // Заполняется после успешного оригинального BeginPaint, а не при
        // onBegin (внутри BeginPaint может быть вложенный GetDCEx/WM_NCPAINT).
        HDC paintDc = nullptr;
        bool hotKeyFillHook = false;
        bool numberBackgroundCleared = false;
    };

    thread_local std::vector<EditPaintSession> g_editSessionStack;  // BeginPaint/EndPaint
    thread_local std::vector<EditPaintSession> g_editDcStack;       // GetDC/ReleaseDC

    static std::atomic<bool> g_shutdown{ false };

    [[nodiscard]] static bool IsDelphiMemo(HWND hwnd) noexcept
    {
        WCHAR cls[64]{};
        return hwnd && GetClassNameW(hwnd, cls, _countof(cls)) > 0
            && _wcsicmp(cls, L"TMemo") == 0;
    }

    // Compatible bitmap оконного DC не гарантирует сохранение alpha при
    // промежуточной композиции. Для VCL Memo используем явный 32bpp DIB.
    [[nodiscard]] static HBITMAP CreateMemoComposeBitmap(HDC dc, int width, int height, void** outBits = nullptr) noexcept
    {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (outBits) *outBits = bitmap ? bits : nullptr;
        return bitmap;
    }

    // =========================================================================
    //  EditBufferedSession — сессионный DIB для многострочного Edit
    // =========================================================================
    struct MemoBackgroundOp
    {
        RECT rect;
        COLORREF color;
        bool fill;
    };

    struct EditBufferedSession
    {
        HWND     hWnd = nullptr;
        HDC      screenDC = nullptr;
        HDC      memDC = nullptr;
        HBITMAP  dib = nullptr;
        HGDIOBJ  oldBmp = nullptr;
        RGBQUAD* bits = nullptr;
        int      width = 0;
        int      height = 0;

        // ── Новые поля ────────────────────────────────────────────────────────
        // bgSnapshot: чистый glass-фон, захваченный ДО первого ExtTextOut.
        // Использован в FlushEditBuffer вместо BitBlt(screenDC) — устраняет
        // двоение из-за «загрязнённого» screen DC.
        HBITMAP bgSnapshot = nullptr;

        // hasHScroll: признак WS_HSCROLL стиля.
        // true  → single-pass рендер (скорость при выделении длинных строк)
        // false → dual-pass рендер   (качество, аналог GetDrawTextWithGlow)
        bool hasHScroll = false;

        // useDualPass: передаётся в TextSessionBuffer для ExtTextOutWithAlphaToSession
        bool useDualPass = false;

        // fromGetDC: сессия создана для GetDC-канала (не BeginPaint)
        bool fromGetDC = false;
        bool delphiMemo = false;
        COLORREF memoSolidBackground = CLR_INVALID;
        // Только фон TMemo; текст в отдельном DIB, чтобы tblend не ослаблял заливку.
        std::vector<MemoBackgroundOp> memoBackgrounds;
    };

    thread_local EditBufferedSession* g_activeEditBuffer = nullptr;
    thread_local bool                g_bypassEditBuffer = false;
    thread_local std::vector<RECT>   g_sessionBlacknessRects;

#if defined(_DEBUG)
    // Никакого текста пользователя; лимит отдельно для вызовов и flush.
    [[nodiscard]] static bool AllowDelphiMemoLog(HWND hwnd, unsigned channel) noexcept
    {
        if (!IsDelphiMemo(hwnd))
            return false;
        static thread_local ULONGLONG epoch[3]{};
        static thread_local unsigned budget[3]{};
        const ULONGLONG now = GetTickCount64();
        if (!epoch[channel] || now - epoch[channel] >= 1000)
        {
            epoch[channel] = now;
            budget[channel] = 16;
        }
        if (!budget[channel]) return false;
        --budget[channel];
        return true;
    }

    [[nodiscard]] static bool AllowDelphiComboEditLog(HWND hwnd) noexcept
    {
        WCHAR cls[64]{}, parentClass[64]{};
        const HWND parent = hwnd ? GetParent(hwnd) : nullptr;
        if (!parent || !GetClassNameW(hwnd, cls, _countof(cls))
            || _wcsicmp(cls, L"Edit") != 0
            || !GetClassNameW(parent, parentClass, _countof(parentClass))
            || _wcsicmp(parentClass, L"TComboBox") != 0)
            return false;
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
        if (!budget) return false;
        --budget;
        return true;
    }

    // Диагностика новых VCL Edit-классов в существующих хуках, без текста.
    [[nodiscard]] static bool AllowDelphiInputLog(HWND hwnd, unsigned channel) noexcept
    {
        WCHAR cls[64]{};
        if (!hwnd || !GetClassNameW(hwnd, cls, _countof(cls))) return false;
        if (_wcsicmp(cls, L"TLabeledEdit") != 0 && _wcsicmp(cls, L"TButtonedEdit") != 0
            && _wcsicmp(cls, L"TMaskEdit") != 0 && _wcsicmp(cls, L"TSearchBox") != 0
            && _wcsicmp(cls, L"TNumberBox") != 0 && _wcsicmp(cls, L"TSpinEdit") != 0 && _wcsicmp(cls, L"THotKey") != 0
            && _wcsicmp(cls, L"TRichEdit") != 0)
            return false;
        static thread_local ULONGLONG epoch[2]{};
        static thread_local unsigned budget[2]{};
        const ULONGLONG now = GetTickCount64();
        if (!epoch[channel] || now - epoch[channel] >= 1000) { epoch[channel] = now; budget[channel] = 24; }
        if (!budget[channel]) return false;
        --budget[channel];
        return true;
    }

    // Только суммарная alpha-статистика, не содержимое DIB. Ограничиваем работу.
    static void MemoAlphaStats(const RGBQUAD* bits, int width, int height,
        size_t& nonzero, unsigned& maximum) noexcept
    {
        nonzero = 0;
        maximum = 0;
        if (!bits || width <= 0 || height <= 0) return;
        const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
        if (count > 4000000) return;
        for (size_t i = 0; i < count; ++i)
        {
            const unsigned alpha = bits[i].rgbReserved;
            if (alpha) ++nonzero;
            if (alpha > maximum) maximum = alpha;
        }
    }
#endif

    // RichEdit20: политика BWM на весь клиент
    thread_local HWND    g_re20FullClientBgPrimedHwnd = nullptr;
    thread_local COLORREF g_re20LastAppliedEmBg = CLR_INVALID;

    static void PushSessionBlacknessRect(const RECT& rc, int bufW, int bufH) noexcept
    {
        RECT r = rc;
        RECT clip{ 0, 0, bufW, bufH };
        if (!IntersectRect(&r, &r, &clip) || IsRectEmpty(&r)) return;
        g_sessionBlacknessRects.push_back(r);
    }

    [[nodiscard]] static bool UseSessionBufferForDC(HWND sessionHwnd, HDC hdc) noexcept
    {
        if (!sessionHwnd || !hdc) return false;
        if (WindowFromDC(hdc) == sessionHwnd)
            return true;
        // memDC сессионного буфера регистрируется в стеке через HookCreateCompatibleDC — не путать с DC отрисовки COMCTL32.
        if (g_activeEditBuffer && hdc == g_activeEditBuffer->memDC)
            return false;
        return GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(sessionHwnd, hdc);
    }

    [[nodiscard]] static bool EditSessionBufferAcceptsSourceDc(
        const EditBufferedSession* buf, HWND sessionHwnd, HDC hdc) noexcept
    {
        if (!buf || !sessionHwnd || !hdc || buf->hWnd != sessionHwnd)
            return false;
        if (hdc == buf->memDC)
            return false;
        if (hdc == buf->screenDC)
            return true;
        return GlassCore::PaintDispatcher::IsPaintStackDcForHwnd(sessionHwnd, hdc);
    }

    [[nodiscard]] static bool ShouldUseMultilineEditSessionBuffer(
        HWND hWnd, GlassShared::GlassControlKind kind) noexcept
    {
        using namespace GlassShared;
        if (!hWnd) return false;
        const bool plainEdit = kind == GlassControlKind::Edit
            || kind == GlassControlKind::WinForms_Edit;
        if (!plainEdit) return false;
        const DWORD style = static_cast<DWORD>(GetWindowLongW(hWnd, GWL_STYLE));
        return (style & ES_MULTILINE) != 0;
    }

    // После reload ink на GlassGuiThread globals уже новые; useDualPass в активной сессии — на UI-потоке.
    static void SyncActiveEditBufferDualPassFromGlobals() noexcept
    {
        if (!g_activeEditBuffer || !g_activeEditBuffer->hWnd)
            return;
        const bool dual = GlassCore::Text::PlainEditUseDualPass(
            g_activeEditBuffer->hWnd, GlassCore::Text::g_ExtTextOutRenderMode);
        if (g_activeEditBuffer->useDualPass != dual)
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Edit] SyncActiveEditBufferDualPass: hWnd=0x%p dualPass %d→%d rm=%u",
                g_activeEditBuffer->hWnd,
                static_cast<int>(g_activeEditBuffer->useDualPass),
                static_cast<int>(dual),
                static_cast<unsigned>(GlassCore::Text::g_ExtTextOutRenderMode));
            g_activeEditBuffer->useDualPass = dual;
        }
    }

    // ── EnsureEditBuffer ────────────────────────────────────────────────────
    // Создаёт session buffer и захватывает bgSnapshot (чистый фон) за один вызов.
    // bgSnapshot ДОЛЖЕН быть захвачен ДО первого ExtTextOut — пока screen DC чистый.
    static void EnsureEditBuffer(HWND hWnd, HDC screenDC, bool fromGetDC = false) noexcept
    {
        if (g_bypassEditBuffer) return;
        if (g_activeEditBuffer) return;

        RECT rc{};
        GetClientRect(hWnd, &rc);
        if (rc.right <= 0 || rc.bottom <= 0) return;

        HDC memDC = CreateCompatibleDC(screenDC);
        if (!memDC) return;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = rc.right;
        bmi.bmiHeader.biHeight = -rc.bottom;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib) { DeleteDC(memDC); return; }

        auto* buf = new (std::nothrow) EditBufferedSession{};
        if (!buf) { DeleteObject(dib); DeleteDC(memDC); return; }

        buf->hWnd = hWnd;
        buf->screenDC = screenDC;
        buf->memDC = memDC;
        buf->dib = dib;
        buf->oldBmp = SelectObject(memDC, dib);
        buf->bits = static_cast<RGBQUAD*>(bits);
        buf->width = rc.right;
        buf->height = rc.bottom;
        buf->fromGetDC = fromGetDC;
        buf->delphiMemo = IsDelphiMemo(hWnd);
        if (buf->delphiMemo)
            buf->memoSolidBackground = GlassShared::GetDelphiMemoBackground(hWnd);

        const DWORD style = static_cast<DWORD>(GetWindowLongW(hWnd, GWL_STYLE));
        buf->hasHScroll = (style & WS_HSCROLL) != 0;
        BYTE rm = GlassCore::Text::g_ExtTextOutRenderMode;
        buf->useDualPass = GlassCore::Text::PlainEditUseDualPass(hWnd, rm);

        // ── Захват чистого glass-фона (bgSnapshot) ────────────────────────
        // КРИТИЧНО: захватываем ДО возврата, пока screenDC не «загрязнён»
        // ExtTextOut вызовами. Если screenDC уже содержит текст — двоение.
        // При отрисовке в MEMDC снимок берём с оконного DC из стека paint, иначе BitBlt с MEMDC даёт неверный фон.
        HDC bltSrc = screenDC;
        if (GetObjectType(screenDC) == OBJ_MEMDC)
        {
            if (HDC base = GlassCore::PaintDispatcher::GetPaintSessionBaseWindowDc(hWnd))
                bltSrc = base;
        }
        HBITMAP snapBmp = buf->delphiMemo
            ? CreateMemoComposeBitmap(bltSrc, rc.right, rc.bottom) : nullptr;
        if (!snapBmp)
            snapBmp = CreateCompatibleBitmap(bltSrc, rc.right, rc.bottom);
        if (snapBmp)
        {
            HDC snapDC = CreateCompatibleDC(bltSrc);
            if (snapDC)
            {
                HGDIOBJ oldSnap = SelectObject(snapDC, snapBmp);
                BitBlt(snapDC, 0, 0, rc.right, rc.bottom, bltSrc, 0, 0, SRCCOPY);
                SelectObject(snapDC, oldSnap);
                DeleteDC(snapDC);
                buf->bgSnapshot = snapBmp;
            }
            else
            {
                DeleteObject(snapBmp);
            }
        }

        g_activeEditBuffer = buf;

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Edit] EnsureEditBuffer: hWnd=0x%p %dx%d hasHScroll=%d dualPass=%d fromGetDC=%d bgSnap=%d",
            hWnd, rc.right, rc.bottom,
            static_cast<int>(buf->hasHScroll),
            static_cast<int>(buf->useDualPass),
            static_cast<int>(fromGetDC),
            buf->bgSnapshot ? 1 : 0);
#if defined(_DEBUG)
        if (buf->delphiMemo && AllowDelphiMemoLog(hWnd, 1))
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiMemo] BufferTarget hwnd=0x%p screenDc=0x%p paintDc=0x%p memDc=0x%p",
                hWnd, screenDC, g_editSessionStack.empty() ? nullptr : g_editSessionStack.back().paintDc, memDC);
#endif
    }

    // Не используем устаревшую CTLCOLOR-property без подтверждения хотя бы
    // одной текущей ETO_OPAQUE заливкой тем же цветом. Только буфер TMemo и
    // клиентский BeginPaint: копирование на screenDC ограничено update clip.
    [[nodiscard]] static bool HasConfirmedMemoSolidBackground(const EditBufferedSession& buf) noexcept
    {
        if (!buf.delphiMemo || buf.memoSolidBackground == CLR_INVALID) return false;
        for (const auto& op : buf.memoBackgrounds)
            if (op.fill && (op.color & 0xFFFFFFu) == buf.memoSolidBackground)
                return true;
        return false;
    }

    static bool PrimeMemoSolidBackground(HDC dc, const EditBufferedSession& buf) noexcept
    {
        if (!dc || !HasConfirmedMemoSolidBackground(buf)) return false;
        wil::unique_hbrush brush{
            Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(buf.memoSolidBackground, 255) };
        if (!brush) return false;
        RECT rect{ 0, 0, buf.width, buf.height };
        if (!PatBlt(dc, 0, 0, buf.width, buf.height, BLACKNESS)) return false;
        return FillRect(dc, &rect, brush.get()) != 0;
    }

    // Как на прямом GetDC: сначала прозрачная очистка, затем непрозрачная
    // пользовательская/selection заливка. Порядок важен при пересечении rect.
    static void ReplayMemoBackgrounds(HDC dc, const EditBufferedSession& buf) noexcept
    {
        for (const auto& op : buf.memoBackgrounds)
        {
            const RECT& r = op.rect;
            PatBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, BLACKNESS);
            if (op.fill)
            {
                wil::unique_hbrush brush{
                    Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(op.color, 255) };
                if (brush) FillRect(dc, &r, brush.get());
            }
        }
    }

    // ── FlushEditBuffer ─────────────────────────────────────────────────────
    // Компонует результат и выводит на экран.
    // Использует bgSnapshot вместо повторного BitBlt(screenDC):
    //   bgSnapshot → фон без «загрязнения»
    //   обычный Edit: g_sessionBlacknessRects → BLACKNESS для ETO_OPAQUE
    //   TMemo: memoBackgrounds → BLACKNESS и цветные заливки по порядку
    //   buf->memDC  → текст с premultiplied alpha
    static void FlushEditBuffer() noexcept
    {
        auto* buf = g_activeEditBuffer;
        if (!buf)
        {
            g_sessionBlacknessRects.clear();
            return;
        }
        g_activeEditBuffer = nullptr;
        GdiFlush();
#if defined(_DEBUG)
        const bool logMemoFlush = AllowDelphiMemoLog(buf->hWnd, 1);
        if (logMemoFlush)
        {
            size_t nonzero = 0;
            unsigned maxAlpha = 0;
            MemoAlphaStats(buf->bits, buf->width, buf->height, nonzero, maxAlpha);
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiMemo] Flush hwnd=0x%p focused=%d blackRects=%zu memoBackgrounds=%zu paintDepth=%zu dcDepth=%zu inkAlphaCount=%zu inkAlphaMax=%u",
                buf->hWnd, GetFocus() == buf->hWnd ? 1 : 0, g_sessionBlacknessRects.size(),
                buf->memoBackgrounds.size(), g_editSessionStack.size(), g_editDcStack.size(), nonzero, maxAlpha);
        }
#endif

        const DWORD editStyle =
            buf->hWnd ? static_cast<DWORD>(GetWindowLongW(buf->hWnd, GWL_STYLE)) : 0u;

        // ── Compose-буфер ────────────────────────────────────────────────────
        HDC     composeDC = CreateCompatibleDC(buf->screenDC);
        HBITMAP composeBmp = nullptr;
        void* composeBits = nullptr;
        HGDIOBJ oldComposeBmp = nullptr;
        bool composed = false;

        if (composeDC)
        {
            if (buf->delphiMemo)
                composeBmp = CreateMemoComposeBitmap(buf->screenDC, buf->width, buf->height, &composeBits);
            if (!composeBmp)
                composeBmp = CreateCompatibleBitmap(buf->screenDC, buf->width, buf->height);
            if (composeBmp)
            {
                oldComposeBmp = SelectObject(composeDC, composeBmp);

                // ── 1. Фон: bgSnapshot (чистый glass) или fallback screenDC ─
                if (buf->bgSnapshot)
                {
                    HDC snapDC = CreateCompatibleDC(buf->screenDC);
                    if (snapDC)
                    {
                        HGDIOBJ oldSnap = SelectObject(snapDC, buf->bgSnapshot);
                        BitBlt(composeDC, 0, 0, buf->width, buf->height,
                            snapDC, 0, 0, SRCCOPY);
                        SelectObject(snapDC, oldSnap);
                        DeleteDC(snapDC);
                    }
                    else
                    {
                        // snap DC не создался — используем screenDC как fallback
                        BitBlt(composeDC, 0, 0, buf->width, buf->height,
                            buf->screenDC, 0, 0, SRCCOPY);
                    }
                }
                else
                {
                    BitBlt(composeDC, 0, 0, buf->width, buf->height,
                        buf->screenDC, 0, 0, SRCCOPY);
                }

                // ── 2. Фон для ETO_OPAQUE областей ───────────────────────────
                if (buf->delphiMemo)
                {
                    const bool primed = PrimeMemoSolidBackground(composeDC, *buf);
#if defined(_DEBUG)
                    if (logMemoFlush)
                        DebugLog(DBG_NORMAL, L"[GlassCore][DelphiMemo] SolidBackground hwnd=0x%p color=0x%08X confirmed=%d primed=%d target=compose",
                            buf->hWnd, static_cast<unsigned>(buf->memoSolidBackground),
                            HasConfirmedMemoSolidBackground(*buf) ? 1 : 0, primed ? 1 : 0);
#else
                    (void)primed;
#endif
                    ReplayMemoBackgrounds(composeDC, *buf);
                }
                else
                {
                    for (RECT r : g_sessionBlacknessRects)
                    {
                        if (editStyle == kEditStyleInsetTextRect)
                            InflateRect(&r, 2, 2);
                        RECT clip{ 0, 0, buf->width, buf->height };
                        if (!IntersectRect(&r, &r, &clip) || IsRectEmpty(&r)) continue;
                        PatBlt(composeDC, r.left, r.top, r.right - r.left, r.bottom - r.top, BLACKNESS);
                    }
                }

                // ── 3. AlphaBlend текста (tblend — из профиля капсулы, не RAM-превью) ──
                const BYTE sessionTblend =
                    GlassCore::Text::ResolveEffectiveTargetBlendAlpha(buf->useDualPass);
                BLENDFUNCTION bf{ AC_SRC_OVER, 0, sessionTblend, AC_SRC_ALPHA };
                const BOOL blendOk = GdiAlphaBlend(composeDC, 0, 0, buf->width, buf->height,
                    buf->memDC, 0, 0, buf->width, buf->height, bf);
#if defined(_DEBUG)
                if (logMemoFlush)
                {
                    GdiFlush();
                    // Проверяем фактический результат CreateDIBSection, а не
                    // знак высоты в GetObject(DIBSECTION): он может нормализоваться.
                    const bool isDib = composeBits != nullptr;
                    size_t nonzero = 0;
                    unsigned maxAlpha = 0;
                    if (isDib)
                        MemoAlphaStats(static_cast<const RGBQUAD*>(composeBits),
                            buf->width, buf->height, nonzero, maxAlpha);
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][DelphiMemo] Compose hwnd=0x%p dib=%d blend=%d tblend=%u alphaCount=%zu alphaMax=%u",
                        buf->hWnd, isDib ? 1 : 0, blendOk ? 1 : 0,
                        static_cast<unsigned>(sessionTblend), nonzero, maxAlpha);
                }
#else
                (void)blendOk;
#endif

                // ── 4. Один BitBlt на экран ─────────────────────────────────
                const BOOL copyOk = BitBlt(buf->screenDC, 0, 0, buf->width, buf->height,
                    composeDC, 0, 0, SRCCOPY);
#if defined(_DEBUG)
                if (logMemoFlush)
                    DebugLog(DBG_NORMAL, L"[GlassCore][DelphiMemo] Copy hwnd=0x%p ok=%d",
                        buf->hWnd, copyOk ? 1 : 0);
#else
                (void)copyOk;
#endif

                SelectObject(composeDC, oldComposeBmp);
                DeleteObject(composeBmp);
                composed = true;
            }
            DeleteDC(composeDC);
        }

        // Fallback если compose DC не создался
        if (!composed)
        {
#if defined(_DEBUG)
            if (logMemoFlush)
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiMemo] ComposeFallback hwnd=0x%p screenDc=0x%p",
                    buf->hWnd, buf->screenDC);
#endif
            if (buf->delphiMemo)
            {
                const bool primed = PrimeMemoSolidBackground(buf->screenDC, *buf);
#if defined(_DEBUG)
                if (logMemoFlush)
                    DebugLog(DBG_NORMAL, L"[GlassCore][DelphiMemo] SolidBackground hwnd=0x%p color=0x%08X confirmed=%d primed=%d target=fallback",
                        buf->hWnd, static_cast<unsigned>(buf->memoSolidBackground),
                        HasConfirmedMemoSolidBackground(*buf) ? 1 : 0, primed ? 1 : 0);
#else
                (void)primed;
#endif
                ReplayMemoBackgrounds(buf->screenDC, *buf);
            }
            else
            {
                for (RECT r : g_sessionBlacknessRects)
                {
                    if (editStyle == kEditStyleInsetTextRect)
                        InflateRect(&r, 2, 2);
                    RECT clip{ 0, 0, buf->width, buf->height };
                    if (!IntersectRect(&r, &r, &clip) || IsRectEmpty(&r)) continue;
                    PatBlt(buf->screenDC, r.left, r.top, r.right - r.left, r.bottom - r.top, BLACKNESS);
                }
            }
            const BYTE sessionTblend =
                GlassCore::Text::ResolveEffectiveTargetBlendAlpha(buf->useDualPass);
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, sessionTblend, AC_SRC_ALPHA };
            GdiAlphaBlend(buf->screenDC, 0, 0, buf->width, buf->height,
                buf->memDC, 0, 0, buf->width, buf->height, bf);
        }

        g_sessionBlacknessRects.clear();

        // Освобождаем ресурсы
        if (buf->bgSnapshot)
        {
            DeleteObject(buf->bgSnapshot);
            buf->bgSnapshot = nullptr;
        }
        SelectObject(buf->memDC, buf->oldBmp);
        DeleteObject(buf->dib);
        DeleteDC(buf->memDC);
        delete buf;
    }

    // ── ApplyEditBackgroundRect ─────────────────────────────────────────────
    // Обработка ETO_OPAQUE: PatBlt(BLACKNESS) + опциональный FillRect.
    static void ApplyEditBackgroundRect(
        HDC targetDC,
        HDC hdc,
        HWND sessionHwnd,
        const RECT* lprect,
        bool useSession,
        const COLORREF* backgroundOverride = nullptr) noexcept
    {
        if (!lprect || !sessionHwnd) return;

        RECT r = *lprect;
        const DWORD dwStyle = static_cast<DWORD>(GetWindowLongW(sessionHwnd, GWL_STYLE));
        if (dwStyle == kEditStyleInsetTextRect)
            InflateRect(&r, 2, 2);
        RECT clientClip{};
        GetClientRect(sessionHwnd, &clientClip);
        if (!IntersectRect(&r, &r, &clientClip) || IsRectEmpty(&r)) return;

        const BOOL cleared = PatBlt(targetDC, r.left, r.top, r.right - r.left, r.bottom - r.top, BLACKNESS);
#if defined(_DEBUG)
        if (AllowDelphiMemoLog(sessionHwnd, 2))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiMemo] ApplyBackground api=PatBlt hwnd=0x%p source=0x%p target=0x%p targetWnd=0x%p session=%d rect=[%ld,%ld,%ld,%ld] rop=BLACKNESS result=%d bk=0x%08X",
                sessionHwnd, hdc, targetDC, WindowFromDC(targetDC), useSession ? 1 : 0,
                r.left, r.top, r.right, r.bottom, cleared, static_cast<unsigned>(GetBkColor(hdc)));
#else
        (void)cleared;
#endif
        const COLORREF color = backgroundOverride ? *backgroundOverride : GetBkColor(hdc);
        const COLORREF rgb = color & 0xFFFFFFu;
        const COLORREF sysWnd = GetSysColor(COLOR_WINDOW) & 0xFFFFFFu;
        const COLORREF sysBtn = GetSysColor(COLOR_BTNFACE) & 0xFFFFFFu;
        const bool fill = rgb != 0 && rgb != sysBtn && rgb != sysWnd;

        if (useSession && g_activeEditBuffer && targetDC == g_activeEditBuffer->memDC)
        {
            if (g_activeEditBuffer->delphiMemo)
            {
                // BLACKNESS выше стирает старый текст из DIB. Заливку откладываем
                // до композиции, иначе она смешивается с текстом при tblend < 255.
                g_activeEditBuffer->memoBackgrounds.push_back({ r, color, fill });
                return;
            }
            PushSessionBlacknessRect(r, g_activeEditBuffer->width, g_activeEditBuffer->height);
        }

        wil::unique_hbrush brush{
            Shared::WinApiUtils::CreateSolidColorBrushWithAlpha(color, 100) };
        if (!brush) return;

        if (rgb == 0 || rgb == sysBtn || rgb == sysWnd)
        {
            // Прозрачный фон — только BLACKNESS, без FillRect
        }
        else
        {
            FillRect(targetDC, &r, brush.get());
        }
    }

    [[nodiscard]] static COLORREF GetRichEdit20BackgroundFromEm(HWND hWnd) noexcept
    {
        if (!hWnd) return RGB(0, 0, 0);
        const LRESULT lr = SendMessageW(hWnd, EM_GETBKGNDCOLOR, 0, 0);
        if (static_cast<COLORREF>(lr) == static_cast<COLORREF>(ECO_AUTOBKGND))
            return GetSysColor(COLOR_WINDOW);
        return static_cast<COLORREF>(lr);
    }

    [[nodiscard]] static COLORREF GetRichEdit20EffectiveBackground(HWND hWnd, HDC hdc) noexcept
    {
        if (!hWnd || !hdc) return RGB(0, 0, 0);
        const COLORREF em = GetRichEdit20BackgroundFromEm(hWnd);
        const COLORREF dc = GetBkColor(hdc);
        if ((em & 0xFFFFFFu) == 0 && (dc & 0xFFFFFFu) != 0) return dc;
        return em;
    }

    [[nodiscard]] static bool IsRichEdit20Only(GlassShared::GlassControlKind kind) noexcept
    {
        using namespace GlassShared;
        return kind == GlassControlKind::RichEdit20W
            || kind == GlassControlKind::WinForms_RichEdit20W;
    }

    // =========================================================================
    //  OVERALL DISPATCHER MAP
    // =========================================================================
    static bool IsEditKind(GlassShared::GlassControlKind kind);
    static bool IsRichEditKind(GlassShared::GlassControlKind kind) noexcept;

    BOOL WINAPI Edit_MyExtTextOutW(HDC, int, int, UINT, const RECT*, LPCWSTR, UINT, const INT*);
    BOOL WINAPI Edit_MyExtTextOutA(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*);
    BOOL WINAPI Edit_MyFillRect(HDC, const RECT*, HBRUSH);

    enum EditHookIndex : size_t
    {
        EHK_FILLRECT = 0,
        EHK_EXTTEXTOUT = 1,
        EHK_EXTTEXTOUTA = 2,
        EHK_COUNT
    };

    inline const HookDispatcherDependency<EHK_COUNT, 3> g_editHookDep
    { {
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "FillRect", reinterpret_cast<PVOID>(Edit_MyFillRect) },
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(Edit_MyExtTextOutW) },
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "ExtTextOutA", reinterpret_cast<PVOID>(Edit_MyExtTextOutA) },
    } };

    using EditDispatcher = HookDispatcher<EHK_COUNT, 3>;

    static std::unordered_map<void*, EditDispatcher> g_overallMap;
    static wil::srwlock g_overallLock;

    static bool EnableHooksOnModule(void* baseAddress, size_t index, bool enable)
    {
        if (!baseAddress) return false;
        auto lock = g_overallLock.lock_exclusive();
        auto it = g_overallMap.find(baseAddress);
        if (it == g_overallMap.end())
        {
            auto [inserted, ok] = g_overallMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(baseAddress),
                std::forward_as_tuple(g_editHookDep, baseAddress));
            it = inserted;
            it->second.CacheHookData();
        }
        else
        {
            it->second.moduleAddress = baseAddress;
        }
        it->second.EnableHook(index, enable);
        return true;
    }

    static void DisableAllOverallHooks()
    {
        auto lock = g_overallLock.lock_exclusive();
        for (auto& [addr, dispatcher] : g_overallMap)
            dispatcher.DisableAllHooks();
        g_overallMap.clear();
    }

    static HMODULE GetCallerModule(void* returnAddress)
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(returnAddress), &hMod);
        return hMod;
    }

    template <size_t index, typename T>
    static T GetOverallOrigSafe(HMODULE callerModule)
    {
        auto lock = g_overallLock.lock_shared();
        auto it = g_overallMap.find(static_cast<void*>(callerModule));
        if (it != g_overallMap.end())
            return it->second.template GetOrg<index, T>();
        return nullptr;
    }

    [[nodiscard]] static int GetScrollBandSizeForDpiCached(HDC hdc) noexcept
    {
        static thread_local UINT s_cachedDpi{ 0 };
        static thread_local int  s_cachedBand{ 0 };

        HWND hwnd = WindowFromDC(hdc);
        if (!hwnd || !IsWindow(hwnd))
            hwnd = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();

        UINT dpi = 96;
        if (hwnd && IsWindow(hwnd)) dpi = GetDpiForWindow(hwnd);
        if (!dpi) dpi = 96;

        if (s_cachedDpi != dpi || s_cachedBand <= 0)
        {
            int band = 0;
            if (dpi != 96)
            {
                band = std::max(
                    GetSystemMetricsForDpi(SM_CXVSCROLL, dpi),
                    GetSystemMetricsForDpi(SM_CYHSCROLL, dpi));
            }
            else
            {
                band = std::max(GetSystemMetrics(SM_CXVSCROLL), GetSystemMetrics(SM_CYHSCROLL));
            }
            if (band <= 0)
                band = std::max(GetSystemMetrics(SM_CXVSCROLL), GetSystemMetrics(SM_CYHSCROLL));
            s_cachedDpi = dpi;
            s_cachedBand = band;
        }
        return s_cachedBand;
    }

    // =========================================================================
    //  Hook implementations
    // =========================================================================

    static HWND GetSessionHwnd() noexcept
    {
        if (!g_editSessionStack.empty()) return g_editSessionStack.back().hwnd;
        if (!g_editDcStack.empty())      return g_editDcStack.back().hwnd;
        return nullptr;
    }

    // Резолв HWND для ExtTextOut: стек BeginPaint/GetDC → RAM-превью GlassSettings → оконный DC Edit → MEMDC + хост paint.
    // Единое правило с внешними процессами: не полагаться только на превью-ветку (иначе часть вызовов уходит в orig).
    [[nodiscard]] static HWND ResolveEditSessionHwndForExtTextOut(HDC hdc, const wchar_t** outResolveTag) noexcept
    {
        auto setTag = [outResolveTag](const wchar_t* t) noexcept
            {
                if (outResolveTag) *outResolveTag = t;
            };

        HWND s = GetSessionHwnd();
        if (s)
        {
            setTag(L"stack");
            return s;
        }

        HWND w = WindowFromDC(hdc);
        if (w && IsEditKind(GlassShared::GlassGetWindowKind(w)))
        {
            setTag(L"windowDc");
            return w;
        }

        if (!w && GetObjectType(hdc) == OBJ_MEMDC)
        {
            HWND ph = GlassCore::PaintDispatcher::GetPaintSessionHostHwnd();
            if (ph && IsEditKind(GlassShared::GlassGetWindowKind(ph)))
            {
                setTag(L"paintHostMemDc");
                return ph;
            }
        }

        setTag(L"fail");
        return nullptr;
    }

    // Служебные проходы comctl32 (логи: c=1, ETO_IGNORELANGUAGE, без ETO_GLYPH_INDEX) не подменяем
    // RAM-капсулой превью — иначе параметры ink суммируются в сессионном DIB иначе, чем в других процессах.
    [[nodiscard]] static bool ShouldApplyPreviewCapsuleToExtTextOut(UINT options, UINT c) noexcept
    {
        if (c != 1u)
            return true;
        if ((options & ETO_IGNORELANGUAGE) == 0u)
            return true;
        if ((options & ETO_GLYPH_INDEX) != 0u)
            return true;
        return false;
    }

    // ── Edit_WorkExtTextOutCore ─────────────────────────────────────────────
    static BOOL Edit_WorkExtTextOutCore(
        HWND sessionHwnd,
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx,
        HMODULE callerMod, decltype(&ExtTextOutW) origW)
    {
        const GlassShared::GlassControlKind controlKind = GlassShared::GlassGetWindowKind(sessionHwnd);

        // В VCL TMemo BeginPaint может синхронно вызвать WM_NCPAINT и GetDCEx
        // с DCX_WINDOW до того, как вернёт клиентский HDC. Этот временный DC
        // освобождается раньше EndPaint: привязывать к нему буфер нельзя.
        // WindowFromDC не различает эти два DC; используем точный HDC из
        // post-BeginPaint и не переадресуем в буфер чужие/вложенные DC.
        if (!g_editSessionStack.empty() && (IsDelphiMemo(sessionHwnd)
            || GlassCore::DelphiInput::IsSpinEdit(sessionHwnd)))
        {
            const auto& paint = g_editSessionStack.back();
            if (paint.hwnd != sessionHwnd || !paint.paintDc || paint.paintDc != hdc
                || !GlassCore::PaintDispatcher::IsCurrentPaintDcForHwnd(sessionHwnd, hdc)
                || (g_activeEditBuffer && (g_activeEditBuffer->hWnd != sessionHwnd
                    || g_activeEditBuffer->screenDC != paint.paintDc)))
            {
#if defined(_DEBUG)
                if (AllowDelphiMemoLog(sessionHwnd, 0))
                    DebugLog(DBG_NORMAL,
                        L"[GlassCore][DelphiMemo] NonPaintPass hwnd=0x%p dc=0x%p paintHwnd=0x%p paintDc=0x%p options=0x%X count=%u",
                        sessionHwnd, hdc, paint.hwnd, paint.paintDc, options, c);
#endif
                return origW(hdc, x, y, options, lprect, lpString, c, lpDx);
            }
        }

        if (!g_editSessionStack.empty() && c && !g_editSessionStack.back().numberBackgroundCleared
            && g_editSessionStack.back().hwnd == sessionHwnd
            && g_editSessionStack.back().paintDc == hdc
            && GlassCore::DelphiInput::ClearNumberBoxGlyphBackground(sessionHwnd, hdc, lprect, options))
            g_editSessionStack.back().numberBackgroundCleared = true;

        SyncActiveEditBufferDualPassFromGlobals();

        // ── Определяем нужен ли session buffer (только канал BeginPaint) ───────
        const bool wantSession =
            !g_bypassEditBuffer
            && ShouldUseMultilineEditSessionBuffer(sessionHwnd, controlKind)
            && UseSessionBufferForDC(sessionHwnd, hdc);

        // Сессионный DIB только при активном BeginPaint (WM_PAINT). При перетаскивании
        // выделения comctl32 вызывает только GetDC/ReleaseDC — без этого условия буфер
        // накапливался бы и Flush на Release заливал бы экран неполным кадром (артефакт фона).
        if (wantSession && !g_activeEditBuffer && !g_editSessionStack.empty())
            EnsureEditBuffer(sessionHwnd, hdc, /*fromGetDC=*/false);

        const bool useSession = wantSession && g_activeEditBuffer != nullptr
            && !g_editSessionStack.empty();

        HDC targetDC = hdc;
        if (useSession && g_activeEditBuffer
            && EditSessionBufferAcceptsSourceDc(g_activeEditBuffer, sessionHwnd, hdc))
        {
            targetDC = g_activeEditBuffer->memDC;
            SelectObject(targetDC, GetCurrentObject(hdc, OBJ_FONT));
            SetTextColor(targetDC, GetTextColor(hdc));
            SetBkColor(targetDC, GetBkColor(hdc));
            SetBkMode(targetDC, GetBkMode(hdc));
        }

#if defined(_DEBUG)
        if (AllowDelphiMemoLog(sessionHwnd, 0))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiMemo] ExtTextOut hwnd=0x%p focused=%d dc=0x%p dcWnd=0x%p "
                L"paintDepth=%zu dcDepth=%zu count=%u options=0x%X textColor=0x%08X bk=0x%08X "
                L"wantSession=%d useSession=%d accepted=%d rects=%zu",
                sessionHwnd, GetFocus() == sessionHwnd ? 1 : 0, hdc, WindowFromDC(hdc),
                g_editSessionStack.size(), g_editDcStack.size(), c, options,
                static_cast<unsigned>(GetTextColor(hdc)), static_cast<unsigned>(GetBkColor(hdc)),
                wantSession ? 1 : 0, useSession ? 1 : 0,
                EditSessionBufferAcceptsSourceDc(g_activeEditBuffer, sessionHwnd, hdc) ? 1 : 0,
                g_sessionBlacknessRects.size());
        if (AllowDelphiInputLog(sessionHwnd, 0))
        {
            WCHAR cls[64]{};
            GetClassNameW(sessionHwnd, cls, _countof(cls));
            const RECT r = lprect ? *lprect : RECT{};
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiInput] ExtTextOut hwnd=0x%p class=%s focus=%d dc=0x%p dcWnd=0x%p obj=%u caller=0x%p paintDepth=%zu dcDepth=%zu options=0x%X count=%u rect=[%ld,%ld,%ld,%ld] bk=0x%08X textColor=0x%08X",
                sessionHwnd, cls, GetFocus() == sessionHwnd ? 1 : 0, hdc, WindowFromDC(hdc),
                static_cast<unsigned>(GetObjectType(hdc)), callerMod, g_editSessionStack.size(), g_editDcStack.size(),
                options, c, r.left, r.top, r.right, r.bottom,
                static_cast<unsigned>(GetBkColor(hdc)), static_cast<unsigned>(GetTextColor(hdc)));
        }
        if (AllowDelphiComboEditLog(sessionHwnd))
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiComboEdit] ExtTextOut hwnd=0x%p parent=0x%p focused=%d dc=0x%p dcWnd=0x%p caller=0x%p paintDepth=%zu dcDepth=%zu options=0x%X count=%u x=%d y=%d bk=0x%08X",
                sessionHwnd, GetParent(sessionHwnd), GetFocus() == sessionHwnd ? 1 : 0,
                hdc, WindowFromDC(hdc), callerMod, g_editSessionStack.size(), g_editDcStack.size(),
                options, c, x, y, static_cast<unsigned>(GetBkColor(hdc)));
        if (AllowDelphiMemoLog(sessionHwnd, 0))
        {
            RECT clip{};
            const int clipKind = GetClipBox(hdc, &clip);
            const RECT r = lprect ? *lprect : RECT{};
            DebugLog(DBG_NORMAL,
                L"[GlassCore][DelphiMemo] TextGeometry hwnd=0x%p caller=0x%p dc=0x%p target=0x%p obj=%d x=%d y=%d rectPresent=%d rect=[%ld,%ld,%ld,%ld] clipKind=%d clip=[%ld,%ld,%ld,%ld] align=0x%X mode=%d",
                sessionHwnd, callerMod, hdc, targetDC, GetObjectType(hdc), x, y, lprect ? 1 : 0,
                r.left, r.top, r.right, r.bottom, clipKind, clip.left, clip.top, clip.right, clip.bottom,
                GetTextAlign(hdc), GetBkMode(hdc));
        }
#endif

        // ── RichEdit20: фон на весь клиент (без изменений) ────────────────────
        if (IsRichEdit20Only(controlKind) && sessionHwnd)
        {
            const COLORREF emBg = GetRichEdit20BackgroundFromEm(sessionHwnd);
            if (g_re20FullClientBgPrimedHwnd != sessionHwnd || g_re20LastAppliedEmBg != emBg)
            {
                RECT cr{};
                GetClientRect(sessionHwnd, &cr);
                const COLORREF reBgEff = GetRichEdit20EffectiveBackground(sessionHwnd, hdc);
                ApplyEditBackgroundRect(targetDC, hdc, sessionHwnd, &cr, useSession, &reBgEff);
                g_re20FullClientBgPrimedHwnd = sessionHwnd;
                g_re20LastAppliedEmBg = emBg;
            }
        }

        // ── 1. Фон (ETO_OPAQUE) ────────────────────────────────────────────────
        // ETO_OPAQUE обрабатывается здесь через PatBlt/FillRect.
        // В ExtTextOutWithAlpha(ToSession) options приходят уже БЕЗ ETO_OPAQUE (drawOptions).
        if ((options & ETO_OPAQUE) && lprect)
            ApplyEditBackgroundRect(targetDC, hdc, sessionHwnd, lprect, useSession);

        // ── 2. Нет символов — фон нарисован, текста нет ───────────────────────
        if (!c) return TRUE;

        // ── 3. Текст ───────────────────────────────────────────────────────────
#if 0
        // Hot path: отладочный лог и syscalls только для него (perf).
        if (c >= 2)
        {
            const DWORD style =
                sessionHwnd && IsWindow(sessionHwnd)
                    ? static_cast<DWORD>(GetWindowLongW(sessionHwnd, GWL_STYLE))
                    : 0u;
            const bool hscroll = (style & WS_HSCROLL) != 0;
            const BYTE rmGlobal = GlassCore::Text::g_ExtTextOutRenderMode;
            const int plainDual =
                sessionHwnd
                    ? (GlassCore::Text::PlainEditUseDualPass(sessionHwnd, rmGlobal) ? 1 : 0)
                    : -1;
            const int bufDual =
                g_activeEditBuffer ? static_cast<int>(g_activeEditBuffer->useDualPass) : -1;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][TextInk] EditExtOut hwnd=0x%p hscroll=%d rmGlob=%u plainDual=%d useSess=%d bufDual=%d glowR=%u gamma=%.4f tblend=%u",
                sessionHwnd,
                hscroll ? 1 : 0,
                static_cast<unsigned>(rmGlobal),
                plainDual,
                useSession ? 1 : 0,
                bufDual,
                static_cast<unsigned>(GlassCore::Text::g_ExtTextOutGlowRadiusThousandths),
                GlassCore::Text::g_ExtTextOutCoverageGamma,
                static_cast<unsigned>(GlassCore::Text::g_ExtTextOutTargetBlendAlpha));
            if (sessionHwnd && ::IsWindow(sessionHwnd))
            {
                const UINT dpi = GetDpiForWindow(sessionHwnd);
                LOGFONTW lf{};
                if (HFONT hf = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT)))
                {
                    if (GetObjectW(hf, sizeof(lf), &lf) == sizeof(lf))
                    {
                        DebugLog(DBG_NORMAL,
                            L"[GlassCore][TextInk] EditExtOut rasterInput dpi=%u lfHeight=%d lfQuality=%u face=\"%s\"",
                            dpi,
                            static_cast<int>(lf.lfHeight),
                            static_cast<unsigned>(lf.lfQuality),
                            lf.lfFaceName);
                    }
                }
            }
        }
#endif

#if defined(GLASS_INK_CAPSULE_VERIFY)
        if (applyPreviewCapsule && c > 0)
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Edit] ink verify preview: rm=%u dualPass=%d gamma=%.4f",
                static_cast<unsigned>(GlassCore::Text::g_ExtTextOutRenderMode),
                g_activeEditBuffer ? static_cast<int>(g_activeEditBuffer->useDualPass) : -1,
                GlassCore::Text::g_ExtTextOutCoverageGamma);
        }
#endif

#if 0
        // Hot path: per-call ExtTextOut DRAW log (perf).
        if (lprect && !IsRectEmpty(lprect))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Edit] ExtTextOut DRAW: hwnd=0x%p mod=0x%p c=%u opt=0x%X useSession=%d dualPass=%d rect=(%ld,%ld,%ld,%ld)",
                sessionHwnd, callerMod, c, options,
                static_cast<int>(useSession),
                g_activeEditBuffer ? static_cast<int>(g_activeEditBuffer->useDualPass) : -1,
                static_cast<long>(lprect->left), static_cast<long>(lprect->top),
                static_cast<long>(lprect->right), static_cast<long>(lprect->bottom));
        }
        else
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Edit] ExtTextOut DRAW: hwnd=0x%p mod=0x%p c=%u opt=0x%X useSession=%d dualPass=%d",
                sessionHwnd, callerMod, c, options,
                static_cast<int>(useSession),
                g_activeEditBuffer ? static_cast<int>(g_activeEditBuffer->useDualPass) : -1);
        }
#endif

        // ETO_OPAQUE уже обработан выше — убираем из опций для рендера
        const UINT drawOptions = options & ~static_cast<UINT>(ETO_OPAQUE);

        if (useSession && g_activeEditBuffer
            && EditSessionBufferAcceptsSourceDc(g_activeEditBuffer, sessionHwnd, hdc))
        {
            const bool useDualPass = g_activeEditBuffer->useDualPass;
            const auto& inkCapsule = GlassCore::Text::GetRuntimeInkCapsuleProfile(useDualPass);
            GlassCore::Text::TextSessionBuffer session{
                g_activeEditBuffer->bits,
                g_activeEditBuffer->width,
                g_activeEditBuffer->height,
                useDualPass,
                true
            };

            if (SUCCEEDED(GlassCore::Text::ExtTextOutWithAlphaToSession(
                targetDC, x, y, drawOptions, lprect, lpString, c, lpDx, origW, session, &inkCapsule)))
                return TRUE;

            // Session render failed: сбрасываем буфер, рисуем напрямую
            FlushEditBuffer();
            g_bypassEditBuffer = true;
            return origW(hdc, x, y, drawOptions, lprect, lpString, c, lpDx);
        }

        // Прямой путь: многострочный plain GetDC — как WM_PAINT-сессия (узкий rect + тот же dual/single).
        const bool multilinePlainGetDc =
            !g_bypassEditBuffer
            && ShouldUseMultilineEditSessionBuffer(sessionHwnd, controlKind)
            && UseSessionBufferForDC(sessionHwnd, hdc)
            && g_editSessionStack.empty();
        if (multilinePlainGetDc)
        {
            BYTE rm = GlassCore::Text::g_ExtTextOutRenderMode;
            const bool sessionLikeUseDualPass = GlassCore::Text::PlainEditUseDualPass(sessionHwnd, rm);
            const auto& inkCapsule =
                GlassCore::Text::GetRuntimeInkCapsuleProfile(sessionLikeUseDualPass);
            const HRESULT hrGetDc = GlassCore::Text::ExtTextOutWithAlphaMultilinePlainGetDc(
                hdc, x, y, drawOptions, lprect, lpString, c, lpDx, origW, sessionLikeUseDualPass, &inkCapsule);
#if defined(_DEBUG)
            if (AllowDelphiMemoLog(sessionHwnd, 2))
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiMemo] DirectGetDC hwnd=0x%p focused=%d dc=0x%p dcWnd=0x%p options=0x%X count=%u dual=%d hr=0x%08lX fallback=%d",
                    sessionHwnd, GetFocus() == sessionHwnd ? 1 : 0, hdc, WindowFromDC(hdc),
                    options, c, sessionLikeUseDualPass ? 1 : 0,
                    static_cast<unsigned long>(hrGetDc), FAILED(hrGetDc) ? 1 : 0);
#endif
            if (SUCCEEDED(hrGetDc)) return TRUE;
            return origW(hdc, x, y, drawOptions, lprect, lpString, c, lpDx);
        }

        // Однострочный plain / RichEdit: прежние режимы.
        // TODO(perf): расширить TextSessionBuffer / g_activeEditBuffer (сейчас multiline + BeginPaint)
        // на однострочный plain Edit и RichEdit — один BeginBufferedPaint/EndBufferedPaint на цикл
        // перерисовки контрола, а не на каждый ExtTextOutW. Отдельная задача после замеров 1–5.
        const bool singleLinePlainEdit =
            !ShouldUseMultilineEditSessionBuffer(sessionHwnd, controlKind)
            && (controlKind == GlassShared::GlassControlKind::Edit
                || controlKind == GlassShared::GlassControlKind::WinForms_Edit);
        const auto directMode = singleLinePlainEdit
            ? GlassCore::Text::ExtTextOutAlphaMode::EditSingleLinePlain
            : GlassCore::Text::ExtTextOutAlphaMode::FullQuality;
        if (singleLinePlainEdit)
        {
            BYTE rm = GlassCore::Text::g_ExtTextOutRenderMode;
            const bool plainDual = GlassCore::Text::PlainEditUseDualPassSingleLinePlain(sessionHwnd, rm);
            const auto& inkCapsule = GlassCore::Text::GetRuntimeInkCapsuleProfile(plainDual);
            const HRESULT hrPlain = GlassCore::Text::ExtTextOutWithAlpha(
                hdc, x, y, drawOptions, lprect, lpString, c, lpDx, origW, directMode, sessionHwnd, &inkCapsule);
#if defined(_DEBUG)
            if (AllowDelphiInputLog(sessionHwnd, 1))
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiInput] TextResult hwnd=0x%p dc=0x%p dcWnd=0x%p options=0x%X count=%u hr=0x%08lX fallback=%d",
                    sessionHwnd, hdc, WindowFromDC(hdc), options, c,
                    static_cast<unsigned long>(hrPlain), FAILED(hrPlain) ? 1 : 0);
            if (AllowDelphiComboEditLog(sessionHwnd))
                DebugLog(DBG_NORMAL, L"[GlassCore][DelphiComboEdit] TextResult hwnd=0x%p dc=0x%p options=0x%X count=%u hr=0x%08lX fallback=%d",
                    sessionHwnd, hdc, options, c, static_cast<unsigned long>(hrPlain), FAILED(hrPlain) ? 1 : 0);
#endif
            if (SUCCEEDED(hrPlain))
                return TRUE;
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Edit] ExtTextOutWithAlpha plain fallback origW: hr=0x%08lX hwnd=0x%p c=%u dual=%d",
                static_cast<unsigned long>(hrPlain), sessionHwnd, c, plainDual ? 1 : 0);
        }
        else
        {
            const bool dualPass = GlassCore::Text::g_ExtTextOutRenderMode != 1;
            const auto& inkCapsule = GlassCore::Text::GetRuntimeInkCapsuleProfile(dualPass);
            if (SUCCEEDED(GlassCore::Text::ExtTextOutWithAlpha(
                hdc, x, y, drawOptions, lprect, lpString, c, lpDx, origW, directMode, nullptr, &inkCapsule)))
                return TRUE;
        }

        return origW(hdc, x, y, drawOptions, lprect, lpString, c, lpDx);
    }

    BOOL WINAPI Edit_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<EHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);
        if (!orig)
            return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        const wchar_t* resolveTag = L"?";
        HWND sessionHwnd = ResolveEditSessionHwndForExtTextOut(hdc, &resolveTag);
        if (!sessionHwnd)
        {
            DebugLog(DBG_NORMAL, L"[GlassCore][Edit] ExtTextOutW SKIP: resolve=%s mod=0x%p c=%u", resolveTag, callerMod, c);
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
        }

        HWND dcWnd = WindowFromDC(hdc);
        if (dcWnd && !IsEditKind(GlassShared::GlassGetWindowKind(dcWnd)))
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

#if defined(_DEBUG)
        if (IsDelphiMemo(sessionHwnd) && (options & ETO_GLYPH_INDEX))
        {
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 4; }
            if (budget)
            {
                --budget;
                void* frames[8]{};
                const USHORT captured = CaptureStackBackTrace(0, _countof(frames), frames, nullptr);
                DebugLog(DBG_NORMAL,
                    L"[GlassCore][DelphiMemo] ExtTextOutStack hwnd=0x%p focused=%d dc=0x%p dcWnd=0x%p caller=0x%p options=0x%X frames=%u stack=[%p,%p,%p,%p,%p,%p,%p,%p]",
                    sessionHwnd, GetFocus() == sessionHwnd ? 1 : 0, hdc, dcWnd, callerMod, options,
                    static_cast<unsigned>(captured), frames[0], frames[1], frames[2], frames[3],
                    frames[4], frames[5], frames[6], frames[7]);
            }
        }
#endif
        return Edit_WorkExtTextOutCore(sessionHwnd, hdc, x, y, options, lprect, lpString, c, lpDx, callerMod, orig);
    }

    BOOL WINAPI Edit_MyFillRect(HDC hdc, const RECT* lprc, HBRUSH hbr)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<EHK_FILLRECT, decltype(&FillRect)>(callerMod);
        if (!orig) return FillRect(hdc, lprc, hbr);

        if (g_editSessionStack.empty() && g_editDcStack.empty())
            return orig(hdc, lprc, hbr);
        if (!lprc || !hbr)
            return orig(hdc, lprc, hbr);

        if (!g_editSessionStack.empty())
        {
            const auto& paint = g_editSessionStack.back();
            if (paint.hotKeyFillHook && paint.paintDc == hdc
                && GlassCore::DelphiInput::ClearHotKeyBackground(paint.hwnd, hdc, lprc, hbr))
                return TRUE;
        }

        LOGBRUSH lb{};
        if (GetObject(hbr, sizeof(lb), &lb) && lb.lbStyle == BS_SOLID)
        {
            const int rcW = lprc->right - lprc->left;
            const int rcH = lprc->bottom - lprc->top;
            const int band = GetScrollBandSizeForDpiCached(hdc);
            if (lb.lbColor == GetSysColor(COLOR_SCROLLBAR)
                && rcW > 0 && rcH > 0 && band > 0
                && std::abs(rcW - rcH) <= 1
                && std::abs(rcW - band) <= 1
                && std::abs(rcH - band) <= 1)
            {
                PatBlt(hdc, lprc->left, lprc->top, rcW, rcH, BLACKNESS);
                return TRUE;
            }
        }
        return orig(hdc, lprc, hbr);
    }

    BOOL WINAPI Edit_MyExtTextOutA(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto origA = GetOverallOrigSafe<EHK_EXTTEXTOUTA, decltype(&ExtTextOutA)>(callerMod);
        auto origW = GetOverallOrigSafe<EHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!origA) return ExtTextOutA(hdc, x, y, options, lprect, lpString, c, lpDx);
        if (!origW)
            origW = reinterpret_cast<decltype(&ExtTextOutW)>(
                GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "ExtTextOutW"));

        const wchar_t* resolveTagA = L"?";
        HWND sessionHwnd = ResolveEditSessionHwndForExtTextOut(hdc, &resolveTagA);
        if (!sessionHwnd)
        {
            DebugLog(DBG_NORMAL, L"[GlassCore][Edit] ExtTextOutA SKIP: resolve=%s mod=0x%p c=%u", resolveTagA, callerMod, c);
            return origA(hdc, x, y, options, lprect, lpString, c, lpDx);
        }

        if (c >= 2u)
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][Edit] ExtTextOutA resolve=%s hwnd=0x%p bpDepth=%zu getdcDepth=%zu hdcWnd=0x%p hdcObj=%d",
                resolveTagA,
                sessionHwnd,
                g_editSessionStack.size(),
                g_editDcStack.size(),
                WindowFromDC(hdc),
                static_cast<int>(GetObjectType(hdc)));
        }

        HWND dcWnd = WindowFromDC(hdc);
        if (dcWnd && !IsEditKind(GlassShared::GlassGetWindowKind(dcWnd)))
            return origA(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (!c)
        {
            if (!origW) return origA(hdc, x, y, options, lprect, lpString, c, lpDx);
            return Edit_WorkExtTextOutCore(sessionHwnd, hdc, x, y, options, lprect, nullptr, 0, lpDx, callerMod, origW);
        }
        if (!lpString)
            return origA(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (options & ETO_GLYPH_INDEX)
        {
            if (!origW) return origA(hdc, x, y, options, lprect, lpString, c, lpDx);
            return Edit_WorkExtTextOutCore(sessionHwnd, hdc, x, y, options, lprect,
                reinterpret_cast<LPCWSTR>(lpString), c, lpDx, callerMod, origW);
        }

        const int wlen = MultiByteToWideChar(CP_ACP, 0, lpString, static_cast<int>(c), nullptr, 0);
        if (wlen <= 0) return origA(hdc, x, y, options, lprect, lpString, c, lpDx);

        std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
        if (MultiByteToWideChar(CP_ACP, 0, lpString, static_cast<int>(c), wbuf.data(), wlen) != wlen)
            return origA(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (!origW) return origA(hdc, x, y, options, lprect, lpString, c, lpDx);
        return Edit_WorkExtTextOutCore(sessionHwnd, hdc, x, y, options, lprect,
            wbuf.c_str(), static_cast<UINT>(wlen), lpDx, callerMod, origW);
    }

    // =========================================================================
    //  EnableEditHooksForModule (без изменений)
    // =========================================================================
    static void EnableEditHooksForModule(
        HMODULE callerMod,
        GlassShared::GlassControlKind kind,
        bool enable)
    {
        using namespace GlassShared;

        /*auto enableFillRectOnKnownModules = [&](HMODULE preferredTextModule) noexcept
            {
                EnableHooksOnModule(callerMod, EHK_FILLRECT, enable);
                EnableHooksOnModule(GetModuleHandleW(L"user32.dll"), EHK_FILLRECT, enable);
                EnableHooksOnModule(GlassCore::ModuleHandler::g_comctl32New, EHK_FILLRECT, enable);
                EnableHooksOnModule(GlassCore::ModuleHandler::g_comctl32Legacy, EHK_FILLRECT, enable);
                EnableHooksOnModule(preferredTextModule, EHK_FILLRECT, enable);
            };*/

        switch (kind)
        {
        case GlassControlKind::Edit:
        case GlassControlKind::WinForms_Edit:
        {
            //enableFillRectOnKnownModules(nullptr);
            // Текст Edit рисуется из user32 (IAT → gdi32); только gdi32full не покрывает все сборки.
            if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
            {
                EnableHooksOnModule(user32, EHK_EXTTEXTOUT, enable);
                EnableHooksOnModule(user32, EHK_EXTTEXTOUTA, enable);
            }
            if (void* gdi32full = GetModuleHandleW(L"gdi32full.dll"))
                EnableHooksOnModule(gdi32full, EHK_EXTTEXTOUT, enable);
            break;
        }
        case GlassControlKind::RichEdit20W:
        case GlassControlKind::WinForms_RichEdit20W:
        {
            void* riched20 = GetModuleHandleW(L"riched20.dll");
            //enableFillRectOnKnownModules(static_cast<HMODULE>(riched20));
            EnableHooksOnModule(riched20, EHK_EXTTEXTOUT, enable);
            EnableHooksOnModule(riched20, EHK_EXTTEXTOUTA, enable);
            break;
        }
        case GlassControlKind::RichEdit50W:
        case GlassControlKind::WinForms_RichEdit50W:
        {
            void* msftedit = GetModuleHandleW(L"msftedit.dll");
            void* gdi32full = GetModuleHandleW(L"gdi32full.dll");
            //enableFillRectOnKnownModules(static_cast<HMODULE>(msftedit));
            EnableHooksOnModule(gdi32full, EHK_EXTTEXTOUT, enable);
            break;
        }
        default: break;
        }
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================

    static bool IsEditKind(GlassShared::GlassControlKind kind)
    {
        using namespace GlassShared;
        return kind == GlassControlKind::Edit
            || kind == GlassControlKind::WinForms_Edit
            || kind == GlassControlKind::RichEdit20W
            || kind == GlassControlKind::WinForms_RichEdit20W
            || kind == GlassControlKind::RichEdit50W
            || kind == GlassControlKind::WinForms_RichEdit50W;
    }

    static bool IsRichEditKind(GlassShared::GlassControlKind kind) noexcept
    {
        using namespace GlassShared;
        return kind == GlassControlKind::RichEdit20W
            || kind == GlassControlKind::WinForms_RichEdit20W
            || kind == GlassControlKind::RichEdit50W
            || kind == GlassControlKind::WinForms_RichEdit50W;
    }

    // ── Канал 1: BeginPaint / EndPaint ────────────────────────────────────────
    static void OnBeginPaintEdit(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        auto kind = GlassGetWindowKind(hWnd);
        if (!IsEditKind(kind)) return;

        GlassCore::TextInk::EnsureTextInkClientRegistered(hWnd);

        g_bypassEditBuffer = false;
        g_sessionBlacknessRects.clear();
        if (IsRichEdit20Only(kind))
        {
            g_re20FullClientBgPrimedHwnd = nullptr;
            g_re20LastAppliedEmBg = CLR_INVALID;
        }
        g_editSessionStack.push_back({ hWnd, callerMod, kind });
        DebugLog(DBG_NORMAL,
            L"[GlassCore][Edit] BeginPaint: mod=0x%p hWnd=0x%p kind=%d depth=%zu",
            callerMod, hWnd, static_cast<int>(kind), g_editSessionStack.size());
        EnableEditHooksForModule(callerMod, kind, true);
        if (callerMod && GlassCore::DelphiInput::IsHotKey(hWnd))
        {
            EnableHooksOnModule(callerMod, EHK_FILLRECT, true);
            g_editSessionStack.back().hotKeyFillHook = true;
        }
    }

    [[nodiscard]] static bool OnEditAfterBeginPaint(
        HWND hWnd, HDC hdc, LPPAINTSTRUCT /*ps*/, HMODULE /*callerMod*/) noexcept
    {
        if (g_shutdown.load(std::memory_order_acquire) || !hdc
            || (!IsDelphiMemo(hWnd) && !GlassCore::DelphiInput::IsSpinEdit(hWnd)
                && !GlassCore::DelphiInput::IsNumberBox(hWnd) && !GlassCore::DelphiInput::IsHotKey(hWnd))
            || g_editSessionStack.empty() || g_editSessionStack.back().hwnd != hWnd)
            return false;
        g_editSessionStack.back().paintDc = hdc;
#if defined(_DEBUG)
        if (AllowDelphiMemoLog(hWnd, 1))
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiMemo] PaintBind hwnd=0x%p dc=0x%p depth=%zu",
                hWnd, hdc, g_editSessionStack.size());
#endif
        return false; // только связываем HDC, фон не очищаем
    }

    static void OnEndPaintEdit(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        auto kind = GlassGetWindowKind(hWnd);
        if (!IsEditKind(kind)) return;
        if (g_editSessionStack.empty()) return;

        FlushEditBuffer();
        g_bypassEditBuffer = false;

        const EditPaintSession s = g_editSessionStack.back();
        g_editSessionStack.pop_back();
        if (s.hotKeyFillHook) EnableHooksOnModule(s.mod, EHK_FILLRECT, false);
        EnableEditHooksForModule(s.mod, s.kind, false);
        DebugLog(DBG_NORMAL, L"[GlassCore][Edit] EndPaint: hWnd=0x%p depth=%zu",
            hWnd, g_editSessionStack.size());
    }

    // ── Канал 2: GetDC / ReleaseDC (интерактив: выделение, каретка) ───────────
    static void OnGetDCEdit(HWND hWnd, HMODULE callerMod)
    {
        if (g_shutdown.load(std::memory_order_acquire)) return;

        using namespace GlassShared;
        auto kind = GlassGetWindowKind(hWnd);
        if (!IsEditKind(kind)) return;

        GlassCore::TextInk::EnsureTextInkClientRegistered(hWnd);

        // GetDC может быть вложен в WM_PAINT: его начало не должно стирать
        // накопленные ETO_OPAQUE-прямоугольники или сбрасывать fallback буфера.
        if (g_editSessionStack.empty() && g_editDcStack.empty())
        {
            g_bypassEditBuffer = false;
            g_sessionBlacknessRects.clear();
        }
        if (IsRichEdit20Only(kind) && g_editSessionStack.empty())
        {
            g_re20FullClientBgPrimedHwnd = nullptr;
            g_re20LastAppliedEmBg = CLR_INVALID;
        }

        g_editDcStack.push_back({ hWnd, callerMod, kind });
        EnableEditHooksForModule(callerMod, kind, true);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][Edit] GetDC: hWnd=0x%p kind=%d depth=%zu",
            hWnd, static_cast<int>(kind), g_editDcStack.size());
    }

    static void OnReleaseDCEdit(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        auto kind = GlassGetWindowKind(hWnd);
        if (!IsEditKind(kind)) return;
        if (g_editDcStack.empty()) return;

        const EditPaintSession s = g_editDcStack.back();
        g_editDcStack.pop_back();
        EnableEditHooksForModule(s.mod, s.kind, false);

        // Flush только когда завершились все GetDC-сессии И нет BeginPaint-сессии
        if (g_editSessionStack.empty() && g_editDcStack.empty())
        {
            FlushEditBuffer();
            g_bypassEditBuffer = false;
        }

        DebugLog(DBG_NORMAL, L"[GlassCore][Edit] ReleaseDC: hWnd=0x%p depth=%zu",
            hWnd, g_editDcStack.size());
    }

    // =========================================================================
    //  Lifecycle
    // =========================================================================
    void Prepare() {}

    // Предпрогрев IAT-кеша (только чтение: emplace + CacheHookData, без записей).
    // Холодный скан PE уходит из paint-кадра: в Startup (уже загруженные)
    // и в подписку ModuleHandler (поздняя подгрузка, под loader-lock безопасно).
    static void PrewarmModule(void* mod) noexcept
    {
        if (!mod) return;
        auto lock = g_overallLock.lock_exclusive();
        auto [it, inserted] = g_overallMap.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(mod),
            std::forward_as_tuple(g_editHookDep, mod));
        if (inserted)
            it->second.CacheHookData();
    }

    // Только свои модули — чужие не кешируем (карта не раздувается).
    static void PrewarmForModule(HMODULE mod, const wchar_t* base) noexcept
    {
        if (!mod || !base) return;
        if (mod == GlassCore::ModuleHandler::g_entryPointModule)
        {
            PrewarmModule(mod);
            return;
        }
        if (_wcsicmp(base, L"user32.dll") != 0 && _wcsicmp(base, L"gdi32full.dll") != 0
            && _wcsicmp(base, L"gdi32.dll") != 0 && _wcsicmp(base, L"riched20.dll") != 0
            && _wcsicmp(base, L"msftedit.dll") != 0)
            return;
        PrewarmModule(mod);
    }

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintEdit, OnEndPaintEdit });
        GlassCore::PaintDispatcher::RegisterPostPaintProvider({ OnEditAfterBeginPaint });
        GlassCore::PaintDispatcher::RegisterGetDCProvider({ OnGetDCEdit, OnReleaseDCEdit });
        PrewarmModule(GetModuleHandleW(L"user32.dll"));
        PrewarmModule(GetModuleHandleW(L"gdi32full.dll"));
        PrewarmModule(GetModuleHandleW(L"gdi32.dll"));
        PrewarmModule(GetModuleHandleW(L"riched20.dll"));
        PrewarmModule(GetModuleHandleW(L"msftedit.dll"));
        GlassCore::ModuleHandler::RegisterModulePrewarmHandler(PrewarmForModule);
        DebugLog(DBG_IMPORTANT, L"[GlassCore][Edit] Startup: PaintProvider + GetDCProvider зарегистрированы");
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        FlushEditBuffer();
        g_bypassEditBuffer = false;
        DisableAllOverallHooks();
        g_editSessionStack.clear();
        g_editDcStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
        DebugLog(DBG_IMPORTANT, L"[GlassCore][Edit] Shutdown complete");
    }

    void Update() {}
}