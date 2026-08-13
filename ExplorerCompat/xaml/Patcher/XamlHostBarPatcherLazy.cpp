#include "ExplorerPch.h"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/Patcher/XamlHostBarPatcherState.h"
#include "xaml/Patcher/XamlHostBarPatcherInternals.h"
#include "xaml/Patcher/XamlHostBarPatcherCommon.h"
#include "xaml/Patcher/XamlHostBarPatcherPrivate.h"
#include "xaml/XamlShutdownMarshal.hpp"
#include "xaml/XamlTapSite.hpp"
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

namespace
{
    std::mutex g_repatchTabElementsMutex;
} // namespace

namespace XamlHostBarPatcher::Internal
{
    bool IsElementHandleLive(IXamlDiagnostics* diag, InstanceHandle elem) noexcept
    {
        if (!diag || elem == 0)
            return false;
        IInspectable* insp = nullptr;
        const HRESULT hr = diag->GetIInspectableFromHandle(elem, &insp);
        if (FAILED(hr) || !insp)
            return false;
        insp->Release();
        return true;
    }
} // namespace XamlHostBarPatcher::Internal

namespace
{
    [[nodiscard]] bool TryRepatchTransparentBrush(
        IXamlDiagnostics* diag,
        IVisualTreeService3* vts3,
        InstanceHandle elem,
        unsigned propIndex,
        const wchar_t* brushTypeName) noexcept
    {
        if (!diag || !vts3 || elem == 0 || !brushTypeName || !brushTypeName[0])
            return false;
        if (!XamlHostBarPatcher::Internal::IsElementHandleLive(diag, elem))
            return false;
        if (wcsstr(brushTypeName, L"Brush") == nullptr)
            return false;

        BSTR tb = SysAllocString(brushTypeName);
        BSTR ti = SysAllocString(kExplorerHostBarBrushInitTransparent);
        if (!tb || !ti)
        {
            if (tb)
                SysFreeString(tb);
            if (ti)
                SysFreeString(ti);
            return false;
        }

        InstanceHandle newBrush = 0;
        const HRESULT hrCreate = vts3->CreateInstance(tb, ti, &newBrush);
        SysFreeString(tb);
        SysFreeString(ti);
        if (FAILED(hrCreate) || newBrush == 0)
            return false;

        const HRESULT hrSet = vts3->SetProperty(elem, newBrush, propIndex);
        return SUCCEEDED(hrSet);
    }
}

namespace XamlHostBarPatcher::Internal
{
    void ProcessPendingLazyHostBars(IXamlDiagnostics* diag) noexcept
    {
        if (!diag)
            return;
        std::vector<PendingLazyHostBar> batch;
        {
            std::lock_guard lock(g_xamlMutex);
            batch = std::move(g_pendingLazyHostBars);
            g_pendingLazyHostBars.clear();
        }
        if (batch.empty())
            return;

        std::vector<PendingLazyHostBar> stillPending;
        stillPending.reserve(batch.size());
        for (const auto& e : batch)
        {
            BSTR t = SysAllocString(e.type.c_str());
            if (!t)
            {
                stillPending.push_back(e);
                continue;
            }
            BSTR n = e.name.empty() ? nullptr : SysAllocString(e.name.c_str());
            VisualElement ve{};
            ve.Type = t;
            ve.Name = n;
            ve.Handle = static_cast<InstanceHandle>(e.handle);
            ve.NumChildren = e.numChildren;
            const int p = TryPatchFileExplorerHostBars(diag, ve);
            SysFreeString(t);
            if (n)
                SysFreeString(n);
            if (p == 0)
            {
                bool dropStale = false;
                {
                    std::lock_guard lock(g_xamlMutex);
                    dropStale = (g_parentOf.find(e.handle) == g_parentOf.end());
                }
                if (!dropStale)
                    stillPending.push_back(e);
            }
        }
        if (!stillPending.empty())
        {
            std::lock_guard lock(g_xamlMutex);
            for (const auto& e : stillPending)
            {
                bool dup = false;
                for (const auto& p : g_pendingLazyHostBars)
                {
                    if (p.handle == e.handle)
                    {
                        dup = true;
                        break;
                    }
                }
                if (!dup && g_pendingLazyHostBars.size() < 128)
                    g_pendingLazyHostBars.push_back(e);
            }
        }
    }
} // namespace XamlHostBarPatcher::Internal

void XamlHostBarPatcher::RepatchTabElements(IXamlDiagnostics* diag) noexcept
{
    if (!diag)
        return;
    if (!IsExplorerXamlPatchingAllowed())
        return;

    // IVisualTreeService3::CreateInstance — только на VTC-потоке (иначе AV в Microsoft.UI.Xaml.dll).
    const DWORD vtcTid = g_vtcCallbackTid.load(std::memory_order_acquire);
    const DWORD curTid = GetCurrentThreadId();
    if (vtcTid != 0 && curTid != vtcTid)
    {
        XamlTapSite::RequestRepatchTabElementsOnVtcThread();
        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] RepatchTabElements: defer curTid=%u vtcTid=%u",
            static_cast<unsigned>(curTid),
            static_cast<unsigned>(vtcTid));
        return;
    }

    std::lock_guard repatchLock(g_repatchTabElementsMutex);

    Internal::ProcessPendingLazyHostBars(diag);

    IVisualTreeService* vts = nullptr;
    if (FAILED(diag->QueryInterface(IID_IVisualTreeService, reinterpret_cast<void**>(&vts))) || !vts)
        return;
    IVisualTreeService3* vts3 = nullptr;
    if (FAILED(vts->QueryInterface(IID_IVisualTreeService3, reinterpret_cast<void**>(&vts3))) || !vts3)
    {
        vts->Release();
        return;
    }

    XamlHostBarPatcher::Detail::ProcessPendingDwxsForPopup(diag, vts3);

    std::unordered_map<UINT64, std::vector<TabRepatchEntry>> targets;
    {
        std::lock_guard lock(g_xamlMutex);
        targets = g_tabRepatchTargets;
    }

    if (targets.empty())
    {
        vts3->Release();
        vts->Release();
        return;
    }

    int repatched = 0;
    std::vector<UINT64> keysToEraseOnFailure;
    keysToEraseOnFailure.reserve(targets.size());

    for (const auto& [elemKey, entries] : targets)
    {
        const InstanceHandle elem = static_cast<InstanceHandle>(elemKey);
        if (!Internal::IsElementHandleLive(diag, elem))
        {
            keysToEraseOnFailure.push_back(elemKey);
            continue;
        }

        bool removeKey = false;
        for (const auto& e : entries)
        {
            if (e.brushTypeName == L"__opacity__")
            {
                const InstanceHandle zeroHandle = Internal::CreateBoxedDouble(diag, 0.0);
                if (zeroHandle == 0)
                {
                    removeKey = true;
                    break;
                }
                const HRESULT hrOpacity = vts3->SetProperty(elem, zeroHandle, e.propIndex);
                if (FAILED(hrOpacity))
                {
                    removeKey = true;
                    break;
                }
                ++repatched;
                continue;
            }

            if (!TryRepatchTransparentBrush(diag, vts3, elem, e.propIndex, e.brushTypeName.c_str()))
            {
                removeKey = true;
                break;
            }
            ++repatched;
        }
        if (removeKey)
            keysToEraseOnFailure.push_back(elemKey);
    }

    vts3->Release();
    vts->Release();

    {
        std::lock_guard lock(g_xamlMutex);
        for (UINT64 k : keysToEraseOnFailure)
            g_tabRepatchTargets.erase(k);
    }

    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][XamlTree] RepatchTabElements: repatched=%d failedKeys=%zu tid=%u",
        repatched,
        keysToEraseOnFailure.size(),
        static_cast<unsigned>(curTid));
}
