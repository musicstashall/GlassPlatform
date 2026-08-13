#include "SharedPch.h"

#include "LogViewerWndCompatScan.h"
#include "LogViewerGlassHost.h"

#include <GlassControlKind.h>

#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace
{
    [[nodiscard]] bool IsCheckboxOrGroupBoxStyle(UINT style) noexcept
    {
        switch (style & BS_TYPEMASK)
        {
        case BS_GROUPBOX:
        case BS_CHECKBOX:
        case BS_AUTOCHECKBOX:
        case BS_3STATE:
        case BS_AUTO3STATE:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool ShouldMarkManuallyInsteadOfWndCompat(HWND hWnd, HWND hInkPreviewHostPage) noexcept
    {
        if (!hWnd || !::IsWindow(hWnd))
            return false;

        wchar_t cls[256]{};
        if (::GetClassNameW(hWnd, cls, static_cast<int>(_countof(cls))) <= 0)
            return false;

        if (wcscmp(cls, L"Button") == 0)
        {
            const UINT st = static_cast<UINT>(::GetWindowLongPtrW(hWnd, GWL_STYLE)) & BS_TYPEMASK;
            return !IsCheckboxOrGroupBoxStyle(st);
        }
        if (wcscmp(cls, WC_TABCONTROLW) == 0)
            return true;
        if (wcscmp(cls, L"ComboBox") == 0)
            return true;
        if (wcscmp(cls, L"Edit") == 0 && hInkPreviewHostPage && ::IsWindow(hInkPreviewHostPage)
            && ::IsChild(hInkPreviewHostPage, hWnd))
            return true;
        return false;
    }

    void ApplyManualMark(HWND hWnd) noexcept
    {
        using namespace GlassShared;
        const GlassControlKind existing = GlassGetWindowKind(hWnd);

        wchar_t cls[256]{};
        if (::GetClassNameW(hWnd, cls, static_cast<int>(_countof(cls))) <= 0)
            return;

        if (wcscmp(cls, WC_TABCONTROLW) == 0)
        {
            if (existing == GlassControlKind::None)
                GlassMarkWindow(hWnd, GlassControlKind::TabControl);
            return;
        }
        if (wcscmp(cls, L"ComboBox") == 0)
        {
            if (existing == GlassControlKind::None)
                GlassMarkWindow(hWnd, GlassControlKind::ComboBox);
            return;
        }
        if (wcscmp(cls, L"Edit") == 0)
        {
            if (existing == GlassControlKind::None)
                GlassMarkWindow(hWnd, GlassControlKind::Edit);
            return;
        }
        if (wcscmp(cls, L"Button") == 0)
        {
            if (existing == GlassControlKind::None)
                GlassMarkWindow(hWnd, GlassControlKind::Button);
        }
    }

    void DispatchSyntheticCreateForHwnd(
        HWND hWnd,
        HWND hInkPreviewHostPage,
        LogViewerGlass::WndCompatHandleWinEventFn pfn) noexcept
    {
        if (!hWnd || !::IsWindow(hWnd))
            return;

        if (ShouldMarkManuallyInsteadOfWndCompat(hWnd, hInkPreviewHostPage))
            ApplyManualMark(hWnd);
        else if (pfn)
        {
            pfn(
                EVENT_OBJECT_CREATE,
                hWnd,
                OBJID_WINDOW,
                CHILDID_SELF,
                ::GetCurrentThreadId(),
                0);
        }
    }

    void VisitRecursive(HWND hWnd, HWND hInkPreviewHostPage, LogViewerGlass::WndCompatHandleWinEventFn pfn) noexcept
    {
        DispatchSyntheticCreateForHwnd(hWnd, hInkPreviewHostPage, pfn);

        for (HWND c = ::GetWindow(hWnd, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT))
            VisitRecursive(c, hInkPreviewHostPage, pfn);
    }
}

namespace LogViewerWndCompatScan
{
    void AttachLogViewerWindowTree(HWND hRoot, HWND hInkPreviewHostPageOpt) noexcept
    {
        if (!hRoot || !::IsWindow(hRoot))
            return;
        const auto pfn = LogViewerGlass::GetWndCompatHandleWinEvent();
        DispatchSyntheticCreateForHwnd(hRoot, hInkPreviewHostPageOpt, pfn);
        for (HWND c = ::GetWindow(hRoot, GW_CHILD); c; c = ::GetWindow(c, GW_HWNDNEXT))
            VisitRecursive(c, hInkPreviewHostPageOpt, pfn);
    }

    void AttachLogViewerControlSubtree(HWND hControlRoot, HWND hInkPreviewHostPageOpt) noexcept
    {
        if (!hControlRoot || !::IsWindow(hControlRoot))
            return;
        const auto pfn = LogViewerGlass::GetWndCompatHandleWinEvent();
        VisitRecursive(hControlRoot, hInkPreviewHostPageOpt, pfn);
    }
}
