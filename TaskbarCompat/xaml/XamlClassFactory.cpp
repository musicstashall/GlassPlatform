#include "TaskbarPch.h"
#include "xaml/XamlClassFactory.hpp"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/TaskbarTapSite.hpp"

#include <DebugLog.h>

namespace TaskbarCompat
{
    HRESULT STDMETHODCALLTYPE TaskbarTapSiteFactory::QueryInterface(
        REFIID riid, void** ppvObject) noexcept
    {
        if (!ppvObject)
            return E_POINTER;
        *ppvObject = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        {
            *ppvObject = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE TaskbarTapSiteFactory::AddRef() noexcept
    {
        return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE TaskbarTapSiteFactory::Release() noexcept
    {
        // Singleton-фабрика (см. DllGetClassObject) — никогда не уничтожается.
        const ULONG c = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0)
            m_ref.fetch_add(1, std::memory_order_relaxed);
        return c;
    }

    HRESULT STDMETHODCALLTYPE TaskbarTapSiteFactory::CreateInstance(
        IUnknown* pUnkOuter, REFIID riid, void** ppvObject) noexcept
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][Factory] CreateInstance enter tid=%u outer=%d",
            static_cast<unsigned>(GetCurrentThreadId()),
            pUnkOuter ? 1 : 0);

        if (!ppvObject)
            return E_POINTER;
        *ppvObject = nullptr;
        if (pUnkOuter)
            return CLASS_E_NOAGGREGATION;

        auto* site = new (std::nothrow) TaskbarTapSite();
        if (!site)
            return E_OUTOFMEMORY;

        const HRESULT hr = site->QueryInterface(riid, ppvObject);
        site->Release();

        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][Factory] CreateInstance leave hr=0x%08X",
            static_cast<unsigned>(hr));
        return hr;
    }

    HRESULT STDMETHODCALLTYPE TaskbarTapSiteFactory::LockServer(BOOL fLock) noexcept
    {
        if (fLock)
            TaskbarCompatCom::g_serverLocks.fetch_add(1, std::memory_order_relaxed);
        else
            TaskbarCompatCom::g_serverLocks.fetch_sub(1, std::memory_order_relaxed);
        return S_OK;
    }
}
