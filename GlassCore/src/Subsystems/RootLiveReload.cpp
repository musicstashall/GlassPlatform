// Live-reload Root backdrop / DrawTheme по реестру g_rootWindows (без EnumWindows / тяжёлого reapply).

#include "CorePch.h"
#include <Subsystems/RootLiveReload.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Subsystems/TextGlowClientRegistry.h>
#include <Subsystems/TextInkClientRegistry.h>
#include <GlassMessages.h>
#include <functional>
#include <vector>

namespace GlassCore::RootLiveReload
{
    namespace
    {
        using CopyTrackedRootsFn = void(WINAPI*)(HWND*, DWORD*) noexcept;
        using PostLiveBackdropFn = void(WINAPI*)(DWORD) noexcept;
        using PostLiveColorizationFn = void(WINAPI*)(LPARAM) noexcept;

        [[nodiscard]] HMODULE GetWndCompatModule() noexcept
        {
            HMODULE h = GetModuleHandleW(L"WndCompat.dll");
            if (h)
                return h;
            return LoadLibraryW(L"WndCompat.dll");
        }

        void ForEachTrackedRoot(const std::function<void(HWND)>& action) noexcept
        {
            const HMODULE hWndCompat = GetWndCompatModule();
            if (!hWndCompat)
                return;

            const auto pfnCopy = reinterpret_cast<CopyTrackedRootsFn>(
                GetProcAddress(hWndCompat, "WndCompat_CopyTrackedRoots"));
            if (!pfnCopy)
                return;

            DWORD count = 0;
            pfnCopy(nullptr, &count);
            if (count == 0)
                return;

            std::vector<HWND> roots(count);
            pfnCopy(roots.data(), &count);
            roots.resize(count);

            for (HWND hRoot : roots)
            {
                if (!hRoot || !IsWindow(hRoot))
                    continue;
                action(hRoot);
            }
        }
    }

    void PostLiveRootBackdropReapply(DWORD scope) noexcept
    {
        const HMODULE hWndCompat = GetWndCompatModule();
        if (!hWndCompat)
            return;

        const auto pfn = reinterpret_cast<PostLiveBackdropFn>(
            GetProcAddress(hWndCompat, "WndCompat_PostLiveRootBackdropReapply"));
        if (pfn)
            pfn(scope);
    }

    void PostLiveRootColorizationToAppliedRoots(const LPARAM liveRootScope) noexcept
    {
        const HMODULE hWndCompat = GetWndCompatModule();
        if (!hWndCompat)
            return;

        const auto pfn = reinterpret_cast<PostLiveColorizationFn>(
            GetProcAddress(hWndCompat, "WndCompat_PostLiveRootColorizationToAppliedRoots"));
        if (pfn)
            pfn(liveRootScope);
    }

    void InvalidateThemedChromeOnTrackedRoots() noexcept
    {
        GlassCore::ThemePaint::ReloadThemePaintSettingsFromRegistry();

        ForEachTrackedRoot([](HWND hRoot) noexcept {
            const DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);
            if (wndTid == GetCurrentThreadId())
            {
                GlassCore::ThemePaint::RunDeferredThemedChromeSoftInvalidate(hRoot);
                return;
            }

            (void)::PostMessageW(
                hRoot,
                WndCompat::WM_GLASS_DEFER_THEMED_CHROME_INVALIDATE,
                0,
                0);
        });
    }

    void InvalidateTextGlowClientsOnTrackedRoots() noexcept
    {
        ForEachTrackedRoot([](HWND hRoot) noexcept {
            const DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);
            if (wndTid == GetCurrentThreadId())
            {
                GlassCore::TextGlow::RunDeferredTextGlowSoftInvalidate(hRoot);
                return;
            }

            (void)::PostMessageW(
                hRoot,
                WndCompat::WM_GLASS_DEFER_TEXT_GLOW_INVALIDATE,
                0,
                0);
        });
    }

    void InvalidateTextInkClientsOnTrackedRoots() noexcept
    {
        ForEachTrackedRoot([](HWND hRoot) noexcept {
            const DWORD wndTid = GetWindowThreadProcessId(hRoot, nullptr);
            if (wndTid == GetCurrentThreadId())
            {
                GlassCore::TextInk::RunDeferredTextInkSoftInvalidate(hRoot);
                return;
            }

            (void)::PostMessageW(
                hRoot,
                WndCompat::WM_GLASS_DEFER_TEXT_INK_INVALIDATE,
                0,
                0);
        });
    }
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_PostLiveRootBackdropReapply(DWORD scope) noexcept
{
    GlassCore::RootLiveReload::PostLiveRootBackdropReapply(scope);
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_InvalidateThemedChromeOnTrackedRoots() noexcept
{
    GlassCore::RootLiveReload::InvalidateThemedChromeOnTrackedRoots();
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_InvalidateTextGlowClientsOnTrackedRoots() noexcept
{
    GlassCore::RootLiveReload::InvalidateTextGlowClientsOnTrackedRoots();
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_InvalidateTextInkClientsOnTrackedRoots() noexcept
{
    GlassCore::RootLiveReload::InvalidateTextInkClientsOnTrackedRoots();
}

extern "C" __declspec(dllexport) void WINAPI GlassCore_PostLiveRootColorizationToAppliedRoots(
    const LPARAM liveRootScope) noexcept
{
    GlassCore::RootLiveReload::PostLiveRootColorizationToAppliedRoots(liveRootScope);
}
