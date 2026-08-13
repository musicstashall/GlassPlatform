// Реестр InkGlow-клиентов: register на attach, glow только для hwnd в реестре; soft-invalidate при reload.

#include "CorePch.h"
#include <Subsystems/TextGlowClientRegistry.h>
#include <Subsystems/RenderText.h>
#include <Hooks/Static.h>
#include <GlassControlKind.h>
#include <DebugLog.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GlassCore::TextGlow
{
    namespace
    {
        std::mutex g_glowMutex;
        std::unordered_map<std::uintptr_t, TextGlowSpreadCategory> g_categoryByHwnd;
        std::unordered_map<std::uintptr_t, TextGlowClientRole>   g_roleByHwnd;
        std::unordered_map<std::uintptr_t, std::unordered_set<std::uintptr_t>> g_glowHwndByRoot;

        [[nodiscard]] bool IsWindowClassHwnd(HWND hWnd, LPCWSTR cls) noexcept
        {
            if (!hWnd || !cls)
                return false;
            wchar_t cn[256]{};
            if (GetClassNameW(hWnd, cn, static_cast<int>(std::size(cn))) <= 0)
                return false;
            return _wcsicmp(cn, cls) == 0;
        }

        [[nodiscard]] HWND ResolveRegistryRoot(HWND hControl) noexcept
        {
            if (!hControl || !IsWindow(hControl))
                return nullptr;
            return GetAncestor(hControl, GA_ROOT);
        }

        void EraseHwndFromMaps(HWND hControl) noexcept
        {
            const std::uintptr_t hKey = reinterpret_cast<std::uintptr_t>(hControl);
            const auto catIt = g_categoryByHwnd.find(hKey);
            if (catIt == g_categoryByHwnd.end())
                return;

            g_categoryByHwnd.erase(catIt);
            g_roleByHwnd.erase(hKey);

            if (!IsWindow(hControl))
            {
                for (auto rIt = g_glowHwndByRoot.begin(); rIt != g_glowHwndByRoot.end();)
                {
                    rIt->second.erase(hKey);
                    if (rIt->second.empty())
                        rIt = g_glowHwndByRoot.erase(rIt);
                    else
                        ++rIt;
                }
                return;
            }

            const HWND hRoot = ResolveRegistryRoot(hControl);
            if (!hRoot)
                return;
            const auto rIt = g_glowHwndByRoot.find(reinterpret_cast<std::uintptr_t>(hRoot));
            if (rIt == g_glowHwndByRoot.end())
                return;
            rIt->second.erase(hKey);
            if (rIt->second.empty())
                g_glowHwndByRoot.erase(rIt);
        }

        [[nodiscard]] bool SoftInvalidateMenuBarOwnerNc(HWND hOwner) noexcept
        {
            MENUBARINFO mbi{ sizeof(mbi) };
            if (GetMenuBarInfo(hOwner, OBJID_MENU, 0, &mbi) && !IsRectEmpty(&mbi.rcBar))
            {
                RECT rc = mbi.rcBar;
                POINT pts[2] = { { rc.left, rc.top }, { rc.right, rc.bottom } };
                if (MapWindowPoints(nullptr, hOwner, pts, 2) != 0)
                {
                    rc.left   = pts[0].x;
                    rc.top    = pts[0].y;
                    rc.right  = pts[1].x;
                    rc.bottom = pts[1].y;
                    return RedrawWindow(hOwner, &rc, nullptr,
                        RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW) != FALSE;
                }
            }
            return RedrawWindow(hOwner, nullptr, nullptr,
                RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW) != FALSE;
        }

        [[nodiscard]] bool SoftInvalidateClientHwnd(HWND hCtrl, TextGlowClientRole role) noexcept
        {
            if (!hCtrl || !IsWindow(hCtrl) || !IsWindowVisible(hCtrl))
                return false;

            if (role == TextGlowClientRole::MenuBarOwner)
                return SoftInvalidateMenuBarOwnerNc(hCtrl);

            using GK = GlassShared::GlassControlKind;
            const GK kind = GlassShared::GlassGetWindowKind(hCtrl);
            if ((kind == GK::Static || kind == GK::WinForms_Static)
                && GlassCore::StaticControl::IsInsideStaticPaint(hCtrl))
                return false;

            return InvalidateRect(hCtrl, nullptr, FALSE) != FALSE;
        }

        [[nodiscard]] TextGlowClientRole LookupClientRoleLocked(std::uintptr_t hKey) noexcept
        {
            const auto it = g_roleByHwnd.find(hKey);
            if (it == g_roleByHwnd.end())
                return TextGlowClientRole::Control;
            return it->second;
        }

        [[nodiscard]] bool IsCategoryAllowedBySpreadMode(TextGlowSpreadCategory category) noexcept
        {
            const std::uint32_t spreadMode = std::min(GlassCore::Text::g_ExtTextOutGlowSpreadMode, 2u);
            return static_cast<std::uint8_t>(category) <= spreadMode;
        }

        void PurgeRegistryClientsAboveSpreadLocked() noexcept
        {
            std::vector<HWND> purge;
            purge.reserve(g_categoryByHwnd.size());
            for (const auto& entry : g_categoryByHwnd)
            {
                if (!IsCategoryAllowedBySpreadMode(entry.second))
                    purge.push_back(reinterpret_cast<HWND>(entry.first));
            }
            for (HWND hWnd : purge)
                EraseHwndFromMaps(hWnd);
        }

        void TryRegisterKnownPopupClient(HWND hWnd) noexcept
        {
            if (!hWnd || !IsWindow(hWnd))
                return;

            if (IsWindowClassHwnd(hWnd, L"DropDown")
                || IsWindowClassHwnd(hWnd, L"ViewControlClass")
                || IsWindowClassHwnd(hWnd, L"ComboLBox")
                || IsWindowClassHwnd(hWnd, L"ListviewPopup")
                || IsWindowClassHwnd(hWnd, L"tooltips_class32")
                || IsWindowClassHwnd(hWnd, L"MicrosoftWindowsTooltip"))
            {
                RegisterTextGlowPopupClient(hWnd, TextGlowSpreadCategory::Minimal);
            }
        }

        void RebindOneWindow(HWND hWnd) noexcept
        {
            if (!hWnd || !IsWindow(hWnd))
                return;

            TryRegisterTextGlowClient(hWnd);
            TryRegisterKnownPopupClient(hWnd);
        }

        BOOL CALLBACK EnumChildRebindProc(HWND hChild, LPARAM) noexcept
        {
            RebindOneWindow(hChild);
            EnumChildWindows(hChild, EnumChildRebindProc, 0);
            return TRUE;
        }

        BOOL CALLBACK EnumTopLevelRebindProc(HWND hWnd, LPARAM) noexcept
        {
            RebindOneWindow(hWnd);
            EnumChildWindows(hWnd, EnumChildRebindProc, 0);
            return TRUE;
        }
    } // namespace

    bool IsUnderWindowClassAncestor(HWND hWnd, LPCWSTR cls) noexcept
    {
        if (!hWnd || !cls)
            return false;
        for (HWND w = hWnd; w; w = GetParent(w))
        {
            if (IsWindowClassHwnd(w, cls))
                return true;
        }
        return false;
    }

    bool IsTextGlowRegisterCandidate(GlassShared::GlassControlKind kind) noexcept
    {
        using GK = GlassShared::GlassControlKind;
        switch (kind)
        {
        case GK::Static:
        case GK::WinForms_Static:
        case GK::Button:
        case GK::WinForms_Button:
        case GK::TreeView:
        case GK::ListView:
        case GK::WinForms_ListView:
        case GK::ListBox:
        case GK::WinForms_ListBox:
        case GK::Header:
        case GK::StatusBar:
        case GK::Toolbar:
        case GK::Rebar:
        case GK::DirectUI:
            return true;
        default:
            return false;
        }
    }

    TextGlowSpreadCategory CategoryForControlKind(GlassShared::GlassControlKind kind) noexcept
    {
        using GK = GlassShared::GlassControlKind;
        switch (kind)
        {
        case GK::StatusBar:
        case GK::Toolbar:
        case GK::Header:
        case GK::Rebar:
            return TextGlowSpreadCategory::Moderate;
        case GK::Static:
        case GK::WinForms_Static:
        case GK::Button:
        case GK::WinForms_Button:
        case GK::TreeView:
        case GK::ListView:
        case GK::WinForms_ListView:
        case GK::ListBox:
        case GK::WinForms_ListBox:
        case GK::DirectUI:
            return TextGlowSpreadCategory::Global;
        default:
            return TextGlowSpreadCategory::Global;
        }
    }

    TextGlowSpreadCategory ResolveTextGlowSpreadCategory(
        HWND hWnd,
        GlassShared::GlassControlKind kind) noexcept
    {
        using GK = GlassShared::GlassControlKind;
        if (kind == GK::ListView || kind == GK::WinForms_ListView)
        {
            const HWND parent = GetParent(hWnd);
            if (parent && IsWindowClassHwnd(parent, L"ListviewPopup"))
                return TextGlowSpreadCategory::Minimal;
            if (IsUnderWindowClassAncestor(hWnd, L"ListviewPopup"))
                return TextGlowSpreadCategory::Minimal;
            const HWND hRoot = GetAncestor(hWnd, GA_ROOT);
            if (hRoot && IsWindowClassHwnd(hRoot, L"DropDown"))
                return TextGlowSpreadCategory::Minimal;
        }
        if (kind == GK::Header && IsUnderWindowClassAncestor(hWnd, L"DropDown"))
            return TextGlowSpreadCategory::Minimal;

        if (kind == GK::Toolbar)
        {
            if (IsUnderWindowClassAncestor(hWnd, L"ViewControlClass"))
                return TextGlowSpreadCategory::Minimal;
        }

        return CategoryForControlKind(kind);
    }

    TextGlowSpreadCategory ResolveEffectiveSpreadCategory(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return TextGlowSpreadCategory::Global;

        const GlassShared::GlassControlKind kind = GlassShared::GlassGetWindowKind(hWnd);
        if (kind != GlassShared::GlassControlKind::None && IsTextGlowRegisterCandidate(kind))
            return ResolveTextGlowSpreadCategory(hWnd, kind);

        {
            std::lock_guard lock(g_glowMutex);
            const auto it = g_categoryByHwnd.find(reinterpret_cast<std::uintptr_t>(hWnd));
            if (it != g_categoryByHwnd.end())
                return it->second;
        }

        return TextGlowSpreadCategory::Global;
    }

    TextGlowSpreadCategory ResolveSpreadCategoryForWindow(HWND hWnd) noexcept
    {
        return ResolveEffectiveSpreadCategory(hWnd);
    }

    void RegisterTextGlowClient(
        HWND hControl,
        TextGlowSpreadCategory category,
        TextGlowClientRole role) noexcept
    {
        if (!hControl || !IsWindow(hControl))
            return;

        const HWND hRoot = ResolveRegistryRoot(hControl);
        if (!hRoot)
            return;

        const std::uintptr_t rootKey = reinterpret_cast<std::uintptr_t>(hRoot);
        const std::uintptr_t hKey    = reinterpret_cast<std::uintptr_t>(hControl);

        std::lock_guard lock(g_glowMutex);
        if (!IsCategoryAllowedBySpreadMode(category))
        {
            EraseHwndFromMaps(hControl);
            return;
        }
        EraseHwndFromMaps(hControl);
        g_categoryByHwnd[hKey] = category;
        g_roleByHwnd[hKey]     = role;
        g_glowHwndByRoot[rootKey].insert(hKey);

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextGlow] Register hCtrl=0x%p cat=%u role=%u gaRoot=0x%p",
            hControl,
            static_cast<unsigned>(static_cast<std::uint8_t>(category)),
            static_cast<unsigned>(static_cast<std::uint8_t>(role)),
            hRoot);
    }

    void UnregisterTextGlowClient(HWND hControl) noexcept
    {
        if (!hControl)
            return;
        std::lock_guard lock(g_glowMutex);
        EraseHwndFromMaps(hControl);
        DebugLog(DBG_IMPORTANT, L"[GlassCore][TextGlow] Unregister hCtrl=0x%p", hControl);
    }

    void TryRegisterTextGlowClient(HWND hControl) noexcept
    {
        if (!hControl || !IsWindow(hControl))
            return;
        const GlassShared::GlassControlKind kind = GlassShared::GlassGetWindowKind(hControl);
        if (!IsTextGlowRegisterCandidate(kind))
            return;
        const TextGlowSpreadCategory cat = ResolveTextGlowSpreadCategory(hControl, kind);
        if (!IsCategoryAllowedBySpreadMode(cat))
        {
            UnregisterTextGlowClient(hControl);
            return;
        }
        RegisterTextGlowClient(hControl, cat);
    }

    void EnsureTextGlowClientRegistered(HWND hControl) noexcept
    {
        if (!hControl || !IsWindow(hControl))
            return;

        const GlassShared::GlassControlKind kind = GlassShared::GlassGetWindowKind(hControl);
        if (!IsTextGlowRegisterCandidate(kind))
            return;

        const TextGlowSpreadCategory effective = ResolveTextGlowSpreadCategory(hControl, kind);
        if (!IsCategoryAllowedBySpreadMode(effective))
        {
            UnregisterTextGlowClient(hControl);
            return;
        }
        {
            std::lock_guard lock(g_glowMutex);
            const auto it = g_categoryByHwnd.find(reinterpret_cast<std::uintptr_t>(hControl));
            if (it != g_categoryByHwnd.end() && it->second == effective)
                return;
        }

        RegisterTextGlowClient(hControl, effective);
    }

    void RegisterTextGlowPopupClient(HWND hControl, TextGlowSpreadCategory category) noexcept
    {
        RegisterTextGlowClient(hControl, category);
    }

    void TryRegisterMenuBarOwner(HWND hOwner) noexcept
    {
        if (!hOwner || !IsWindow(hOwner))
            return;
        if (!GetMenu(hOwner))
            return;

        MENUBARINFO mbi{ sizeof(mbi) };
        if (!GetMenuBarInfo(hOwner, OBJID_MENU, 0, &mbi))
            return;
        if (IsRectEmpty(&mbi.rcBar))
            return;

        RegisterTextGlowClient(
            hOwner,
            TextGlowSpreadCategory::Moderate,
            TextGlowClientRole::MenuBarOwner);

        if (GlassCore::Text::g_ExtTextOutGlowEnabled && IsWindowVisible(hOwner))
            (void)SoftInvalidateMenuBarOwnerNc(hOwner);
    }

    bool IsTextGlowClientRegistered(HWND hWnd) noexcept
    {
        if (!hWnd || !IsWindow(hWnd))
            return false;
        std::lock_guard lock(g_glowMutex);
        return g_categoryByHwnd.find(reinterpret_cast<std::uintptr_t>(hWnd)) != g_categoryByHwnd.end();
    }

    bool IsInkGlowAllowedForWindow(HWND hWnd) noexcept
    {
        const std::uint32_t spreadMode = std::min(GlassCore::Text::g_ExtTextOutGlowSpreadMode, 2u);

        if (!hWnd || !IsWindow(hWnd))
            return spreadMode >= 2u;

        const TextGlowSpreadCategory category = ResolveEffectiveSpreadCategory(hWnd);
        if (category == TextGlowSpreadCategory::Minimal)
            return true;

        return static_cast<std::uint8_t>(category) <= spreadMode;
    }

    void SoftInvalidateTextGlowClientsForRoot(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;

        const bool forceSweep = !GlassCore::Text::g_ExtTextOutGlowEnabled;
        const std::uint32_t spreadMode = std::min(GlassCore::Text::g_ExtTextOutGlowSpreadMode, 2u);

        std::vector<HWND> copy;
        {
            std::lock_guard lock(g_glowMutex);
            const auto it = g_glowHwndByRoot.find(reinterpret_cast<std::uintptr_t>(hRoot));
            if (it == g_glowHwndByRoot.end())
                return;
            copy.reserve(it->second.size());
            for (const std::uintptr_t p : it->second)
                copy.push_back(reinterpret_cast<HWND>(p));
        }

        std::size_t invalidated = 0;
        for (HWND h : copy)
        {
            if (!h || !IsWindow(h))
                continue;

            TextGlowSpreadCategory category = TextGlowSpreadCategory::Minimal;
            TextGlowClientRole role         = TextGlowClientRole::Control;
            {
                std::lock_guard lock(g_glowMutex);
                const std::uintptr_t hKey = reinterpret_cast<std::uintptr_t>(h);
                const auto catIt = g_categoryByHwnd.find(hKey);
                if (catIt == g_categoryByHwnd.end())
                    continue;
                role = LookupClientRoleLocked(hKey);
            }

            category = ResolveEffectiveSpreadCategory(h);

            if (forceSweep)
            {
                if (category == TextGlowSpreadCategory::Minimal)
                    continue;
            }
            else if (category == TextGlowSpreadCategory::Minimal)
            {
                continue;
            }

            if (SoftInvalidateClientHwnd(h, role))
                ++invalidated;
        }

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextGlow] SoftInvalidate gaRoot=0x%p spread=%u force=%d invalidated=%zu",
            hRoot,
            spreadMode,
            forceSweep ? 1 : 0,
            invalidated);
    }

    void RunDeferredTextGlowSoftInvalidate(HWND hRoot) noexcept
    {
        if (!hRoot || !IsWindow(hRoot))
            return;
        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextGlow] DeferredSoftInvalidate gaRoot=0x%p",
            hRoot);
        SoftInvalidateTextGlowClientsForRoot(hRoot);
    }

    void RebindTextGlowClientsFromMarkedWindows() noexcept
    {
        {
            std::lock_guard lock(g_glowMutex);
            PurgeRegistryClientsAboveSpreadLocked();
        }
        EnumWindows(EnumTopLevelRebindProc, 0);
        DebugLog(DBG_IMPORTANT, L"[GlassCore][TextGlow] RebindTextGlowClientsFromMarkedWindows done");
    }

    void SoftInvalidateRegisteredClientsDroppedBySpreadDecrease(
        std::uint32_t previousSpread,
        std::uint32_t newSpread) noexcept
    {
        previousSpread = std::min(previousSpread, 2u);
        newSpread        = std::min(newSpread, 2u);
        if (newSpread >= previousSpread)
            return;

        struct Target
        {
            HWND                   hWnd;
            TextGlowClientRole     role;
            TextGlowSpreadCategory category;
        };

        std::vector<Target> targets;
        {
            std::lock_guard lock(g_glowMutex);
            targets.reserve(g_categoryByHwnd.size());
            for (const auto& entry : g_categoryByHwnd)
            {
                const auto uCat = static_cast<std::uint8_t>(entry.second);
                if (uCat <= newSpread || uCat > previousSpread)
                    continue;
                targets.push_back({
                    reinterpret_cast<HWND>(entry.first),
                    LookupClientRoleLocked(entry.first),
                    entry.second });
            }
        }

        std::size_t invalidated = 0;
        for (const Target& t : targets)
        {
            if (SoftInvalidateClientHwnd(t.hWnd, t.role))
                ++invalidated;
        }

        DebugLog(DBG_IMPORTANT,
            L"[GlassCore][TextGlow] SpreadDecreaseInvalidate prev=%u new=%u targets=%zu invalidated=%zu",
            previousSpread,
            newSpread,
            targets.size(),
            invalidated);
    }

    void RemoveTextGlowClientsForRoot(HWND hRoot) noexcept
    {
        if (!hRoot)
            return;
        std::lock_guard lock(g_glowMutex);
        const auto it = g_glowHwndByRoot.find(reinterpret_cast<std::uintptr_t>(hRoot));
        if (it == g_glowHwndByRoot.end())
            return;
        for (const std::uintptr_t hKey : it->second)
        {
            g_categoryByHwnd.erase(hKey);
            g_roleByHwnd.erase(hKey);
        }
        g_glowHwndByRoot.erase(it);
    }

    void ClearAllTextGlowClients() noexcept
    {
        std::lock_guard lock(g_glowMutex);
        g_categoryByHwnd.clear();
        g_roleByHwnd.clear();
        g_glowHwndByRoot.clear();
    }
} // namespace GlassCore::TextGlow
