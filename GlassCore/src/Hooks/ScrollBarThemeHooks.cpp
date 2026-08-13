#include "CorePch.h"

#include <Hooks/DirectUI.h>
#include <Hooks/ScrollBar.h>
#include <Hooks/ScrollBarDrawTheme.h>
#include <Hooks/ScrollBarTheme.h>
#include <Module/ModuleHandler.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Core/Intercept/HookHelper.h>
#include <Helpers/ThemeHelper.h>
#include <DebugLog.h>
#include <GlassBackdropMark.h>
#include <GlassComctlScrollHost.h>
#include <GlassControlKind.h>

#include <wil/resource.h>
#include <detours.h>
#include <intrin.h>
#include <unordered_map>

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::ScrollBarTheme
{
    static thread_local int s_tlScrollBarDrawThemeOrigDepth{};

    bool IsInsideScrollBarDrawThemeOrig() noexcept
    {
        return s_tlScrollBarDrawThemeOrigDepth > 0;
    }

    DrawThemeOrigCallDepth::DrawThemeOrigCallDepth() noexcept
    {
        ++s_tlScrollBarDrawThemeOrigDepth;
    }

    DrawThemeOrigCallDepth::~DrawThemeOrigCallDepth() noexcept
    {
        --s_tlScrollBarDrawThemeOrigDepth;
    }

    // ── comctl32 ScrollBar IAT (DrawThemeBackground + BitBlt) ─────────────────

    static std::atomic<bool> s_comctlScrollBarShutdown{ false };

    HRESULT WINAPI ComctlScrollBarMyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect);

    enum ComctlScrollBarIATIndex : size_t
    {
        OHK_CSB_DTB = 0,
        OHK_CSB_BITBLT,
        OHK_CSB_COUNT
    };

    inline const HookDispatcherDependency<OHK_CSB_COUNT, 4> g_comctlScrollBarHookDep
    { {
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(ComctlScrollBarMyDrawThemeBackground) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv, ""sv },
            "BitBlt", reinterpret_cast<PVOID>(ComctlScrollBarMyBitBlt) },
    } };

    using ComctlScrollBarDispatcher = HookDispatcher<OHK_CSB_COUNT, 4>;

    static wil::srwlock g_comctlScrollBarMapLock;
    static std::unordered_map<void*, ComctlScrollBarDispatcher> g_comctlScrollBarMap;

    [[nodiscard]] static decltype(&DrawThemeBackground) GetOrigComctlScrollBarDrawTheme(HMODULE callerMod) noexcept
    {
        auto lock = g_comctlScrollBarMapLock.lock_shared();
        auto it = g_comctlScrollBarMap.find(static_cast<void*>(callerMod));
        if (it != g_comctlScrollBarMap.end())
            return it->second.GetOrg<OHK_CSB_DTB, decltype(&DrawThemeBackground)>();
        return nullptr;
    }

    decltype(&DrawThemeBackground) GetComctlScrollBarDrawThemeOrig(HMODULE comctl32Module) noexcept
    {
        return GetOrigComctlScrollBarDrawTheme(comctl32Module);
    }

    HRESULT WINAPI ComctlScrollBarMyDrawThemeBackground(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId,
        LPCRECT pRect, LPCRECT pClipRect)
    {
        HMODULE callerMod = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(_ReturnAddress()), &callerMod);
        auto orig = GetOrigComctlScrollBarDrawTheme(callerMod);
        if (!orig)
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        if (s_comctlScrollBarShutdown.load(std::memory_order_acquire)
            || GlassCore::DirectUI::IsShuttingDown())
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        WCHAR themeClass[MAX_PATH + 1]{};
        if (FAILED(Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH))
            || _wcsicmp(themeClass, L"ScrollBar") != 0)
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        const HWND hWnd = WindowFromDC(hdc);
        if (hWnd && !GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);

        GlassCore::ScrollBarDrawTheme::LogScrollBarDrawThemeCall(
            L"[GlassCore][ScrollBarTheme]",
            iPartId,
            iStateId);

        RECT r{};
        if (!GlassCore::ScrollBarDrawTheme::TryGetScrollBarDrawRect(pRect, pClipRect, r))
            return S_OK;

        if (GlassCore::ScrollBarDrawTheme::IsScrollBarTrackBackgroundPart(iPartId))
        {
            PatBlt(hdc, r.left, r.top, r.right - r.left, r.bottom - r.top, BLACKNESS);
            return S_OK;
        }

        if (iPartId == GlassCore::ScrollBarDrawTheme::kScrollBarPartSizeBox)
            return GlassCore::ScrollBarDrawTheme::DrawScrollBarSizeBoxBlackThenOrig(
                orig, hTheme, hdc, iPartId, iStateId, r);

        if (GlassCore::ScrollBarDrawTheme::IsScrollBarInteractivePart(iPartId)
            && GlassCore::ScrollBarDrawTheme::TryDrawScrollBarInteractiveGlass(
                orig, hTheme, hdc, iPartId, iStateId, r))
            return S_OK;

        const DrawThemeOrigCallDepth depth;
        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    BOOL WINAPI ComctlScrollBarMyBitBlt(
        HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
    {
        auto orig = GetComctlScrollBarBitBltOrig(_ReturnAddress());
        if (!orig)
            return BitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        if (s_comctlScrollBarShutdown.load(std::memory_order_acquire)
            || GlassCore::DirectUI::IsShuttingDown())
            return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

        DebugLog(
            DBG_NORMAL,
            L"[GlassCore][ScrollBarTheme] BitBlt(comctl32 SB) hdc=%p dst=(%d,%d) %dx%d hdcSrc=%p src=(%d,%d) rop=0x%08X",
            hdc,
            x,
            y,
            cx,
            cy,
            hdcSrc,
            x1,
            y1,
            rop);

        return orig(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
    }

    [[nodiscard]] static bool ModuleImageIsComctl32(HMODULE hModule) noexcept
    {
        if (!hModule)
            return false;
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(hModule, path, static_cast<int>(std::size(path))) == 0)
            return false;
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        return _wcsicmp(base, L"comctl32.dll") == 0;
    }

    // GetDCEx из цепочки темизации: возврат в uxtheme, а не в comctl32 — см. HookGetDCEx + Ensure.
    [[nodiscard]] static bool ModuleImageIsUxTheme(HMODULE hModule) noexcept
    {
        if (!hModule)
            return false;
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(hModule, path, static_cast<int>(std::size(path))) == 0)
            return false;
        const wchar_t* base = wcsrchr(path, L'\\');
        base = base ? base + 1 : path;
        return _wcsicmp(base, L"uxtheme.dll") == 0
            || _wcsicmp(base, L"ext-ms-win-uxtheme-themes-l1-1-0.dll") == 0;
    }

    static void InstallComctlScrollBarIATForModule(HMODULE hModule)
    {
        if (!hModule)
            return;

        {
            auto lock = g_comctlScrollBarMapLock.lock_shared();
            if (g_comctlScrollBarMap.count(static_cast<void*>(hModule)))
                return;
        }

        auto lock = g_comctlScrollBarMapLock.lock_exclusive();
        if (g_comctlScrollBarMap.count(static_cast<void*>(hModule)))
            return;

        auto [it, ok] = g_comctlScrollBarMap.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(static_cast<void*>(hModule)),
            std::forward_as_tuple(g_comctlScrollBarHookDep, static_cast<void*>(hModule)));
        if (!ok)
            return;

        it->second.CacheHookData();
        it->second.EnableHook(OHK_CSB_DTB, true);
        if (!it->second.EnableHook(OHK_CSB_BITBLT, true))
        {
            DebugLog(
                DBG_WARNING,
                L"[GlassCore][ScrollBarTheme] comctl32 BitBlt IAT не найден module=%p",
                hModule);
        }
        DebugLog(DBG_IMPORTANT, L"[GlassCore][ScrollBarTheme] comctl32 ScrollBar IAT installed module=%p", hModule);
    }

    static void InstallComctlScrollBarOnKnownComctlInstances() noexcept
    {
        GlassCore::ModuleHandler::RunInActCtx([&] {
            HMODULE m = GlassCore::ModuleHandler::g_comctl32New;
            if (!m)
                m = GetModuleHandleW(L"comctl32.dll");
            InstallComctlScrollBarIATForModule(m);
        });
        InstallComctlScrollBarIATForModule(GlassCore::ModuleHandler::g_comctl32Legacy);
    }

    static void UninstallComctlScrollBarIATForModule(HMODULE hModule)
    {
        if (!hModule)
            return;

        auto lock = g_comctlScrollBarMapLock.lock_exclusive();
        auto cit = g_comctlScrollBarMap.find(static_cast<void*>(hModule));
        if (cit == g_comctlScrollBarMap.end())
            return;

        cit->second.DisableAllHooks();
        g_comctlScrollBarMap.erase(cit);
        DebugLog(DBG_IMPORTANT, L"[GlassCore][ScrollBarTheme] comctl32 ScrollBar IAT removed module=%p", hModule);
    }

    bool ShouldInstallScrollBarIatForHwnd(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        if (GlassShared::GlassIsComctlScrollHost(hWnd))
            return true;

        using GK = GlassShared::GlassControlKind;
        switch (GlassShared::GlassGetWindowKind(hWnd))
        {
        case GK::ListView:
        case GK::TreeView:
        case GK::Edit:
        case GK::WinForms_Edit:
        case GK::RichEdit20W:
        case GK::WinForms_RichEdit20W:
        case GK::RichEdit50W:
        case GK::WinForms_RichEdit50W:
        case GK::ComboLBox:
        case GK::ComboBox:
        case GK::DirectUI:
        case GK::NetUI:
        case GK::NetUiToolWindow:
        case GK::WinForms_Window:
        case GK::WinForms_ListView:
        case GK::ScrollBar:
        case GK::MonthCal:
        case GK::TabControl:
        case GK::Header:
        case GK::Rebar:
        case GK::Toolbar:
        case GK::InternetExplorer_Server:
            return true;
        default:
            return false;
        }
    }

    void PrepareComctlScrollBarIat() noexcept {}

    void EnsureComctlScrollBarIatIfComctlCaller(HWND hWnd, HMODULE callerModule) noexcept
    {
        if (s_comctlScrollBarShutdown.load(std::memory_order_acquire)
            || GlassCore::DirectUI::IsShuttingDown()
            || !callerModule)
            return;
        if (!ModuleImageIsComctl32(callerModule) && !ModuleImageIsUxTheme(callerModule))
            return;
        if (!ShouldInstallScrollBarIatForHwnd(hWnd))
            return;
        if (!GlassShared::GlassIsBackdropAllowedForHwnd(hWnd))
            return;
        if (ModuleImageIsComctl32(callerModule))
            InstallComctlScrollBarIATForModule(callerModule);
        else
            InstallComctlScrollBarOnKnownComctlInstances();
    }

    void OnComctl32ModuleUnload(HMODULE hModule) noexcept
    {
        if (s_comctlScrollBarShutdown.load(std::memory_order_acquire) || !hModule)
            return;
        UninstallComctlScrollBarIATForModule(hModule);
    }

    void ShutdownComctlScrollBarIat() noexcept
    {
        s_comctlScrollBarShutdown.store(true, std::memory_order_release);
        auto lock = g_comctlScrollBarMapLock.lock_exclusive();
        for (auto& [mod, d] : g_comctlScrollBarMap)
            d.DisableAllHooks();
        g_comctlScrollBarMap.clear();
        s_comctlScrollBarShutdown.store(false, std::memory_order_release);
    }

    decltype(&BitBlt) GetComctlScrollBarBitBltOrig(void* returnAddress) noexcept
    {
        void* stack[48]{};
        const USHORT n = CaptureStackBackTrace(
            0,
            static_cast<DWORD>(std::size(stack)),
            stack,
            nullptr);

        auto lock = g_comctlScrollBarMapLock.lock_shared();

        for (USHORT i = 0; i < n; ++i)
        {
            HMODULE frameMod = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCWSTR>(stack[i]),
                    &frameMod))
                continue;

            if (auto it = g_comctlScrollBarMap.find(static_cast<void*>(frameMod));
                it != g_comctlScrollBarMap.end())
                return it->second.GetOrg<OHK_CSB_BITBLT, decltype(&BitBlt)>();
        }

        HMODULE raMod = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(returnAddress),
                &raMod))
        {
            if (auto it = g_comctlScrollBarMap.find(static_cast<void*>(raMod));
                it != g_comctlScrollBarMap.end())
                return it->second.GetOrg<OHK_CSB_BITBLT, decltype(&BitBlt)>();
        }

        return nullptr;
    }
} // namespace GlassCore::ScrollBarTheme

namespace GlassCore::ScrollBar
{
    void Prepare() noexcept
    {
        GlassCore::ScrollBarTheme::PrepareComctlScrollBarIat();
    }

    void Startup() noexcept
    {
        DebugLog(DBG_NORMAL, L"[GlassCore][ScrollBar] Startup");
    }

    void Shutdown() noexcept
    {
        GlassCore::ScrollBarTheme::ShutdownComctlScrollBarIat();
        DebugLog(DBG_NORMAL, L"[GlassCore][ScrollBar] Shutdown");
    }
}
