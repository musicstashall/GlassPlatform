#include "SharedPch.h"

#include "framework.h"
#include "LogViewerComboLBoxSubclass.h"
#include "LogViewerBackdrop.h"
#include "LogViewerGlassHost.h"

#include <GlassControlKind.h>
#include <GlassCoreProviders.h>
#include <GlassMessages.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace
{
    std::vector<HWND> g_comboLBoxList;

    WndCompat::DropDown::DropDownEventProvider* g_cachedDropdownPtr = nullptr;
    bool                                          g_cachedDropdownTried = false;
    GlassCoreProviders                            g_cachedProviders{};

    const WndCompat::DropDown::DropDownEventProvider* GetDropDownProvider() noexcept
    {
        if (g_cachedDropdownTried)
            return g_cachedDropdownPtr;
        g_cachedDropdownTried = true;
        HMODULE const hm = LogViewerGlass::GetGlassCoreModule();
        if (!hm)
            return nullptr;
        using PFN = BOOL(WINAPI*)(GlassCoreProviders*);
        const auto fn = reinterpret_cast<PFN>(::GetProcAddress(hm, "GlassCore_GetProviders"));
        if (!fn)
            return nullptr;
        if (!fn(&g_cachedProviders))
            return nullptr;
        g_cachedDropdownPtr = &g_cachedProviders.dropdown;
        return g_cachedDropdownPtr;
    }

    void FlushPendingUiMessagesImpl(DWORD maxWaitMs) noexcept
    {
        const DWORD deadline = ::GetTickCount() + maxWaitMs;
        while (::GetTickCount() < deadline)
        {
            MSG msg{};
            while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                if (msg.message == WM_QUIT)
                {
                    ::PostQuitMessage(static_cast<int>(msg.wParam));
                    return;
                }
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
            }
            ::Sleep(1);
        }
    }

    LRESULT CALLBACK LogViewerComboLBoxSubclassProc(
        HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
        UINT_PTR /*uIdSubclass*/, DWORD_PTR /*dwRefData*/)
    {
        if (uMsg == WndCompat::WM_GLASS_DETACH_COMBOLVOX)
        {
            GlassShared::GlassUnmarkWindow(hWnd);
            {
                auto it = std::find(g_comboLBoxList.begin(), g_comboLBoxList.end(), hWnd);
                if (it != g_comboLBoxList.end())
                    g_comboLBoxList.erase(it);
            }
            ::RemoveWindowSubclass(hWnd, LogViewerComboLBoxSubclassProc, 7);
            return 0;
        }

        switch (uMsg)
        {
        case WM_SHOWWINDOW:
        {
            if (!wParam)
            {
                if (const auto* p = GetDropDownProvider())
                {
                    if (p->OnComboLBoxDetach)
                        p->OnComboLBoxDetach(hWnd);
                }
                else
                    LogViewer::ClearWindowBackdrop(hWnd);
                return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
            }

            BOOL comboAnim = TRUE;
            ::SystemParametersInfoW(SPI_GETCOMBOBOXANIMATION, 0, &comboAnim, 0);
            if (const auto* p = GetDropDownProvider())
            {
                if (p->OnComboLBoxShow)
                    p->OnComboLBoxShow(hWnd, comboAnim != FALSE);
                else
                    LogViewer::ApplyWindowBackdrop(hWnd);
            }
            else
                LogViewer::ApplyWindowBackdrop(hWnd);

            const LRESULT lr = ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
            ::RedrawWindow(hWnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            return lr;
        }

        case WM_ERASEBKGND:
        {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc{};
            ::GetClientRect(hWnd, &rc);
            ::PatBlt(
                hdc,
                rc.left,
                rc.top,
                rc.right - rc.left,
                rc.bottom - rc.top,
                BLACKNESS);
            return TRUE;
        }

        case WM_PRINTCLIENT:
        {
            const auto* p = GetDropDownProvider();
            if (p && p->OnComboLBoxPrintClientBegin)
                p->OnComboLBoxPrintClientBegin(hWnd);
            const LRESULT lr = ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
            if (p && p->OnComboLBoxPrintClientEnd)
                p->OnComboLBoxPrintClientEnd(hWnd);
            return lr;
        }

        case WM_NCDESTROY:
        {
            if (const auto* p = GetDropDownProvider())
            {
                if (p->OnComboLBoxDetach)
                    p->OnComboLBoxDetach(hWnd);
            }
            GlassShared::GlassUnmarkWindow(hWnd);
            ::RemoveWindowSubclass(hWnd, LogViewerComboLBoxSubclassProc, 7);
            {
                auto it = std::find(g_comboLBoxList.begin(), g_comboLBoxList.end(), hWnd);
                if (it != g_comboLBoxList.end())
                    g_comboLBoxList.erase(it);
            }
            break;
        }

        default:
            break;
        }

        return ::DefSubclassProc(hWnd, uMsg, wParam, lParam);
    }

    bool IsAttachedImpl(HWND hList) noexcept
    {
        if (!hList)
            return false;
        return std::find(g_comboLBoxList.begin(), g_comboLBoxList.end(), hList) != g_comboLBoxList.end();
    }

    void AttachImpl(HWND hList) noexcept
    {
        if (!hList || !::IsWindow(hList))
            return;
        if (IsAttachedImpl(hList))
            return;
        if (::SetWindowSubclass(hList, LogViewerComboLBoxSubclassProc, 7, 0))
        {
            GlassShared::GlassMarkWindow(hList, GlassShared::GlassControlKind::ComboLBox);
            g_comboLBoxList.push_back(hList);
        }
    }

    void DetachImpl(HWND hList) noexcept
    {
        if (!hList)
            return;
        if (::IsWindow(hList))
        {
            if (const auto* p = GetDropDownProvider())
            {
                if (p->OnComboLBoxDetach)
                    p->OnComboLBoxDetach(hList);
            }
        }
        GlassShared::GlassUnmarkWindow(hList);
        if (::IsWindow(hList))
            ::RemoveWindowSubclass(hList, LogViewerComboLBoxSubclassProc, 7);
        auto it = std::find(g_comboLBoxList.begin(), g_comboLBoxList.end(), hList);
        if (it != g_comboLBoxList.end())
            g_comboLBoxList.erase(it);
    }

    void DetachAllImpl() noexcept
    {
        auto v = g_comboLBoxList;
        g_comboLBoxList.clear();
        const DWORD curTid = ::GetCurrentThreadId();
        for (HWND h : v)
        {
            if (!h || !::IsWindow(h))
                continue;
            if (const auto* p = GetDropDownProvider())
            {
                if (p->OnComboLBoxDetach)
                    p->OnComboLBoxDetach(h);
            }
            if (::GetWindowThreadProcessId(h, nullptr) == curTid)
            {
                GlassShared::GlassUnmarkWindow(h);
                ::RemoveWindowSubclass(h, LogViewerComboLBoxSubclassProc, 7);
            }
            else
            {
                DWORD_PTR r = 0;
                (void)::SendMessageTimeoutW(
                    h,
                    WndCompat::WM_GLASS_DETACH_COMBOLVOX,
                    0,
                    0,
                    SMTO_ABORTIFHUNG,
                    1000,
                    &r);
            }
        }
    }

    void NotifyComboBoxDropDownImpl(HWND hComboBox) noexcept
    {
        if (!hComboBox || !::IsWindow(hComboBox))
            return;
        const auto* p = GetDropDownProvider();
        if (p && p->OnComboBoxDropDown)
            p->OnComboBoxDropDown(hComboBox);
    }

    void ClearCachedGlassProvidersImpl() noexcept
    {
        g_cachedDropdownTried = false;
        g_cachedDropdownPtr = nullptr;
        g_cachedProviders = {};
    }
} // namespace

namespace LogViewer::ComboLBoxUi
{
    bool IsAttached(HWND hList) noexcept
    {
        return IsAttachedImpl(hList);
    }

    void Attach(HWND hList) noexcept
    {
        AttachImpl(hList);
    }

    void Detach(HWND hList) noexcept
    {
        DetachImpl(hList);
    }

    void DetachAll() noexcept
    {
        DetachAllImpl();
    }

    void NotifyComboBoxDropDown(HWND hComboBox) noexcept
    {
        NotifyComboBoxDropDownImpl(hComboBox);
    }

    void FlushPendingUiMessages(DWORD maxWaitMs) noexcept
    {
        FlushPendingUiMessagesImpl(maxWaitMs);
    }

    void ClearCachedGlassProviders() noexcept
    {
        ClearCachedGlassProvidersImpl();
    }
} // namespace LogViewer::ComboLBoxUi
