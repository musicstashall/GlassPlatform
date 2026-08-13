// msctls_updown32 - themed: DrawThemeBackground Spin via IAT comctl32.
// ExtTextOutW (ETO_OPAQUE) via IAT uxtheme.dll.
// PatBlt(BLACKNESS) + DrawThemeBackgroundWithConstantAlpha (like push Button).
// Session: PaintDispatcher BeginPaint/EndPaint + GetDC/ReleaseDC.

#include "CorePch.h"

#include <Hooks/UpDown.h>
#include <Subsystems/ThemePaintRegistry.h>
#include <Core/Intercept/HookDispatcher.h>
#include <Module/ModuleHandler.h>
#include <Subsystems/PaintDispatcher.h>
#include <Helpers/ThemeHelper.h>
#include <GlassControlKind.h>
#include <WinApiUtils.h>
#include <wil.h>
#include <uxtheme.h>
#include <DebugLog.h>

#pragma comment(lib, "msimg32.lib")

using namespace GlassCore::Hooks;
using namespace std::literals;

namespace GlassCore::UpDownControl
{
    struct UpDownPaintSession
    {
        HMODULE mod{};
        HWND    hWnd{};
    };

    thread_local std::vector<UpDownPaintSession> g_upDownSessionStack;

    static std::atomic<bool>     g_shutdown{ false };
    static std::atomic<uint32_t> g_dtbSeq{ 0 };

    HRESULT WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme, HDC hdc, int iPartId, int iStateId, LPCRECT pRect, LPCRECT pClipRect);

    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx);

    enum OverallHookIndex : size_t
    {
        OHK_DRAWTHEMEBG = 0,
        OHK_EXTTEXTOUT = 1,
        OHK_COUNT
    };

    inline const HookDispatcherDependency<OHK_COUNT, 3> g_overallHookDep
    { {
        std::tuple{ std::array{ "uxtheme.dll"sv, "ext-ms-win-uxtheme-themes-l1-1-0.dll"sv, ""sv },
            "DrawThemeBackground", reinterpret_cast<PVOID>(Overall_MyDrawThemeBg) },
        std::tuple{ std::array{ "gdi32.dll"sv, "gdi32full.dll"sv, "ext-ms-win-gdi-desktop-l1-1-0.dll"sv },
            "ExtTextOutW", reinterpret_cast<PVOID>(Overall_MyExtTextOutW) },
    } };

    using OverallDispatcher = HookDispatcher<OHK_COUNT, 3>;

    static std::unordered_map<void*, OverallDispatcher> g_overallMap;
    static wil::srwlock g_overallLock;

    static bool StrEq(LPCWSTR a, LPCWSTR b) noexcept
    {
        return a && b && _wcsicmp(a, b) == 0;
    }

    static HMODULE GetCallerModule(void* ra) noexcept
    {
        HMODULE hMod = nullptr;
        GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(ra),
            &hMod);
        return hMod;
    }

    static bool EnableHooksOnModule(void* baseAddress, size_t index, bool enable)
    {
        if (!baseAddress)
            return false;

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
            it->second.moduleAddress = baseAddress;

        it->second.EnableHook(index, enable);
        return true;
    }

    static void DisableAllOverallHooks()
    {
        auto lock = g_overallLock.lock_exclusive();
        for (auto& [addr, d] : g_overallMap)
            d.DisableAllHooks();
        g_overallMap.clear();
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

    [[nodiscard]] static HMODULE ResolveUpDownComctlHookModule(HMODULE callerMod) noexcept
    {
        auto* g_new = GlassCore::ModuleHandler::g_comctl32New;
        auto* g_legacy = GlassCore::ModuleHandler::g_comctl32Legacy;
        if (callerMod == g_new || callerMod == g_legacy)
            return callerMod;
        return g_new ? g_new : g_legacy;
    }

    static void EnableUpDownUxThemeExtTextOut(bool enable) noexcept
    {
        if (HMODULE const ux = GetModuleHandleW(L"uxtheme.dll"))
            EnableHooksOnModule(ux, OHK_EXTTEXTOUT, enable);
        if (HMODULE const ext = GetModuleHandleW(L"ext-ms-win-uxtheme-themes-l1-1-0.dll"))
            EnableHooksOnModule(ext, OHK_EXTTEXTOUT, enable);
    }

    [[nodiscard]] static bool TryGetThemeDrawRect(
        LPCRECT pRect, LPCRECT pClipRect, RECT& out) noexcept
    {
        if (pRect && !IsRectEmpty(pRect))
        {
            out = *pRect;
            if (pClipRect)
            {
                RECT inter{};
                if (!IntersectRect(&inter, &out, pClipRect))
                    return false;
                out = inter;
            }
            return !IsRectEmpty(&out);
        }
        if (pClipRect && !IsRectEmpty(pClipRect))
        {
            out = *pClipRect;
            return true;
        }
        return false;
    }

    static void EnableUpDownHooksForModule(HWND hWnd, HMODULE callerMod, bool enable) noexcept
    {
        using namespace GlassShared;
        if (!hWnd || !IsWindow(hWnd))
            return;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::UpDown)
            return;
        if (!Shared::WinApiUtils::IsThemed(hWnd, L"Spin"))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UpDown] EnableHooks: skip reason=noWindowTheme hWnd=0x%p enable=%d",
                hWnd,
                enable ? 1 : 0);
            return;
        }

        HMODULE const comctlMod = ResolveUpDownComctlHookModule(callerMod);
        if (!comctlMod)
        {
            DebugLog(DBG_WARNING,
                L"[GlassCore][UpDown] EnableHooks: skip reason=noComctlMod hWnd=0x%p callerMod=0x%p enable=%d",
                hWnd,
                callerMod,
                enable ? 1 : 0);
            return;
        }

        EnableHooksOnModule(static_cast<void*>(comctlMod), OHK_DRAWTHEMEBG, enable);
        EnableUpDownUxThemeExtTextOut(enable);

        int dtbRef = 0;
        int etoRef = 0;
        {
            auto lock = g_overallLock.lock_shared();
            const auto it = g_overallMap.find(static_cast<void*>(comctlMod));
            if (it != g_overallMap.end())
                dtbRef = it->second.GetRef(OHK_DRAWTHEMEBG);
            if (HMODULE const ux = GetModuleHandleW(L"uxtheme.dll"))
            {
                const auto uxIt = g_overallMap.find(static_cast<void*>(ux));
                if (uxIt != g_overallMap.end())
                    etoRef = uxIt->second.GetRef(OHK_EXTTEXTOUT);
            }
        }

        DebugLog(DBG_NORMAL,
            L"[GlassCore][UpDown] EnableHooks: enable=%d hWnd=0x%p callerMod=0x%p comctlMod=0x%p DTB_ref=%d ETO_ref=%d",
            enable ? 1 : 0,
            hWnd,
            callerMod,
            comctlMod,
            dtbRef,
            etoRef);
    }

    BOOL WINAPI Overall_MyExtTextOutW(
        HDC hdc, int x, int y, UINT options, const RECT* lprect,
        LPCWSTR lpString, UINT c, const INT* lpDx)
    {
        HMODULE const callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_EXTTEXTOUT, decltype(&ExtTextOutW)>(callerMod);

        if (!orig)
            return ExtTextOutW(hdc, x, y, options, lprect, lpString, c, lpDx);

        if (g_upDownSessionStack.empty())
            return orig(hdc, x, y, options, lprect, lpString, c, lpDx);

        if ((options & ETO_OPAQUE) && lprect)
        {
            PatBlt(hdc, lprect->left, lprect->top,
                lprect->right - lprect->left, lprect->bottom - lprect->top,
                BLACKNESS);
            return TRUE;
        }

        return orig(hdc, x, y, options, lprect, lpString, c, lpDx);
    }

    HRESULT WINAPI Overall_MyDrawThemeBg(
        HTHEME hTheme,
        HDC hdc,
        int iPartId,
        int iStateId,
        LPCRECT pRect,
        LPCRECT pClipRect)
    {
        const uint32_t seq = g_dtbSeq.fetch_add(1, std::memory_order_relaxed) + 1;
        HMODULE callerMod = GetCallerModule(_ReturnAddress());
        auto orig = GetOverallOrigSafe<OHK_DRAWTHEMEBG, decltype(&DrawThemeBackground)>(callerMod);

        const size_t sessDepth = g_upDownSessionStack.size();
        const int rectL = pRect ? pRect->left : 0;
        const int rectT = pRect ? pRect->top : 0;
        const int rectR = pRect ? pRect->right : 0;
        const int rectB = pRect ? pRect->bottom : 0;

        DebugLog(DBG_NORMAL,
            L"[GlassCore][UpDown] DTB: seq=%u entry caller=%p hdc=%p hTheme=%p part=%d state=%d "
            L"rect=[%d,%d,%d,%d] sessDepth=%zu hasOrig=%d",
            seq,
            callerMod,
            hdc,
            hTheme,
            iPartId,
            iStateId,
            rectL,
            rectT,
            rectR,
            rectB,
            sessDepth,
            orig ? 1 : 0);

        if (!orig)
        {
            DebugLog(DBG_WARNING,
                L"[GlassCore][UpDown] DTB: seq=%u branch=NoOrig caller=%p part=%d state=%d",
                seq,
                callerMod,
                iPartId,
                iStateId);
            return DrawThemeBackground(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        if (g_shutdown.load(std::memory_order_acquire))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UpDown] DTB: seq=%u branch=ShutdownOrig caller=%p part=%d state=%d",
                seq,
                callerMod,
                iPartId,
                iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        if (g_upDownSessionStack.empty())
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UpDown] DTB: seq=%u branch=NoSessionOrig caller=%p part=%d state=%d",
                seq,
                callerMod,
                iPartId,
                iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        if (!hTheme)
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UpDown] DTB: seq=%u branch=NullThemeOrig caller=%p part=%d state=%d",
                seq,
                callerMod,
                iPartId,
                iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        WCHAR themeClass[MAX_PATH + 1]{};
        const HRESULT hrThemeClass =
            Shared::ThemeHelper::GetThemeClass(hTheme, themeClass, MAX_PATH);
        if (FAILED(hrThemeClass))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UpDown] DTB: seq=%u branch=GetThemeClassFailedOrig hr=0x%08X caller=%p part=%d state=%d",
                seq,
                static_cast<unsigned>(hrThemeClass),
                callerMod,
                iPartId,
                iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        if (!StrEq(themeClass, L"Spin"))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UpDown] DTB: seq=%u branch=NotSpinOrig theme=%s caller=%p part=%d state=%d",
                seq,
                themeClass,
                callerMod,
                iPartId,
                iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        RECT drawRect{};
        if (!TryGetThemeDrawRect(pRect, pClipRect, drawRect))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UpDown] DTB: seq=%u branch=EmptyRectOrig theme=Spin caller=%p part=%d state=%d",
                seq,
                callerMod,
                iPartId,
                iStateId);
            return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
        }

        const UpDownPaintSession& sess = g_upDownSessionStack.back();
        const HWND hostHwnd = sess.hWnd ? sess.hWnd : WindowFromDC(hdc);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][UpDown] DTB: seq=%u branch=AlphaPatBlt hostHwnd=%p caller=%p part=%d state=%d "
            L"drawRect=[%d,%d,%d,%d]",
            seq,
            hostHwnd,
            callerMod,
            iPartId,
            iStateId,
            drawRect.left,
            drawRect.top,
            drawRect.right,
            drawRect.bottom);

        PatBlt(hdc,
            drawRect.left,
            drawRect.top,
            drawRect.right - drawRect.left,
            drawRect.bottom - drawRect.top,
            BLACKNESS);

        const BYTE effAlpha =
            GlassCore::ThemePaint::GetEffectiveDrawThemeConstantAlphaForSession(
                GlassShared::GlassControlKind::UpDown,
                hostHwnd);

        if (Shared::WinApiUtils::DrawThemeBackgroundWithConstantAlpha(
                reinterpret_cast<Shared::WinApiUtils::DrawThemeBackground_pfn>(orig),
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pRect,
                pClipRect,
                effAlpha))
        {
            DebugLog(DBG_NORMAL,
                L"[GlassCore][UpDown] DTB: seq=%u branch=AlphaOk effAlpha=%u part=%d state=%d",
                seq,
                static_cast<unsigned>(effAlpha),
                iPartId,
                iStateId);
            return S_OK;
        }

        DebugLog(DBG_WARNING,
            L"[GlassCore][UpDown] DTB: seq=%u branch=AlphaFallbackOrig effAlpha=%u part=%d state=%d",
            seq,
            static_cast<unsigned>(effAlpha),
            iPartId,
            iStateId);
        return orig(hTheme, hdc, iPartId, iStateId, pRect, pClipRect);
    }

    static void OnBeginPaintGlobal(HWND hWnd, HMODULE callerMod)
    {
        using namespace GlassShared;
        if (g_shutdown.load(std::memory_order_acquire))
            return;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::UpDown)
            return;

        g_upDownSessionStack.push_back({ callerMod, hWnd });
        EnableUpDownHooksForModule(hWnd, callerMod, true);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][UpDown] BeginPaint: callerMod=0x%p hWnd=0x%p depth=%zu",
            callerMod,
            hWnd,
            g_upDownSessionStack.size());
    }

    static void PopUpDownPaintSession(HWND hWnd, const wchar_t* endReason) noexcept
    {
        using namespace GlassShared;
        if (GlassGetWindowKind(hWnd) != GlassControlKind::UpDown)
            return;
        if (g_upDownSessionStack.empty())
            return;

        const UpDownPaintSession s = g_upDownSessionStack.back();
        g_upDownSessionStack.pop_back();
        EnableUpDownHooksForModule(s.hWnd ? s.hWnd : hWnd, s.mod, false);

        DebugLog(DBG_NORMAL,
            L"[GlassCore][UpDown] %s: hWnd=0x%p depth=%zu",
            endReason,
            hWnd,
            g_upDownSessionStack.size());
    }

    static void OnEndPaintGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        PopUpDownPaintSession(hWnd, L"EndPaint");
    }

    static void OnGetDCBeginGlobal(HWND hWnd, HMODULE callerMod)
    {
        OnBeginPaintGlobal(hWnd, callerMod);
    }

    static void OnGetDCEndGlobal(HWND hWnd, HMODULE /*callerMod*/)
    {
        PopUpDownPaintSession(hWnd, L"ReleaseDC");
    }

    void Prepare() {}

    void Startup()
    {
        GlassCore::PaintDispatcher::RegisterPaintProvider({ OnBeginPaintGlobal, OnEndPaintGlobal });
        //GlassCore::PaintDispatcher::RegisterGetDCProvider({ OnGetDCBeginGlobal, OnGetDCEndGlobal });

        auto cacheModule = [&](void* mod)
            {
                if (!mod)
                    return;
                auto lock = g_overallLock.lock_exclusive();
                auto [it, inserted] = g_overallMap.emplace(
                    std::piecewise_construct,
                    std::forward_as_tuple(mod),
                    std::forward_as_tuple(g_overallHookDep, mod));
                if (inserted)
                {
                    it->second.CacheHookData();
                    DebugLog(DBG_NORMAL, L"[GlassCore][UpDown] Startup: cached 0x%p", mod);
                }
            };
        cacheModule(GlassCore::ModuleHandler::g_comctl32New);
        cacheModule(GlassCore::ModuleHandler::g_comctl32Legacy);
        if (HMODULE const ux = GetModuleHandleW(L"uxtheme.dll"))
            cacheModule(ux);
        if (HMODULE const ext = GetModuleHandleW(L"ext-ms-win-uxtheme-themes-l1-1-0.dll"))
            cacheModule(ext);
    }

    void Shutdown()
    {
        g_shutdown.store(true, std::memory_order_seq_cst);
        DisableAllOverallHooks();
        g_upDownSessionStack.clear();
        g_shutdown.store(false, std::memory_order_seq_cst);
    }
}