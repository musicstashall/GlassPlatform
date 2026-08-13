#include "WndPch.h"
#include "DirectUISubclass.h"
#include <WndCompat.h>
#include <DirectUIEventProvider.h>
#include <RootEventProvider.h>
#include <wil.h>
#include <algorithm>
#include <vector>
#include <Root/RootExplorerSubclassBridge.h>
#include <RootPolicyQuery.h>
#include <GlassBackdropMark.h>
#include "Tracker/WinEventHandler.h"

namespace
{
    void RedrawDirectUIWindow(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return;

        InvalidateRect(hWnd, nullptr, TRUE);

        const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
        const DWORD curTid = GetCurrentThreadId();

        if (wndTid == curTid)
        {
            RedrawWindow(hWnd, nullptr, nullptr,
                RDW_ERASENOW | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
        else
        {
            DWORD_PTR result = 0;
            SendMessageTimeoutW(hWnd, WM_PAINT, 0, 0,
                SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &result);
        }
    }
    // Реестр зарегистрированных DirectUIHWND-окон.
    // Заполняется в DirectUI_Attach (из EVENT_OBJECT_CREATE),
    // опустошается в DirectUI_Detach (из EVENT_OBJECT_DESTROY) и DetachAll.
    static std::vector<HWND> g_directuiWindows;
    static SRWLOCK           g_directuiRegLock = SRWLOCK_INIT;
}

namespace WndCompat
{
    void DirectUI_Attach(HWND hWnd)
    {
        if (!IsWindow(hWnd))
            return;

        AcquireSRWLockExclusive(&g_directuiRegLock);
        const auto unlock = wil::scope_exit([&] { ReleaseSRWLockExclusive(&g_directuiRegLock); });

        // Не регистрируем дважды
        if (std::find(g_directuiWindows.begin(), g_directuiWindows.end(), hWnd)
            != g_directuiWindows.end())
            return;

        g_directuiWindows.push_back(hWnd);
        WndCompat::RegisterTextGlowPopupClientWithGlassCore(hWnd, 2);
        WndCompat::Root::ExplorerBridge::ApplyRootClassBlackBrush(hWnd);
        if (const auto* p = WndCompat::DirectUI::GetDirectUIEventProvider())
        {
            if (p->OnDirectUIRootHooksAttach)
                p->OnDirectUIRootHooksAttach(hWnd);
        }

        DebugLog(L"[WndCompat][DirectUI] DirectUI_Attach: 0x%p (total=%zu)",
            hWnd, g_directuiWindows.size());
    }

    void DirectUI_Detach(HWND hWnd)
    {
        AcquireSRWLockExclusive(&g_directuiRegLock);
        const auto unlock = wil::scope_exit([&] { ReleaseSRWLockExclusive(&g_directuiRegLock); });

        auto it = std::find(g_directuiWindows.begin(), g_directuiWindows.end(), hWnd);
        if (it == g_directuiWindows.end())
            return;

        if (auto* hp = WndCompat::Root::GetRootEventProvider())
        {
            if (hp->OnRootHooksDetach)
                hp->OnRootHooksDetach(hWnd);
        }

        WndCompat::UnregisterTextGlowClientWithGlassCore(hWnd);

        g_directuiWindows.erase(it);

        DebugLog(L"[WndCompat][DirectUI] DirectUI_Detach: 0x%p (total=%zu)",
            hWnd, g_directuiWindows.size());
    }

    void DirectUI_DetachAll()
    {
        std::vector<HWND> windows;
        {
            AcquireSRWLockExclusive(&g_directuiRegLock);
            DebugLog(L"[WndCompat][DirectUI] DirectUI_DetachAll: count=%zu", g_directuiWindows.size());
            windows = std::move(g_directuiWindows);
            g_directuiWindows.clear();
            ReleaseSRWLockExclusive(&g_directuiRegLock);
        }

        for (HWND hWnd : windows)
        {
            if (!IsWindow(hWnd))
                continue;

            if (auto* hp = WndCompat::Root::GetRootEventProvider())
            {
                if (hp->OnRootHooksDetach)
                    hp->OnRootHooksDetach(hWnd);
            }

            const DWORD wndTid = GetWindowThreadProcessId(hWnd, nullptr);
            const DWORD curTid = GetCurrentThreadId();

            // InvalidateRect потокобезопасен — работает из любого потока
            InvalidateRect(hWnd, nullptr, TRUE);

            if (wndTid == curTid)
            {
                // Свой поток — синхронная перерисовка
                RedrawWindow(hWnd, nullptr, nullptr,
                    RDW_ERASENOW | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
            else
            {
                // Чужой поток — SendMessage доставляет WM_PAINT синхронно
                // в поток-владелец и блокируется до завершения отрисовки
                DWORD_PTR result = 0;
                SendMessageTimeoutW(hWnd, WM_PAINT, 0, 0,
                    SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &result);
            }

            DebugLog(L"[WndCompat][DirectUI] DirectUI_DetachAll: redrawn 0x%p", hWnd);
        }
    }

    void DirectUI_ApplyBackdropPolicyToTracked() noexcept
    {
        const bool allowed = WndCompat::IsGlassVisualsAllowed();

        std::vector<HWND> windows;
        {
            AcquireSRWLockShared(&g_directuiRegLock);
            windows = g_directuiWindows;
            ReleaseSRWLockShared(&g_directuiRegLock);
        }

        for (HWND hWnd : windows)
        {
            if (!hWnd || !IsWindow(hWnd))
                continue;

            if (allowed)
            {
                if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
                    continue;

                WndCompat::Root::ExplorerBridge::ApplyRootClassBlackBrush(hWnd);
                if (const auto* p = WndCompat::DirectUI::GetDirectUIEventProvider())
                {
                    if (p->OnDirectUIRootHooksAttach)
                        p->OnDirectUIRootHooksAttach(hWnd);
                }
            }
            else
            {
                WndCompat::Root::ExplorerBridge::RestoreRootClassBrushForHwnd(hWnd);
            }

            RedrawDirectUIWindow(hWnd);
        }

        DebugLog(DBG_IMPORTANT,
            L"[WndCompat][DirectUI] DirectUI_ApplyBackdropPolicy: allowed=%d count=%zu",
            allowed ? 1 : 0,
            windows.size());
    }
}
