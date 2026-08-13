#include "WndPch.h"
#include "WndCompat.h"
#include "UiDispatcher.h"
#include "Root/RootSubclass.h"


#include <atomic>
#include <vector>
#include <mutex>

namespace
{
    struct ThreadContext
    {
        bool initialized = false;
        std::vector<HWND> attachedRoots;

        bool HasAttachedWindow(HWND hWnd) const
        {
            return std::find(attachedRoots.begin(), attachedRoots.end(), hWnd) != attachedRoots.end();
        }

        bool AddWindow(HWND hWnd)
        {
            if (!HasAttachedWindow(hWnd))
            {
                attachedRoots.push_back(hWnd);
                return true;
            }
            return false;
        }

        void ClearWindows()
        {
            attachedRoots.clear();
        }
    };

    static ThreadContext g_context;
    static std::mutex    g_mutex;
    static bool          g_dispatcherInitialized = false;
}

namespace UiDispatcher
{
    void Initialize()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (g_dispatcherInitialized)
        {
            DebugLog(L"[UiDispatcher] Already initialized");
            return;
        }

        g_dispatcherInitialized = true;
        g_context.initialized = true;
        g_context.attachedRoots.clear();

        DebugLog(L"[UiDispatcher] Dispatcher initialized (single-thread mode)");
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (!g_dispatcherInitialized)
            return;

        DebugLog(L"[UiDispatcher] Shutdown: BEGIN");

        for (HWND hWnd : g_context.attachedRoots)
        {
            if (IsWindow(hWnd))
            {
                WndCompat::Root_Detach(hWnd);
            }
        }

        g_context.attachedRoots.clear();
        g_context.initialized = false;
        g_dispatcherInitialized = false;

        DebugLog(L"[UiDispatcher] Shutdown: END");
    }

    void ShutdownHostExit() noexcept
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        if (!g_dispatcherInitialized)
            return;

        g_context.attachedRoots.clear();
        g_context.initialized = false;
        g_dispatcherInitialized = false;
    }

    bool ProcessWindowsMessage(UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (!g_dispatcherInitialized)
            return false;

        // Здесь можно добавить обработку своих WM_APP_* сообщений,
        // если захочешь расширять протокол.
        UNREFERENCED_PARAMETER(msg);
        UNREFERENCED_PARAMETER(wParam);
        UNREFERENCED_PARAMETER(lParam);

        return false;
    }
}
