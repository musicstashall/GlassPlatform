#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/Patcher/XamlHostBarPatcherState.h"
#include "xaml/Patcher/XamlHostBarPatcherInternals.h"
#include "xaml/Patcher/XamlHostBarPatcherCommon.h"
#include "xaml/Patcher/XamlHostBarPatcherPrivate.h"
#include "xaml/XamlShutdownMarshal.hpp"
#include "xaml/XamlTapSite.hpp"
#include "xaml/XamlComLifetime.hpp"
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
#include <algorithm>

namespace XamlHostBarPatcher::Internal
{
    // Microsoft.UI.Xaml.IUIElement — GuidAttribute из Microsoft.UI.Xaml.winmd (WinUI 3 в explorer).
    // Нельзя QI на ABI::Windows::UI::Xaml::IUIElement: другой IID и другая ветка WinRT.
    static constexpr GUID IID_IMicrosoftUI_IUIElement{
        0xc3c01020,
        0x320c,
        0x5cf6,
        { 0x9d, 0x24, 0xd3, 0x96, 0xbb, 0xfa, 0x4d, 0x8b }
    };

    // Microsoft.UI.winmd — Dispatching (WinUI 3). IXamlDiagnostics::GetDispatcher даёт не Windows.System.*
    using MuiInvalidateFn = HRESULT(STDMETHODCALLTYPE*)(void* self);

    // IUnknown(3) + IInspectable(3) + индекс в winmd для Microsoft.UI.Xaml.IUIElement (см. WinMdDump).
    static constexpr UINT kWinRtInspectablePrefixSlots = 6;
    static constexpr UINT kMuiInvalidateMeasureMethodIndex = 212;
    static constexpr UINT kMuiInvalidateArrangeMethodIndex = 213;
    static constexpr UINT kMuiUpdateLayoutMethodIndex = 214;

    [[nodiscard]] bool InvalidateLayoutForInspectable(IInspectable* insp) noexcept
    {
        if (!insp)
            return false;

        void* pmui = nullptr;
        if (SUCCEEDED(insp->QueryInterface(IID_IMicrosoftUI_IUIElement, &pmui)) && pmui)
        {
            void** const vtbl = *reinterpret_cast<void***>(pmui);
            auto* const fnM = reinterpret_cast<MuiInvalidateFn>(vtbl[kWinRtInspectablePrefixSlots + kMuiInvalidateMeasureMethodIndex]);
            auto* const fnA = reinterpret_cast<MuiInvalidateFn>(vtbl[kWinRtInspectablePrefixSlots + kMuiInvalidateArrangeMethodIndex]);
            auto* const fnU = reinterpret_cast<MuiInvalidateFn>(vtbl[kWinRtInspectablePrefixSlots + kMuiUpdateLayoutMethodIndex]);
            (void)fnM(pmui);
            (void)fnA(pmui);
            (void)fnU(pmui);
            static_cast<IUnknown*>(pmui)->Release();
            return true;
        }

        ABI::Windows::UI::Xaml::IUIElement* uie = nullptr;
        if (SUCCEEDED(insp->QueryInterface(
            __uuidof(ABI::Windows::UI::Xaml::IUIElement),
            reinterpret_cast<void**>(&uie)))
            && uie)
        {
            (void)uie->InvalidateMeasure();
            (void)uie->InvalidateArrange();
            (void)uie->UpdateLayout();
            uie->Release();
            return true;
        }

        return false;
    }

    struct LayoutDqHandler final : ABI::Windows::System::IDispatcherQueueHandler
    {
        AgileDqHandlerRef m_ref;
        IXamlDiagnostics* m_diag{};
        std::vector<UINT64> m_handles;

        LayoutDqHandler(IXamlDiagnostics* diag, std::vector<UINT64>&& handles) noexcept
            : m_diag(diag)
            , m_handles(std::move(handles))
        {
            if (m_diag)
                m_diag->AddRef();
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            // Маркер agile — иначе TryEnqueue (WinUI) даёт RO_E_MUST_BE_AGILE (0x8000001C).
            if (riid == IID_IAgileObject)
            {
                *ppv = static_cast<IUnknown*>(this);
                AddRef();
                return S_OK;
            }
            // Тот же vtable, что у Windows.System / Microsoft.UI.Dispatching DispatcherQueueHandler.
            if (riid == IID_IUnknown || riid == __uuidof(ABI::Windows::System::IDispatcherQueueHandler)
                || riid == IID_IMicrosoftUIDispatcherQueueHandler)
            {
                *ppv = static_cast<ABI::Windows::System::IDispatcherQueueHandler*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() noexcept override { return m_ref.AddRef(); }

        ULONG STDMETHODCALLTYPE Release() noexcept override
        {
            const ULONG c = m_ref.Release();
            if (c == 0)
            {
                if (m_diag)
                {
                    m_diag->Release();
                    m_diag = nullptr;
                }
                delete this;
            }
            return c;
        }

        HRESULT STDMETHODCALLTYPE Invoke() noexcept override
        {
            if (m_diag)
            {
                for (const UINT64 h : m_handles)
                {
                    IInspectable* insp = nullptr;
                    const HRESULT hrG = m_diag->GetIInspectableFromHandle(static_cast<InstanceHandle>(h), &insp);
                    if (FAILED(hrG) || !insp)
                        continue;
                    (void)InvalidateLayoutForInspectable(insp);
                    insp->Release();
                }
            }
            return S_OK;
        }
    };

    // После ClearProperty: опционально TryEnqueue на очередь WinUI (без ожидания — см. комментарий в теле).
    void TryPostLayoutInvalidateAsyncBestEffort(
        IXamlDiagnostics* diag,
        const std::unordered_set<UINT64>& uniqueElems) noexcept
    {
        if (!diag || uniqueElems.empty())
            return;

        IInspectable* dispInsp = nullptr;
        HRESULT hr = diag->GetDispatcher(&dispInsp);
        if (FAILED(hr) || !dispInsp)
        {
            /*DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlTree] Restore: GetDispatcher hr=0x%08X",
                static_cast<unsigned>(hr));*/
            return;
        }

        // Сначала WinUI (Microsoft.UI.Dispatching.DispatcherQueue), иначе классический Windows.System.
        ABI::Windows::System::IDispatcherQueue* dq = nullptr;
        hr = dispInsp->QueryInterface(
            IID_IMicrosoftUIDispatcherQueue,
            reinterpret_cast<void**>(&dq));
        if (FAILED(hr) || !dq)
        {
            const HRESULT hrW = dispInsp->QueryInterface(
                __uuidof(ABI::Windows::System::IDispatcherQueue),
                reinterpret_cast<void**>(&dq));
            if (FAILED(hrW) || !dq)
            {
                dispInsp->Release();
                /*DebugLog(
                    DBG_WARNING,
                    L"[ExplorerCompat][XamlTree] Restore: QI IDispatcherQueue (MUI hr=0x%08X, Win hr=0x%08X)",
                    static_cast<unsigned>(hr),
                    static_cast<unsigned>(hrW));*/
                return;
            }
        }
        dispInsp->Release();

        std::vector<UINT64> handles;
        handles.reserve(uniqueElems.size());
        for (UINT64 h : uniqueElems)
            handles.push_back(h);

        // Колбэк очереди WinUI во время Restore часто не дренируется секундами (ожидание 8s и return true
        // отключали синхронный fallback → layoutInvalidate=0 и «зависание»). Fire-and-forget + всегда sync.
        auto* const raw = new (std::nothrow) LayoutDqHandler(diag, std::move(handles));
        if (!raw)
        {
            dq->Release();
            return;
        }

        ABI::Windows::System::IDispatcherQueueHandler* cb = raw;
        boolean posted = FALSE;
        hr = dq->TryEnqueue(cb, &posted);
        cb->Release();
        dq->Release();
        /*if (SUCCEEDED(hr))
        {
            DebugLog(
                DBG_NORMAL,
                L"[ExplorerCompat][XamlTree] Restore: TryEnqueue layout async posted=%d (дублирует sync)",
                posted ? 1 : 0);
        }
        else
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlTree] Restore: TryEnqueue hr=0x%08X",
                static_cast<unsigned>(hr));
        }*/
    }

    [[nodiscard]] bool TryInvalidateElementLayout(IXamlDiagnostics* diag, InstanceHandle handle) noexcept
    {
        if (!diag || handle == 0)
            return false;
        IInspectable* insp = nullptr;
        const HRESULT hrG = diag->GetIInspectableFromHandle(handle, &insp);
        if (FAILED(hrG) || !insp)
        {
            /*DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlTree] Restore: layout GetInspectable elem=%llu hr=0x%08X",
                static_cast<unsigned long long>(handle),
                static_cast<unsigned>(hrG));*/
            return false;
        }
        const bool ok = InvalidateLayoutForInspectable(insp);
        insp->Release();
        return ok;
    }

    // Имена DP, которыми пользуется TryPatchFileExplorerHostBars (+ Opacity). Для восстановления при E_FAIL ClearProperty.
    static const wchar_t* const kHostBarRestorePropertyCandidates[] = {
        L"Microsoft.UI.Xaml.UIElement.Opacity",
        L"Windows.UI.Xaml.UIElement.Opacity",
        L"Microsoft.UI.Xaml.Controls.Grid.Background",
        L"Microsoft.UI.Xaml.Controls.Grid.BorderBrush",
        L"Windows.UI.Xaml.Controls.Grid.Background",
        L"Windows.UI.Xaml.Controls.Grid.BorderBrush",
        L"Microsoft.UI.Xaml.Controls.StackPanel.Background",
        L"Microsoft.UI.Xaml.Controls.Border.BorderBrush",
        L"Microsoft.UI.Xaml.Controls.Border.Background",
        L"Microsoft.UI.Xaml.Controls.CommandBar.Background",
        L"Microsoft.UI.Xaml.Controls.AutoSuggestBox.Background",
        L"Microsoft.UI.Xaml.Controls.AutoSuggestBox.BorderBrush",
        L"Microsoft.UI.Xaml.Shapes.Path.Fill",
        L"Windows.UI.Xaml.Shapes.Path.Fill",
        L"Microsoft.UI.Xaml.Shapes.Rectangle.Fill",
        L"Windows.UI.Xaml.Shapes.Rectangle.Fill",
        L"Microsoft.UI.Xaml.Controls.Control.Background",
        L"Microsoft.UI.Xaml.Controls.TextBox.Background",
        L"Microsoft.UI.Xaml.Controls.TextBox.BorderBrush",
        L"Microsoft.UI.Xaml.Controls.ContentPresenter.Background",
    };

    [[nodiscard]] const wchar_t* TryMatchHostBarCandidateByPropIndex(
        IVisualTreeService3* vts3,
        InstanceHandle elem,
        unsigned propIndex) noexcept
    {
        if (!vts3 || elem == 0)
            return nullptr;
        for (const wchar_t* cand : kHostBarRestorePropertyCandidates)
        {
            unsigned idx = 0;
            if (FAILED(vts3->GetPropertyIndex(elem, cand, &idx)))
                continue;
            if (idx == propIndex)
                return cand;
        }
        return nullptr;
    }

    // WinUI 3 (острова проводника): QI на Windows.UI.Xaml.Controls.* часто не срабатывает —
    // объекты реализуют Microsoft.UI.Xaml.Controls.*. IID и порядок методов — как в cppwinrt abi<>.
    using MicrosoftUiPutBrushFn = HRESULT(STDMETHODCALLTYPE*)(void* thisPtr, void* brush);

    [[nodiscard]] bool MicrosoftUiTryVtablePutBrush(void* const iface, std::size_t const vtblSlot) noexcept
    {
        if (!iface || vtblSlot < 6)
            return false;
        void** const* const vtblPtr = reinterpret_cast<void***>(iface);
        if (!vtblPtr || !*vtblPtr)
            return false;
        void** const vtbl = *vtblPtr;
        auto const fn = reinterpret_cast<MicrosoftUiPutBrushFn>(vtbl[vtblSlot]);
        if (!fn)
            return false;
        return SUCCEEDED(fn(iface, nullptr));
    }

    static const GUID kIID_Microsoft_UI_Xaml_Controls_IContentPresenter = {
        0x075E6CA8, 0xDA46, 0x5EC6, { 0x8C, 0x64, 0x03, 0xB3, 0xDD, 0xEA, 0xF7, 0x4F }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IControl = {
        0x857D6E8A, 0xD45A, 0x5C69, { 0xA9, 0x9C, 0xBF, 0x6A, 0x5C, 0x54, 0xFB, 0x38 }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IPanel = {
        0x27A1B418, 0x56F3, 0x525E, { 0xB8, 0x83, 0xCE, 0xFE, 0xD9, 0x05, 0xEE, 0xD3 }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IBorder = {
        0x1CA13B47, 0xFF5C, 0x5ABC, { 0xA4, 0x11, 0xA1, 0x77, 0xDF, 0x94, 0x82, 0xA9 }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IGrid = {
        0xC4496219, 0x9014, 0x58A1, { 0xB4, 0xAD, 0xC5, 0x04, 0x49, 0x13, 0xA5, 0xBB }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IStackPanel = {
        0x493AB00B, 0x3A6A, 0x5E4A, { 0x94, 0x52, 0x40, 0x7C, 0xD5, 0x19, 0x74, 0x06 }
    };

    // Статики WinUI (cppwinrt guid_v) — RoGetActivationFactory + get_*Property → ClearValue на DO.
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IControlStatics = {
        0xC3AE388D, 0xAA36, 0x5E10, { 0xAC, 0xAC, 0x98, 0x41, 0x5F, 0x47, 0xBC, 0xC7 }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IPanelStatics = {
        0x76A9CAA7, 0xA5D4, 0x5061, { 0xA3, 0x25, 0x17, 0xC7, 0x6F, 0x66, 0xDE, 0x51 }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IBorderStatics = {
        0x9385CD66, 0xC178, 0x5176, { 0xB4, 0xA5, 0x16, 0x0E, 0x00, 0xE9, 0x39, 0x35 }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IContentPresenterStatics = {
        0xC3E55F6C, 0x7ABE, 0x568F, { 0xB6, 0x7B, 0x8E, 0xBD, 0x32, 0x74, 0xF3, 0x3E }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IGridStatics = {
        0xEF9CF81D, 0xA431, 0x50F4, { 0xAB, 0xF5, 0x30, 0x23, 0xFE, 0x44, 0x77, 0x04 }
    };
    static const GUID kIID_Microsoft_UI_Xaml_Controls_IStackPanelStatics = {
        0x10BB04E3, 0xEB01, 0x5EA8, { 0x9F, 0x96, 0x69, 0x50, 0x84, 0x79, 0xDE, 0xF9 }
    };
    static constexpr GUID kIID_Microsoft_UI_Xaml_IDependencyObject{
        0xe7beaee7,
        0x160e,
        0x50f7,
        { 0x87, 0x89, 0xd6, 0x34, 0x63, 0xf9, 0x79, 0xfa }
    };
    static constexpr UINT kMuiDependencyObjectGetDispatcherQueueVtableSlot = 14U;

    [[nodiscard]] bool RoGetActivationFactoryForRuntimeId(
        const wchar_t* runtimeClassId,
        REFIID riid,
        void** ppv) noexcept
    {
        if (!runtimeClassId || !ppv)
            return false;
        *ppv = nullptr;
        HSTRING_HEADER hdr{};
        HSTRING hs = nullptr;
        const UINT32 len = static_cast<UINT32>(wcslen(runtimeClassId));
        if (FAILED(WindowsCreateStringReference(runtimeClassId, len, &hdr, &hs)) || !hs)
            return false;
        return SUCCEEDED(RoGetActivationFactory(hs, riid, ppv)) && *ppv;
    }

    using GetDpPropertyFn = HRESULT(STDMETHODCALLTYPE*)(void* statics, void** prop);
    using ClearValueDoFn = HRESULT(STDMETHODCALLTYPE*)(void* pdo, void* dp);

    /// Снять локальное значение как в XAML ClearValue, а не put(nullptr) (иначе часто «дыра»/чёрное).
    [[nodiscard]] bool TryMicrosoftUiClearValueBrushByMatchedName(
        IInspectable* const target,
        const wchar_t* const matchedFullName,
        const bool wantBackground) noexcept
    {
        if (!target || !matchedFullName)
            return false;

        const GUID* staticsIid = nullptr;
        UINT slotGetProp = 0;
        const wchar_t* activatable = nullptr;

        if (wantBackground)
        {
            if (wcsstr(matchedFullName, L"ContentPresenter.Background") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IContentPresenterStatics;
                slotGetProp = 28;
                activatable = L"Microsoft.UI.Xaml.Controls.ContentPresenter";
            }
            else if (wcsstr(matchedFullName, L"Controls.Border.Background") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IBorderStatics;
                slotGetProp = 8;
                activatable = L"Microsoft.UI.Xaml.Controls.Border";
            }
            else if (wcsstr(matchedFullName, L"Controls.Grid.Background") != nullptr
                || wcsstr(matchedFullName, L"Controls.StackPanel.Background") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IPanelStatics;
                slotGetProp = 6;
                activatable = L"Microsoft.UI.Xaml.Controls.Panel";
            }
            else if (wcsstr(matchedFullName, L".Background") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IControlStatics;
                slotGetProp = 24;
                activatable = L"Microsoft.UI.Xaml.Controls.Control";
            }
        }
        else
        {
            if (wcsstr(matchedFullName, L"ContentPresenter.BorderBrush") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IContentPresenterStatics;
                slotGetProp = 24;
                activatable = L"Microsoft.UI.Xaml.Controls.ContentPresenter";
            }
            else if (wcsstr(matchedFullName, L"Controls.Border.BorderBrush") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IBorderStatics;
                slotGetProp = 6;
                activatable = L"Microsoft.UI.Xaml.Controls.Border";
            }
            else if (wcsstr(matchedFullName, L"Controls.Grid.BorderBrush") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IGridStatics;
                slotGetProp = 7;
                activatable = L"Microsoft.UI.Xaml.Controls.Grid";
            }
            else if (wcsstr(matchedFullName, L"Controls.StackPanel.BorderBrush") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IStackPanelStatics;
                slotGetProp = 9;
                activatable = L"Microsoft.UI.Xaml.Controls.StackPanel";
            }
            else if (wcsstr(matchedFullName, L".BorderBrush") != nullptr)
            {
                staticsIid = &kIID_Microsoft_UI_Xaml_Controls_IControlStatics;
                slotGetProp = 27;
                activatable = L"Microsoft.UI.Xaml.Controls.Control";
            }
        }

        if (!staticsIid || !activatable || slotGetProp < 6)
            return false;

        void* pStatics = nullptr;
        if (!RoGetActivationFactoryForRuntimeId(activatable, *staticsIid, &pStatics) || !pStatics)
            return false;

        void** const vtStatics = *reinterpret_cast<void***>(pStatics);
        auto const getProp = reinterpret_cast<GetDpPropertyFn>(vtStatics[slotGetProp]);
        IInspectable* dp = nullptr;
        const HRESULT hrGet = getProp(pStatics, reinterpret_cast<void**>(&dp));
        static_cast<IUnknown*>(pStatics)->Release();
        if (FAILED(hrGet) || !dp)
            return false;

        void* pdo = nullptr;
        if (FAILED(target->QueryInterface(kIID_Microsoft_UI_Xaml_IDependencyObject, &pdo)) || !pdo)
        {
            dp->Release();
            return false;
        }

        void** const vtDo = *reinterpret_cast<void***>(pdo);
        // ClearValue — третий метод IDependencyObject после IInspectable.
        auto const clearVal = reinterpret_cast<ClearValueDoFn>(vtDo[8]);
        const HRESULT hrClear =
            clearVal ? clearVal(pdo, static_cast<void*>(dp)) : E_FAIL;
        static_cast<IUnknown*>(pdo)->Release();
        dp->Release();

        if (SUCCEEDED(hrClear))
        {
            /*DebugLog(DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] Restore: Microsoft.UI DependencyObject.ClearValue (%ls) OK",
                wantBackground ? L"Background" : L"BorderBrush");*/
            return true;
        }
        return false;
    }

    [[nodiscard]] std::wstring DescribeAncestorNamesForDebug(UINT64 leaf, int maxDepth = 12) noexcept
    {
        std::wstring out;
        if (leaf == 0)
            return out;

        std::lock_guard lock(g_xamlMutex);
        UINT64 cur = leaf;
        for (int depth = 0; depth < maxDepth; ++depth)
        {
            auto nit = g_nameOf.find(cur);
            if (nit != g_nameOf.end() && !nit->second.empty())
            {
                if (!out.empty())
                    out.append(L" <- ");
                out.append(nit->second);
            }
            auto pit = g_parentOf.find(cur);
            if (pit == g_parentOf.end() || pit->second == 0 || pit->second == cur)
                break;
            cur = pit->second;
        }
        if (out.empty())
            out = L"(no named ancestors in cache)";
        return out;
    }

    void CollectAncestorsBestEffort(UINT64 leaf, std::unordered_set<UINT64>& out, int maxDepth = 24) noexcept
    {
        if (leaf == 0)
            return;
        std::lock_guard lock(g_xamlMutex);
        UINT64 cur = leaf;
        for (int depth = 0; depth < maxDepth; ++depth)
        {
            out.insert(cur);
            auto pit = g_parentOf.find(cur);
            if (pit == g_parentOf.end() || pit->second == 0 || pit->second == cur)
                break;
            cur = pit->second;
        }
    }

    /// Сброс Background/BorderBrush через ABI Microsoft.UI (put_* nullptr), когда Windows.UI QI не проходит.
    [[nodiscard]] bool TryMicrosoftUiComResetBrushOrOpacity(
        IInspectable* const insp,
        const wchar_t* const matchedFullName,
        InstanceHandle elemHandle = 0) noexcept
    {
        if (!insp || !matchedFullName)
            return false;

        const bool isBackground = wcsstr(matchedFullName, L".Background") != nullptr;
        const bool isBorderBrush = wcsstr(matchedFullName, L".BorderBrush") != nullptr;
        if (!isBackground && !isBorderBrush)
            return false;

        struct SlotTry
        {
            const GUID* iid;
            std::size_t slot;
            const wchar_t* label;
        };

        if (isBackground)
        {
            // У Grid/StackPanel то же DP Background, что и у Control; QI на IControl успевает раньше IPanel и
            // put_Background(nullptr) через IControl даёт «дыру» на полосе адреса (FileExplorerAddressBarGrid).
            // Для Panel DP: ClearValue → IPanel … → IControl в конце. CommandBar/прочие Control.* — по-прежнему IControl первым.
            const bool isPanelSurfaceBackground =
                wcsstr(matchedFullName, L"Controls.Grid.Background") != nullptr
                || wcsstr(matchedFullName, L"Controls.StackPanel.Background") != nullptr;

            if (isPanelSurfaceBackground)
            {
                if (TryMicrosoftUiClearValueBrushByMatchedName(insp, matchedFullName, true))
                    return true;
                const SlotTry kPanelFirst[] = {
                    { &kIID_Microsoft_UI_Xaml_Controls_IPanel, 8, L"IPanel" },
                    { &kIID_Microsoft_UI_Xaml_Controls_IBorder, 11, L"IBorder" },
                    { &kIID_Microsoft_UI_Xaml_Controls_IContentPresenter, 53, L"IContentPresenter" },
                    { &kIID_Microsoft_UI_Xaml_Controls_IControl, 41, L"IControl" },
                };
                for (const auto& e : kPanelFirst)
                {
                    void* pv = nullptr;
                    if (FAILED(insp->QueryInterface(*e.iid, &pv)) || !pv)
                        continue;
                    const bool ok = MicrosoftUiTryVtablePutBrush(pv, e.slot);
                    static_cast<IUnknown*>(pv)->Release();
                    if (ok)
                    {
                        /*DebugLog(DBG_IMPORTANT,
                            L"[ExplorerCompat][XamlTree] Restore: Microsoft.UI %ls put_Background(nullptr) OK (panel DP)",
                            e.label);*/
                        return true;
                    }
                }
                return false;
            }

            // Дочерние элементы полосы адреса патчатся как Control.Background — без обхода дерева снова берётся IControl.
            const bool isControlDpBackground =
                wcsstr(matchedFullName, L"Controls.Control.Background") != nullptr;
            const UINT64 elemU = static_cast<UINT64>(elemHandle);
            if (isControlDpBackground && elemU != 0 && IsUnderExplorerAddressRibbonChrome(elemU))
            {
                if (TryMicrosoftUiClearValueBrushByMatchedName(insp, matchedFullName, true))
                {
                    /*DebugLog(DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlTree] Restore: ClearValue Control.Background (под адресной сеткой) "
                        L"elem=%llu",
                        static_cast<unsigned long long>(elemU));*/
                    return true;
                }
                const SlotTry kAddressChild[] = {
                    { &kIID_Microsoft_UI_Xaml_Controls_IPanel, 8, L"IPanel" },
                    { &kIID_Microsoft_UI_Xaml_Controls_IBorder, 11, L"IBorder" },
                    { &kIID_Microsoft_UI_Xaml_Controls_IContentPresenter, 53, L"IContentPresenter" },
                    { &kIID_Microsoft_UI_Xaml_Controls_IControl, 41, L"IControl" },
                };
                for (const auto& e : kAddressChild)
                {
                    void* pv = nullptr;
                    if (FAILED(insp->QueryInterface(*e.iid, &pv)) || !pv)
                        continue;
                    const bool ok = MicrosoftUiTryVtablePutBrush(pv, e.slot);
                    static_cast<IUnknown*>(pv)->Release();
                    if (ok)
                    {
                        /*DebugLog(DBG_IMPORTANT,
                            L"[ExplorerCompat][XamlTree] Restore: Microsoft.UI %ls put_Background(nullptr) OK "
                            L"(адресная лента)",
                            e.label);*/
                        return true;
                    }
                }
                return false;
            }

            // Сначала put_Background(nullptr) по IControl — совместимо с шаблоном CommandBar (overflow / правая группа).
            const SlotTry kOrder[] = {
                { &kIID_Microsoft_UI_Xaml_Controls_IControl, 41, L"IControl" },
                { &kIID_Microsoft_UI_Xaml_Controls_IPanel, 8, L"IPanel" },
                { &kIID_Microsoft_UI_Xaml_Controls_IContentPresenter, 53, L"IContentPresenter" },
                { &kIID_Microsoft_UI_Xaml_Controls_IBorder, 11, L"IBorder" },
            };
            for (const auto& e : kOrder)
            {
                void* pv = nullptr;
                if (FAILED(insp->QueryInterface(*e.iid, &pv)) || !pv)
                    continue;
                const bool ok = MicrosoftUiTryVtablePutBrush(pv, e.slot);
                static_cast<IUnknown*>(pv)->Release();
                if (ok)
                {
                    /*DebugLog(DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlTree] Restore: Microsoft.UI %ls put_Background(nullptr) OK",
                        e.label);*/
                    return true;
                }
            }
            if (TryMicrosoftUiClearValueBrushByMatchedName(insp, matchedFullName, true))
                return true;
        }
        else if (isBorderBrush)
        {
            const SlotTry kOrder[] = {
                { &kIID_Microsoft_UI_Xaml_Controls_IBorder, 7, L"IBorder" },
                { &kIID_Microsoft_UI_Xaml_Controls_IGrid, 11, L"IGrid" },
                { &kIID_Microsoft_UI_Xaml_Controls_IStackPanel, 13, L"IStackPanel" },
                { &kIID_Microsoft_UI_Xaml_Controls_IControl, 47, L"IControl" },
                { &kIID_Microsoft_UI_Xaml_Controls_IContentPresenter, 45, L"IContentPresenter" },
            };
            for (const auto& e : kOrder)
            {
                void* pv = nullptr;
                if (FAILED(insp->QueryInterface(*e.iid, &pv)) || !pv)
                    continue;
                const bool ok = MicrosoftUiTryVtablePutBrush(pv, e.slot);
                static_cast<IUnknown*>(pv)->Release();
                if (ok)
                {
                    /*DebugLog(DBG_IMPORTANT,
                        L"[ExplorerCompat][XamlTree] Restore: Microsoft.UI %ls put_BorderBrush(nullptr) OK",
                        e.label);*/
                    return true;
                }
            }
            if (TryMicrosoftUiClearValueBrushByMatchedName(insp, matchedFullName, false))
                return true;
        }

        return false;
    }

    /// «Дробовик» по Microsoft.UI: QI + vtable put_* (nullptr) для всех типичных интерфейсов проводника.
    /// slotBg/slotBb: 0 = не вызывать (зарезервировано; слоты vtable < 6 не используются).
    [[nodiscard]] bool TryMicrosoftUiShotgunClearBrushes(IInspectable* const insp) noexcept
    {
        if (!insp)
            return false;

        bool any = false;
        const auto tryPair = [&](REFIID riid, std::size_t const slotBg, std::size_t const slotBb) noexcept
            {
                void* pv = nullptr;
                if (FAILED(insp->QueryInterface(riid, &pv)) || !pv)
                    return;
                if (slotBg >= 6 && MicrosoftUiTryVtablePutBrush(pv, slotBg))
                    any = true;
                if (slotBb >= 6 && MicrosoftUiTryVtablePutBrush(pv, slotBb))
                    any = true;
                static_cast<IUnknown*>(pv)->Release();
            };

        tryPair(kIID_Microsoft_UI_Xaml_Controls_IControl, 41, 47);
        tryPair(kIID_Microsoft_UI_Xaml_Controls_IPanel, 8, 0);
        tryPair(kIID_Microsoft_UI_Xaml_Controls_IContentPresenter, 53, 45);
        tryPair(kIID_Microsoft_UI_Xaml_Controls_IBorder, 11, 7);
        tryPair(kIID_Microsoft_UI_Xaml_Controls_IGrid, 0, 11);
        tryPair(kIID_Microsoft_UI_Xaml_Controls_IStackPanel, 0, 13);

        /*if (any)
        {
            DebugLog(DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] Restore: Microsoft.UI shotgun (≥1 put_* nullptr) OK");
        }*/
        return any;
    }

    [[nodiscard]] bool TryComResetBrushOrOpacityDp(
        IXamlDiagnostics* diag,
        IVisualTreeService3* vts3,
        InstanceHandle elem,
        unsigned propIndex,
        const wchar_t* matchedFullName) noexcept
    {
        if (wcsstr(matchedFullName, L"UIElement.Opacity") != nullptr)
        {
            const InstanceHandle one = CreateBoxedDouble(diag, 1.0);
            if (one == 0)
                return false;
            return SUCCEEDED(vts3->SetProperty(elem, one, propIndex));
        }

        IInspectable* insp = nullptr;
        if (FAILED(diag->GetIInspectableFromHandle(elem, &insp)) || !insp)
            return false;

        bool ok = false;

        if (wcsstr(matchedFullName, L".Fill") != nullptr)
        {
            ABI::Windows::UI::Xaml::Shapes::IShape* shape = nullptr;
            if (SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Shapes::IShape),
                reinterpret_cast<void**>(&shape))) && shape)
            {
                ok = SUCCEEDED(shape->put_Fill(nullptr));
                shape->Release();
            }
        }
        else if (wcsstr(matchedFullName, L".BorderBrush") != nullptr)
        {
            ABI::Windows::UI::Xaml::Controls::IBorder* b = nullptr;
            if (SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IBorder),
                reinterpret_cast<void**>(&b))) && b)
            {
                if (SUCCEEDED(b->put_BorderBrush(nullptr)))
                    ok = true;
                b->Release();
            }
            if (!ok)
            {
                ABI::Windows::UI::Xaml::Controls::IControl* c = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Controls::IControl),
                    reinterpret_cast<void**>(&c))) && c)
                {
                    ok = SUCCEEDED(c->put_BorderBrush(nullptr));
                    c->Release();
                }
            }
            if (!ok)
            {
                ABI::Windows::UI::Xaml::Controls::IGrid2* g2 = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Controls::IGrid2),
                    reinterpret_cast<void**>(&g2))) && g2)
                {
                    ok = SUCCEEDED(g2->put_BorderBrush(nullptr));
                    g2->Release();
                }
            }
            if (!ok)
            {
                ABI::Windows::UI::Xaml::Controls::IStackPanel2* sp2 = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Controls::IStackPanel2),
                    reinterpret_cast<void**>(&sp2))) && sp2)
                {
                    ok = SUCCEEDED(sp2->put_BorderBrush(nullptr));
                    sp2->Release();
                }
            }
        }
        else if (wcsstr(matchedFullName, L".Background") != nullptr)
        {
            ABI::Windows::UI::Xaml::Controls::IPanel* p = nullptr;
            if (SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IPanel),
                reinterpret_cast<void**>(&p))) && p)
            {
                if (SUCCEEDED(p->put_Background(nullptr)))
                    ok = true;
                p->Release();
            }
            if (!ok)
            {
                ABI::Windows::UI::Xaml::Controls::IControl* c = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Controls::IControl),
                    reinterpret_cast<void**>(&c))) && c)
                {
                    ok = SUCCEEDED(c->put_Background(nullptr));
                    c->Release();
                }
            }
            if (!ok)
            {
                ABI::Windows::UI::Xaml::Controls::IBorder* b2 = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Controls::IBorder),
                    reinterpret_cast<void**>(&b2))) && b2)
                {
                    ok = SUCCEEDED(b2->put_Background(nullptr));
                    b2->Release();
                }
            }
        }

        if (!ok
            && (wcsstr(matchedFullName, L".BorderBrush") != nullptr
                || wcsstr(matchedFullName, L".Background") != nullptr))
        {
            ok = TryMicrosoftUiComResetBrushOrOpacity(insp, matchedFullName, elem);
        }

        insp->Release();
        return ok;
    }

    // Цепочка значений DP из диагностики — узнаём полное имя по индексу, когда статический список не совпал.
    [[nodiscard]] bool TryRestoreFromPropertyValuesChain(
        IVisualTreeService* vts,
        IXamlDiagnostics* diag,
        IVisualTreeService3* vts3,
        InstanceHandle elem,
        unsigned propIndex) noexcept
    {
        if (!vts)
            return false;

        unsigned sourceCount = 0;
        unsigned propCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;

        const HRESULT hrChain =
            vts->GetPropertyValuesChain(elem, &sourceCount, &sources, &propCount, &values);
        if (FAILED(hrChain))
        {
            /*DebugLog(DBG_NORMAL,
                L"[ExplorerCompat][XamlTree] Restore: GetPropertyValuesChain elem=%llu hr=0x%08X",
                static_cast<unsigned long long>(elem),
                static_cast<unsigned>(hrChain));*/
            return false;
        }

        bool outOk = false;
        const PropertyChainValue* hit = nullptr;
        for (unsigned i = 0; i < propCount; ++i)
        {
            if (values[i].Index == propIndex)
            {
                hit = &values[i];
                break;
            }
        }
        if (!hit)
        {
            for (unsigned i = 0; i < propCount; ++i)
            {
                if (values[i].PropertyChainIndex == propIndex)
                {
                    hit = &values[i];
                    break;
                }
            }
        }

        if (hit)
        {
            wchar_t fullName[768]{};
            if (hit->DeclaringType && hit->PropertyName && hit->DeclaringType[0] && hit->PropertyName[0])
            {
                swprintf_s(fullName, L"%ls.%ls", hit->DeclaringType, hit->PropertyName);
            }
            else if (hit->PropertyName && wcschr(hit->PropertyName, L'.'))
            {
                wcscpy_s(fullName, hit->PropertyName);
            }

            if (fullName[0] != L'\0' && TryComResetBrushOrOpacityDp(diag, vts3, elem, propIndex, fullName))
            {
                /*DebugLog(DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] Restore: сброс DP по PropertyValuesChain «%ls» elem=%llu",
                    fullName,
                    static_cast<unsigned long long>(elem));*/
                outOk = true;
            }
        }
        /*else
        {
            DebugLog(DBG_NORMAL,
                L"[ExplorerCompat][XamlTree] Restore: PropertyValuesChain: нет записи Index=%u (count=%u) elem=%llu",
                propIndex,
                static_cast<unsigned>(propCount),
                static_cast<unsigned long long>(elem));
        }*/

        if (sources)
        {
            for (unsigned i = 0; i < sourceCount; ++i)
            {
                SysFreeString(sources[i].TargetType);
                SysFreeString(sources[i].Name);
            }
            CoTaskMemFree(sources);
        }
        if (values)
        {
            for (unsigned i = 0; i < propCount; ++i)
            {
                SysFreeString(values[i].Type);
                SysFreeString(values[i].DeclaringType);
                SysFreeString(values[i].ValueType);
                SysFreeString(values[i].ItemType);
                SysFreeString(values[i].Value);
                SysFreeString(values[i].PropertyName);
            }
            CoTaskMemFree(values);
        }

        return outOk;
    }

    // Последняя попытка: снять локальные кисти через ABI, не зная точного имени DP.
    [[nodiscard]] bool TryShotgunClearInspectorBrushes(IXamlDiagnostics* diag, InstanceHandle elem) noexcept
    {
        IInspectable* insp = nullptr;
        if (FAILED(diag->GetIInspectableFromHandle(elem, &insp)) || !insp)
            return false;

        bool anyOk = false;

        if (ABI::Windows::UI::Xaml::Shapes::IShape* shape = nullptr;
            SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Shapes::IShape),
                reinterpret_cast<void**>(&shape))) && shape)
        {
            if (SUCCEEDED(shape->put_Fill(nullptr)))
                anyOk = true;
            shape->Release();
        }

        if (ABI::Windows::UI::Xaml::Controls::IPanel* p = nullptr;
            SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IPanel),
                reinterpret_cast<void**>(&p))) && p)
        {
            if (SUCCEEDED(p->put_Background(nullptr)))
                anyOk = true;
            p->Release();
        }

        // Grid.BorderBrush / StackPanel.BorderBrush — IGrid2 / IStackPanel2 (не IFrameworkElement в ABI).
        if (ABI::Windows::UI::Xaml::Controls::IGrid2* g2 = nullptr;
            SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IGrid2),
                reinterpret_cast<void**>(&g2))) && g2)
        {
            if (SUCCEEDED(g2->put_BorderBrush(nullptr)))
                anyOk = true;
            g2->Release();
        }
        if (ABI::Windows::UI::Xaml::Controls::IStackPanel2* sp2 = nullptr;
            SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IStackPanel2),
                reinterpret_cast<void**>(&sp2))) && sp2)
        {
            if (SUCCEEDED(sp2->put_BorderBrush(nullptr)))
                anyOk = true;
            sp2->Release();
        }

        if (ABI::Windows::UI::Xaml::Controls::IControl* c = nullptr;
            SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IControl),
                reinterpret_cast<void**>(&c))) && c)
        {
            if (SUCCEEDED(c->put_Background(nullptr)))
                anyOk = true;
            if (SUCCEEDED(c->put_BorderBrush(nullptr)))
                anyOk = true;
            c->Release();
        }

        if (ABI::Windows::UI::Xaml::Controls::IBorder* b = nullptr;
            SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IBorder),
                reinterpret_cast<void**>(&b))) && b)
        {
            if (SUCCEEDED(b->put_Background(nullptr)))
                anyOk = true;
            if (SUCCEEDED(b->put_BorderBrush(nullptr)))
                anyOk = true;
            b->Release();
        }

        if (TryMicrosoftUiShotgunClearBrushes(insp))
            anyOk = true;

        insp->Release();
        return anyOk;
    }

    [[nodiscard]] bool TrySetTransparentBrushOnPropertyIndex(
        IVisualTreeService3* vts3,
        InstanceHandle elem,
        unsigned propIndex) noexcept
    {
        if (!vts3 || elem == 0)
            return false;

        static const wchar_t* const kBrushTypes[] = {
            L"Microsoft.UI.Xaml.Media.SolidColorBrush",
            L"Windows.UI.Xaml.Media.SolidColorBrush",
        };

        for (const wchar_t* brushType : kBrushTypes)
        {
            BSTR tb = SysAllocString(brushType);
            BSTR ti = SysAllocString(kExplorerHostBarBrushInitTransparent);
            if (!tb || !ti)
            {
                if (tb)
                    SysFreeString(tb);
                if (ti)
                    SysFreeString(ti);
                return false;
            }
            InstanceHandle newVal = 0;
            const HRESULT hrC = vts3->CreateInstance(tb, ti, &newVal);
            SysFreeString(tb);
            SysFreeString(ti);
            if (FAILED(hrC) || newVal == 0)
                continue;
            if (SUCCEEDED(vts3->SetProperty(elem, newVal, propIndex)))
            {
                /*DebugLog(DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] Restore: SetProperty Transparent brush OK elem=%llu prop=%u",
                    static_cast<unsigned long long>(elem),
                    propIndex);*/
                return true;
            }
        }
        return false;
    }

    // Вся цепочка резервного сброса DP (вызывать с потока владельца визуала для GetPropertyValuesChain / COM).
    [[nodiscard]] bool TryRestoreHostBarDpCore(
        IXamlDiagnostics* diag,
        IVisualTreeService* vts,
        IVisualTreeService3* vts3,
        InstanceHandle elem,
        unsigned propIndex,
        HRESULT clearHr) noexcept
    {
        if (!diag || !vts3 || elem == 0)
            return false;

        const wchar_t* matched = nullptr;
        for (const wchar_t* cand : kHostBarRestorePropertyCandidates)
        {
            unsigned idx = 0;
            if (FAILED(vts3->GetPropertyIndex(elem, cand, &idx)))
                continue;
            if (idx == propIndex)
            {
                matched = cand;
                break;
            }
        }

        if (matched && TryComResetBrushOrOpacityDp(diag, vts3, elem, propIndex, matched))
            return true;

        /*if (!matched)
        {
            DebugLog(DBG_NORMAL,
                L"[ExplorerCompat][XamlTree] Restore: propIndex=%u не найден в kHostBarRestorePropertyCandidates для "
                L"elem=%llu — shotgun Win+Microsoft.UI",
                propIndex,
                static_cast<unsigned long long>(elem));
        }*/

        // Сразу после COM-сброса по имени: Windows.UI + Microsoft.UI vtable (чисто WinUI без Windows QI; и случай
        // без matched). Раньше PropertyValuesChain — на STA там часто RPC_E_WRONG_THREAD.
        if (TryShotgunClearInspectorBrushes(diag, elem))
        {
            /*DebugLog(DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] Restore: shotgun COM (Win+Microsoft.UI) OK elem=%llu prop=%u",
                static_cast<unsigned long long>(elem),
                propIndex);*/
            return true;
        }

        // Тот же путь, что при патче (CreateInstance SolidColorBrush Transparent + SetProperty).
        if (TrySetTransparentBrushOnPropertyIndex(vts3, elem, propIndex))
        {
            /*DebugLog(DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] Restore: SetProperty(Transparent) после ClearProperty E_FAIL "
                L"elem=%llu prop=%u",
                static_cast<unsigned long long>(elem),
                propIndex);*/
            return true;
        }

        if (TryRestoreFromPropertyValuesChain(vts, diag, vts3, elem, propIndex))
            return true;

        const HRESULT hr0 = vts3->SetProperty(elem, 0, propIndex);
        if (SUCCEEDED(hr0))
        {
            /*DebugLog(DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] Restore: ClearProperty hr=0x%08X → SetProperty(0) OK elem=%llu prop=%u",
                static_cast<unsigned>(clearHr),
                static_cast<unsigned long long>(elem),
                propIndex);*/
            return true;
        }

        return false;
    }

    // Синхронный маршалинг Core на очередь WinUI: на STA XAML GetPropertyValuesChain даёт RPC_E_WRONG_THREAD (0x8001010E).
    struct RestoreFallbackDqHandler final : ABI::Windows::System::IDispatcherQueueHandler
    {
        AgileDqHandlerRef m_ref;
        IXamlDiagnostics* m_diag{};
        IVisualTreeService* m_vts{};
        IVisualTreeService3* m_vts3{};
        InstanceHandle m_elem{};
        unsigned m_propIndex{};
        HRESULT m_clearHr{};
        std::shared_ptr<DqSyncEvent> m_sync;
        /// Результат сброса DP (живёт дольше кадра ожидания — нельзя писать в bool на стеке вызывающего).
        std::shared_ptr<std::atomic<bool>> m_spOk;

        RestoreFallbackDqHandler(
            IXamlDiagnostics* diag,
            IVisualTreeService* vts,
            IVisualTreeService3* vts3,
            InstanceHandle elem,
            unsigned propIndex,
            HRESULT clearHr,
            std::shared_ptr<DqSyncEvent> sync,
            std::shared_ptr<std::atomic<bool>> spOk) noexcept
            : m_elem(elem)
            , m_propIndex(propIndex)
            , m_clearHr(clearHr)
            , m_sync(std::move(sync))
            , m_spOk(std::move(spOk))
        {
            m_diag = diag;
            m_vts = vts;
            m_vts3 = vts3;
            if (m_diag)
                m_diag->AddRef();
            if (m_vts)
                m_vts->AddRef();
            if (m_vts3)
                m_vts3->AddRef();
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            if (riid == IID_IAgileObject)
            {
                *ppv = static_cast<IUnknown*>(this);
                AddRef();
                return S_OK;
            }
            if (riid == IID_IUnknown || riid == __uuidof(ABI::Windows::System::IDispatcherQueueHandler)
                || riid == IID_IMicrosoftUIDispatcherQueueHandler)
            {
                *ppv = static_cast<ABI::Windows::System::IDispatcherQueueHandler*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() noexcept override { return m_ref.AddRef(); }

        ULONG STDMETHODCALLTYPE Release() noexcept override
        {
            const ULONG c = m_ref.Release();
            if (c == 0)
            {
                if (m_diag)
                {
                    m_diag->Release();
                    m_diag = nullptr;
                }
                if (m_vts)
                {
                    m_vts->Release();
                    m_vts = nullptr;
                }
                if (m_vts3)
                {
                    m_vts3->Release();
                    m_vts3 = nullptr;
                }
                delete this;
            }
            return c;
        }

        HRESULT STDMETHODCALLTYPE Invoke() noexcept override
        {
            const bool ok =
                TryRestoreHostBarDpCore(m_diag, m_vts, m_vts3, m_elem, m_propIndex, m_clearHr);
            // Чистый COM put_*(nullptr) на потоке острова снимает локальное значение в WinUI, но IVisualTreeService
            // мог остаться с устаревшим состоянием после первоначального E_FAIL ClearProperty — тогда последующие
            // Redraw/тема дают «прозрачные» дыры. На том же потоке повторный ClearProperty часто проходит и синхронизирует VTS.
            if (ok && m_vts3)
            {
                const HRESULT hrSync = m_vts3->ClearProperty(m_elem, m_propIndex);
                if (FAILED(hrSync))
                    (void)m_vts3->SetProperty(m_elem, 0, m_propIndex);
            }
            if (m_spOk)
                m_spOk->store(ok, std::memory_order_release);
            if (m_sync)
                m_sync->Signal();
            return S_OK;
        }
    };

    /// Очередь WinUI острова, на котором живёт элемент (не то же самое, что IXamlDiagnostics::GetDispatcher).
    [[nodiscard]] bool TryGetDispatcherQueueFromDependencyObject(
        IXamlDiagnostics* diag,
        InstanceHandle elem,
        ABI::Windows::System::IDispatcherQueue** outDq) noexcept
    {
        *outDq = nullptr;
        if (!diag || elem == 0)
            return false;

        IInspectable* insp = nullptr;
        if (FAILED(diag->GetIInspectableFromHandle(elem, &insp)) || !insp)
            return false;

        void* pdo = nullptr;
        if (FAILED(insp->QueryInterface(kIID_Microsoft_UI_Xaml_IDependencyObject, &pdo)) || !pdo)
        {
            insp->Release();
            return false;
        }
        insp->Release();

        void** const vt = *reinterpret_cast<void***>(pdo);
        if (!vt)
        {
            static_cast<IUnknown*>(pdo)->Release();
            return false;
        }
        using GetDqFn = HRESULT(STDMETHODCALLTYPE*)(void* self, void** dispatcherQueue);
        auto const getDq = reinterpret_cast<GetDqFn>(vt[kMuiDependencyObjectGetDispatcherQueueVtableSlot]);
        if (!getDq)
        {
            static_cast<IUnknown*>(pdo)->Release();
            return false;
        }
        IInspectable* dqObj = nullptr;
        const HRESULT hrDq = getDq(pdo, reinterpret_cast<void**>(&dqObj));
        static_cast<IUnknown*>(pdo)->Release();
        if (FAILED(hrDq) || !dqObj)
            return false;

        ABI::Windows::System::IDispatcherQueue* dq = nullptr;
        if (SUCCEEDED(dqObj->QueryInterface(
            IID_IMicrosoftUIDispatcherQueue,
            reinterpret_cast<void**>(&dq)))
            && dq)
        {
            dqObj->Release();
            *outDq = dq;
            return true;
        }
        if (SUCCEEDED(dqObj->QueryInterface(
            __uuidof(ABI::Windows::System::IDispatcherQueue),
            reinterpret_cast<void**>(&dq)))
            && dq)
        {
            dqObj->Release();
            *outDq = dq;
            return true;
        }
        dqObj->Release();
        return false;
    }

    [[nodiscard]] bool TryRestoreHostBarDpCoreOnUiQueueSync(
        IXamlDiagnostics* diag,
        IVisualTreeService* vts,
        IVisualTreeService3* vts3,
        InstanceHandle elem,
        unsigned propIndex,
        HRESULT clearHr) noexcept
    {
        if (!diag || !vts || !vts3)
            return false;

        if (g_hostBarRestoreOnIslandUiQueue)
        {
            const bool ok = TryRestoreHostBarDpCore(diag, vts, vts3, elem, propIndex, clearHr);
            if (ok)
            {
                const HRESULT hrSync = vts3->ClearProperty(elem, propIndex);
                if (FAILED(hrSync))
                    (void)vts3->SetProperty(elem, 0, propIndex);
            }
            return ok;
        }

        auto sync = DqSyncEvent::Create();
        if (!sync)
            return false;

        auto spOk = std::make_shared<std::atomic<bool>>(false);
        HRESULT hr = E_FAIL;

        ABI::Windows::System::IDispatcherQueue* dq = nullptr;
        if (TryGetDispatcherQueueFromDependencyObject(diag, elem, &dq) && dq)
        {
            /*DebugLog(DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] Restore: UI-queue с DependencyObject.get_DispatcherQueue (остров) "
                L"elem=%llu",
                static_cast<unsigned long long>(elem));*/
        }
        else
        {
            IInspectable* dispInsp = nullptr;
            hr = diag->GetDispatcher(&dispInsp);
            if (FAILED(hr) || !dispInsp)
            {
                /*DebugLog(DBG_WARNING,
                    L"[ExplorerCompat][XamlTree] Restore: UI-queue fallback GetDispatcher hr=0x%08X",
                    static_cast<unsigned>(hr));*/
                return false;
            }

            hr = dispInsp->QueryInterface(
                IID_IMicrosoftUIDispatcherQueue,
                reinterpret_cast<void**>(&dq));
            if (FAILED(hr) || !dq)
            {
                const HRESULT hrW = dispInsp->QueryInterface(
                    __uuidof(ABI::Windows::System::IDispatcherQueue),
                    reinterpret_cast<void**>(&dq));
                if (FAILED(hrW) || !dq)
                {
                    dispInsp->Release();
                    /*DebugLog(DBG_WARNING,
                        L"[ExplorerCompat][XamlTree] Restore: UI-queue QI DispatcherQueue (MUI hr=0x%08X Win hr=0x%08X)",
                        static_cast<unsigned>(hr),
                        static_cast<unsigned>(hrW));*/
                    return false;
                }
            }
            dispInsp->Release();
        }

        auto* const raw = new (std::nothrow)
            RestoreFallbackDqHandler(diag, vts, vts3, elem, propIndex, clearHr, sync, spOk);
        if (!raw)
        {
            dq->Release();
            return false;
        }

        ABI::Windows::System::IDispatcherQueueHandler* cb = raw;
        boolean posted = FALSE;
        hr = dq->TryEnqueue(cb, &posted);
        cb->Release();
        dq->Release();
        if (FAILED(hr) || !posted)
        {
            /*DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlTree] Restore: UI-queue TryEnqueue hr=0x%08X posted=%d",
                static_cast<unsigned>(hr),
                posted ? 1 : 0);*/
            return false;
        }

        constexpr DWORD kMaxWaitMs = 2000;
        if (!WaitForDqSyncEvent(sync, kMaxWaitMs))
        {
            /*DebugLog(DBG_WARNING,
                L"[ExplorerCompat][XamlTree] Restore: UI-queue fallback wait timeout=%lu elem=%llu",
                static_cast<unsigned long>(kMaxWaitMs),
                static_cast<unsigned long long>(elem));*/
            return false;
        }

        return spOk->load(std::memory_order_acquire);
    }

    [[nodiscard]] bool WaitForDispatcherQueueEvent(HANDLE ev, DWORD maxWaitMs) noexcept
    {
        if (!ev)
            return false;

        constexpr DWORD kSliceMs = 100;
        const DWORD deadline = GetTickCount() + maxWaitMs;
        DWORD w = WAIT_TIMEOUT;
        while (GetTickCount() < deadline)
        {
            DWORD left = deadline - GetTickCount();
            if (left > kSliceMs)
                left = kSliceMs;
            if (left < 1)
                left = 1;
            const DWORD r = MsgWaitForMultipleObjects(1, &ev, FALSE, left, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0)
            {
                w = WAIT_OBJECT_0;
                break;
            }
            if (r == WAIT_FAILED)
            {
                w = WAIT_FAILED;
                break;
            }
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (WaitForSingleObject(ev, 0) == WAIT_OBJECT_0)
            {
                w = WAIT_OBJECT_0;
                break;
            }
        }
        if (w != WAIT_OBJECT_0 && w != WAIT_FAILED)
            w = WaitForSingleObject(ev, 0);
        return w == WAIT_OBJECT_0;
    }

    [[nodiscard]] bool WaitForDqSyncEvent(const std::shared_ptr<DqSyncEvent>& sync, DWORD maxWaitMs) noexcept
    {
        if (!sync || !sync->WaitHandle())
            return false;

        const bool waited = WaitForDispatcherQueueEvent(sync->WaitHandle(), maxWaitMs);
        sync->CloseWaitHandle();
        return waited;
    }

    [[nodiscard]] bool IsExcludedAnchorType(std::wstring_view typeStr) noexcept
    {
        if (typeStr.find(L"PopupRoot") != std::wstring_view::npos)
            return true;
        if (typeStr.find(L"RootScrollViewer") != std::wstring_view::npos)
            return true;
        if (typeStr.find(L".Internal.") != std::wstring_view::npos)
            return true;
        return false;
    }

    struct RestoreToolbarUiBatchEntry
    {
        InstanceHandle elem{};
        unsigned propIndex{};
        std::wstring matched;
    };

    [[nodiscard]] unsigned RunRestoreToolbarUiBatchBody(
        IXamlDiagnostics* diag,
        IVisualTreeService* vts,
        IVisualTreeService3* vts3,
        const std::vector<RestoreToolbarUiBatchEntry>& batch) noexcept
    {
        if (!diag || !vts3 || batch.empty())
            return 0;

        unsigned okCount = 0;
        for (const auto& e : batch)
        {
            if (e.elem == 0 || e.matched.empty())
                continue;

            bool entryOk = false;
            IInspectable* insp = nullptr;
            if (SUCCEEDED(diag->GetIInspectableFromHandle(e.elem, &insp)) && insp)
            {
                if (wcsstr(e.matched.c_str(), L"UIElement.Opacity") != nullptr)
                {
                    entryOk = TryComResetBrushOrOpacityDp(diag, vts3, e.elem, e.propIndex, e.matched.c_str());
                }
                else if (wcsstr(e.matched.c_str(), L".Fill") != nullptr
                    || wcsstr(e.matched.c_str(), L".Background") != nullptr
                    || wcsstr(e.matched.c_str(), L".BorderBrush") != nullptr)
                {
                    const bool wantBackground =
                        wcsstr(e.matched.c_str(), L".Background") != nullptr
                        || wcsstr(e.matched.c_str(), L".Fill") != nullptr;
                    if (TryMicrosoftUiClearValueBrushByMatchedName(insp, e.matched.c_str(), wantBackground))
                        entryOk = true;
                    else if (TryMicrosoftUiShotgunClearBrushes(insp))
                        entryOk = true;
                    else if (vts)
                        entryOk = TryRestoreFromPropertyValuesChain(vts, diag, vts3, e.elem, e.propIndex);
                }
                insp->Release();
            }

            const HRESULT hrSync = vts3->ClearProperty(e.elem, e.propIndex);
            if (FAILED(hrSync))
                (void)vts3->SetProperty(e.elem, 0, e.propIndex);
            if (entryOk || SUCCEEDED(hrSync))
                ++okCount;
        }

        return okCount;
    }

    struct RestoreToolbarUiBatchHandler final : ABI::Windows::System::IDispatcherQueueHandler
    {
        AgileDqHandlerRef m_ref;
        IXamlDiagnostics* m_diag{};
        IVisualTreeService* m_vts{};
        IVisualTreeService3* m_vts3{};
        std::vector<RestoreToolbarUiBatchEntry> m_batch;
        std::shared_ptr<DqSyncEvent> m_sync;
        std::shared_ptr<std::atomic<unsigned>> m_spOkCount;

        RestoreToolbarUiBatchHandler(
            IXamlDiagnostics* diag,
            IVisualTreeService* vts,
            IVisualTreeService3* vts3,
            std::vector<RestoreToolbarUiBatchEntry> batch,
            std::shared_ptr<DqSyncEvent> sync,
            std::shared_ptr<std::atomic<unsigned>> spOkCount) noexcept
            : m_batch(std::move(batch))
            , m_sync(std::move(sync))
            , m_spOkCount(std::move(spOkCount))
        {
            m_diag = diag;
            m_vts = vts;
            m_vts3 = vts3;
            if (m_diag)
                m_diag->AddRef();
            if (m_vts)
                m_vts->AddRef();
            if (m_vts3)
                m_vts3->AddRef();
        }

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) noexcept override
        {
            if (!ppv)
                return E_POINTER;
            *ppv = nullptr;
            if (riid == IID_IAgileObject)
            {
                *ppv = static_cast<IUnknown*>(this);
                AddRef();
                return S_OK;
            }
            if (riid == IID_IUnknown || riid == __uuidof(ABI::Windows::System::IDispatcherQueueHandler)
                || riid == IID_IMicrosoftUIDispatcherQueueHandler)
            {
                *ppv = static_cast<ABI::Windows::System::IDispatcherQueueHandler*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() noexcept override { return m_ref.AddRef(); }

        ULONG STDMETHODCALLTYPE Release() noexcept override
        {
            const ULONG c = m_ref.Release();
            if (c == 0)
            {
                if (m_diag)
                {
                    m_diag->Release();
                    m_diag = nullptr;
                }
                if (m_vts)
                {
                    m_vts->Release();
                    m_vts = nullptr;
                }
                if (m_vts3)
                {
                    m_vts3->Release();
                    m_vts3 = nullptr;
                }
                delete this;
            }
            return c;
        }

        HRESULT STDMETHODCALLTYPE Invoke() noexcept override
        {
            const unsigned okCount =
                RunRestoreToolbarUiBatchBody(m_diag, m_vts, m_vts3, m_batch);
            if (m_spOkCount)
                m_spOkCount->store(okCount, std::memory_order_release);
            if (m_sync)
                m_sync->Signal();
            return S_OK;
        }
    };

    [[nodiscard]] unsigned TryClearValueBatchOnIslandUiQueueSync(
        IXamlDiagnostics* diag,
        IVisualTreeService* vts,
        IVisualTreeService3* vts3,
        const std::vector<RestoreToolbarUiBatchEntry>& batch) noexcept
    {
        if (!diag || !vts || !vts3 || batch.empty())
            return 0;

        if (g_hostBarRestoreOnIslandUiQueue)
            return RunRestoreToolbarUiBatchBody(diag, vts, vts3, batch);

        auto sync = DqSyncEvent::Create();
        if (!sync)
            return 0;

        auto spOk = std::make_shared<std::atomic<unsigned>>(0);
        ABI::Windows::System::IDispatcherQueue* dq = nullptr;
        if (!TryGetDispatcherQueueFromDependencyObject(diag, batch[0].elem, &dq) || !dq)
            return 0;

        auto local = batch;
        auto* const raw = new (std::nothrow) RestoreToolbarUiBatchHandler(
            diag, vts, vts3, std::move(local), sync, spOk);
        if (!raw)
        {
            dq->Release();
            return 0;
        }

        ABI::Windows::System::IDispatcherQueueHandler* cb = raw;
        boolean posted = FALSE;
        const HRESULT hr = dq->TryEnqueue(cb, &posted);
        cb->Release();
        dq->Release();
        if (FAILED(hr) || !posted)
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlTree] Restore: UI-queue sync batch TryEnqueue hr=0x%08X posted=%d",
                static_cast<unsigned>(hr),
                posted ? 1 : 0);
            return 0;
        }

        if (!WaitForDqSyncEvent(sync, 3000))
        {
            DebugLog(
                DBG_WARNING,
                L"[ExplorerCompat][XamlTree] Restore: UI-queue sync batch timeout count=%zu",
                batch.size());
            return 0;
        }

        return spOk->load(std::memory_order_acquire);
    }

    void LogRestorePropertyChainSourceSample(
        IVisualTreeService* vts,
        InstanceHandle elem,
        unsigned propIndex,
        const wchar_t* phase,
        unsigned sampleIndex) noexcept
    {
        if (!vts || elem == 0 || !phase || sampleIndex >= 5)
            return;

        unsigned sourceCount = 0;
        unsigned propCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* values = nullptr;
        const HRESULT hr = vts->GetPropertyValuesChain(elem, &sourceCount, &sources, &propCount, &values);
        if (FAILED(hr) || !values)
        {
            XamlHostBarPatcher::Detail::ReleaseDiagnosticPropertyChain(sources, sourceCount, values, propCount);
            return;
        }

        for (unsigned i = 0; i < propCount; ++i)
        {
            const PropertyChainValue& v = values[i];
            if (v.Index != propIndex)
                continue;

            wchar_t fullName[512]{};
            if (v.DeclaringType && v.PropertyName && v.DeclaringType[0] && v.PropertyName[0])
                swprintf_s(fullName, L"%ls.%ls", v.DeclaringType, v.PropertyName);
            else if (v.PropertyName)
                wcscpy_s(fullName, v.PropertyName);

            unsigned sourceKind = 0;
            if (sources && v.PropertyChainIndex < sourceCount)
                sourceKind = static_cast<unsigned>(sources[v.PropertyChainIndex].Source);

            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] RestoreDiag: %ls elem=%llu prop=%u chainSource=%u overridden=%d "
                L"metadataBits=0x%llX name=%ls",
                phase,
                static_cast<unsigned long long>(elem),
                propIndex,
                sourceKind,
                v.Overridden ? 1 : 0,
                static_cast<unsigned long long>(v.MetadataBits),
                fullName[0] != L'\0' ? fullName : L"(unknown)");
            break;
        }

        XamlHostBarPatcher::Detail::ReleaseDiagnosticPropertyChain(sources, sourceCount, values, propCount);
    }

    [[nodiscard]] bool IsNamedToolbarLayoutAnchor(UINT64 elem) noexcept
    {
        if (elem == 0)
            return false;
        std::lock_guard lock(g_xamlMutex);
        const auto nit = g_nameOf.find(elem);
        if (nit == g_nameOf.end() || nit->second.empty())
            return false;
        const std::wstring& n = nit->second;
        return _wcsicmp(n.c_str(), L"TabContainer") == 0
            || _wcsicmp(n.c_str(), L"TabContainerGrid") == 0
            || _wcsicmp(n.c_str(), L"NavigationBarControlGrid") == 0
            || _wcsicmp(n.c_str(), L"FileExplorerCommandBar") == 0
            || _wcsicmp(n.c_str(), L"FileExplorerSecondaryCommandBar") == 0
            || _wcsicmp(n.c_str(), L"FileExplorerAddressBarGrid") == 0;
    }

    [[nodiscard]] bool TryRestoreHostBarDpAfterClearPropertyFailed(
        IXamlDiagnostics* diag,
        IVisualTreeService* vts,
        IVisualTreeService3* vts3,
        InstanceHandle elem,
        unsigned propIndex,
        HRESULT clearHr) noexcept
    {
        if (!diag || !vts3 || elem == 0)
            return false;

        /*DebugLog(DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] Restore: ClearProperty failed hr=0x%08X → fallback elem=%llu prop=%u",
            static_cast<unsigned>(clearHr),
            static_cast<unsigned long long>(elem),
            propIndex);*/

        if (TryRestoreHostBarDpCore(diag, vts, vts3, elem, propIndex, clearHr))
            return true;

        /*DebugLog(DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] Restore: fallback на STA не прошёл — маршалинг на UI DispatcherQueue "
            L"(GetPropertyValuesChain часто RPC_E_WRONG_THREAD) elem=%llu",
            static_cast<unsigned long long>(elem));*/

        return TryRestoreHostBarDpCoreOnUiQueueSync(diag, vts, vts3, elem, propIndex, clearHr);
    }

    // Восстановление host bar property patches.
    void RestoreHostBarPropertyPatches(IXamlDiagnostics* diag) noexcept
    {
        if (!diag)
            return;

        // IVisualTreeService3::SetProperty/ClearProperty — только на VTC-потоке / island DQ.
        // ShellExt-stop: HWND_MESSAGE часто на popup-tid (== last VTC). Прямой apply там не
        // восстанавливает host bar; GS-stop работал через defer→UI-queue (curTid!=vtcTid).
        // На shutdown всегда UI-queue через RequestRestore, пока не на island queue.
        const DWORD vtcTid = g_vtcCallbackTid.load(std::memory_order_acquire);
        const DWORD curTid = GetCurrentThreadId();
        const bool onIslandQueue = g_hostBarRestoreOnIslandUiQueue;
        const bool shutdownRestore = GlassExplorerCom::g_shutdownDone.load(std::memory_order_acquire);
        const bool defer =
            !onIslandQueue
            && ((vtcTid != 0 && curTid != vtcTid) || shutdownRestore);
        if (defer)
        {
            XamlTapSite::RequestRestoreHostBarChromeOnVtcThread();
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] RestoreHostBarPropertyPatches: defer curTid=%u vtcTid=%u "
                L"shutdown=%d",
                static_cast<unsigned>(curTid),
                static_cast<unsigned>(vtcTid),
                shutdownRestore ? 1 : 0);
            return;
        }

        // Остановить таймер re-patch.
        HWND hMsg = XamlShutdownMarshal::GetMessageWindowHwnd();
        if (hMsg)
            KillTimer(hMsg, XamlShutdownMarshal::kRepatchTimerId);

        IVisualTreeService* vts = nullptr;
        if (FAILED(diag->QueryInterface(IID_IVisualTreeService, reinterpret_cast<void**>(&vts))) || !vts)
            return;
        IVisualTreeService3* vts3 = nullptr;
        if (FAILED(vts->QueryInterface(IID_IVisualTreeService3, reinterpret_cast<void**>(&vts3))) || !vts3)
        {
            vts->Release();
            return;
        }

        std::unordered_map<HostBarPropPatchKey, UINT64, HostBarPropPatchKeyHash> local;
        std::unordered_map<HostBarPropPatchKey, IInspectable*, HostBarPropPatchKeyHash> localInspectables;
        std::unordered_map<UINT64, std::vector<TabRepatchEntry>> tabTargetsForOpacityRestore;
        {
            std::lock_guard lock(g_xamlMutex);
            tabTargetsForOpacityRestore = g_tabRepatchTargets;
            local.swap(g_hostBarPropertyPatches);
            localInspectables.swap(g_hostBarOriginalInspectables);
            g_tabRepatchTargets.clear();
            g_tabContainerHandles.clear();
            g_pendingLazyHostBars.clear();
            g_repatchCyclesRemaining = 0;
            g_loggedPatchSkipAlreadyInRegistry.store(false, std::memory_order_release);
        }

        for (const auto& [elemKey, entries] : tabTargetsForOpacityRestore)
        {
            const InstanceHandle elem = static_cast<InstanceHandle>(elemKey);
            if (!IsElementHandleLive(diag, elem))
                continue;
            for (const auto& entry : entries)
            {
                if (entry.brushTypeName != L"__opacity__")
                    continue;
                const InstanceHandle one = CreateBoxedDouble(diag, 1.0);
                if (one != 0)
                    (void)vts3->SetProperty(elem, one, entry.propIndex);
            }
        }

        unsigned diagSampleBefore = 0;
        for (const auto& [key, oldVal] : local)
        {
            (void)oldVal;
            if (!IsUnderKnownToolbarChrome(key.element))
                continue;
            LogRestorePropertyChainSourceSample(
                vts,
                static_cast<InstanceHandle>(key.element),
                key.propIndex,
                L"before",
                diagSampleBefore++);
            if (diagSampleBefore >= 5)
                break;
        }

        int restoredByOriginal = 0;
        int setOldVal = 0;
        int clearPropertyOk = 0;
        int failed = 0;
        std::vector<RestoreToolbarUiBatchEntry> uiBatch;
        uiBatch.reserve(local.size());

        for (const auto& [key, oldVal] : local)
        {
            const InstanceHandle elem = static_cast<InstanceHandle>(key.element);
            if (!IsElementHandleLive(diag, elem))
                continue;

            bool done = false;

            auto itI = localInspectables.find(key);
            if (itI != localInspectables.end() && itI->second)
            {
                InstanceHandle h = 0;
                const HRESULT hrH = diag->GetHandleFromIInspectable(itI->second, &h);
                if (SUCCEEDED(hrH) && h != 0)
                {
                    const HRESULT hrS = vts3->SetProperty(elem, h, key.propIndex);
                    if (SUCCEEDED(hrS))
                    {
                        ++restoredByOriginal;
                        done = true;
                    }
                }
            }

            if (!done && oldVal != 0)
            {
                const HRESULT hrS = vts3->SetProperty(elem, static_cast<InstanceHandle>(oldVal), key.propIndex);
                if (SUCCEEDED(hrS))
                {
                    ++setOldVal;
                    done = true;
                }
            }

            if (done)
                continue;

            const HRESULT hrClear = vts3->ClearProperty(elem, key.propIndex);
            if (SUCCEEDED(hrClear))
                ++clearPropertyOk;
            else if (!TryRestoreHostBarDpAfterClearPropertyFailed(diag, vts, vts3, elem, key.propIndex, hrClear))
                ++failed;

            if (!IsUnderKnownToolbarChrome(static_cast<UINT64>(elem)))
                continue;

            const wchar_t* matched = TryMatchHostBarCandidateByPropIndex(vts3, elem, key.propIndex);
            if (!matched)
                continue;

            uiBatch.push_back(RestoreToolbarUiBatchEntry{ elem, key.propIndex, matched });
        }

        const unsigned uiClearValueOk =
            uiBatch.empty() ? 0u : TryClearValueBatchOnIslandUiQueueSync(diag, vts, vts3, uiBatch);

        unsigned diagSampleAfter = 0;
        for (const auto& [key, oldVal] : local)
        {
            (void)oldVal;
            if (!IsUnderKnownToolbarChrome(key.element))
                continue;
            LogRestorePropertyChainSourceSample(
                vts,
                static_cast<InstanceHandle>(key.element),
                key.propIndex,
                L"after",
                diagSampleAfter++);
            if (diagSampleAfter >= 5)
                break;
        }

        for (auto& [k, insp] : localInspectables)
        {
            if (insp)
                insp->Release();
        }

        std::unordered_set<UINT64> uniqueElems;
        for (const auto& [key, oldVal] : local)
        {
            (void)oldVal;
            uniqueElems.insert(key.element);
        }

        {
            std::unordered_set<UINT64> extra;
            for (UINT64 eh : uniqueElems)
            {
                if (IsUnderKnownToolbarChrome(eh) || IsUnderExplorerAddressRibbonChrome(eh))
                    CollectAncestorsBestEffort(eh, extra);
            }
            for (UINT64 a : extra)
                uniqueElems.insert(a);
        }

        std::unordered_set<UINT64> layoutInvalidateElems;
        for (UINT64 eh : uniqueElems)
        {
            if (IsUnderKnownToolbarChrome(eh) || IsUnderExplorerAddressRibbonChrome(eh)
                || IsNamedToolbarLayoutAnchor(eh) || IsInsideTabContainer(eh))
            {
                layoutInvalidateElems.insert(eh);
            }
        }

        TryPostLayoutInvalidateAsyncBestEffort(diag, layoutInvalidateElems);
        unsigned layoutOk = 0;
        for (UINT64 eh : layoutInvalidateElems)
        {
            if (TryInvalidateElementLayout(diag, static_cast<InstanceHandle>(eh)))
                ++layoutOk;
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RestoreHostBar: patches=%zu restoredByOriginal=%d setOldVal=%d "
            L"clearPropertyOk=%d uiClearValueOk=%u failed=%d layoutInvalidate=%u/%zu",
            local.size(),
            restoredByOriginal,
            setOldVal,
            clearPropertyOk,
            uiClearValueOk,
            failed,
            static_cast<unsigned>(layoutOk),
            layoutInvalidateElems.size());

        vts3->Release();
        vts->Release();
    }

    void RestoreExplorerDiagBackgroundEntry(
        IXamlDiagnostics* diag,
        UINT64 handle,
        const ExplorerDiagBgPatch& patch) noexcept
    {
        if (!diag)
            return;
        IInspectable* insp = nullptr;
        if (FAILED(diag->GetIInspectableFromHandle(static_cast<InstanceHandle>(handle), &insp)) || !insp)
        {
            if (patch.original)
                patch.original->Release();
            return;
        }
        switch (patch.target)
        {
        case BgPatchTarget::Panel:
        {
            ABI::Windows::UI::Xaml::Controls::IPanel* p = nullptr;
            if (SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IPanel),
                reinterpret_cast<void**>(&p))))
            {
                p->put_Background(patch.original);
                p->Release();
            }
            break;
        }
        case BgPatchTarget::Control:
        {
            ABI::Windows::UI::Xaml::Controls::IControl* c = nullptr;
            if (SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IControl),
                reinterpret_cast<void**>(&c))))
            {
                c->put_Background(patch.original);
                c->Release();
            }
            break;
        }
        case BgPatchTarget::Border:
        {
            ABI::Windows::UI::Xaml::Controls::IBorder* b = nullptr;
            if (SUCCEEDED(insp->QueryInterface(
                __uuidof(ABI::Windows::UI::Xaml::Controls::IBorder),
                reinterpret_cast<void**>(&b))))
            {
                b->put_Background(patch.original);
                b->Release();
            }
            break;
        }
        }
        insp->Release();
        if (patch.original)
            patch.original->Release();
    }
    void RestoreExplorerChromeHostBarOnlyImpl(IXamlDiagnostics* diag) noexcept
    {
        g_disableHostBarPatching.store(true, std::memory_order_release);
        RestoreHostBarPropertyPatches(diag);
        XamlHostBarPatcher::RestoreDiagBackgrounds(diag);
    }

    void RestoreExplorerChromeFullImpl(IXamlDiagnostics* diag) noexcept
    {
        g_disableHostBarPatching.store(true, std::memory_order_release);
        RestoreExplorerChromeHostBarOnlyImpl(diag);

        std::lock_guard lock(g_xamlMutex);
        for (auto& [handle, brush] : g_originalFills)
        {
            if (!brush)
                continue;
            IInspectable* insp = nullptr;
            if (diag && SUCCEEDED(diag->GetIInspectableFromHandle(handle, &insp)) && insp)
            {
                ABI::Windows::UI::Xaml::Shapes::IShape* shape = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Shapes::IShape),
                    reinterpret_cast<void**>(&shape))))
                {
                    shape->put_Fill(brush);
                    shape->Release();
                }
                insp->Release();
            }
            brush->Release();
        }
        g_originalFills.clear();

        for (auto& [handle, brush] : g_originalBorderBackgrounds)
        {
            if (!brush)
                continue;
            IInspectable* insp = nullptr;
            if (diag && SUCCEEDED(diag->GetIInspectableFromHandle(handle, &insp)) && insp)
            {
                ABI::Windows::UI::Xaml::Controls::IBorder* border = nullptr;
                if (SUCCEEDED(insp->QueryInterface(
                    __uuidof(ABI::Windows::UI::Xaml::Controls::IBorder),
                    reinterpret_cast<void**>(&border))))
                {
                    border->put_Background(brush);
                    border->Release();
                }
                insp->Release();
            }
            brush->Release();
        }
        g_originalBorderBackgrounds.clear();
        g_parentOf.clear();
        g_nameOf.clear();
        g_typeOf.clear();
        g_pendingLazyHostBars.clear();

        // Оставить g_disableHostBarPatching=true до Unadvise в shutdown — иначе VTC может перепатчить.
    }
} // namespace XamlHostBarPatcher::Internal
