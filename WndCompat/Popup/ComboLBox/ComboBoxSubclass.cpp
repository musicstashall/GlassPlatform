#include "WndPch.h"
#include "ComboBoxSubclass.h"
#include "ComboLBoxSubclass.h"
#include <WndCompat.h>
#include <GlassControlKind.h>
#include <GlassBackdropMark.h>
#include <InjectionMode.h>
#include <DropDownEventProvider.h>
#include "Tracker/WindowClassUtils.h"
#include <Tracker/WinEventHandler.h>

#include <vector>
#include <algorithm>

namespace
{
    // Exact VCL window classes only; native ComboBox/WinForms keep their policy.
    bool IsDelphiComboBox(HWND hwnd) noexcept
    {
        return WndCompat::IsWindowClass(hwnd, L"TComboBox")
            || WndCompat::IsWindowClass(hwnd, L"TColorBox")
            || WndCompat::IsWindowClass(hwnd, L"TDriveComboBox")
            || WndCompat::IsWindowClass(hwnd, L"TFilterComboBox");
    }

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
        // A disabled or read-only Edit asks for its brush with WM_CTLCOLORSTATIC,
        // never with WM_CTLCOLOREDIT. VCL answers both itself with one brush.
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        {
            // Только настоящий дочерний Edit VCL ComboBox в Full. Редактор сам
            // посылает CTLCOLOR родителю, поэтому root окна это не перехватывает.
            // The checks call into user32; the native handler gets the message
            // as it came, last error included, whoever sent it.
            const DWORD entryError = GetLastError();
            if (WndCompat::WinEventHook_GetMode() != Shared::InjectionMode::Full
                || !GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)
                || !IsDelphiComboBox(hWnd))
            { SetLastError(entryError); break; }
            COMBOBOXINFO info{ sizeof(info) };
            HWND edit = reinterpret_cast<HWND>(lParam);
            if (!GetComboBoxInfo(hWnd, &info) || info.hwndItem != edit
                || !edit || !WndCompat::IsWindowClass(edit, L"Edit"))
            { SetLastError(entryError); break; }
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetLastError(entryError);
            if (!dc) break;
            const LRESULT original = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            const COLORREF bk = GetBkColor(dc);
            LOGBRUSH lb{};
            const HBRUSH brush = reinterpret_cast<HBRUSH>(original);
            const bool solid = brush && GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb)
                && lb.lbStyle == BS_SOLID;
            const COLORREF windowColor = GetSysColor(COLOR_WINDOW) & 0xFFFFFFu;
            // Не менять произвольный цвет фона/owner-draw. Уже чёрную кисть,
            // возвращённую родителем, тоже оставляем в покое.
            const bool replace = solid && (lb.lbColor & 0xFFFFFFu) == windowColor
                && (bk & 0xFFFFFFu) == windowColor;
            LRESULT result = original;
            if (replace)
            {
                SetBkColor(dc, 0);
                result = reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
            }
#if defined(_DEBUG)
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
            if (budget)
            {
                --budget;
                DebugLog(DBG_NORMAL,
                    L"[WndCompat][DelphiComboEdit] CTLCOLOR combo=0x%p edit=0x%p msg=0x%X dc=0x%p focused=%d bk=0x%08X brush=0x%p solid=%d color=0x%08X replaced=%d",
                    hWnd, edit, uMsg, dc, GetFocus() == edit ? 1 : 0, static_cast<unsigned>(bk),
                    brush, solid ? 1 : 0, static_cast<unsigned>(lb.lbColor), replace ? 1 : 0);
            }
#endif
            return result;
        }

        case WM_CTLCOLORLISTBOX:
        {
            // VCL can consume this message itself, without forwarding to Root.
            // Adapt only the background of this TComboBox's attached popup,
            // after the native chain has established its text/brush state.
            const DWORD entryError = GetLastError();
            const auto native = [&]() -> LRESULT
            {
                SetLastError(entryError);
                return DefSubclassProc(hWnd, uMsg, wParam, lParam);
            };
            const HWND list = reinterpret_cast<HWND>(lParam);
            const HDC dc = reinterpret_cast<HDC>(wParam);
            const auto isOwnList = [&]() -> bool
            {
                COMBOBOXINFO info{ sizeof(info) };
                return !WndCompat::IsShuttingDown()
                    && WndCompat::WinEventHook_GetMode() == Shared::InjectionMode::Full
                    && WndCompat::IsWindowClass(hWnd, L"TComboBox")
                    && GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)
                    && list && IsWindow(list)
                    && (WndCompat::IsWindowClass(list, L"ComboLBox")
                        || WndCompat::IsWindowClass(list, L"TPopupListbox"))
                    && WndCompat::ComboLBox_IsAttached(list)
                    && GlassShared::GlassIsBackdropAllowedForHwnd(list)
                    && GetComboBoxInfo(hWnd, &info)
                    && info.hwndCombo == hWnd && info.hwndList == list;
            };
            if (!dc || !isOwnList()) return native();
            const LRESULT original = native();
            const DWORD originalError = GetLastError();
            // A native callback may have detached/rebound the list or disabled
            // glass. Never carry pre-call eligibility across that reentrancy.
            const bool eligible = isOwnList();
            const COLORREF bk = GetBkColor(dc);
            const HBRUSH brush = reinterpret_cast<HBRUSH>(original);
            LOGBRUSH lb{};
            const bool solid = brush && GetObjectW(brush, sizeof(lb), &lb) == sizeof(lb)
                && lb.lbStyle == BS_SOLID;
            const COLORREF windowColor = GetSysColor(COLOR_WINDOW) & 0xFFFFFFu;
            bool replaced = false;
            LRESULT result = original;
            if (eligible && solid && bk != CLR_INVALID
                && (lb.lbColor & 0xFFFFFFu) == windowColor && (bk & 0xFFFFFFu) == windowColor)
            {
                const HGDIOBJ black = GetStockObject(BLACK_BRUSH);
                if (black && SetBkColor(dc, 0) != CLR_INVALID)
                {
                    result = reinterpret_cast<LRESULT>(black);
                    replaced = true;
                }
            }
#if defined(_DEBUG)
            static thread_local ULONGLONG epoch = 0;
            static thread_local unsigned budget = 0;
            const ULONGLONG now = GetTickCount64();
            if (!epoch || now - epoch >= 1000) { epoch = now; budget = 16; }
            if (budget)
            {
                --budget;
                DebugLog(DBG_NORMAL,
                    L"[WndCompat][DelphiComboList] CTLCOLOR combo=0x%p list=0x%p dc=0x%p bk=0x%08X brush=0x%p solid=%d color=0x%08X eligible=%d replaced=%d",
                    hWnd, list, dc, static_cast<unsigned>(bk), brush, solid ? 1 : 0,
                    static_cast<unsigned>(lb.lbColor), eligible ? 1 : 0, replaced ? 1 : 0);
            }
#endif
            (void)replaced;
            SetLastError(originalError);
            return result;
        }

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
            // Edit может быть отрисован до появления subclass ComboBox. После
            // attach отложенно попросить один повторный paint, не принуждая
            // синхронный WM_PAINT внутри обработки события создания.
            if (WndCompat::WinEventHook_GetMode() == Shared::InjectionMode::Full
                && GlassShared::GlassIsBackdropAllowedForHwnd(hWnd)
                && IsDelphiComboBox(hWnd))
            {
                COMBOBOXINFO info{ sizeof(info) };
                if (GetComboBoxInfo(hWnd, &info) && info.hwndItem
                    && WndCompat::IsWindowClass(info.hwndItem, L"Edit"))
                    InvalidateRect(info.hwndItem, nullptr, TRUE);
            }
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
