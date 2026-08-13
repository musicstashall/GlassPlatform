#include "WndPch.h"
#include "TabControlSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <WinApiUtils.h>
#include <uxtheme.h>

#include <vector>
#include <algorithm>

#pragma comment(lib, "uxtheme.lib")

namespace
{
    std::vector<HWND> g_tabControlWindows;

    // Совпадает с чтением в GlassCore/TabHooks (GetProp при BeginPaint).
    const wchar_t kGlassTabUseThemeProp[] = L"Glass.TabUseTheme";

    // Оригинальный brush фона класса SysTabControl32, сохраняется при первом Attach.
    // SetClassLongPtr(GCLP_HBRBACKGROUND) — операция на уровне класса окна, поэтому
    // сохранять нужно один раз, восстанавливать тоже один раз в DetachAll.
    static LONG_PTR g_origTabBrush = 0;
    static bool     g_brushSaved   = false;
}

namespace WndCompat
{
    void TabControl_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) == GlassControlKind::TabControl)
            return;

        GlassMarkWindow(hWnd, GlassControlKind::TabControl);
        WndCompat::RegisterThemedChromeClientWithGlassCore(hWnd);
        g_tabControlWindows.push_back(hWnd);

        // Тема TAB доступна — оставляем визуальные стили (GlassCore рисует фон через DrawThemeBackground+alpha).
        // Если темы нет, не вызываем SetWindowTheme("", "") — стиль уже «выключен» для таба.
        const bool useTheme = Shared::WinApiUtils::IsThemed(hWnd, L"TAB");
        SetPropW(hWnd, kGlassTabUseThemeProp, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(useTheme ? 1u : 0u)));

        if (!useTheme)
        {
            // SetClassLongPtr меняет brush для всего класса SysTabControl32.
            // Сохраняем оригинал только при первом вызове.
            const LONG_PTR blackBrush = reinterpret_cast<LONG_PTR>(GetStockObject(BLACK_BRUSH));
            if (!g_brushSaved)
            {
                g_origTabBrush = SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, blackBrush);
                g_brushSaved = true;
            }
            else
            {
                SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, blackBrush);
            }
        }
        
        DebugLog(L"[WndCompat][TabControl] TabControl_Attach: hwnd=0x%p", hWnd);
    }

    void TabControl_Detach(HWND hWnd)
    {
        WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);
        RemovePropW(hWnd, kGlassTabUseThemeProp);
        GlassShared::GlassUnmarkWindow(hWnd);

        auto it = std::find(g_tabControlWindows.begin(), g_tabControlWindows.end(), hWnd);
        if (it != g_tabControlWindows.end())
            g_tabControlWindows.erase(it);

        DebugLog(L"[WndCompat][TabControl] TabControl_Detach: hwnd=0x%p", hWnd);
    }

    void TabControl_DetachAll()
    {
        auto windows = g_tabControlWindows;
        g_tabControlWindows.clear();

        // Восстановить оригинальный brush класса (один раз, для любого живого окна).
        if (g_brushSaved)
        {
            g_brushSaved = false;
            for (HWND hWnd : windows)
            {
                if (IsWindow(hWnd))
                {
                    SetClassLongPtr(hWnd, GCLP_HBRBACKGROUND, g_origTabBrush);
                    DebugLog(L"[WndCompat][TabControl] TabControl_DetachAll: оригинальный brush восстановлен hwnd=0x%p", hWnd);
                    break;
                }
            }
            g_origTabBrush = 0;
        }

        // Восстановить визуальный стиль и снять маркировку
        for (HWND hWnd : windows)
        {
            if (IsWindow(hWnd))
            {
                WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);
                RemovePropW(hWnd, kGlassTabUseThemeProp);
                SetWindowTheme(hWnd, L"Tab", nullptr);
                GlassShared::GlassUnmarkWindow(hWnd);
                InvalidateRect(hWnd, nullptr, TRUE);
                DebugLog(L"[WndCompat][TabControl] TabControl_DetachAll: стиль восстановлен, unmarked 0x%p", hWnd);
            }
        }
    }
}
