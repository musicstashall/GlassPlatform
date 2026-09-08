// GlassCore/src/Core/Intercept/Hooks.cpp
#include "CorePch.h"
#include <Core/Intercept/Hooks.h>
#include "Module\ModuleHandler.h"
#include <Core/Intercept/IATRegistry.h>
#include <InjectProcessMap.h>

#include <vector>
#include <mutex>

namespace GlassCore::Hooks
{
    static std::vector<HookCallback> g_callbacks;
    static std::mutex g_cbMutex;
    static HHOOK g_msgHook = nullptr;

    bool Install()
    {
        DebugLog(L"[GlassCore][Hooks] Hooks::Install\n");

        (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagLifecycleHooks,
            true);

        GlassCore::ModuleHandler::Startup();
        (void)Shared::InjectProcessMap::TrySetDiagLifecycle(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagLifecycleHooks,
            false);
        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagHooks,
            true);

        DebugLog(L"[GlassCore][Hooks] Hooks::Install done\n");
        return true;
    }

    void Uninstall()
    {
        DebugLog(L"[GlassCore][Hooks] Hooks::Uninstall\n");

        (void)Shared::InjectProcessMap::TrySetDiagFlag(
            GetCurrentProcessId(),
            Shared::InjectProcessMap::DiagHooks,
            false);

        GlassCore::ModuleHandler::Shutdown();

        GlassCore::Hooks::IATRegistry::Instance().Shutdown();

        // Если позже появятся message‑хуки — снимаем их здесь
        if (g_msgHook)
        {
            UnhookWindowsHookEx(g_msgHook);
            g_msgHook = nullptr;
            DebugLog(L"[GlassCore][Hooks] Message hook removed\n");
        }
    }

    void RegisterWindowCallback(HookCallback cb)
    {
        std::lock_guard<std::mutex> lk(g_cbMutex);
        g_callbacks.push_back(cb);
    }

    // Вспомогательная функция — дернуть все зарегистрированные коллбеки для окна
    void InvokeCallbacks(HWND hwnd)
    {
        std::lock_guard<std::mutex> lk(g_cbMutex);
        for (auto cb : g_callbacks)
        {
            if (cb)
                cb(hwnd);
        }
    }
}
