#pragma once
#include <unknwn.h>
#include <atomic>

// IClassFactory для CLSID GlassXamlTapSite (WinUI). UWP-фабрика удалена вместе
// с UwpXamlTapSite (зона трея переехала в TaskbarCompat.dll).
template <typename Tag>
struct XamlClassFactory final : IClassFactory
{
    XamlClassFactory() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) noexcept override;
    ULONG STDMETHODCALLTYPE AddRef() noexcept override;
    ULONG STDMETHODCALLTYPE Release() noexcept override;

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) noexcept override;
    HRESULT STDMETHODCALLTYPE LockServer(BOOL fLock) noexcept override;

private:
    std::atomic<ULONG> m_ref{ 1 };
};

struct XamlTapSiteTag {};

using WinUIXamlClassFactory = XamlClassFactory<XamlTapSiteTag>;
