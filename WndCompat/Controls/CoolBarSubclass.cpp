// Сабкласс Delphi TCoolBar (точные классы окна TCoolBar / CoolBar).
// WM_PAINT / WM_PRINTCLIENT сохраняют native paint внутри отдельной
// GlassCore-сессии delphi/TCoolBarDraw; generic rebar не используется.
// WM_ERASEBKGND чистит фон; рамку VCL продолжает рисовать через DrawEdge.

#include "WndPch.h"
#include <Controls/CoolBarSubclass.h>
#include <Tracker/WinEventHandler.h>
#include <InjectionMode.h>
#include <GlassMessages.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <CoolBarEventProvider.h>
#include <WndCompat.h>
#include <wil.h>

namespace
{
    std::vector<HWND> g_coolBarWindows;

    [[nodiscard]] bool IsCoolBarClass(HWND hWnd) noexcept
    {
        wchar_t cls[64]{};
        return GetClassNameW(hWnd, cls, static_cast<int>(_countof(cls))) > 0
            && WndCompat::CoolBar_IsClassName(cls);
    }

    LRESULT CALLBACK CoolBarSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*id*/, DWORD_PTR /*data*/)
    {
        if (uMsg == WndCompat::WM_GLASS_DETACH_COOLBAR)
        {
            WndCompat::CoolBar_Detach(hWnd);
            WndCompat::WinEventHook_SignalDetachComplete();
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_PAINT:
        case WM_PRINTCLIENT:
        {
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            const auto* p = WndCompat::CoolBar::GetCoolBarEventProvider();
            // Снимок End и результат Begin: отказ во вложенном paint не
            // должен закрывать сессию родителя; сброс провайдера не теряет пару.
            const auto end = p ? p->OnPaintEnd : nullptr;
            const bool began = p && p->OnPaintBegin && end
                && p->OnPaintBegin(hWnd, reinterpret_cast<HDC>(wParam));
            auto finish = wil::scope_exit([&]() noexcept { if (began) end(); });

            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
            if (budget)
            {
                --budget;
                DebugLog(DBG_NORMAL,
                    L"[WndCompat][CoolBar] paint hwnd=0x%p msg=0x%04X dc=0x%p provider=%d began=%d",
                    hWnd, uMsg, reinterpret_cast<HDC>(wParam), p ? 1 : 0, began ? 1 : 0);
            }
            // В том числе WM_PRINTCLIENT: нельзя подменять его одним BLACKNESS,
            // иначе double-buffered VCL получает снимок без текста и полос.
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case WM_ERASEBKGND:
        {
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            // Только стирание фона; WM_PRINTCLIENT выше рисует и содержимое.
            HDC hdc = reinterpret_cast<HDC>(wParam);
            if (!hdc)
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            if (!IsRectEmpty(&rc))
            {
                PatBlt(hdc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, BLACKNESS);
                return TRUE;
            }
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);
        }

        case WM_NCDESTROY:
            WndCompat::CoolBar_Detach(hWnd);
            break;

        default:
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    bool CoolBar_IsClassName(const wchar_t* className) noexcept
    {
        // Не тема Rebar, не текст подписи и не подстрока имени окна:
        // GetClassNameW в зонде возвращает именно TCoolBar.
        return className && (_wcsicmp(className, L"TCoolBar") == 0
            || _wcsicmp(className, L"CoolBar") == 0);
    }

    void CoolBar_Attach(HWND hWnd)
    {
        if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;
        if (!IsWindow(hWnd))
            return;
        if (std::find(g_coolBarWindows.begin(), g_coolBarWindows.end(), hWnd) != g_coolBarWindows.end())
            return;
        if (!IsCoolBarClass(hWnd))
        {
            DebugLog(DBG_NORMAL, L"[WndCompat][CoolBar] CoolBar_Attach: skip (class) hwnd=0x%p", hWnd);
            return;
        }
        if (!SetWindowSubclass(hWnd, CoolBarSubclassProc, 1, 0))
        {
            DebugLog(DBG_WARNING, L"[WndCompat][CoolBar] CoolBar_Attach: SetWindowSubclass FAILED hwnd=0x%p", hWnd);
            return;
        }

        GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::CoolBar);
        g_coolBarWindows.push_back(hWnd);
        wchar_t cls[64]{};
        GetClassNameW(hWnd, cls, static_cast<int>(_countof(cls)));
        DebugLog(DBG_NORMAL, L"[WndCompat][CoolBar] CoolBar_Attach: субкласс установлен hwnd=0x%p class=\"%s\"", hWnd, cls);
    }

    void CoolBar_Detach(HWND hWnd)
    {
        GlassShared::GlassUnmarkWindow(hWnd);
        auto it = std::find(g_coolBarWindows.begin(), g_coolBarWindows.end(), hWnd);
        if (it != g_coolBarWindows.end())
            g_coolBarWindows.erase(it);
        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, CoolBarSubclassProc, 1);
        DebugLog(DBG_NORMAL, L"[WndCompat][CoolBar] CoolBar_Detach: hwnd=0x%p", hWnd);
    }

    void CoolBar_DetachAll()
    {
        const auto windows = g_coolBarWindows;
        g_coolBarWindows.clear();
        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;
            GlassShared::GlassUnmarkWindow(hWnd);
            if (!RemoveWindowSubclass(hWnd, CoolBarSubclassProc, 1)
                && !WndCompat::IsShuttingDown())
            {
                DWORD_PTR unused = 0;
                SendMessageTimeoutW(hWnd, WndCompat::WM_GLASS_DETACH_COOLBAR, 0, 0,
                    SMTO_ABORTIFHUNG, 500, &unused);
            }
            DebugLog(DBG_NORMAL, L"[WndCompat][CoolBar] CoolBar_DetachAll: detached 0x%p", hWnd);
        }
    }

    bool CoolBar_IsTracked(HWND hWnd) noexcept
    {
        return std::find(g_coolBarWindows.begin(), g_coolBarWindows.end(), hWnd) != g_coolBarWindows.end();
    }

    namespace
    {
        BOOL CALLBACK CoolBarCatchUpProc(HWND hWnd, LPARAM lParam) noexcept
        {
            if (!IsCoolBarClass(hWnd))
                return TRUE;
            auto* count = reinterpret_cast<unsigned*>(lParam);
            if (count)
                ++*count;
            WndCompat::CoolBar_Attach(hWnd);
            return TRUE;
        }
    }

    void CoolBar_CatchUp(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot) || WndCompat::IsShuttingDown())
            return;
        if (WinEventHook_GetMode() != Shared::InjectionMode::Full)
            return;
        unsigned seen = 0;
        EnumChildWindows(hRoot, CoolBarCatchUpProc, reinterpret_cast<LPARAM>(&seen));
        if (seen)
            DebugLog(DBG_NORMAL, L"[WndCompat][CoolBar] CoolBar_CatchUp: root=0x%p windows=%u", hRoot, seen);
    }
}
