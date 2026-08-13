#include "WndPch.h"
#include "MonthCalSubclass.h"
#include <WndCompat.h>
#include <CommCtrl.h>
#include <DropDownEventProvider.h>
#include <GlassMessages.h>
#include <GlassControlKind.h>
#include <wil.h>
#include <Tracker/WindowClassUtils.h>
#include <Popup/DropDown/DropDownSubclass.h>
#include <algorithm>
#include <mutex>
#include <vector>

namespace
{
    constexpr UINT_PTR kMonthCalSubclassId = 1;
    constexpr DWORD kDetachTimeoutMs = 1000;
    constexpr int kDetachRetryCount = 2;

    std::vector<HWND> g_monthCalWindows;
    std::mutex g_monthCalMutex;

    // ── SubclassProc ─────────────────────────────────────────────────────────

    LRESULT CALLBACK MonthCalSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        if (uMsg == WndCompat::WM_GLASS_DETACH_MONTHCAL)
        {
            WndCompat::MonthCal_Detach(hWnd);
            DebugLog(L"[WndCompat][MonthCal] WM_GLASS_DETACH_MONTHCAL: снят hwnd=0x%p", hWnd);
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
            // ── NC-рамка ─────────────────────────────────────────────────────────
            // SysMonthCal32 может иметь WS_BORDER / WS_EX_STATICEDGE.
            // Без перехвата система рисует белую рамку поверх DWM-эффекта popup.
            // Закрашиваем NC-полосу (1 px) чёрным, клиентскую зону исключаем.
        case WM_NCPAINT:
        {
            HDC hdc = GetWindowDC(hWnd);
            if (!hdc)
                break;

            auto releaseDC = wil::scope_exit([&] { ReleaseDC(hWnd, hdc); });

            // Применяем регион-клип, если он передан (wParam == HRGN).
            if (wParam != NULLREGION && wParam != ERROR)
                SelectClipRgn(hdc, reinterpret_cast<HRGN>(wParam));

            RECT wr{};
            GetWindowRect(hWnd, &wr);
            OffsetRect(&wr, -wr.left, -wr.top);   // window-origin coords

            // Исключаем клиентскую зону, рисуем только border-полосу.
            {
                int saved = SaveDC(hdc);
                ExcludeClipRect(hdc,
                    wr.left + 1, wr.top + 1,
                    wr.right - 1, wr.bottom - 1);
                PatBlt(hdc,
                    wr.left, wr.top,
                    wr.right - wr.left,
                    wr.bottom - wr.top,
                    BLACKNESS);
                RestoreDC(hdc, saved);
            }

            return 0;   // не вызываем DefSubclassProc — блокируем системную рамку
        }

        // ── Фон ──────────────────────────────────────────────────────────────
        case WM_ERASEBKGND:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT clip{};
            GetClipBox(hdc, &clip);
            PatBlt(hdc,
                clip.left, clip.top,
                clip.right - clip.left,
                clip.bottom - clip.top,
                BLACKNESS);
            return TRUE;   // сообщаем, что фон обработан
        }

        // ── Обычная отрисовка (тема + GDI) ───────────────────────────────────
        // PaintDispatcher перехватывает BeginPaint и автоматически активирует
        // IAT-хуки через провайдер, проверяя SetProp-маркер на hWnd.
        case WM_PAINT:
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        // ── AnimateWindow snapshot (WM_PRINTCLIENT) ───────────────────────────
        // AnimateWindow передаёт готовый HDC через wParam — comctl32 рисует
        // напрямую, не вызывая BeginPaint. PaintDispatcher не перехватывает.
        // Включаем IAT-хуки явно до и после DefSubclassProc.
        case WM_PRINTCLIENT:
        {
            auto* p = WndCompat::DropDown::GetDropDownEventProvider();
            if (p && p->OnMonthCalPrintClientBegin)
                p->OnMonthCalPrintClientBegin(hWnd);

            LRESULT result = DefSubclassProc(hWnd, uMsg, wParam, lParam);

            if (p && p->OnMonthCalPrintClientEnd)
                p->OnMonthCalPrintClientEnd(hWnd);

            return result;
        }

        case WM_NCDESTROY:
            WndCompat::MonthCal_Detach(hWnd);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace WndCompat
{
    void MonthCal_Attach(HWND hWnd, HWND parent, bool parentIsDropDown)
    {
        if (!IsWindow(hWnd))
            return;

        // Для не-themed календарей (визуальные стили отключены) задаём
        // чёрный системный цвет фона через MCM_SETCOLOR. При включённых
        // темах это не оказывает видимого эффекта — цвет определяет тема,
        // а не MCSC_BACKGROUND.
        MonthCal_SetColor(hWnd, MCSC_BACKGROUND, RGB(0, 0, 0));
        MonthCal_SetColor(hWnd, MCSC_TITLEBK, RGB(0, 0, 0));
        MonthCal_SetColor(hWnd, MCSC_MONTHBK, RGB(0, 0, 0));

        if (SetWindowSubclass(hWnd, MonthCalSubclassProc, kMonthCalSubclassId, 0))
        {
            // Постоянный маркер для PaintDispatcher — определяет тип окна
            // в момент BeginPaint / GetDC без предварительной «взводки».
            GlassShared::GlassMarkWindow(hWnd, GlassShared::GlassControlKind::MonthCal);

            {
                std::lock_guard lock(g_monthCalMutex);
                if (std::find(g_monthCalWindows.begin(), g_monthCalWindows.end(), hWnd) == g_monthCalWindows.end())
                    g_monthCalWindows.push_back(hWnd);
            }

            DebugLog(L"[WndCompat][MonthCal] MonthCal_Attach: subclassed 0x%p", hWnd);
        }
        else
        {
            DebugLog(DBG_WARNING, L"[WndCompat][MonthCal] MonthCal_Attach: FAILED 0x%p", hWnd);
        }

        // Если родитель — DropDown: субклассируем его и уведомляем провайдер.
        if (parentIsDropDown)
        {
            WndCompat::DropDown_Attach(parent);

            auto* p = WndCompat::DropDown::GetDropDownEventProvider();
            if (p && p->OnMonthCalAttach)
                p->OnMonthCalAttach(hWnd, parentIsDropDown ? parent : nullptr);

            DebugLog(L"[WndCompat][EVENT_OBJECT_CREATE] DropDown_Attach (via MonthCal): hwnd=0x%p", parent);
        }

        DebugLog(L"[WndCompat][EVENT_OBJECT_CREATE] MonthCal_Attach: hwnd=0x%p, dropDown=0x%p",
            hWnd, parentIsDropDown ? parent : nullptr);
    }

    void MonthCal_Detach(HWND hWnd)
    {
        GlassShared::GlassUnmarkWindow(hWnd);

        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, MonthCalSubclassProc, kMonthCalSubclassId);

        {
            std::lock_guard lock(g_monthCalMutex);
            const auto it = std::find(g_monthCalWindows.begin(), g_monthCalWindows.end(), hWnd);
            if (it != g_monthCalWindows.end())
                g_monthCalWindows.erase(it);
        }

        DebugLog(L"[WndCompat][MonthCal] MonthCal_Detach: 0x%p", hWnd);
    }

    void MonthCal_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_monthCalMutex);
            windows = g_monthCalWindows;
            g_monthCalWindows.clear();
        }

        const DWORD curTid = GetCurrentThreadId();
        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
            if (wndTid == 0 || wndTid == curTid)
            {
                MonthCal_Detach(hWnd);
                DebugLog(L"[WndCompat][MonthCal] MonthCal_DetachAll: detached hwnd=0x%p", hWnd);
                continue;
            }

            bool detached = false;
            for (int attempt = 1; attempt <= kDetachRetryCount; ++attempt)
            {
                DWORD_PTR sendResult = 0;
                if (SendMessageTimeoutW(
                        hWnd,
                        WndCompat::WM_GLASS_DETACH_MONTHCAL,
                        0,
                        0,
                        SMTO_ABORTIFHUNG,
                        kDetachTimeoutMs,
                        &sendResult))
                {
                    detached = true;
                    DebugLog(
                        L"[WndCompat][MonthCal] MonthCal_DetachAll: cross-thread detached hwnd=0x%p attempt=%d",
                        hWnd,
                        attempt);
                    break;
                }
            }

            if (!detached)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[WndCompat][MonthCal] MonthCal_DetachAll: cross-thread timeout hwnd=0x%p",
                    hWnd);
            }
        }
    }

    bool MonthCal_IsSubclassed(HWND hWnd)
    {
        DWORD_PTR dwRefData = 0;
        return GetWindowSubclass(hWnd, MonthCalSubclassProc, kMonthCalSubclassId, &dwRefData) != FALSE;
    }
}
