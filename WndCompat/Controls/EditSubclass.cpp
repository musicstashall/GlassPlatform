// Субклассирование Edit / RichEdit — без SetWindowSubclass.
// Паттерн: Button, Static, ListView, TreeView — только GlassMarkWindow + вектор HWND.

#include "WndPch.h"
#include "EditSubclass.h"
#include <GlassControlKind.h>
#include <DebugLog.h>
#include <Richedit.h>

#include <vector>
#include <algorithm>
#include <mutex>

namespace
{
    std::vector<HWND> g_editWindows;
    std::vector<HWND> g_richEditWindows;
    std::mutex        g_editMutex;
    std::mutex        g_richEditMutex;
}

namespace WndCompat
{
    // ── Edit (Edit, TEdit, TMemo, .EDIT, ATL:EDIT) ───────────────────────────

    void Edit_Attach(HWND hWnd)
    {
        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Edit][Trace] Edit_Attach: enter hwnd=0x%p tid=%u", hWnd, GetCurrentThreadId());
        if (!IsWindow(hWnd))
        {
            DebugLog(DBG_IMPORTANT, L"[WndCompat][Edit][Trace] Edit_Attach: !IsWindow → return");
            return;
        }

        using namespace GlassShared;
        DebugLog(DBG_IMPORTANT, L"[WndCompat][Edit][Trace] Edit_Attach: before GlassGetWindowKind");
        auto existing = GlassGetWindowKind(hWnd);
        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Edit][Trace] Edit_Attach: existing kind=%d", static_cast<int>(existing));
        if (existing == GlassControlKind::Edit || existing == GlassControlKind::WinForms_Edit)
            return;

        // Определяем kind: ".EDIT" = WinForms, иначе нативный
        WCHAR cls[64]{};
        DebugLog(DBG_IMPORTANT, L"[WndCompat][Edit][Trace] Edit_Attach: before GetClassNameW");
        GetClassNameW(hWnd, cls, _countof(cls));
        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Edit][Trace] Edit_Attach: class=\"%.63s\"", cls);
        auto kind = (cls[0] == L'.') ? GlassControlKind::WinForms_Edit
                                     : GlassControlKind::Edit;

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][Edit][Trace] Edit_Attach: before GlassMarkWindow kind=%d",
            static_cast<int>(kind));
        GlassMarkWindow(hWnd, kind);
        DebugLog(DBG_IMPORTANT, L"[WndCompat][Edit][Trace] Edit_Attach: after GlassMarkWindow");

        {
            std::lock_guard lock(g_editMutex);
            g_editWindows.push_back(hWnd);
        }
        DebugLog(DBG_IMPORTANT, L"[WndCompat][Edit][Trace] Edit_Attach: after push_back");

        DebugLog(L"[WndCompat][Edit] Edit_Attach: hwnd=0x%p kind=%d", hWnd, static_cast<int>(kind));
    }

    void Edit_Detach(HWND hWnd)
    {
        GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_editMutex);
            auto it = std::find(g_editWindows.begin(), g_editWindows.end(), hWnd);
            if (it != g_editWindows.end())
                g_editWindows.erase(it);
        }

        DebugLog(L"[WndCompat][Edit] Edit_Detach: hwnd=0x%p", hWnd);
    }

    void Edit_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_editMutex);
            windows.swap(g_editWindows);
        }

        for (HWND hWnd : windows)
        {
            if (IsWindow(hWnd))
                GlassShared::GlassUnmarkWindow(hWnd);
        }

        DebugLog(L"[WndCompat][Edit] Edit_DetachAll: снято %zu окон", windows.size());
    }

    // ── RichEdit (RichEdit20W, RICHEDIT50W, WinForms-варианты) ───────────────

    void RichEdit_Attach(HWND hWnd, GlassShared::GlassControlKind kind)
    {
        if (!IsWindow(hWnd))
            return;

        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::None)
            return;

        // Не вызываем EM_SETBKGNDCOLOR(0,0): GlassCore читает EM_GETBKGNDCOLOR для полного клиента;
        // принудительный чёрный ломает согласование с фоном, заданным приложением (COLOR_WINDOW / свой цвет).

        GlassMarkWindow(hWnd, kind);

        {
            std::lock_guard lock(g_richEditMutex);
            g_richEditWindows.push_back(hWnd);
        }

        DebugLog(L"[WndCompat][RichEdit] RichEdit_Attach: hwnd=0x%p kind=%d", hWnd, static_cast<int>(kind));
    }

    void RichEdit_Detach(HWND hWnd)
    {
        // Восстанавливаем системный цвет фона
        if (IsWindow(hWnd))
            SendMessageW(hWnd, EM_SETBKGNDCOLOR, 1, 0);

        GlassShared::GlassUnmarkWindow(hWnd);

        {
            std::lock_guard lock(g_richEditMutex);
            auto it = std::find(g_richEditWindows.begin(), g_richEditWindows.end(), hWnd);
            if (it != g_richEditWindows.end())
                g_richEditWindows.erase(it);
        }

        DebugLog(L"[WndCompat][RichEdit] RichEdit_Detach: hwnd=0x%p", hWnd);
    }

    void RichEdit_DetachAll()
    {
        std::vector<HWND> windows;
        {
            std::lock_guard lock(g_richEditMutex);
            windows.swap(g_richEditWindows);
        }

        for (HWND hWnd : windows)
        {
            if (IsWindow(hWnd))
            {
                SendMessageW(hWnd, EM_SETBKGNDCOLOR, 1, 0);
                GlassShared::GlassUnmarkWindow(hWnd);
            }
        }

        DebugLog(L"[WndCompat][RichEdit] RichEdit_DetachAll: снято %zu окон", windows.size());
    }
}
