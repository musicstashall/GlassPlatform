// DrawTextW Detour for AMCCustomTab (BWM PaintHandler: MFC42u.dll + AMCCustomTab).
// Глобальный Detour user32!DrawTextW с ref-count; PaintDispatcher провайдер
// определяет сессию AMCCustomTab по GlassControlKind::AMCCustomTab.
// Проверки caller-модуля нет (паритет BWM FormsHooks::MyDrawTextW): текст вкладок
// рисует хост (mmc.exe / WinForms-часть AMCCustomTab), скоупинг — по paint-сессии.

#include "CorePch.h"

#include <Hooks/forms/FormsHooks.h>
#include <Hooks/forms/FormsHooksInternal.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/PaintDispatcher.h>
#include <Helpers/ThemeHelper.h>
#include <Core/Intercept/HookHelper.h>
#include <GlassControlKind.h>
#include <DebugLog.h>
#include <wil.h>

using namespace GlassCore::Hooks;

namespace GlassCore::FormsHooks
{
    // =========================================================================
    //  AMCCustomTab paint session — thread_local стек
    // =========================================================================

    namespace
    {
        struct CustomTabPaintSession
        {
            HMODULE mod;
            HWND hWnd;
        };
        thread_local std::vector<CustomTabPaintSession> g_customTabStack;

        std::atomic<bool>        g_drawTextShutdown{ false };
        wil::srwlock             g_drawTextLock;
        size_t                   g_drawTextRef{};
        decltype(&::DrawTextW)   g_trueDrawTextW = ::DrawTextW;
        thread_local uint32_t    g_customTabPaintSeq{};

        // Защита от рекурсии: коллбэк uxtheme #126 внутри DrawTextWithAlpha снова
        // вызывает user32!DrawTextW, а Detours-хук пропатчен in-place — повторный вход
        // через InvokeUser32DrawTextW вернулся бы в этот же детур (цикл в дампе
        // mmc.exe.84668.dmp). Повторный вход идёт в трамплин оригинала.
        thread_local bool        s_inCustomTabDrawTextAlpha = false;
    }

    // =========================================================================
    //  PaintDispatcher provider callbacks
    // =========================================================================

    static void OnCustomTabBeginPaint(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::AMCCustomTab)
            return;
        g_customTabStack.push_back({ callerMod, hWnd });
        DrawTextAttach(true);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][FormsHooks][CustomTab] BeginPaint: callerMod=0x%p hWnd=0x%p depth=%zu",
            callerMod, hWnd, g_customTabStack.size());
    }

    static void OnCustomTabEndPaint(HWND hWnd, HMODULE /*callerMod*/)
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::AMCCustomTab)
            return;
        if (g_customTabStack.empty())
            return;
        g_customTabStack.pop_back();
        DrawTextAttach(false);
        DebugLog(DBG_NORMAL,
            L"[GlassCore][FormsHooks][CustomTab] EndPaint: hWnd=0x%p depth=%zu",
            hWnd, g_customTabStack.size());
    }

    // =========================================================================
    //  Detour: DrawTextW
    // =========================================================================

    int WINAPI CustomTab_MyDrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        if (g_drawTextShutdown.load(std::memory_order_acquire))
            return g_trueDrawTextW(hdc, lpchText, cchText, lprc, format);

        const HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());

        // Повторный вход из коллбэка uxtheme #126 (внутри нашего же DrawTextWithAlpha):
        // только трамплин оригинала, иначе бесконечная рекурсия и переполнение стека.
        if (s_inCustomTabDrawTextAlpha)
        {
            return g_trueDrawTextW(hdc, lpchText, cchText, lprc, format);
        }

        // Паритет BWM FormsHooks::MyDrawTextW: проверки caller-модуля нет —
        // текст вкладок рисует не MFC42u.dll, а хост (mmc.exe / WinForms-часть AMCCustomTab).
        // Скоупинг — только по paint-сессии CustomTab (thread_local стек).
        if (g_customTabStack.empty())
        {
            return g_trueDrawTextW(hdc, lpchText, cchText, lprc, format);
        }

        if (format & DT_CALCRECT)
        {
            return g_trueDrawTextW(hdc, lpchText, cchText, lprc, format);
        }

        if (!lpchText || cchText == 0)
            return g_trueDrawTextW(hdc, lpchText, cchText, lprc, format);

        const uint32_t seq = ++g_customTabPaintSeq;
        WCHAR textPreview[12]{};
        const int previewLen = std::min(cchText, 11);
        if (previewLen > 0)
            wcsncpy_s(textPreview, lpchText, static_cast<size_t>(previewLen));

        DebugLog(DBG_NORMAL,
            L"[GlassCore][FormsHooks][CustomTab] DrawTextW: seq=%u caller=0x%p hdc=%p textLen=%d rect=[%d,%d,%d,%d] format=0x%X text=\"%s\"",
            seq, callerMod, hdc, cchText,
            lprc ? lprc->left : 0, lprc ? lprc->top : 0,
            lprc ? lprc->right : 0, lprc ? lprc->bottom : 0,
            format, textPreview);

        // Паритет BWM: BkColor/BkMode не трогаем — DrawTextWithAlpha сам управляет
        // фоном через buffered paint (в memDC выставляется TRANSPARENT).

        int result = 0;
        s_inCustomTabDrawTextAlpha = true;
        auto clearAlphaFlag = wil::scope_exit([]() noexcept { s_inCustomTabDrawTextAlpha = false; });
        const HRESULT alphaHr = GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result);
        if (!FAILED(alphaHr))
        {
            return result;
        }

        // Fallback — паритет BWM: обычный DrawTextW без смены атрибутов DC.
        return g_trueDrawTextW(hdc, lpchText, cchText, lprc, format);
    }

    // =========================================================================
    //  DrawTextAttach — ref-counted Detour on user32!DrawTextW
    // =========================================================================

    void DrawTextAttach(bool enable) noexcept
    {
        auto lock = g_drawTextLock.lock_exclusive();

        if (enable)
        {
            if (g_drawTextRef == 0)
            {
                const HRESULT hr = Detours::Write([&]() {
                    Detours::Attach(
                        reinterpret_cast<void**>(&g_trueDrawTextW),
                        reinterpret_cast<void*>(&CustomTab_MyDrawTextW));
                });
                if (FAILED(hr))
                {
                    DebugLog(DBG_ERROR,
                        L"[GlassCore][FormsHooks] DrawTextAttach Attach hr=0x%08X",
                        static_cast<unsigned>(hr));
                    return;
                }
                DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] DrawTextAttach: attached");
            }
            ++g_drawTextRef;
            return;
        }

        if (g_drawTextRef == 0)
            return;

        --g_drawTextRef;
        if (g_drawTextRef == 0)
        {
            const HRESULT hr = Detours::Write([&]() {
                Detours::Detach(
                    reinterpret_cast<void**>(&g_trueDrawTextW),
                    reinterpret_cast<void*>(&CustomTab_MyDrawTextW));
            });
            if (FAILED(hr))
                DebugLog(DBG_ERROR,
                    L"[GlassCore][FormsHooks] DrawTextAttach Detach hr=0x%08X",
                    static_cast<unsigned>(hr));
            else
                DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] DrawTextAttach: detached");
        }
    }

    // =========================================================================
    //  ForceDetachDrawText — вызывается при Shutdown
    // =========================================================================

    void ForceDetachDrawText() noexcept
    {
        g_drawTextShutdown.store(true, std::memory_order_release);
        auto lock = g_drawTextLock.lock_exclusive();
        if (g_drawTextRef == 0)
            return;
        const HRESULT hr = Detours::Write([&]() {
            Detours::Detach(
                reinterpret_cast<void**>(&g_trueDrawTextW),
                reinterpret_cast<void*>(&CustomTab_MyDrawTextW));
        });
        if (FAILED(hr))
            DebugLog(DBG_ERROR,
                L"[GlassCore][FormsHooks] ForceDetachDrawText hr=0x%08X",
                static_cast<unsigned>(hr));
        g_drawTextRef = 0;
    }

    // =========================================================================
    //  RegisterCustomTabPaintProvider — вызывается из FormsHooks::Startup
    // =========================================================================

    void RegisterCustomTabPaintProvider() noexcept
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnCustomTabBeginPaint, OnCustomTabEndPaint });
        DebugLog(DBG_IMPORTANT, L"[GlassCore][FormsHooks] CustomTab PaintDispatcher provider registered");
    }
}
