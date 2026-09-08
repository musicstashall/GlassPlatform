#include "WndPch.h"
#include "ComboBoxSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <DropDownEventProvider.h>
#include "Tracker/WindowClassUtils.h"
#include <Tracker/WinEventHandler.h>

#include <vector>
#include <algorithm>

namespace
{
    LRESULT CALLBACK ComboBoxSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        // Снятие субкласса по запросу из чужого потока (ComboBox_DetachAll → SendMessageTimeout).
        // Обрабатываем до шлюза IsShuttingDown — это и есть цель shutdown-пути.
        // RemoveWindowSubclass здесь безопасен: мы в потоке-владельце окна.
        if (uMsg == WndCompat::WM_GLASS_DETACH_COMBOBOX)
        {
            RemoveWindowSubclass(hWnd, ComboBoxSubclassProc, 2);
            DebugLog(L"[WndCompat][ComboBox] WM_GLASS_DETACH_COMBOBOX: субкласс снят hwnd=0x%p", hWnd);
            WndCompat::WinEventHook_SignalDetachComplete();
            return 0;
        }

        if (WndCompat::IsShuttingDown())
            return DefSubclassProc(hWnd, uMsg, wParam, lParam);

        switch (uMsg)
        {
        case WM_LBUTTONDOWN:
        {
            // Только для ComboBox внутри FloatNotifySink (Explorer address bar и др.)
            if (WndCompat::IsWindowClass(GetParent(hWnd), L"FloatNotifySink"))
            {
                // Если ComboBox ещё не раскрыт — включаем перехват AnimateWindow
                if (!SendMessageW(hWnd, CB_GETDROPPEDSTATE, 0, 0))
                {
                    auto* p = WndCompat::DropDown::GetDropDownEventProvider();
                    if (p && p->OnComboBoxDropDown)
                        p->OnComboBoxDropDown(hWnd);

                    DebugLog(L"[WndCompat][ComboBox] ComboBox: WM_LBUTTONDOWN → AnimateWindow hook enabled, hwnd=0x%p", hWnd);
                }
            }
            break;
        }

        case WM_NCDESTROY:
            WndCompat::ComboBox_Detach(hWnd);
            break;
        }

        return DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }
}

namespace
{
    static std::vector<HWND> g_comboBoxWindows;
}

namespace WndCompat
{
    void ComboBox_Attach(HWND hWnd)
    {
        ComboBox_Attach(hWnd, GlassShared::GlassControlKind::ComboBox);
    }

    void ComboBox_Attach(HWND hWnd, GlassShared::GlassControlKind kind)
    {
        if (!IsWindow(hWnd))
            return;

        // Idempotent
        if (std::find(g_comboBoxWindows.begin(), g_comboBoxWindows.end(), hWnd) != g_comboBoxWindows.end())
            return;

        if (SetWindowSubclass(hWnd, ComboBoxSubclassProc, 2, 0))
        {
            g_comboBoxWindows.push_back(hWnd);
            GlassShared::GlassMarkWindow(hWnd, kind);
            WndCompat::RegisterThemedChromeClientWithGlassCore(hWnd);
            DebugLog(L"[WndCompat][ComboBox] ComboBox_Attach: subclassed 0x%p", hWnd);
        }
        else
        {
            DebugLog(L"[WndCompat][ComboBox] ComboBox_Attach: FAILED 0x%p", hWnd);
        }
    }

    void ComboBox_Detach(HWND hWnd)
    {
        WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);
        if (IsWindow(hWnd))
            RemoveWindowSubclass(hWnd, ComboBoxSubclassProc, 2);

        auto it = std::find(g_comboBoxWindows.begin(), g_comboBoxWindows.end(), hWnd);
        if (it != g_comboBoxWindows.end())
            g_comboBoxWindows.erase(it);

        GlassShared::GlassUnmarkWindow(hWnd);
        DebugLog(L"[WndCompat][ComboBox] ComboBox_Detach: 0x%p", hWnd);
    }

    void ComboBox_DetachAll()
    {
        auto windows = g_comboBoxWindows;
        g_comboBoxWindows.clear();

        const DWORD curTid = GetCurrentThreadId();

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            WndCompat::UnregisterThemedChromeClientWithGlassCore(hWnd);

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);

            if (wndTid == curTid)
            {
                RemoveWindowSubclass(hWnd, ComboBoxSubclassProc, 2);
                DebugLog(L"[WndCompat][ComboBox] ComboBox_DetachAll: detached 0x%p", hWnd);
            }
            else
            {
                WinEventHook_RegisterPendingDetach();
                BOOL posted = PostMessageW(hWnd, WM_GLASS_DETACH_COMBOBOX, 0, 0);
                if (!posted)
                {
                    WinEventHook_SignalDetachComplete();
                    DebugLog(L"[WndCompat][ComboBox] ComboBox_DetachAll: PostMessageW FAILED for 0x%p", hWnd);
                }
                else
                {
                    DebugLog(L"[WndCompat][ComboBox] ComboBox_DetachAll: cross-thread detached hwnd=0x%p",
                        hWnd);
                }
            }
        }
    }
}
