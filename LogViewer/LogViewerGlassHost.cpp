#include "SharedPch.h"

#include "LogViewerGlassHost.h"

#include <GlassModulePath.h>
#include <InjectionMode.h>

#include <atomic>
#include <string>

namespace
{
    [[nodiscard]] std::wstring DllPathNextToExe(const wchar_t* dllName) noexcept
    {
        const std::wstring resolved =
            GlassShared::GlassModulePath::ResolveGlassModulePathNextToModule(nullptr, dllName);
        if (!resolved.empty())
            return resolved;
        return dllName ? dllName : L"";
    }

    void FlushPendingUiMessages(DWORD maxWaitMs) noexcept
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

    HMODULE                                           g_hGlassCore = nullptr;
    HMODULE                                           g_hWndCompat = nullptr;
    LogViewerGlass::WndCompatHandleWinEventFn         g_pfnWndCompatHandleWinEvent = nullptr;
    LogViewerGlass::GlassCoreApplyDropDownShowFn      g_pfnGlassCoreApplyDropDownShow = nullptr;
    using GlassCoreReloadRenderTextInkFn = void(WINAPI*)();
    GlassCoreReloadRenderTextInkFn                    g_pfnGlassCoreReloadRenderTextInk = nullptr;
    std::atomic<bool>                                 g_executeShutdownDone{ false };

    void RunExecuteShutdownBeforeFreeLibrary() noexcept
    {
        bool expected = false;
        if (!g_executeShutdownDone.compare_exchange_strong(expected, true))
            return;

        if (g_hGlassCore)
        {
            using PFN = void(WINAPI*)();
            const auto pShutdown = reinterpret_cast<PFN>(::GetProcAddress(g_hGlassCore, "GlassCore_Shutdown"));
            if (pShutdown)
                pShutdown();
        }

        FlushPendingUiMessages(200);
        (void)::SendNotifyMessageW(HWND_BROADCAST, WM_NULL, 0, 0);
        FlushPendingUiMessages(50);

        if (g_hWndCompat)
        {
            using PFNBegin = void(WINAPI*)();
            const auto pBegin = reinterpret_cast<PFNBegin>(::GetProcAddress(g_hWndCompat, "WndCompat_BeginShutdown"));
            if (pBegin)
                pBegin();
        }
    }

    void UnloadGlassCoreModule() noexcept
    {
        if (!g_hGlassCore)
            return;
        g_pfnGlassCoreApplyDropDownShow = nullptr;
        g_pfnGlassCoreReloadRenderTextInk = nullptr;
        ::FreeLibrary(g_hGlassCore);
        g_hGlassCore = nullptr;
    }

    void UnloadWndCompatModule() noexcept
    {
        if (!g_hWndCompat)
            return;
        g_pfnWndCompatHandleWinEvent = nullptr;
        ::FreeLibrary(g_hWndCompat);
        g_hWndCompat = nullptr;
    }
}

namespace LogViewerGlass
{
    bool LoadGlassCoreDll() noexcept
    {
        if (g_hGlassCore)
            return true;

        const std::wstring path = DllPathNextToExe(L"GlassCore.dll");
        g_hGlassCore = ::LoadLibraryW(path.c_str());
        if (!g_hGlassCore)
            g_hGlassCore = ::LoadLibraryW(L"GlassCore.dll");

        if (g_hGlassCore)
        {
            using PFN = void(WINAPI*)();
            const auto pEnsure = reinterpret_cast<PFN>(
                ::GetProcAddress(g_hGlassCore, "GlassCore_LogViewerEnsureEngine"));
            if (pEnsure)
                pEnsure();
            g_pfnGlassCoreApplyDropDownShow =
                reinterpret_cast<LogViewerGlass::GlassCoreApplyDropDownShowFn>(
                    ::GetProcAddress(g_hGlassCore, "GlassCore_ApplyDropDownShow"));
            g_pfnGlassCoreReloadRenderTextInk =
                reinterpret_cast<GlassCoreReloadRenderTextInkFn>(
                    ::GetProcAddress(g_hGlassCore, "GlassCore_ReloadRenderTextInkFromRegistry"));
        }
        return g_hGlassCore != nullptr;
    }

    bool EnsureWndCompatRuntime() noexcept
    {
        if (!LoadGlassCoreDll())
            return false;

        using PFNSetInjectionMode = void(WINAPI*)(DWORD);
        if (const auto pSetMode = reinterpret_cast<PFNSetInjectionMode>(
                ::GetProcAddress(g_hGlassCore, "GlassCore_SetInjectionMode")))
        {
            pSetMode(static_cast<DWORD>(Shared::InjectionMode::Full));
        }

        if (g_hWndCompat && g_pfnWndCompatHandleWinEvent)
            return true;

        const std::wstring path = DllPathNextToExe(L"WndCompat.dll");
        g_hWndCompat = ::LoadLibraryW(path.c_str());
        if (!g_hWndCompat)
            g_hWndCompat = ::LoadLibraryW(L"WndCompat.dll");
        if (!g_hWndCompat)
            return false;

        g_pfnWndCompatHandleWinEvent = reinterpret_cast<WndCompatHandleWinEventFn>(
            ::GetProcAddress(g_hWndCompat, "WndCompat_HandleWinEvent"));

        using PFNInit = BOOL(WINAPI*)(DWORD);
        const auto pInit = reinterpret_cast<PFNInit>(::GetProcAddress(g_hWndCompat, "WndCompat_Initialize"));
        if (pInit)
            (void)pInit(static_cast<DWORD>(Shared::InjectionMode::Full));

        return g_pfnWndCompatHandleWinEvent != nullptr;
    }

    bool IsWndCompatLoaded() noexcept
    {
        return g_hWndCompat != nullptr;
    }

    WndCompatHandleWinEventFn GetWndCompatHandleWinEvent() noexcept
    {
        return g_pfnWndCompatHandleWinEvent;
    }

    void ShutdownGlassCoreDll() noexcept
    {
        RunExecuteShutdownBeforeFreeLibrary();
        UnloadGlassCoreModule();
        UnloadWndCompatModule();
    }

    bool IsGlassCoreLoaded() noexcept
    {
        return g_hGlassCore != nullptr;
    }

    HMODULE GetGlassCoreModule() noexcept
    {
        return g_hGlassCore;
    }

    GlassCoreApplyDropDownShowFn GetGlassCoreApplyDropDownShow() noexcept
    {
        return g_pfnGlassCoreApplyDropDownShow;
    }

    void ReloadRenderTextInkFromRegistryThrottled(DWORD minIntervalMs) noexcept
    {
        if (!g_pfnGlassCoreReloadRenderTextInk)
            return;
        static DWORD s_lastTick = 0;
        const DWORD now = ::GetTickCount();
        if (s_lastTick != 0 && (now - s_lastTick) < minIntervalMs)
            return;
        s_lastTick = now;
        g_pfnGlassCoreReloadRenderTextInk();
    }
}
