#include "WndPch.h"
#include "CustomTabSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <WinApiUtils.h>
#include <uxtheme.h>

#include <vector>
#include <algorithm>

#pragma comment(lib, "uxtheme.lib")

namespace
{
    std::vector<HWND> g_customTabWindows;

    // Оригинальный brush фона класса AMCCustomTab, сохраняется при первом Attach.
    static LONG_PTR g_origCustomTabBrush = 0;
    static bool     g_customTabBrushSaved = false;
}

namespace WndCompat
{
    void CustomTab_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::AMCCustomTab)
            return;

        GlassMarkWindow(hWnd, GlassControlKind::AMCCustomTab);
        WndCompat::RegisterThemedChromeClientWithGlassCore(hWnd);
        g_customTabWindows.push_back(hWnd);

        // AMCCustomTab — всегда unthemed (кастомная отрисовка C#).
        // Снимаем визуальные стили, устанавливаем чёрный brush фона.
        SetWindowTheme(hWnd, L"", L"");

        const LONG_PTR blackBrush = reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH));
        if (!g_customTabBrushSaved)
        {
            g_origCustomTabBrush = SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, blackBrush);
            g_customTabBrushSaved = true;
        }
        else
        {
            SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, blackBrush);
        }

        DebugLog(L"[WndCompat][CustomTab] CustomTab_Attach: hwnd=0x%p", hWnd);
    }

    void CustomTab_Detach(HWND hWnd)
    {
        WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);
        GlassShared::GlassUnmarkWindow(hWnd);

        auto it = std::find(g_customTabWindows.begin(), g_customTabWindows.end(), hWnd);
        if (it != g_customTabWindows.end())
            g_customTabWindows.erase(it);

        DebugLog(L"[WndCompat][CustomTab] CustomTab_Detach: hwnd=0x%p", hWnd);
    }

    void CustomTab_DetachAll()
    {
        auto windows = g_customTabWindows;
        g_customTabWindows.clear();

        // Восстановить оригинальный brush класса (один раз, для любого живого окна).
        if (g_customTabBrushSaved)
        {
            g_customTabBrushSaved = false;
            for (HWND hWnd : windows)
            {
                if (IsWindow(hWnd))
                {
                    SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, g_origCustomTabBrush);
                    DebugLog(L"[WndCompat][CustomTab] CustomTab_DetachAll: оригинальный brush восстановлен hwnd=0x%p", hWnd);
                    break;
                }
            }
            g_origCustomTabBrush = 0;
        }

        // Восстановить визуальный стиль и снять маркировку
        for (HWND hWnd : windows)
        {
            if (IsWindow(hWnd))
            {
                WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);
                SetWindowTheme(hWnd, L"Tab", nullptr);
                GlassShared::GlassUnmarkWindow(hWnd);
                InvalidateRect(hWnd, nullptr, TRUE);
                DebugLog(L"[WndCompat][CustomTab] CustomTab_DetachAll: стиль восстановлен, unmarked 0x%p", hWnd);
            }
        }
    }
}
