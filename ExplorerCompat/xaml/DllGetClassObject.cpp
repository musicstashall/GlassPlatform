// Без ExplorerPch: в PCH подключаются заголовки, из‑за которых MSVC видит
// несовпадение спецификаций исключений с объявлением DllGetClassObject из combaseapi.
#include <Windows.h>
#include <atomic>
#include <objbase.h>
#include <objidl.h>
#include <DebugLog.h>

#include "xaml/XamlClassFactory.hpp"
#include "xaml/XamlComLifetime.hpp"
#include "xaml/XamlGlassIds.hpp"
#include "xaml/XamlDiagInit.hpp"

namespace GlassExplorerCom
{
    std::atomic<long> g_objectCount{ 0 };
    std::atomic<long> g_serverLocks{ 0 };
    std::atomic<bool> g_shutdownDone{ false };
}

STDAPI DllCanUnloadNow()
{
    if (GlassExplorerCom::g_shutdownDone.load(std::memory_order_acquire))
        return S_OK;

    if (GlassExplorerCom::g_serverLocks.load(std::memory_order_acquire) != 0)
        return S_FALSE;
    if (GlassExplorerCom::g_objectCount.load(std::memory_order_acquire) != 0)
        return S_FALSE;
    return S_OK;
}

STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid, _Outptr_ LPVOID FAR* ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;

    LPOLESTR clsidStr = nullptr;
    LPOLESTR riidStr = nullptr;
    const HRESULT hrClsidStr = StringFromCLSID(rclsid, &clsidStr);
    const HRESULT hrRiidStr = StringFromIID(riid, &riidStr);
    DebugLog(
        DBG_IMPORTANT,
        L"[ExplorerCompat][ComProbe] DllGetClassObject: tid=%u clsid=%s riid=%s",
        static_cast<unsigned>(GetCurrentThreadId()),
        SUCCEEDED(hrClsidStr) && clsidStr ? clsidStr : L"<n/a>",
        SUCCEEDED(hrRiidStr) && riidStr ? riidStr : L"<n/a>");

    if (IsEqualCLSID(rclsid, CLSID_GlassXamlTapSite))
    {
        static WinUIXamlClassFactory s_winUiFactory;
        const HRESULT hr = s_winUiFactory.QueryInterface(riid, reinterpret_cast<void**>(ppv));
        DebugLog(DBG_IMPORTANT, L"[ExplorerCompat][ComProbe] DllGetClassObject WinUI hr=0x%08X", static_cast<unsigned>(hr));
        if (clsidStr)
            CoTaskMemFree(clsidStr);
        if (riidStr)
            CoTaskMemFree(riidStr);
        return hr;
    }

    if (clsidStr)
        CoTaskMemFree(clsidStr);
    if (riidStr)
        CoTaskMemFree(riidStr);
    DebugLog(DBG_WARNING, L"[ExplorerCompat][ComProbe] DllGetClassObject CLASS_E_CLASSNOTAVAILABLE");
    return CLASS_E_CLASSNOTAVAILABLE;
}
