#include "ExplorerPch.h"
#include "xaml/XamlClassFactory.hpp"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/XamlTapSite.hpp"

// IClassFactory для WinUI TAP. UWP-фабрика удалена (зона трея переехала в TaskbarCompat.dll).

template <typename Tag>
HRESULT STDMETHODCALLTYPE XamlClassFactory<Tag>::QueryInterface(REFIID riid, void** ppvObject) noexcept
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

template <typename Tag>
ULONG STDMETHODCALLTYPE XamlClassFactory<Tag>::AddRef() noexcept
{
    return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <typename Tag>
ULONG STDMETHODCALLTYPE XamlClassFactory<Tag>::Release() noexcept
{
    const ULONG c = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (c == 0)
        m_ref.fetch_add(1, std::memory_order_relaxed);
    return c;
}

template <>
HRESULT STDMETHODCALLTYPE XamlClassFactory<XamlTapSiteTag>::CreateInstance(
    IUnknown* pUnkOuter, REFIID riid, void** ppvObject) noexcept
{
    if (!ppvObject)
        return E_POINTER;
    *ppvObject = nullptr;
    if (pUnkOuter)
        return CLASS_E_NOAGGREGATION;

    auto* site = new (std::nothrow) XamlTapSite();
    if (!site)
        return E_OUTOFMEMORY;

    HRESULT hr = site->QueryInterface(riid, ppvObject);
    site->Release();
    return hr;
}

template <typename Tag>
HRESULT STDMETHODCALLTYPE XamlClassFactory<Tag>::LockServer(BOOL fLock) noexcept
{
    if (fLock)
        GlassExplorerCom::g_serverLocks.fetch_add(1, std::memory_order_relaxed);
    else
        GlassExplorerCom::g_serverLocks.fetch_sub(1, std::memory_order_relaxed);
    return S_OK;
}

template struct XamlClassFactory<XamlTapSiteTag>;
