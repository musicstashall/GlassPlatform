// DrawTextExW Detour for System.Windows.Forms.ni.dll (BWM DrawTextExAttach).

#include "CorePch.h"

#include <Hooks/forms/FormsHooks.h>
#include <Hooks/forms/FormsHooksInternal.h>
#include <Hooks/Button.h>
#include <Hooks/ListBox.h>
#include <Hooks/Static.h>
#include <Subsystems/RenderText.h>
#include <Core/Intercept/HookHelper.h>
#include <DebugLog.h>
#include <wil.h>

using namespace GlassCore::Hooks;

namespace GlassCore::FormsHooks
{
    namespace
    {
        std::atomic<bool>        g_drawTextExShutdown{ false };
        wil::srwlock             g_drawTextExLock;
        size_t                   g_drawTextExRef{};
        decltype(&::DrawTextExW) g_trueDrawTextExW = ::DrawTextExW;
        thread_local uint32_t    g_formsNiPaintSeq{};
    }

    int WINAPI FormsNi_MyDrawTextExW(
        HDC hdc, LPWSTR lpchText, int cchText,
        LPRECT lprc, UINT format, LPDRAWTEXTPARAMS lpdtp)
    {
        if (g_drawTextExShutdown.load(std::memory_order_acquire))
            return g_trueDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        const HMODULE callerMod = DetourGetContainingModule(_ReturnAddress());

        if (!Internal::IsSystemWindowsFormsNiModule(callerMod))
            return g_trueDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (!GlassCore::Button::IsInsideButtonPaint()
            && !GlassCore::StaticControl::IsInsideStaticPaint()
            && !GlassCore::ListBoxControl::IsInsideWinFormsListBoxPaint())
            return g_trueDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (format & DT_CALCRECT)
            return g_trueDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        if (!lprc || IsRectEmpty(lprc) || !lpchText || cchText == 0)
            return g_trueDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);

        const uint32_t seq = ++g_formsNiPaintSeq;
        WCHAR textPreview[12]{};
        const int previewLen = std::min(cchText, 11);
        if (previewLen > 0)
            wcsncpy_s(textPreview, lpchText, static_cast<size_t>(previewLen));

        DebugLog(DBG_NORMAL,
            L"[GlassCore][FormsHooks] DrawTextExW: seq=%u hdc=%p textLen=%d rect=[%d,%d,%d,%d] bkMode=%d format=0x%X text=\"%s\"",
            seq, hdc, cchText, lprc->left, lprc->top, lprc->right, lprc->bottom,
            GetBkMode(hdc), format, textPreview);

        int result = 0;
        if (!FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return result;

        return g_trueDrawTextExW(hdc, lpchText, cchText, lprc, format, lpdtp);
    }

    void DrawTextExAttach(bool enable) noexcept
    {
        auto lock = g_drawTextExLock.lock_exclusive();

        if (enable)
        {
            if (!GetModuleHandleW(L"System.Windows.Forms.ni.dll"))
                return;

            if (g_drawTextExRef == 0)
            {
                const HRESULT hr = Detours::Write([&]() {
                    Detours::Attach(
                        reinterpret_cast<void**>(&g_trueDrawTextExW),
                        reinterpret_cast<void*>(&FormsNi_MyDrawTextExW));
                });
                if (FAILED(hr))
                {
                    DebugLog(DBG_ERROR,
                        L"[GlassCore][FormsHooks] DrawTextExAttach Attach hr=0x%08X",
                        static_cast<unsigned>(hr));
                    return;
                }
            }
            ++g_drawTextExRef;
            return;
        }

        if (g_drawTextExRef == 0)
            return;

        --g_drawTextExRef;
        if (g_drawTextExRef == 0)
        {
            const HRESULT hr = Detours::Write([&]() {
                Detours::Detach(
                    reinterpret_cast<void**>(&g_trueDrawTextExW),
                    reinterpret_cast<void*>(&FormsNi_MyDrawTextExW));
            });
            if (FAILED(hr))
                DebugLog(DBG_ERROR,
                    L"[GlassCore][FormsHooks] DrawTextExAttach Detach hr=0x%08X",
                    static_cast<unsigned>(hr));
        }
    }

    void ForceDetachDrawTextEx() noexcept
    {
        g_drawTextExShutdown.store(true, std::memory_order_release);
        auto lock = g_drawTextExLock.lock_exclusive();
        if (g_drawTextExRef == 0)
            return;
        const HRESULT hr = Detours::Write([&]() {
            Detours::Detach(
                reinterpret_cast<void**>(&g_trueDrawTextExW),
                reinterpret_cast<void*>(&FormsNi_MyDrawTextExW));
        });
        if (FAILED(hr))
            DebugLog(DBG_ERROR,
                L"[GlassCore][FormsHooks] ForceDetachDrawTextEx hr=0x%08X",
                static_cast<unsigned>(hr));
        g_drawTextExRef = 0;
    }
}
