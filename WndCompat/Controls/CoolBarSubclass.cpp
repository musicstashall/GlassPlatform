// Сабкласс окна CoolBar — окно Delphi TCoolBar.
//
// У контрола нет темы: полосу заливает пустой ExtTextOutW(ETO_OPAQUE),
// текст рисует comctl32 обычным DrawTextExW, рамку — VCL через DrawEdge.
// Поэтому у CoolBar отдельный набор хуков GlassCore (CoolBarHooks) и своя
// сессия отрисовки; rebar-ветка здесь не используется.

#include "WndPch.h"
#include <Controls/CoolBarSubclass.h>
#include <GlassMessages.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <CoolBarEventProvider.h>
#include <WndCompat.h>

namespace
{
    constexpr wchar_t kCoolBarClass[] = L"CoolBar";

    std::vector<HWND> g_coolBarWindows;

    [[nodiscard]] bool IsCoolBarClass(HWND hWnd) noexcept
    {
        wchar_t cls[64]{};
        return GetClassNameW(hWnd, cls, static_cast<int>(_countof(cls))) > 0
            && _wcsicmp(cls, kCoolBarClass) == 0;
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
        {
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            auto* p = WndCompat::CoolBar::GetCoolBarEventProvider();
            if (p && p->OnPaintBegin)
                p->OnPaintBegin(hWnd);

            const LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (p && p->OnPaintEnd)
                p->OnPaintEnd();
            return result;
        }

        case WM_ERASEBKGND:
        case WM_PRINTCLIENT:
        {
            if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);

            // Фон → прозрачность; содержимое полосы рисуется в WM_PAINT.
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
        DebugLog(DBG_NORMAL, L"[WndCompat][CoolBar] CoolBar_Attach: субкласс установлен hwnd=0x%p", hWnd);
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
