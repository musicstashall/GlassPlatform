#pragma once
#include <windows.h>

namespace WndCompat::DirectUI
{
    // Провайдер событий DirectUIHWND.
    // Заполняется в GlassCore::Engine (через GlassCore_GetProviders),
    // потребляется в DirectUISubclass.cpp (Attach/Detach) и в субклассе WM_PAINT при наличии.
    struct DirectUIEventProvider
    {
        // Устанавливает thread_local g_paintingDUI = hWnd в GlassCore.
        void (*OnDuiPaintBegin)(HWND hWnd, HMODULE callerModule) = nullptr;

        // Сбрасывает thread_local g_paintingDUI = nullptr в GlassCore.
        void (*OnDuiPaintEnd)(HWND hWnd, HMODULE callerModule) = nullptr;
        // DirectUI_Attach: кеш root paint для HWND (IEUI→IEFRAME или DUser+wdc), без Root-сабкласса.
        void (*OnDirectUIRootHooksAttach)(HWND hDuiWnd) = nullptr;
    };

    void SetDirectUIEventProvider(const DirectUIEventProvider& provider);
    const DirectUIEventProvider* GetDirectUIEventProvider();
}