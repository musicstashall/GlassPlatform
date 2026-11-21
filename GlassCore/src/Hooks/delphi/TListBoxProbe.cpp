#include "CorePch.h"
#include <Hooks/delphi/TListBoxProbe.h>
#include <Hooks/RootGdiHooks.h>
#include <Subsystems/RootSwcaGradient.h>
#include <Module/ModuleHandler.h>
#include <GlassBackdropMark.h>
#include <GlassControlKind.h>
#include <DebugLog.h>
#include <vector>

namespace GlassCore::DelphiListBox
{
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
    static thread_local std::vector<HWND> g_drawStack;
#endif

    bool BeginDrawProbe(HWND parent, const DRAWITEMSTRUCT* dis) noexcept
    {
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        if (!GlassCore::Root::IsFullInjectionMode() || !dis || dis->CtlType != ODT_LISTBOX
            || !dis->hwndItem || !dis->hDC || GetParent(dis->hwndItem) != parent
            || GlassShared::GlassGetWindowKind(dis->hwndItem) != GlassShared::GlassControlKind::ListBox
            || !GlassShared::GlassIsBackdropAllowedForHwnd(dis->hwndItem)) return false;
        WCHAR cls[64]{};
        if (!GetClassNameW(dis->hwndItem, cls, _countof(cls))
            || (_wcsicmp(cls, L"TListBox") != 0 && _wcsicmp(cls, L"TCheckListBox") != 0
                && _wcsicmp(cls, L"TColorListBox") != 0
                && _wcsicmp(cls, L"TFileListBox") != 0
                && _wcsicmp(cls, L"TDirectoryListBox") != 0)) return false;
        if (!GlassCore::ModuleHandler::g_entryPointModule) return false;
        g_drawStack.push_back(dis->hwndItem);
        GlassCore::Root::BeginVclDrawProbe(dis->hwndItem, true);
        static thread_local ULONGLONG epoch = 0;
        static thread_local unsigned budget = 0;
        const ULONGLONG now = GetTickCount64();
        if (!epoch || now - epoch >= 1000) { epoch = now; budget = 32; }
        if (budget)
        {
            --budget;
            const RECT& r = dis->rcItem;
            DebugLog(DBG_NORMAL, L"[GlassCore][DelphiListBox] WM_DRAWITEM hwnd=0x%p class=%s dc=0x%p dcWnd=0x%p action=0x%X state=0x%X rect=[%ld,%ld,%ld,%ld]",
                dis->hwndItem, cls, dis->hDC, WindowFromDC(dis->hDC), dis->itemAction,
                dis->itemState, r.left, r.top, r.right, r.bottom);
        }
        return true;
#else
        (void)parent; (void)dis;
        return false;
#endif
    }

    void EndDrawProbe() noexcept
    {
#if defined(_DEBUG) && defined(GLASS_DELPHI_DRAW_PROBE)
        if (g_drawStack.empty()) return;
        const HWND hwnd = g_drawStack.back();
        g_drawStack.pop_back();
        GlassCore::Root::EndVclDrawProbe(hwnd, true);
#endif
    }
}
