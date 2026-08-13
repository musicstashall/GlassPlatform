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
    [[nodiscard]] int AnchorTypePriority(std::wstring_view typeStr) noexcept
    {
        if (IsExcludedAnchorType(typeStr))
            return -1;
        if (typeStr.find(L"NavigationBarControl") != std::wstring_view::npos)
            return 50;
        if (typeStr.find(L"CommandBarControl") != std::wstring_view::npos)
            return 50;
        if (typeStr.find(L"AddressBarControl") != std::wstring_view::npos)
            return 45;
        if (typeStr.find(L"FileExplorerTabControl") != std::wstring_view::npos)
            return 40;
        if (typeStr.find(L"FileExplorerExtensions") != std::wstring_view::npos)
            return 20;
        return 0;
    }

    [[nodiscard]] bool BuildFullReplaySnapshot(FullReplaySnapshot& snap) noexcept
    {
        snap = {};
        {
            std::lock_guard lock(g_xamlMutex);
            snap.nameOf = g_nameOf;
            snap.typeOf = g_typeOf;
            snap.parentOf = g_parentOf;
        }
        snap.childrenOf.reserve(snap.parentOf.size());
        for (const auto& [child, parent] : snap.parentOf)
        {
            if (child != 0 && parent != 0)
                snap.childrenOf[parent].push_back(child);
        }
        snap.dwxsRoots.reserve(32);
        for (const auto& [handleKey, typeStr] : snap.typeOf)
        {
            if (handleKey == 0 || typeStr.empty())
                continue;
            if (typeStr.find(L"DesktopWindowXamlSource") != std::wstring::npos)
                snap.dwxsRoots.push_back(handleKey);
        }
        return !snap.dwxsRoots.empty();
    }

    struct DispatcherQueueProbeHandler final : ABI::Windows::System::IDispatcherQueueHandler
    {
        AgileDqHandlerRef m_ref;
        std::shared_ptr<DqSyncEvent> m_sync;
        std::shared_ptr<std::atomic<DWORD>> m_spTid;

        DispatcherQueueProbeHandler(
            std::shared_ptr<DqSyncEvent> sync,
            std::shared_ptr<std::atomic<DWORD>> spTid) noexcept
            : m_sync(std::move(sync))
            , m_spTid(std::move(spTid))
        {
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
                delete this;
            return c;
        }

        HRESULT STDMETHODCALLTYPE Invoke() noexcept override
        {
            if (m_spTid)
                m_spTid->store(GetCurrentThreadId(), std::memory_order_release);
            if (m_sync)
                m_sync->Signal();
            return S_OK;
        }
    };

    [[nodiscard]] bool TryProbeDispatcherQueueThread(
        ABI::Windows::System::IDispatcherQueue* dq,
        DWORD* outTid) noexcept
    {
        if (!dq || !outTid)
            return false;
        *outTid = 0;

        auto sync = DqSyncEvent::Create();
        if (!sync)
            return false;

        auto spTid = std::make_shared<std::atomic<DWORD>>(0);
        auto* const raw = new (std::nothrow) DispatcherQueueProbeHandler(sync, spTid);
        if (!raw)
            return false;

        ABI::Windows::System::IDispatcherQueueHandler* cb = raw;
        boolean posted = FALSE;
        const HRESULT hr = dq->TryEnqueue(cb, &posted);
        cb->Release();
        if (FAILED(hr) || !posted)
            return false;

        constexpr DWORD kProbeWaitMs = 2000;
        if (!WaitForDqSyncEvent(sync, kProbeWaitMs))
            return false;

        const DWORD tid = spTid->load(std::memory_order_acquire);
        if (tid != 0)
            *outTid = tid;
        return tid != 0;
    }

    [[nodiscard]] bool IsValidIslandDispatcherThread(
        DWORD probeTid,
        DWORD marshalTid,
        DWORD callerTid) noexcept
    {
        if (probeTid == 0)
            return false;
        if (marshalTid != 0 && probeTid == marshalTid)
            return false;
        if (callerTid != 0 && probeTid == callerTid)
            return false;
        return true;
    }

    [[nodiscard]] InstanceHandle FindIslandDispatcherAnchor(
        IXamlDiagnostics* diag,
        UINT64 rootU,
        const FullReplaySnapshot& snap,
        DWORD marshalTid,
        DWORD callerTid,
        DWORD vtcTid) noexcept
    {
        if (!diag || rootU == 0)
            return 0;

        struct Candidate
        {
            UINT64 handle{};
            int priority{};
            std::wstring type;
        };
        std::vector<Candidate> candidates;
        candidates.reserve(64);

        std::vector<UINT64> queue;
        queue.push_back(rootU);
        std::unordered_set<UINT64> seen;
        seen.insert(rootU);

        for (size_t qi = 0; qi < queue.size(); ++qi)
        {
            const UINT64 cur = queue[qi];
            std::wstring typeStr;
            const auto typeIt = snap.typeOf.find(cur);
            if (typeIt != snap.typeOf.end())
                typeStr = typeIt->second;

            const int pri = AnchorTypePriority(typeStr);
            if (pri > 0)
                candidates.push_back(Candidate{ cur, pri, typeStr });

            const auto childIt = snap.childrenOf.find(cur);
            if (childIt != snap.childrenOf.end())
            {
                for (UINT64 childU : childIt->second)
                {
                    if (seen.insert(childU).second)
                        queue.push_back(childU);
                }
            }
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [](const Candidate& a, const Candidate& b) noexcept { return a.priority > b.priority; });

        struct ValidAnchor
        {
            UINT64 handle{};
            int priority{};
            std::wstring type;
            DWORD probeTid{};
            bool vtcMatch{};
        };
        std::vector<ValidAnchor> valid;
        valid.reserve(candidates.size());

        for (const Candidate& cand : candidates)
        {
            ABI::Windows::System::IDispatcherQueue* dq = nullptr;
            if (!TryGetDispatcherQueueFromDependencyObject(
                    diag,
                    static_cast<InstanceHandle>(cand.handle),
                    &dq)
                || !dq)
                continue;

            DWORD probeTid = 0;
            const bool probed = TryProbeDispatcherQueueThread(dq, &probeTid);
            dq->Release();
            if (!probed)
                continue;

            const DWORD marshalNow = XamlShutdownMarshal::GetMessageWindowThreadId();
            if (!IsValidIslandDispatcherThread(probeTid, marshalTid, callerTid)
                || (marshalNow != 0 && probeTid == marshalNow))
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] FindIslandDispatcherAnchor: reject type=%s probeTid=%u "
                    L"marshalTid=%u marshalNow=%u callerTid=%u",
                    cand.type.c_str(),
                    static_cast<unsigned>(probeTid),
                    static_cast<unsigned>(marshalTid),
                    static_cast<unsigned>(marshalNow),
                    static_cast<unsigned>(callerTid));
                continue;
            }

            const bool vtcMatch = (vtcTid != 0 && probeTid == vtcTid);
            valid.push_back(ValidAnchor{
                cand.handle,
                cand.priority,
                cand.type,
                probeTid,
                vtcMatch });
        }

        if (valid.empty())
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] FindIslandDispatcherAnchor: no anchor root=%llu marshalTid=%u "
                L"vtcTid=%u",
                static_cast<unsigned long long>(rootU),
                static_cast<unsigned>(marshalTid),
                static_cast<unsigned>(vtcTid));
            return 0;
        }

        const ValidAnchor* pick = &valid.front();
        for (const ValidAnchor& va : valid)
        {
            if (vtcTid != 0)
            {
                if (va.vtcMatch && !pick->vtcMatch)
                    pick = &va;
                else if (va.vtcMatch == pick->vtcMatch && va.priority > pick->priority)
                    pick = &va;
            }
            else if (va.priority > pick->priority)
            {
                pick = &va;
            }
        }

        if (vtcTid != 0 && !pick->vtcMatch)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] FindIslandDispatcherAnchor: WARN no vtcMatch root=%llu "
                L"vtcTid=%u bestProbeTid=%u type=%s",
                static_cast<unsigned long long>(rootU),
                static_cast<unsigned>(vtcTid),
                static_cast<unsigned>(pick->probeTid),
                pick->type.c_str());
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] FindIslandDispatcherAnchor: root=%llu anchor=%llu type=%s "
            L"probeTid=%u vtcTid=%u marshalTid=%u",
            static_cast<unsigned long long>(rootU),
            static_cast<unsigned long long>(pick->handle),
            pick->type.c_str(),
            static_cast<unsigned>(pick->probeTid),
            static_cast<unsigned>(vtcTid),
            static_cast<unsigned>(marshalTid));
        return static_cast<InstanceHandle>(pick->handle);
    }

    struct ApplyHostBarChromeDqHandler final : ABI::Windows::System::IDispatcherQueueHandler
    {
        AgileDqHandlerRef m_ref;
        IXamlDiagnostics* m_diag{};
        UINT64 m_rootU{};
        std::shared_ptr<const FullReplaySnapshot> m_snap;
        IVisualTreeService* m_vts{};
        std::shared_ptr<DqSyncEvent> m_sync;
        std::shared_ptr<std::atomic<int>> m_spPatched;

        ApplyHostBarChromeDqHandler(
            IXamlDiagnostics* diag,
            UINT64 rootU,
            std::shared_ptr<const FullReplaySnapshot> snap,
            IVisualTreeService* vts,
            std::shared_ptr<DqSyncEvent> sync,
            std::shared_ptr<std::atomic<int>> spPatched) noexcept
            : m_rootU(rootU)
            , m_snap(std::move(snap))
            , m_vts(vts)
            , m_sync(std::move(sync))
            , m_spPatched(std::move(spPatched))
        {
            m_diag = diag;
            if (m_diag)
                m_diag->AddRef();
            if (m_vts)
                m_vts->AddRef();
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
                delete this;
            }
            return c;
        }

        HRESULT STDMETHODCALLTYPE Invoke() noexcept override
        {
            size_t nodes = 0;
            int patched = 0;
            if (m_diag && m_snap && m_vts)
            {
                patched = ApplyExplorerChromeFullReplayFromRoot(
                    m_diag,
                    m_rootU,
                    *m_snap,
                    m_vts,
                    &nodes);
            }
            if (m_spPatched)
                m_spPatched->store(patched, std::memory_order_release);
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] ApplyHostBarChromeOnUiQueue: root=%llu patched=%d nodes=%zu "
                L"dqTid=%u",
                static_cast<unsigned long long>(m_rootU),
                patched,
                nodes,
                static_cast<unsigned>(GetCurrentThreadId()));
            if (m_sync)
                m_sync->Signal();
            return S_OK;
        }
    };

    [[nodiscard]] bool MarshalFullReplayFromRootOnUiQueueSync(
        IXamlDiagnostics* diag,
        UINT64 rootU,
        const std::shared_ptr<const FullReplaySnapshot>& snap,
        IVisualTreeService* vts,
        InstanceHandle anchor,
        int* outPatched) noexcept
    {
        if (!diag || !snap || !vts || anchor == 0)
            return false;

        ABI::Windows::System::IDispatcherQueue* dq = nullptr;
        if (!TryGetDispatcherQueueFromDependencyObject(diag, anchor, &dq) || !dq)
            return false;

        auto sync = DqSyncEvent::Create();
        if (!sync)
            return false;

        auto spPatched = std::make_shared<std::atomic<int>>(0);
        auto* const raw = new (std::nothrow) ApplyHostBarChromeDqHandler(
            diag,
            rootU,
            snap,
            vts,
            sync,
            spPatched);
        if (!raw)
        {
            dq->Release();
            return false;
        }

        ABI::Windows::System::IDispatcherQueueHandler* cb = raw;
        boolean posted = FALSE;
        const HRESULT hr = dq->TryEnqueue(cb, &posted);
        cb->Release();
        dq->Release();
        if (FAILED(hr) || !posted)
            return false;

        constexpr DWORD kMaxWaitMs = 8000;
        if (!WaitForDqSyncEvent(sync, kMaxWaitMs))
            return false;

        const int patched = spPatched->load(std::memory_order_acquire);
        if (outPatched)
            *outPatched += patched;
        return true;
    }

    struct PostReplayIslandDqHandler final : ABI::Windows::System::IDispatcherQueueHandler
    {
        AgileDqHandlerRef m_ref;
        IXamlDiagnostics* m_diag{};
        std::shared_ptr<DqSyncEvent> m_sync;

        PostReplayIslandDqHandler(IXamlDiagnostics* diag, std::shared_ptr<DqSyncEvent> sync) noexcept
            : m_sync(std::move(sync))
        {
            m_diag = diag;
            if (m_diag)
                m_diag->AddRef();
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
                delete this;
            }
            return c;
        }

        HRESULT STDMETHODCALLTYPE Invoke() noexcept override
        {
            if (m_diag)
                XamlTapSite::RequestRepatchTabElementsOnVtcThread();
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] PostReplayIslandOnUiQueue: done dqTid=%u",
                static_cast<unsigned>(GetCurrentThreadId()));
            if (m_sync)
                m_sync->Signal();
            return S_OK;
        }
    };

    [[nodiscard]] bool MarshalPostReplayOnUiQueueSync(
        IXamlDiagnostics* diag,
        InstanceHandle anchor) noexcept
    {
        if (!diag || anchor == 0)
            return false;

        ABI::Windows::System::IDispatcherQueue* dq = nullptr;
        if (!TryGetDispatcherQueueFromDependencyObject(diag, anchor, &dq) || !dq)
            return false;

        auto sync = DqSyncEvent::Create();
        if (!sync)
            return false;

        auto* const raw = new (std::nothrow) PostReplayIslandDqHandler(diag, sync);
        if (!raw)
        {
            dq->Release();
            return false;
        }

        ABI::Windows::System::IDispatcherQueueHandler* cb = raw;
        boolean posted = FALSE;
        const HRESULT hr = dq->TryEnqueue(cb, &posted);
        cb->Release();
        dq->Release();
        if (FAILED(hr) || !posted)
            return false;

        constexpr DWORD kMaxWaitMs = 8000;
        return WaitForDqSyncEvent(sync, kMaxWaitMs);
    }

    struct RestoreHostBarChromeDqHandler final : ABI::Windows::System::IDispatcherQueueHandler
    {
        AgileDqHandlerRef m_ref;
        IXamlDiagnostics* m_diag{};
        std::shared_ptr<DqSyncEvent> m_sync;

        RestoreHostBarChromeDqHandler(IXamlDiagnostics* diag, std::shared_ptr<DqSyncEvent> sync) noexcept
            : m_sync(std::move(sync))
        {
            m_diag = diag;
            if (m_diag)
                m_diag->AddRef();
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
                delete this;
            }
            return c;
        }

        HRESULT STDMETHODCALLTYPE Invoke() noexcept override
        {
            if (m_diag)
            {
                const bool previousUiQueueRestore = g_hostBarRestoreOnIslandUiQueue;
                g_hostBarRestoreOnIslandUiQueue = true;
                XamlTapSite::RestoreExplorerHostBarChrome(m_diag);
                g_hostBarRestoreOnIslandUiQueue = previousUiQueueRestore;
            }
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] RestoreHostBarChromeOnUiQueue: done dqTid=%u",
                static_cast<unsigned>(GetCurrentThreadId()));
            if (m_sync)
                m_sync->Signal();
            return S_OK;
        }
    };

    [[nodiscard]] bool MarshalRestoreHostBarChromeOnUiQueueSync(
        IXamlDiagnostics* diag,
        InstanceHandle anchor) noexcept
    {
        if (!diag || anchor == 0)
            return false;

        ABI::Windows::System::IDispatcherQueue* dq = nullptr;
        if (!TryGetDispatcherQueueFromDependencyObject(diag, anchor, &dq) || !dq)
            return false;

        auto sync = DqSyncEvent::Create();
        if (!sync)
            return false;

        auto* const raw = new (std::nothrow) RestoreHostBarChromeDqHandler(diag, sync);
        if (!raw)
        {
            dq->Release();
            return false;
        }

        ABI::Windows::System::IDispatcherQueueHandler* cb = raw;
        boolean posted = FALSE;
        const HRESULT hr = dq->TryEnqueue(cb, &posted);
        cb->Release();
        dq->Release();
        if (FAILED(hr) || !posted)
            return false;

        constexpr DWORD kMaxWaitMs = 8000;
        return WaitForDqSyncEvent(sync, kMaxWaitMs);
    }

    [[nodiscard]] bool TryApplyExplorerHostBarChromeOnUiQueueSyncImpl(
        IXamlDiagnostics* diag,
        int* outPatched) noexcept
    {
        if (outPatched)
            *outPatched = 0;
        if (!diag)
            return false;
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
            return false;

        XamlShutdownMarshal::EnsureMessageWindow();

        FullReplaySnapshot snap;
        if (!BuildFullReplaySnapshot(snap))
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] TryApplyExplorerHostBarChromeOnUiQueueSync: no DWXS roots");
            return false;
        }

        g_disableHostBarPatching.store(false, std::memory_order_release);
        XamlHostBarPatcher::NotifyExplorerStartupPulse();
        XamlHostBarPatcher::StopHostBarRepatchTimer();
        g_loggedPatchSkipAlreadyInRegistry.store(false, std::memory_order_release);
        g_loggedSetPropertyFailInReplay.store(false, std::memory_order_release);

        size_t patchRegistrySize = 0;
        {
            std::lock_guard lock(g_xamlMutex);
            patchRegistrySize = g_hostBarPropertyPatches.size();
        }
        if (g_replayRestoreAfterOff.exchange(false, std::memory_order_acq_rel) && patchRegistrySize > 0)
        {
            RestoreHostBarPropertyPatches(diag);
        }

        g_inFullReplayPass.store(true, std::memory_order_release);
        struct FullReplayPassGuard
        {
            ~FullReplayPassGuard() noexcept
            {
                g_inFullReplayPass.store(false, std::memory_order_release);
            }
        } fullReplayPassGuard;
        (void)fullReplayPassGuard;

        IVisualTreeService* vts = nullptr;
        if (FAILED(diag->QueryInterface(IID_IVisualTreeService, reinterpret_cast<void**>(&vts))) || !vts)
            return false;

        const auto snapShared = std::make_shared<FullReplaySnapshot>(std::move(snap));
        const DWORD marshalTid = XamlShutdownMarshal::GetMessageWindowThreadId();
        const DWORD callerTid = GetCurrentThreadId();
        const DWORD vtcTid = g_vtcCallbackTid.load(std::memory_order_acquire);

        int totalPatched = 0;
        unsigned islandsAttempted = 0;

        for (const UINT64 rootU : snapShared->dwxsRoots)
        {
            const InstanceHandle anchor = FindIslandDispatcherAnchor(
                diag,
                rootU,
                *snapShared,
                marshalTid,
                callerTid,
                vtcTid);
            if (anchor == 0)
                continue;

            ++islandsAttempted;
            int rootPatched = 0;
            if (!MarshalFullReplayFromRootOnUiQueueSync(
                    diag,
                    rootU,
                    snapShared,
                    vts,
                    anchor,
                    &rootPatched))
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] TryApplyExplorerHostBarChromeOnUiQueueSync: marshal failed "
                    L"root=%llu",
                    static_cast<unsigned long long>(rootU));
                continue;
            }
            totalPatched += rootPatched;
            (void)MarshalPostReplayOnUiQueueSync(diag, anchor);
        }

        EnsureLazyHostBarRepatchTimer();
        vts->Release();

        if (outPatched)
            *outPatched = totalPatched;

        if (totalPatched > 0)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] TryApplyExplorerHostBarChromeOnUiQueueSync: OK patched=%d "
                L"islands=%u curTid=%u",
                totalPatched,
                islandsAttempted,
                static_cast<unsigned>(callerTid));
            return true;
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] TryApplyExplorerHostBarChromeOnUiQueueSync: WARN patched=0 "
            L"islandsAttempted=%u",
            islandsAttempted);
        return false;
    }

    [[nodiscard]] bool TryRestoreExplorerHostBarChromeOnUiQueueSyncImpl(
        IXamlDiagnostics* diag,
        bool* outRestored) noexcept
    {
        if (outRestored)
            *outRestored = false;
        if (!diag)
            return false;

        XamlShutdownMarshal::EnsureMessageWindow();
        XamlHostBarPatcher::StopHostBarRepatchTimer();

        size_t patchRegistrySize = 0;
        {
            std::lock_guard lock(g_xamlMutex);
            patchRegistrySize = g_hostBarPropertyPatches.size();
        }

        FullReplaySnapshot snap;
        if (!BuildFullReplaySnapshot(snap) || snap.dwxsRoots.empty())
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] TryRestoreExplorerHostBarChromeOnUiQueueSync: no DWXS roots "
                L"(patches=%zu)",
                patchRegistrySize);
            return false;
        }

        const DWORD marshalTid = XamlShutdownMarshal::GetMessageWindowThreadId();
        const DWORD callerTid = GetCurrentThreadId();
        // ShellExt-меню травит g_vtcCallbackTid (popup). FindIslandDispatcherAnchor предпочитает
        // vtcMatch → restore уходит на DQ popup, host bar не восстанавливается (GS-stop OK).
        // На shutdown игнорируем VTC и берём якорь по priority (DWXS host bar).
        const bool shutdownRestore = GlassExplorerCom::g_shutdownDone.load(std::memory_order_acquire);
        const DWORD vtcTid = shutdownRestore
            ? 0
            : g_vtcCallbackTid.load(std::memory_order_acquire);

        for (const UINT64 rootU : snap.dwxsRoots)
        {
            const InstanceHandle anchor = FindIslandDispatcherAnchor(
                diag,
                rootU,
                snap,
                marshalTid,
                callerTid,
                vtcTid);
            if (anchor == 0)
                continue;

            if (!MarshalRestoreHostBarChromeOnUiQueueSync(diag, anchor))
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree] TryRestoreExplorerHostBarChromeOnUiQueueSync: marshal failed "
                    L"root=%llu",
                    static_cast<unsigned long long>(rootU));
                continue;
            }

            if (outRestored)
                *outRestored = true;
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] TryRestoreExplorerHostBarChromeOnUiQueueSync: OK patches=%zu "
                L"curTid=%u",
                patchRegistrySize,
                static_cast<unsigned>(callerTid));
            return true;
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] TryRestoreExplorerHostBarChromeOnUiQueueSync: WARN no island anchor "
            L"patches=%zu",
            patchRegistrySize);
        return false;
    }

    [[nodiscard]] int ApplyExplorerChromeIncremental(
        IXamlDiagnostics* diag,
        VisualElement element,
        InstanceHandle /*parentHandle*/) noexcept
    {
        XamlHostBarPatcher::TrackElementMetadata(element);
        if (!XamlHostBarPatcher::IsExplorerXamlPatchingAllowed())
            return 0;

        if (XamlHostBarPatcher::MatchesExplorerDiagnosticIsland(element.Type, element.Name))
        {
            if (!XamlHostBarPatcher::AncestorChainIncludesShellChrome(diag, element.Handle))
            {
                IInspectable* inspDiag = nullptr;
                if (SUCCEEDED(diag->GetIInspectableFromHandle(element.Handle, &inspDiag)) && inspDiag)
                {
                    if (SUCCEEDED(XamlHostBarPatcher::TryDiagnosticBlackenBackground(
                            diag, element.Handle, inspDiag)))
                    {
                        inspDiag->Release();
                        return 0;
                    }
                    inspDiag->Release();
                }
            }
        }

        return TryPatchFileExplorerHostBars(diag, element);
    }

    [[nodiscard]] VisualElement MakeVisualElementFromCache(
        UINT64 handleKey,
        const std::wstring& typeStr,
        const std::wstring& nameStr) noexcept
    {
        VisualElement ve{};
        ve.Handle = static_cast<InstanceHandle>(handleKey);
        ve.NumChildren = 0;
        ve.Type = typeStr.empty() ? nullptr : SysAllocString(typeStr.c_str());
        ve.Name = nameStr.empty() ? nullptr : SysAllocString(nameStr.c_str());
        return ve;
    }

    void FreeVisualElementStrings(VisualElement& ve) noexcept
    {
        if (ve.Type)
        {
            SysFreeString(ve.Type);
            ve.Type = nullptr;
        }
        if (ve.Name)
        {
            SysFreeString(ve.Name);
            ve.Name = nullptr;
        }
    }

    [[nodiscard]] int ApplyExplorerChromeFullReplayFromRoot(
        IXamlDiagnostics* diag,
        UINT64 rootU,
        const FullReplaySnapshot& snap,
        IVisualTreeService* vts,
        size_t* outNodes) noexcept
    {
        if (outNodes)
            *outNodes = 0;
        if (!diag || !vts || rootU == 0)
            return 0;

        size_t nodes = 0;
        int totalPatched = 0;
        constexpr size_t kMaxNodes = 4096;

        std::vector<std::pair<UINT64, UINT64>> queue;
        queue.emplace_back(rootU, 0u);
        std::unordered_set<UINT64> seen;
        seen.insert(rootU);

        for (size_t qi = 0; qi < queue.size() && nodes < kMaxNodes; ++qi)
        {
            const UINT64 cur = queue[qi].first;
            const UINT64 parentU = queue[qi].second;

            std::wstring typeStr;
            std::wstring nameStr;
            const auto typeIt = snap.typeOf.find(cur);
            if (typeIt != snap.typeOf.end())
                typeStr = typeIt->second;
            const auto nameIt = snap.nameOf.find(cur);
            if (nameIt != snap.nameOf.end())
                nameStr = nameIt->second;

            if (nameStr.empty())
            {
                std::wstring resolved;
                XamlHostBarPatcher::Detail::TryAppendNameFromPropertyChain(
                    vts, static_cast<InstanceHandle>(cur), &resolved);
                if (!resolved.empty())
                {
                    nameStr = std::move(resolved);
                    std::lock_guard lock(g_xamlMutex);
                    g_nameOf[cur] = nameStr;
                }
            }

            if (!typeStr.empty())
            {
                VisualElement ve = MakeVisualElementFromCache(cur, typeStr, nameStr);
                if (parentU != 0)
                {
                    XamlHostBarPatcher::TrackParent(
                        static_cast<InstanceHandle>(cur),
                        static_cast<InstanceHandle>(parentU));
                }
                totalPatched += XamlHostBarPatcher::ApplyExplorerChrome(
                    diag,
                    XamlHostBarPatcher::ExplorerChromeApplyScope::Incremental,
                    &ve,
                    static_cast<InstanceHandle>(parentU));
                FreeVisualElementStrings(ve);
                ++nodes;
            }

            const auto childIt = snap.childrenOf.find(cur);
            if (childIt != snap.childrenOf.end())
            {
                for (UINT64 childU : childIt->second)
                {
                    if (seen.insert(childU).second)
                        queue.emplace_back(childU, cur);
                }
            }
        }

        if (outNodes)
            *outNodes = nodes;
        return totalPatched;
    }

    [[nodiscard]] int ApplyExplorerChromeFullReplayImpl(IXamlDiagnostics* diag) noexcept
    {
        if (!diag)
            return 0;
        if (!Shared::RootPolicyQuery::IsRootGlassRenderingEnabled())
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] ApplyExplorerChrome FullReplay: early-exit gate=rootPolicy");
            return 0;
        }

        g_disableHostBarPatching.store(false, std::memory_order_release);
        XamlHostBarPatcher::NotifyExplorerStartupPulse();
        XamlHostBarPatcher::StopHostBarRepatchTimer();
        g_loggedPatchSkipAlreadyInRegistry.store(false, std::memory_order_release);
        g_loggedSetPropertyFailInReplay.store(false, std::memory_order_release);

        size_t patchRegistrySize = 0;
        {
            std::lock_guard lock(g_xamlMutex);
            patchRegistrySize = g_hostBarPropertyPatches.size();
        }
        if (g_replayRestoreAfterOff.exchange(false, std::memory_order_acq_rel) && patchRegistrySize > 0)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] ApplyExplorerChrome FullReplay: WARN stale patchRegistrySize=%zu "
                L"after OFF — restore-before-replay",
                patchRegistrySize);
            RestoreHostBarPropertyPatches(diag);
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] ApplyExplorerChrome FullReplay: patchRegistrySize=0 after "
                L"restore-before-replay");
        }
        else if (patchRegistrySize > 0)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] ApplyExplorerChrome FullReplay: patchRegistrySize=%zu "
                L"(live VTC patches — skip restore-before-replay)",
                patchRegistrySize);
        }
        else
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] ApplyExplorerChrome FullReplay: patchRegistrySize=0");
        }

        g_inFullReplayPass.store(true, std::memory_order_release);
        struct FullReplayPassGuard
        {
            ~FullReplayPassGuard() noexcept
            {
                g_inFullReplayPass.store(false, std::memory_order_release);
            }
        } fullReplayPassGuard;
        (void)fullReplayPassGuard;

        std::unordered_map<UINT64, std::wstring> nameSnapshot;
        std::unordered_map<UINT64, std::wstring> typeSnapshot;
        std::unordered_map<UINT64, UINT64> parentSnapshot;
        {
            std::lock_guard lock(g_xamlMutex);
            nameSnapshot = g_nameOf;
            typeSnapshot = g_typeOf;
            parentSnapshot = g_parentOf;
        }

        FullReplaySnapshot snap;
        snap.nameOf = std::move(nameSnapshot);
        snap.typeOf = std::move(typeSnapshot);
        snap.parentOf = std::move(parentSnapshot);
        snap.childrenOf.reserve(snap.parentOf.size());
        for (const auto& [child, parent] : snap.parentOf)
        {
            if (child != 0 && parent != 0)
                snap.childrenOf[parent].push_back(child);
        }
        for (const auto& [handleKey, typeStr] : snap.typeOf)
        {
            if (handleKey == 0 || typeStr.empty())
                continue;
            if (typeStr.find(L"DesktopWindowXamlSource") != std::wstring::npos)
                snap.dwxsRoots.push_back(handleKey);
        }

        IVisualTreeService* vts = nullptr;
        if (FAILED(diag->QueryInterface(IID_IVisualTreeService, reinterpret_cast<void**>(&vts))) || !vts)
            return 0;

        size_t nodes = 0;
        int totalPatched = 0;

        for (UINT64 rootU : snap.dwxsRoots)
        {
            size_t rootNodes = 0;
            totalPatched += ApplyExplorerChromeFullReplayFromRoot(diag, rootU, snap, vts, &rootNodes);
            nodes += rootNodes;
        }

        vts->Release();

        ProcessPendingLazyHostBars(diag);
        XamlTapSite::RequestRepatchTabElementsOnVtcThread();
        EnsureLazyHostBarRepatchTimer();

        DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] ApplyExplorerChrome FullReplay: nodes=%zu patched=%d",
            nodes,
            totalPatched);
        if (totalPatched == 0 && nodes > 0)
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[ExplorerCompat][XamlTree] ApplyExplorerChrome FullReplay: WARN patched=0 after replay");
        }
        return totalPatched;
    }
} // namespace XamlHostBarPatcher::Internal
