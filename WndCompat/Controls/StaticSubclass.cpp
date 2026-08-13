#include "WndPch.h"
#include "StaticSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <GlassMessages.h>
#include <BitmapUtils.h>
#include <ColorUtils.h>
#include <RegHelper.h>

#include <commctrl.h>
#include <vector>
#include <mutex>
#include <algorithm>

namespace
{
    std::vector<HWND> g_staticWindows;
    std::mutex        g_staticMutex;

    // ── Static c изображением (SS_BITMAP/SS_ICON): субкласс + STM_SETIMAGE ────
    std::vector<HWND> g_staticImageWindows;
    std::mutex        g_staticImageMutex;

    // Защита от рекурсии, когда мы сами переустанавливаем сконвертированный битмап.
    thread_local HWND g_staticImageReentryGuard = nullptr;

    // Конвертирует битмап Static в 32bpp для корректного смешивания со стеклом:
    //   • без ключа Root\Control\Static — просто Promise32BPP (alpha=255), как BWM GdipCreateBitmapAlpha;
    //   • с ключом ColorTreatAsTransparent — дополнительно chroma-key (SpriteEffect).
    // Возвращает новый HBITMAP или nullptr, если конвертация не нужна/не удалась.
    // Прим.: возвращённый битмап живёт вместе с контролом (как в BWM — не отслеживаем/не удаляем;
    // прежний хендл контрол вернёт вызывающему из STM_SETIMAGE).
    [[nodiscard]] HBITMAP ConvertStaticBitmapIfNeeded(HBITMAP src) noexcept
    {
        if (!src || Shared::BitmapUtils::IsOemBitmap(src))
            return nullptr;

        const bool hasAlpha = Shared::BitmapUtils::HasAlphaChannel(src);

        // presence ColorTreatAsTransparent = включённый chroma (как Root\Control\{Kind}).
        const auto colorOpt = Shared::RegHelper::TryGet<DWORD>(
            { L"Root", L"Control", L"Static" }, L"ColorTreatAsTransparent");
        const bool chroma = colorOpt.has_value();

        // Уже есть альфа и ключ не задан — конвертировать нечего.
        if (hasAlpha && !chroma)
            return nullptr;

        HBITMAP conv = Shared::BitmapUtils::Promise32BPP(src);
        if (!conv)
            return nullptr;

        if (chroma)
        {
            const DWORD threshold = Shared::RegHelper::Get<DWORD>(
                { L"Root", L"Control", L"Static" }, L"ColorTreatAsTransparentThreshold", 50);
            Shared::BitmapUtils::SpriteEffect eff(
                Shared::ColorUtils::ARGBToCOLORREF(colorOpt.value()),
                static_cast<BYTE>(threshold));
            eff.Apply(conv);
        }
        return conv;
    }

    LRESULT CALLBACK StaticImageSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*id*/, DWORD_PTR /*data*/)
    {
        // Снятие субкласса из чужого потока (StaticImage_DetachAll → SendMessageTimeout).
        if (uMsg == WndCompat::WM_GLASS_DETACH_STATIC)
        {
            WndCompat::StaticImage_Detach(hWnd);
            DebugLog(L"[WndCompat][Static] WM_GLASS_DETACH_STATIC: снят hwnd=0x%p", hWnd);
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case STM_SETIMAGE:
            if (wParam == IMAGE_BITMAP && g_staticImageReentryGuard != hWnd)
            {
                HBITMAP conv = ConvertStaticBitmapIfNeeded(reinterpret_cast<HBITMAP>(lParam));
                if (conv)
                    return DefSubclassProc(hWnd, uMsg, wParam, reinterpret_cast<LPARAM>(conv));
            }
            break;

        case WM_NCDESTROY:
            WndCompat::StaticImage_Detach(hWnd);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void Static_Attach(HWND hWnd, GlassShared::GlassControlKind kind)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (kind != GlassControlKind::Static && kind != GlassControlKind::WinForms_Static)
        {
            DebugLog(DBG_WARNING, L"[WndCompat][Static] Static_Attach: неверный kind=%u hwnd=0x%p",
                static_cast<unsigned>(kind), hWnd);
            return;
        }

        if (GlassGetWindowKind(hWnd) == GlassControlKind::Static
         || GlassGetWindowKind(hWnd) == GlassControlKind::WinForms_Static)
            return;

        GlassMarkWindow(hWnd, kind);
        WndCompat::TryRegisterTextGlowClientWithGlassCore(hWnd);

        {
            std::lock_guard lock(g_staticMutex);
            g_staticWindows.push_back(hWnd);
        }

        const wchar_t* const kindName =
            (kind == GlassControlKind::WinForms_Static) ? L"WinForms_Static" : L"Static";
        DebugLog(L"[WndCompat][Static] Static_Attach: kind=%s hwnd=0x%p", kindName, hWnd);
    }

    void Static_Detach(HWND hWnd)
    {
        StaticImage_Detach(hWnd);

        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_staticMutex);
            auto it = std::find(g_staticWindows.begin(), g_staticWindows.end(), hWnd);
            if (it != g_staticWindows.end())
                g_staticWindows.erase(it);
        }

        DebugLog(L"[WndCompat][Static] Static_Detach: hwnd=0x%p", hWnd);
    }

    void Static_DetachAll()
    {
        StaticImage_DetachAll();

        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_staticMutex);
            windows.swap(g_staticWindows);
        }

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);
            GlassShared::GlassUnmarkWindow(hWnd);
            DebugLog(L"[WndCompat][Static] Static_DetachAll: unmarked 0x%p", hWnd);
        }
    }

    bool StaticHasImageStyle(HWND hWnd) noexcept
    {
        if (!IsWindow(hWnd))
            return false;
        const LONG type = GetWindowLongW(hWnd, GWL_STYLE) & SS_TYPEMASK;
        return type == SS_BITMAP || type == SS_ICON;
    }

    void StaticImage_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd) || !StaticHasImageStyle(hWnd))
            return;

        {
            std::lock_guard lock(g_staticImageMutex);
            if (std::find(g_staticImageWindows.begin(), g_staticImageWindows.end(), hWnd)
                != g_staticImageWindows.end())
                return;
        }

        if (!SetWindowSubclass(hWnd, StaticImageSubclassProc, 1, 0))
        {
            DebugLog(DBG_WARNING, L"[WndCompat][Static] StaticImage_Attach: SetWindowSubclass FAILED hwnd=0x%p", hWnd);
            return;
        }

        {
            std::lock_guard lock(g_staticImageMutex);
            g_staticImageWindows.push_back(hWnd);
        }

        // Изображение, установленное до субкласса (из ресурса диалога) — конвертируем сразу.
        HBITMAP cur = reinterpret_cast<HBITMAP>(SendMessageW(hWnd, STM_GETIMAGE, IMAGE_BITMAP, 0));
        if (cur)
        {
            HBITMAP conv = ConvertStaticBitmapIfNeeded(cur);
            if (conv)
            {
                g_staticImageReentryGuard = hWnd;
                SendMessageW(hWnd, STM_SETIMAGE, IMAGE_BITMAP, reinterpret_cast<LPARAM>(conv));
                g_staticImageReentryGuard = nullptr;
            }
        }

        DebugLog(L"[WndCompat][Static] StaticImage_Attach: субкласс установлен hwnd=0x%p", hWnd);
    }

    void StaticImage_Detach(HWND hWnd)
    {
        {
            std::lock_guard lock(g_staticImageMutex);
            auto it = std::find(g_staticImageWindows.begin(), g_staticImageWindows.end(), hWnd);
            if (it != g_staticImageWindows.end())
                g_staticImageWindows.erase(it);
        }

        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, StaticImageSubclassProc, 1);

        DebugLog(L"[WndCompat][Static] StaticImage_Detach: hwnd=0x%p", hWnd);
    }

    void StaticImage_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_staticImageMutex);
            windows.swap(g_staticImageWindows);
        }

        const DWORD curTid = GetCurrentThreadId();
        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            if (GetWindowThreadProcessId(hWnd, nullptr) == curTid)
            {
                RemoveWindowSubclass(hWnd, StaticImageSubclassProc, 1);
                DebugLog(L"[WndCompat][Static] StaticImage_DetachAll: detached 0x%p", hWnd);
            }
            else
            {
                DWORD_PTR sendResult = 0;
                if (!SendMessageTimeoutW(hWnd, WM_GLASS_DETACH_STATIC, 0, 0, SMTO_ABORTIFHUNG, 1000, &sendResult))
                    DebugLog(DBG_WARNING, L"[WndCompat][Static] StaticImage_DetachAll: cross-thread timeout hwnd=0x%p", hWnd);
                else
                    DebugLog(L"[WndCompat][Static] StaticImage_DetachAll: cross-thread detached hwnd=0x%p", hWnd);
            }
        }
    }
}
