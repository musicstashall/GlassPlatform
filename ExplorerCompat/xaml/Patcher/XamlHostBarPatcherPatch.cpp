#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/Patcher/XamlHostBarPatcherState.h"
#include "xaml/Patcher/XamlHostBarPatcherInternals.h"
#include "xaml/Patcher/XamlHostBarPatcherCommon.h"
#include "xaml/Patcher/XamlHostBarPatcherPrivate.h"
#include "xaml/XamlShutdownMarshal.hpp"
#include <inspectable.h>
#include <OleAuto.h>
#include <windows.foundation.h>
#include <windows.ui.xaml.media.h>
#include <windows.ui.xaml.shapes.h>
#include <windows.ui.xaml.controls.h>
#include <windows.ui.xaml.h>
#include <windows.system.h>
#include <winstring.h>

#include <DebugLog.h>
#include <RootPolicyQuery.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cstdio>
#include <algorithm>

namespace XamlHostBarPatcher::Internal
{
    std::atomic<bool> g_loggedPatchSkipAlreadyInRegistry{ false };

    // Создать boxed Double через Windows.Foundation.PropertyValue и зарегистрировать
    // как InstanceHandle в XAML Diagnostics. Обходит ненадёжный CreateInstance("Double","0").
    // Кэш НЕ используется: XAML Diagnostics может инвалидировать handle при закрытии окна.
    [[nodiscard]] InstanceHandle CreateBoxedDouble(IXamlDiagnostics* diag, double value) noexcept
    {
        if (!diag)
            return 0;

        // 1. Получить IPropertyValueStatics
        HSTRING_HEADER hdr{};
        HSTRING hsCls = nullptr;
        HRESULT hr = WindowsCreateStringReference(
            L"Windows.Foundation.PropertyValue", 32, &hdr, &hsCls);
        if (FAILED(hr))
        {
            /*DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlTree] CreateBoxedDouble: WindowsCreateStringReference hr=0x%08X",
                static_cast<unsigned>(hr));*/
            return 0;
        }

        ABI::Windows::Foundation::IPropertyValueStatics* statics = nullptr;
        hr = RoGetActivationFactory(
            hsCls,
            __uuidof(ABI::Windows::Foundation::IPropertyValueStatics),
            reinterpret_cast<void**>(&statics));
        if (FAILED(hr) || !statics)
        {
            /*DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlTree] CreateBoxedDouble: RoGetActivationFactory hr=0x%08X",
                static_cast<unsigned>(hr));*/
            return 0;
        }

        // 2. Создать boxed Double
        IInspectable* boxed = nullptr;
        hr = statics->CreateDouble(value, &boxed);
        statics->Release();
        if (FAILED(hr) || !boxed)
        {
            /*DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlTree] CreateBoxedDouble: CreateDouble hr=0x%08X",
                static_cast<unsigned>(hr));*/
            return 0;
        }

        // 3. Зарегистрировать в XAML Diagnostics → получить InstanceHandle
        InstanceHandle handle = 0;
        hr = diag->GetHandleFromIInspectable(boxed, &handle);
        boxed->Release();
        if (FAILED(hr) || handle == 0)
        {
            /*DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlTree] CreateBoxedDouble: GetHandleFromIInspectable hr=0x%08X",
                static_cast<unsigned>(hr));*/
            return 0;
        }

        /*DebugLog(DBG_NORMAL,
            L"[ExplorerCompat][XamlTree] CreateBoxedDouble(%.1f) → handle=%llu",
            value, static_cast<unsigned long long>(handle));*/
        return handle;
    }

    // Установить Opacity=0 на элемент через VTS3 SetProperty + boxed Double.
    [[nodiscard]] bool TrySetOpacityZero(
        IXamlDiagnostics* diag,
        IVisualTreeService3* vts3,
        InstanceHandle target,
        bool isMui,
        unsigned* outPropIndex = nullptr) noexcept
    {
        if (!diag || !vts3 || target == 0)
            return false;

        const wchar_t* propName = isMui
            ? L"Microsoft.UI.Xaml.UIElement.Opacity"
            : L"Windows.UI.Xaml.UIElement.Opacity";

        unsigned propIndex = 0;
        HRESULT hr = vts3->GetPropertyIndex(target, propName, &propIndex);
        if (FAILED(hr))
        {
            /*DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlTree] TrySetOpacityZero: GetPropertyIndex(%ls) hr=0x%08X target=%llu",
                propName, static_cast<unsigned>(hr), static_cast<unsigned long long>(target));*/
            return false;
        }

        // Проверить, не патчили ли уже
        const HostBarPropPatchKey key{ static_cast<UINT64>(target), propIndex };
        {
            std::lock_guard lock(g_xamlMutex);
            if (g_hostBarPropertyPatches.find(key) != g_hostBarPropertyPatches.end())
                return false;
        }

        // Сохранить оригинальное значение
        InstanceHandle oldValue = 0;
        vts3->GetProperty(target, propIndex, &oldValue);

        // Создать boxed 0.0
        InstanceHandle zeroHandle = CreateBoxedDouble(diag, 0.0);
        if (zeroHandle == 0)
            return false;

        hr = vts3->SetProperty(target, zeroHandle, propIndex);
        if (FAILED(hr))
        {
            /*DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlTree] TrySetOpacityZero: SetProperty hr=0x%08X target=%llu prop=%u",
                static_cast<unsigned>(hr), static_cast<unsigned long long>(target), propIndex);*/
            return false;
        }

        {
            std::lock_guard lock(g_xamlMutex);
            g_hostBarPropertyPatches[key] = oldValue;
        }

        if (outPropIndex)
            *outPropIndex = propIndex;

       /* DebugLog(DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] Opacity=0 OK: target=%llu propIndex=%u",
            static_cast<unsigned long long>(target), propIndex);*/
        return true;
    }

    // ========================================================================
    //  Вспомогательные функции (общие хелперы — XamlHostBarPatcherCommon.cpp)
    // ========================================================================
    using XamlHostBarPatcher::Detail::WszContainsI;

    [[nodiscard]] bool IsMicrosoftUiXamlType(BSTR type) noexcept
    {
        return type && wcsncmp(type, L"Microsoft.UI.Xaml", 17) == 0;
    }

    [[nodiscard]] bool ElementNameEquals(BSTR name, const wchar_t* lit) noexcept
    {
        if (!lit || !lit[0])
            return !name || !name[0];
        if (!name)
            return false;
        return wcscmp(name, lit) == 0;
    }

    [[nodiscard]] bool ElementTypeEquals(BSTR type, const wchar_t* lit) noexcept
    {
        return type && lit && wcscmp(type, lit) == 0;
    }

    [[nodiscard]] bool RuntimeClassImpliesShellChromeUi(IXamlDiagnostics* diag, UINT64 handle) noexcept
    {
        if (!diag || handle == 0)
            return true;

        IInspectable* insp = nullptr;
        // Не трактовать сбой GetInspectable как «shell chrome»: иначе ложно блокируется ЗОНА 1 (панель задач).
        if (FAILED(diag->GetIInspectableFromHandle(static_cast<InstanceHandle>(handle), &insp)) || !insp)
            return false;

        HSTRING hstr = nullptr;
        bool exclude = false;
        const HRESULT hrName = insp->GetRuntimeClassName(&hstr);
        if (SUCCEEDED(hrName) && hstr)
        {
            UINT32 len = 0;
            const wchar_t* raw = WindowsGetStringRawBuffer(hstr, &len);
            if (raw && len > 0)
            {
                static const wchar_t* const kExcludeSubstrings[] = {
                    L"StartMenu",
                    L"ActionCenter",
                    L"NotificationCenter",
                    L"ControlCenter",
                    L"QuickSettings",
                };
                for (const wchar_t* sub : kExcludeSubstrings)
                {
                    if (WszContainsI(raw, sub))
                    {
                        exclude = true;
                        break;
                    }
                }
            }
            WindowsDeleteString(hstr);
        }
        insp->Release();
        return exclude;
    }

    // SetProperty Transparent brush через VTS3. Возвращает true если успешно.
    // Если outPropIndex != nullptr, записывает propIndex.
    [[nodiscard]] bool TrySetHostBarBrushProperty(
        IXamlDiagnostics* diag,
        IVisualTreeService3* vts3,
        InstanceHandle target,
        const wchar_t* propertyFullName,
        const wchar_t* solidBrushTypeName,
        LPCWSTR createInstanceInitializer,
        unsigned* outPropIndex = nullptr) noexcept
    {
        if (!diag || !vts3 || !propertyFullName || !solidBrushTypeName || !createInstanceInitializer || target == 0)
            return false;

        unsigned propIndex = 0;
        if (FAILED(vts3->GetPropertyIndex(target, propertyFullName, &propIndex)))
        {
            if (g_inFullReplayPass.load(std::memory_order_acquire)
                && !g_loggedSetPropertyFailInReplay.exchange(true, std::memory_order_relaxed))
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] TrySetHostBarBrushProperty: GetPropertyIndex failed "
                    L"target=%llu prop=%ls tid=%u",
                    static_cast<unsigned long long>(target),
                    propertyFullName,
                    static_cast<unsigned>(GetCurrentThreadId()));
            }
            return false;
        }

        const HostBarPropPatchKey key{ static_cast<UINT64>(target), propIndex };

        InstanceHandle oldValue = 0;
        {
            std::lock_guard lock(g_xamlMutex);
            if (g_hostBarPropertyPatches.find(key) != g_hostBarPropertyPatches.end())
            {
                if (!g_loggedPatchSkipAlreadyInRegistry.exchange(true, std::memory_order_relaxed))
                {
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlTree] TrySetHostBarBrushProperty: skip alreadyInRegistry "
                        L"target=%llu prop=%u",
                        static_cast<unsigned long long>(target),
                        propIndex);
                }
                return false;
            }
        }

        const HRESULT hrGet = vts3->GetProperty(target, propIndex, &oldValue);
        if (FAILED(hrGet))
            oldValue = 0;

        // Сохранить исходную кисть как COM ref — Restore вернёт через SetProperty(original).
        IInspectable* oldInsp = nullptr;
        if (oldValue != 0)
            (void)diag->GetIInspectableFromHandle(oldValue, &oldInsp);
        if (oldInsp)
        {
            std::lock_guard lock(g_xamlMutex);
            if (g_hostBarOriginalInspectables.find(key) == g_hostBarOriginalInspectables.end())
                g_hostBarOriginalInspectables.emplace(key, oldInsp); // владение map
            else
                oldInsp->Release();
        }

        BSTR tb = SysAllocString(solidBrushTypeName);
        BSTR ti = SysAllocString(createInstanceInitializer);
        if (!tb || !ti)
        {
            if (tb)
                SysFreeString(tb);
            if (ti)
                SysFreeString(ti);
            return false;
        }
        InstanceHandle newValue = 0;
        const HRESULT hrC = vts3->CreateInstance(tb, ti, &newValue);
        SysFreeString(tb);
        SysFreeString(ti);
        if (FAILED(hrC) || newValue == 0)
            return false;

        if (FAILED(vts3->SetProperty(target, newValue, propIndex)))
        {
            if (g_inFullReplayPass.load(std::memory_order_acquire)
                && !g_loggedSetPropertyFailInReplay.exchange(true, std::memory_order_relaxed))
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] TrySetHostBarBrushProperty: SetProperty failed "
                    L"target=%llu prop=%u tid=%u",
                    static_cast<unsigned long long>(target),
                    propIndex,
                    static_cast<unsigned>(GetCurrentThreadId()));
            }
            return false;
        }

        // Верификация
        {
            InstanceHandle verifyVal = 0;
            vts3->GetProperty(target, propIndex, &verifyVal);
            if (verifyVal != newValue)
            {
                DebugLog(DBG_WARNING,
                    L"[ExplorerCompat][XamlTree] SetProperty verification MISMATCH: wrote=%llu read=%llu prop=%u target=%llu",
                    static_cast<unsigned long long>(newValue),
                    static_cast<unsigned long long>(verifyVal),
                    propIndex,
                    static_cast<unsigned long long>(target));
            }
        }

        {
            std::lock_guard lock(g_xamlMutex);
            if (g_hostBarPropertyPatches.find(key) != g_hostBarPropertyPatches.end())
            {
                if (oldValue != 0)
                    (void)vts3->SetProperty(target, oldValue, propIndex);
                return false;
            }
            g_hostBarPropertyPatches[key] = oldValue;
        }

        if (outPropIndex)
            *outPropIndex = propIndex;
        return true;
    }

    // Записать tab-элемент в g_tabRepatchTargets и оружить таймер.
    void RecordTabRepatch(InstanceHandle handle, unsigned propIndex, const wchar_t* brushTypeName) noexcept
    {
        const UINT64 key = static_cast<UINT64>(handle);
        {
            std::lock_guard lock(g_xamlMutex);
            auto& vec = g_tabRepatchTargets[key];
            // Не дублировать
            for (const auto& e : vec)
            {
                if (e.propIndex == propIndex)
                    return;
            }
            vec.push_back(TabRepatchEntry{ propIndex, brushTypeName ? brushTypeName : L"" });
            g_repatchCyclesRemaining = kRepatchMaxCycles;
        }

        // Оружить (или переоружить) таймер
        HWND hMsg = XamlShutdownMarshal::GetMessageWindowHwnd();
        if (hMsg)
            SetTimer(hMsg, XamlShutdownMarshal::kRepatchTimerId, 250, nullptr);
    }

    void EnsureLazyHostBarRepatchTimer() noexcept
    {
        XamlShutdownMarshal::EnsureMessageWindow();
        {
            std::lock_guard lock(g_xamlMutex);
            g_repatchCyclesRemaining = kRepatchMaxCycles;
        }
        HWND hMsg = XamlShutdownMarshal::GetMessageWindowHwnd();
        if (hMsg)
            SetTimer(hMsg, XamlShutdownMarshal::kRepatchTimerId, 250, nullptr);
    }

    [[nodiscard]] bool IsExplorerShellIslandChildClass(LPCWSTR cls) noexcept
    {
        if (!cls)
            return false;
        return wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0
            || wcscmp(cls, L"Microsoft.UI.Content.DesktopWindowHost") == 0
            || wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0
            || wcscmp(cls, L"Windows.UI.Composition.DesktopWindowContentBridge") == 0
            || wcscmp(cls, L"FileExplorer_FolderViewIsland") == 0
            || wcscmp(cls, L"SHELLDLL_DefView") == 0;
    }

    [[nodiscard]] VisualElement MakeVisualElementFromCache(
        UINT64 handleKey,
        const std::wstring& typeStr,
        const std::wstring& nameStr) noexcept;

    void FreeVisualElementStrings(VisualElement& ve) noexcept;

    [[nodiscard]] int CountParentChainDepth(UINT64 leaf) noexcept
    {
        if (leaf == 0)
            return 0;
        std::lock_guard lock(g_xamlMutex);
        int depth = 0;
        UINT64 cur = leaf;
        for (int i = 0; i < 64; ++i)
        {
            const auto it = g_parentOf.find(cur);
            if (it == g_parentOf.end() || it->second == 0 || it->second == cur)
                break;
            cur = it->second;
            ++depth;
        }
        return depth;
    }

    [[nodiscard]] bool MatchesLazyHostBarElementName(BSTR name) noexcept
    {
        if (!name)
            return false;
        return ElementNameEquals(name, L"NavigationBarControlGrid") ||
            ElementNameEquals(name, L"CommandBarControlRootGrid") ||
            ElementNameEquals(name, L"FileExplorerAddressBarGrid") ||
            ElementNameEquals(name, L"LayoutRoot") ||
            ElementNameEquals(name, L"PART_LayoutRoot") ||
            ElementNameEquals(name, L"DetailsViewControlRootGrid") ||
            ElementNameEquals(name, L"GalleryRootGrid") ||
            ElementNameEquals(name, L"TabContainerGrid") ||
            ElementNameEquals(name, L"TabContainer");
    }

    [[nodiscard]] bool AddPendingLazyHostBar(const VisualElement& element) noexcept
    {
        if (!element.Type || element.Handle == 0)
            return false;
        const UINT64 key = static_cast<UINT64>(element.Handle);
        std::lock_guard lock(g_xamlMutex);
        for (const auto& p : g_pendingLazyHostBars)
        {
            if (p.handle == key)
                return false;
        }
        if (g_pendingLazyHostBars.size() >= 128)
            return false;
        PendingLazyHostBar row{};
        row.handle = key;
        row.type = element.Type;
        row.name = element.Name ? element.Name : L"";
        row.numChildren = element.NumChildren;
        g_pendingLazyHostBars.push_back(std::move(row));
        return true;
    }

    // Является ли элемент tab-related (должен отслеживаться для re-patch)?
    [[nodiscard]] bool IsTabRelatedElementName(BSTR name) noexcept
    {
        return ElementNameEquals(name, L"TabContainer") ||
            ElementNameEquals(name, L"TabContainerGrid") ||
            ElementNameEquals(name, L"SelectedBackgroundPath");
    }

    // Является ли элемент потомком TabContainer (для LayoutRoot и безымянных)?
    [[nodiscard]] bool IsChildOfTabContainer(UINT64 handle) noexcept
    {
        std::lock_guard lock(g_xamlMutex);
        auto it = g_parentOf.find(handle);
        if (it == g_parentOf.end())
            return false;
        return g_tabContainerHandles.count(it->second) > 0;
    }

    // Рекурсивная проверка: handle или любой его предок — TabContainer?
    [[nodiscard]] bool IsInsideTabContainer(UINT64 handle) noexcept
    {
        std::lock_guard lock(g_xamlMutex);
        UINT64 cur = handle;
        for (int i = 0; i < 16; ++i)
        {
            if (g_tabContainerHandles.count(cur) > 0)
                return true;
            auto it = g_parentOf.find(cur);
            if (it == g_parentOf.end() || it->second == 0 || it->second == cur)
                break;
            cur = it->second;
        }
        return false;
    }

    /// Предок с заданным x:Name в цепочке ParentChildRelation (для маршрутизации Restore без полного DP-имени).
    [[nodiscard]] bool IsUnderNamedVisualAncestor(
        UINT64 leaf,
        const wchar_t* const* names,
        size_t nameCount) noexcept
    {
        if (leaf == 0 || !names || nameCount == 0)
            return false;
        std::lock_guard lock(g_xamlMutex);
        UINT64 cur = leaf;
        for (int depth = 0; depth < 64; ++depth)
        {
            auto nit = g_nameOf.find(cur);
            if (nit != g_nameOf.end())
            {
                for (size_t i = 0; i < nameCount; ++i)
                {
                    if (names[i] && _wcsicmp(nit->second.c_str(), names[i]) == 0)
                        return true;
                }
            }
            auto pit = g_parentOf.find(cur);
            if (pit == g_parentOf.end() || pit->second == 0 || pit->second == cur)
                break;
            cur = pit->second;
        }
        return false;
    }

    [[nodiscard]] bool IsUnderExplorerAddressRibbonChrome(UINT64 elem) noexcept
    {
        static const wchar_t* const kRoots[] = {
            L"FileExplorerAddressBarGrid",
            L"NavigationBarControlGrid",
        };
        return IsUnderNamedVisualAncestor(elem, kRoots, sizeof(kRoots) / sizeof(kRoots[0]));
    }

    // ========================================================================
    //  IsUnderKnownToolbarChrome
    //
    //  Проверяет, что элемент находится внутри поддерева с известным именованным
    //  chrome-предком (тулбар, навигация, адресная строка, вкладки).
    //
    //  Используется для защиты «слишком универсальных» имён (LayoutRoot,
    //  PART_LayoutRoot): эти имена присутствуют и в шаблоне тулбара, и в корне
    //  клиентского острова (папки). Патч Background корня клиентского острова
    //  делает DComp-поверхность прозрачной, что при движении сплиттера
    //  порождает артефакты «кадров прежнего положения» (Ghost-frames регрессия).
    // ========================================================================
    [[nodiscard]] bool IsUnderKnownToolbarChrome(UINT64 handle) noexcept
    {
        static const wchar_t* const kToolbarRoots[] = {
            L"NavigationBarControlGrid",
            L"CommandBarControlRootGrid",
            L"DetailsViewControlRootGrid",
            L"GalleryRootGrid",
            L"HomeViewRootGrid",
            L"FileExplorerAddressBarGrid",
            L"TabContainerGrid",
            L"TabContainer",
            L"FileExplorerCommandBar",
        };
        return IsUnderNamedVisualAncestor(
            handle,
            kToolbarRoots,
            std::size(kToolbarRoots));
    }

    // Основная логика патчинга элементов WinUI host bars проводника.
    [[nodiscard]] int TryPatchFileExplorerHostBars(IXamlDiagnostics* diag, VisualElement element) noexcept
    {
        if (!diag || !element.Type)
            return 0;

        const bool shellChromeBlock = XamlHostBarPatcher::AncestorChainIncludesShellChrome(diag, element.Handle);
        const int parentDepth = CountParentChainDepth(static_cast<UINT64>(element.Handle));
        if (XamlHostBarPatcher::TypeOrNameHintsExplorerUi(element.Type, element.Name))
        {
            DebugLog(DBG_NORMAL,
                L"[ExplorerCompat][XamlTree][HostBarDiag] handle=%llu type=%ls name=%ls shellChromeBlock=%d parentDepth=%d",
                static_cast<unsigned long long>(element.Handle),
                element.Type,
                element.Name ? element.Name : L"",
                shellChromeBlock ? 1 : 0,
                parentDepth);
        }

        if (shellChromeBlock)
        {
            if (MatchesLazyHostBarElementName(element.Name))
            {
                if (AddPendingLazyHostBar(element))
                    EnsureLazyHostBarRepatchTimer();
            }
            return 0;
        }

        IVisualTreeService* vts = nullptr;
        if (FAILED(diag->QueryInterface(IID_IVisualTreeService, reinterpret_cast<void**>(&vts))) || !vts)
            return 0;
        IVisualTreeService3* vts3 = nullptr;
        if (FAILED(vts->QueryInterface(IID_IVisualTreeService3, reinterpret_cast<void**>(&vts3))) || !vts3)
        {
            vts->Release();
            return 0;
        }

        const bool mui = IsMicrosoftUiXamlType(element.Type);
        const wchar_t* brush = mui ? L"Microsoft.UI.Xaml.Media.SolidColorBrush"
            : L"Windows.UI.Xaml.Media.SolidColorBrush";
        BSTR type = element.Type;
        BSTR name = element.Name;
        const InstanceHandle h = element.Handle;
        int patched = 0;

        auto patchGridChromeMui = [&](const wchar_t* bg, const wchar_t* br) noexcept {
            unsigned bgIdx = 0, brIdx = 0;
            if (TrySetHostBarBrushProperty(diag, vts3, h, br, brush, kExplorerHostBarBrushInitTransparent, &brIdx))
                ++patched;
            if (TrySetHostBarBrushProperty(diag, vts3, h, bg, brush, kExplorerHostBarBrushInitTransparent, &bgIdx))
                ++patched;
            if (IsTabRelatedElementName(name) || IsInsideTabContainer(static_cast<UINT64>(h)))
            {
                if (brIdx != 0)
                    RecordTabRepatch(h, brIdx, brush);
                if (bgIdx != 0)
                    RecordTabRepatch(h, bgIdx, brush);
            }
            };

        // ====================================================================
        //  Microsoft.UI.Xaml.Controls.Grid
        // ====================================================================
        if (mui && ElementTypeEquals(type, L"Microsoft.UI.Xaml.Controls.Grid"))
        {
            bool shouldPatch = false;

            if (ElementNameEquals(name, L"NavigationBarControlGrid") ||
                ElementNameEquals(name, L"CommandBarControlRootGrid") ||
                ElementNameEquals(name, L"DetailsViewControlRootGrid") ||
                ElementNameEquals(name, L"GalleryRootGrid") ||
                ElementNameEquals(name, L"HomeViewRootGrid") ||
                ElementNameEquals(name, L"FileExplorerAddressBarGrid") ||
                ElementNameEquals(name, L"TabContainer") ||
                ElementNameEquals(name, L"TabContainerGrid"))
            {
                shouldPatch = true;
            }
            else if (ElementNameEquals(name, L"LayoutRoot") ||
                ElementNameEquals(name, L"PART_LayoutRoot"))
            {
                // «LayoutRoot» / «PART_LayoutRoot» — слишком универсальные имена:
                // они присутствуют как в шаблонах chrome-тулбара, так и в корне
                // клиентского острова (папки). Патчим Background только когда
                // элемент гарантированно находится внутри известного chrome-
                // поддерева, иначе DComp-поверхность острова становится
                // прозрачной → артефакты «кадров» при движении сплиттера.
                shouldPatch = IsUnderKnownToolbarChrome(static_cast<UINT64>(h));
                if (!shouldPatch)
                {
                    /*DebugLog(DBG_NORMAL,
                        L"[ExplorerCompat][XamlTree] Grid LayoutRoot/PART_LayoutRoot SKIPPED "
                        L"(не под known toolbar chrome): handle=%llu",
                        static_cast<unsigned long long>(h));*/
                }
            }

            if (shouldPatch)
            {
                patchGridChromeMui(
                    L"Microsoft.UI.Xaml.Controls.Grid.Background",
                    L"Microsoft.UI.Xaml.Controls.Grid.BorderBrush");
                if (ElementNameEquals(name, L"TabContainer"))
                {
                    std::lock_guard lock(g_xamlMutex);
                    g_tabContainerHandles.insert(static_cast<UINT64>(h));
                }
            }
        }
        // ====================================================================
        //  Windows.UI.Xaml.Controls.Grid
        // ====================================================================
        else if (!mui && ElementTypeEquals(type, L"Windows.UI.Xaml.Controls.Grid"))
        {
            bool shouldPatch = false;

            if (ElementNameEquals(name, L"NavigationBarControlGrid") ||
                ElementNameEquals(name, L"CommandBarControlRootGrid"))
            {
                shouldPatch = true;
            }
            else if (ElementNameEquals(name, L"LayoutRoot") ||
                ElementNameEquals(name, L"PART_LayoutRoot"))
            {
                // Та же защита, что для MUI-ветки: патчим только внутри
                // известного chrome-поддерева.
                shouldPatch = IsUnderKnownToolbarChrome(static_cast<UINT64>(h));
                /*if (!shouldPatch)
                {
                    DebugLog(DBG_NORMAL,
                        L"[ExplorerCompat][XamlTree] UWP Grid LayoutRoot/PART_LayoutRoot SKIPPED "
                        L"(не под known toolbar chrome): handle=%llu",
                        static_cast<unsigned long long>(h));
                }*/
            }

            if (shouldPatch)
            {
                if (TrySetHostBarBrushProperty(
                    diag, vts3, h,
                    L"Windows.UI.Xaml.Controls.Grid.BorderBrush",
                    brush,
                    kExplorerHostBarBrushInitTransparent))
                    ++patched;
                if (TrySetHostBarBrushProperty(
                    diag, vts3, h,
                    L"Windows.UI.Xaml.Controls.Grid.Background",
                    brush,
                    kExplorerHostBarBrushInitTransparent))
                    ++patched;
            }
        }
        else if (mui && ElementTypeEquals(type, L"Microsoft.UI.Xaml.Controls.StackPanel"))
        {
            if (ElementNameEquals(name, L"DetailsViewProperties") ||
                ElementNameEquals(name, L"DetailsViewRelatedConversations") ||
                ElementNameEquals(name, L"DetailsViewActivity"))
            {
                if (TrySetHostBarBrushProperty(
                    diag, vts3, h, L"Microsoft.UI.Xaml.Controls.StackPanel.Background", brush, kExplorerHostBarBrushInitTransparent))
                    ++patched;
            }
        }
        else if (mui && ElementTypeEquals(type, L"Microsoft.UI.Xaml.Controls.Border") &&
            ElementNameEquals(name, L"BorderElement"))
        {
            if (TrySetHostBarBrushProperty(
                diag, vts3, h, L"Microsoft.UI.Xaml.Controls.Border.BorderBrush", brush, kExplorerHostBarBrushInitTransparent))
                ++patched;
        }
        else if (ElementNameEquals(name, L"FileExplorerCommandBar"))
        {
            if (mui && TrySetHostBarBrushProperty(
                diag, vts3, h, L"Microsoft.UI.Xaml.Controls.CommandBar.Background", brush, kExplorerHostBarBrushInitTransparent))
                ++patched;
        }
        else if (mui && ElementTypeEquals(type, L"Microsoft.UI.Xaml.Controls.AutoSuggestBox") &&
            ElementNameEquals(name, L"FileExplorerSearchBox"))
        {
            if (TrySetHostBarBrushProperty(
                diag, vts3, h, L"Microsoft.UI.Xaml.Controls.AutoSuggestBox.Background", brush, kExplorerHostBarBrushInitTransparent))
                ++patched;
            if (TrySetHostBarBrushProperty(
                diag, vts3, h, L"Microsoft.UI.Xaml.Controls.AutoSuggestBox.BorderBrush", brush, kExplorerHostBarBrushInitTransparent))
                ++patched;
        }
        else if (mui && ElementTypeEquals(type, L"Microsoft.UI.Xaml.Shapes.Path") &&
            ElementNameEquals(name, L"SelectedBackgroundPath"))
        {
            unsigned fillIdx = 0;
            if (TrySetHostBarBrushProperty(
                diag, vts3, h, L"Microsoft.UI.Xaml.Shapes.Path.Fill", brush, kExplorerHostBarBrushInitTransparent, &fillIdx))
            {
                ++patched;
                RecordTabRepatch(h, fillIdx, brush);
            }
            unsigned opacityIdx = 0;
            if (TrySetOpacityZero(diag, vts3, h, true, &opacityIdx))
            {
                ++patched;
                RecordTabRepatch(h, opacityIdx, L"__opacity__");
            }
        }
        else if (mui && ElementTypeEquals(type, L"Microsoft.UI.Xaml.Controls.TextBox"))
        {
            if (TrySetHostBarBrushProperty(
                diag, vts3, h, L"Microsoft.UI.Xaml.Controls.TextBox.Background", brush, kExplorerHostBarBrushInitTransparent))
                ++patched;
            if (TrySetHostBarBrushProperty(
                diag, vts3, h, L"Microsoft.UI.Xaml.Controls.TextBox.BorderBrush", brush, kExplorerHostBarBrushInitTransparent))
                ++patched;
        }
        else if (!mui && ElementTypeEquals(type, L"Windows.UI.Xaml.Shapes.Path") &&
            ElementNameEquals(name, L"SelectedBackgroundPath"))
        {
            if (TrySetHostBarBrushProperty(
                diag, vts3, h, L"Windows.UI.Xaml.Shapes.Path.Fill", brush, kExplorerHostBarBrushInitTransparent))
                ++patched;
            // Opacity=0 (UWP вариант)
            if (TrySetOpacityZero(diag, vts3, h, false))
                ++patched;
        }

        // Безымянные дочерние элементы TabContainer
        if (patched == 0 && mui && h != 0)
        {
            bool isTabContainerChild = false;
            {
                std::lock_guard lock(g_xamlMutex);
                auto it = g_parentOf.find(static_cast<UINT64>(h));
                if (it != g_parentOf.end())
                    isTabContainerChild = g_tabContainerHandles.count(it->second) > 0;
            }
            if (isTabContainerChild)
            {
                if (ElementTypeEquals(type, L"Microsoft.UI.Xaml.Controls.Grid"))
                {
                    patchGridChromeMui(
                        L"Microsoft.UI.Xaml.Controls.Grid.Background",
                        L"Microsoft.UI.Xaml.Controls.Grid.BorderBrush");
                }
                else if (ElementTypeEquals(type, L"Microsoft.UI.Xaml.Controls.Border"))
                {
                    if (TrySetHostBarBrushProperty(
                        diag, vts3, h, L"Microsoft.UI.Xaml.Controls.Border.Background", brush, kExplorerHostBarBrushInitTransparent))
                        ++patched;
                    if (TrySetHostBarBrushProperty(
                        diag, vts3, h, L"Microsoft.UI.Xaml.Controls.Border.BorderBrush", brush, kExplorerHostBarBrushInitTransparent))
                        ++patched;
                }
                else if (ElementTypeEquals(type, L"Microsoft.UI.Xaml.Controls.ContentPresenter"))
                {
                    if (TrySetHostBarBrushProperty(
                        diag, vts3, h, L"Microsoft.UI.Xaml.Controls.ContentPresenter.Background", brush, kExplorerHostBarBrushInitTransparent))
                        ++patched;
                }
                /*if (patched > 0)
                {
                    DebugLog(DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlTree] TabContainer child patched: %d правок, type=%ls name=%ls",
                        patched, type, name ? name : L"");
                }*/
            }
        }

        if (patched > 0)
        {
            std::unordered_set<HWND> shells;
            XamlHostBarPatcher::CollectExplorerShellHwndsWithWinUi(shells);
            for (HWND hShell : shells)
                XamlHostBarPatcher::RegisterExplorerShellHwnd(hShell);
        }

        vts3->Release();
        vts->Release();
        return patched;
    }
} // namespace XamlHostBarPatcher::Internal
