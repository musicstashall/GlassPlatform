// IAT-хуки UIRibbon.dll для KbxLabel (BWM KbxLabelHooks).

#include "CorePch.h"

#include <Hooks/KbxLabelHooks.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/RenderText.h>
#include <Subsystems/TooltipContext.h>
#include <wil.h>
#include <unordered_map>
#include <string_view>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::KbxLabelHooks
{
    static std::atomic<bool> g_shutdown{ false };

    int WINAPI Overall_MyDrawTextW(HDC, LPCWSTR, int, LPRECT, UINT);
    BOOL WINAPI Overall_MyGdiGradientFill(HDC, PTRIVERTEX, ULONG, PVOID, ULONG, ULONG);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTEXTW = 0,
        OHK_GDIGRADIENTFILL,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        std::tuple{ std::array{ "user32.dll"sv, "ext-ms-win-ntuser-draw-l1-1-0.dll"sv, "ext-ms-win-ntuser-misc-l1-1-0.dll"sv },
            "DrawTextW", reinterpret_cast<PVOID>(Overall_MyDrawTextW) },
        std::tuple{ std::array{ "gdi32.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "GdiGradientFill", reinterpret_cast<PVOID>(Overall_MyGdiGradientFill) },
    } };

    using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;

    static std::unordered_map<void*, OverallDispatcher> g_overallMap;
    static wil::srwlock g_overallLock;

    static bool EnableHooksOnModule(void* baseAddress, size_t index, bool enable)
    {
        if (!baseAddress) return false;

        auto lock = g_overallLock.lock_exclusive();

        auto it = g_overallMap.find(baseAddress);
        if (it == g_overallMap.end())
        {
            auto [inserted, ok] = g_overallMap.emplace(
                std::piecewise_construct,
                std::forward_as_tuple(baseAddress),
                std::forward_as_tuple(g_overallHookDep, baseAddress));
            it = inserted;
            it->second.CacheHookData();
        }
        else
        {
            it->second.moduleAddress = baseAddress;
        }

        it->second.EnableHook(index, enable);
        return true;
    }

    static void DisableAllOverallHooks()
    {
        auto lock = g_overallLock.lock_exclusive();
        for (auto& [addr, dispatcher] : g_overallMap)
            dispatcher.DisableAllHooks();
        g_overallMap.clear();
    }

    static HMODULE GetCallerModule(void* returnAddress)
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(returnAddress),
            &hMod);
        return hMod;
    }

    template <size_t index, typename T>
    static T GetOverallOrigSafe(HMODULE callerModule)
    {
        auto lock = g_overallLock.lock_shared();
        auto it = g_overallMap.find(static_cast<void*>(callerModule));
        if (it != g_overallMap.end())
            return it->second.template GetOrg<index, T>();
        return nullptr;
    }

    int WINAPI Overall_MyDrawTextW(
        HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTEXTW, decltype(&DrawTextW)>(callerMod);

        if (!orig)
            return DrawTextW(hdc, lpchText, cchText, lprc, format);

        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hdc, lpchText, cchText, lprc, format);

        if ((format & DT_CALCRECT) || (format & DT_INTERNAL) || (format & DT_NOCLIP))
            return orig(hdc, lpchText, cchText, lprc, format);

        auto& ctx = Tooltip::Detail::Context();
        const COLORREF oldColor = SetTextColor(hdc, ctx.rendering.color);
        auto restoreColor = wil::scope_exit([&] { SetTextColor(hdc, oldColor); });

        int result = 0;
        if (FAILED(GlassCore::Text::DrawTextWithAlpha(hdc, lpchText, cchText, lprc, format, result)))
            return orig(hdc, lpchText, cchText, lprc, format);

        return result;
    }

    BOOL WINAPI Overall_MyGdiGradientFill(
        HDC hdc, PTRIVERTEX pVertex, ULONG nVertex, PVOID pMesh, ULONG nCount, ULONG ulMode)
    {
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_GDIGRADIENTFILL, decltype(&GdiGradientFill)>(callerMod);

        if (!orig)
            return GdiGradientFill(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

        if (g_shutdown.load(std::memory_order_acquire))
            return orig(hdc, pVertex, nVertex, pMesh, nCount, ulMode);

        RECT paintRect{};
        GetClipBox(hdc, &paintRect);
        const int w = paintRect.right - paintRect.left;
        const int h = paintRect.bottom - paintRect.top;
        if (w > 0 && h > 0)
            PatBlt(hdc, paintRect.left, paintRect.top, w, h, BLACKNESS);
        return TRUE;
    }

    void Prepare() {}

    void Startup()
    {
        g_shutdown.store(false, std::memory_order_release);

        HMODULE ribbon = GetModuleHandleW(L"UIRibbon.dll");
        if (!ribbon)
            return;

        auto lock = g_overallLock.lock_exclusive();
        void* mod = ribbon;
        auto [it, inserted] = g_overallMap.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(mod),
            std::forward_as_tuple(g_overallHookDep, mod));
        if (inserted)
        {
            it->second.CacheHookData();
            DebugLog(DBG_NORMAL, L"[GlassCore][KbxLabelHooks] Startup: cached UIRibbon 0x%p", mod);
        }
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
    }

    void EnablePaintHooks(bool enable)
    {
        if (g_shutdown.load(std::memory_order_acquire))
            return;

        HMODULE ribbon = GetModuleHandleW(L"UIRibbon.dll");
        if (!ribbon)
            return;

        EnableHooksOnModule(ribbon, OHK_DRAWTEXTW, enable);
        EnableHooksOnModule(ribbon, OHK_GDIGRADIENTFILL, enable);
    }
}
