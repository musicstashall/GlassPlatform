#include "ExplorerPch.h"
#include "xaml/XamlVisualTreeWatcher.hpp"
#include "xaml/Patcher/XamlHostBarPatcher.h"
#include "xaml/XamlDiagTiming.hpp"
#include "xaml/XamlTapSite.hpp"

#include <inspectable.h>
#include <windows.ui.xaml.shapes.h>
#include <windows.ui.xaml.media.h>

#include <DebugLog.h>
#include "xaml/XamlComLifetime.hpp"
#include <array>
#include <atomic>

namespace
{
    [[nodiscard]] bool IsIslandPrimeClass(const wchar_t* cls) noexcept
    {
        if (!cls)
            return false;
        return wcscmp(cls, L"SHELLDLL_DefView") == 0
            || wcscmp(cls, L"FileExplorer_FolderViewIsland") == 0
            || wcscmp(cls, L"Microsoft.UI.Content.DesktopChildSiteBridge") == 0
            || wcscmp(cls, L"XamlExplorerHostIslandWindow") == 0;
    }

    void PrimeBlackForHwnd(HWND hWnd) noexcept
    {
        if (!XamlHostBarPatcher::IsExplorerXamlPatchingAllowed())
            return;
        if (!hWnd || !IsWindow(hWnd))
            return;

        wchar_t cls[160]{};
        if (GetClassNameW(hWnd, cls, static_cast<int>(std::size(cls))) <= 0)
            return;
        if (!IsIslandPrimeClass(cls))
            return;

        RECT rc{};
        if (!GetClientRect(hWnd, &rc))
            return;
        if (rc.right <= rc.left || rc.bottom <= rc.top)
            return;

        HDC hdc = GetDC(hWnd);
        if (!hdc)
            return;

        FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        ReleaseDC(hWnd, hdc);

        /*DebugLog(
            DBG_IMPORTANT,
            L"[ExplorerCompat][XamlTree] PrimeBlack: hwnd=0x%p class=%s rc={%ld,%ld,%ld,%ld}",
            hWnd,
            cls,
            rc.left,
            rc.top,
            rc.right,
            rc.bottom);*/
    }

    void PrimeExplorerIslandBlackOncePerPulse() noexcept
    {
        if (!XamlHostBarPatcher::IsExplorerXamlPatchingAllowed())
            return;
        static ULONGLONG s_lastPrimeTick = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - s_lastPrimeTick < 120)
            return;
        s_lastPrimeTick = now;

        EnumWindows(
            [](HWND top, LPARAM) -> BOOL {
                DWORD pid = 0;
                if (!GetWindowThreadProcessId(top, &pid) || pid != GetCurrentProcessId())
                    return TRUE;

                PrimeBlackForHwnd(top);
                EnumChildWindows(
                    top,
                    [](HWND child, LPARAM) -> BOOL {
                        PrimeBlackForHwnd(child);
                        return TRUE;
                    },
                    0);
                return TRUE;
            },
            0);
    }

    [[nodiscard]] bool ShouldPrimeOnElementAdd(const VisualElement& element) noexcept
    {
        if (!element.Type)
            return false;

        if (wcscmp(element.Type, L"Microsoft.UI.Xaml.Hosting.DesktopWindowXamlSource") == 0)
            return true;
        if (wcscmp(element.Type, L"FileExplorerExtensions.HomeViewControl") == 0)
            return true;
        if (wcscmp(element.Type, L"FileExplorerExtensions.HomeViewListView") == 0)
            return true;
        if (element.Name && wcscmp(element.Name, L"HomeViewRootGrid") == 0)
            return true;
        return false;
    }
}

// Первые N событий Add в сессии Advise (сброс в GlassExplorerXamlDiag_BeginAdviseSequence).
// Счётчик «#n/80» не расходуется на чисто explorer-шум — см. ShouldCountVtcAddFilteredSample.
static constexpr unsigned kVtcAddSampleMax = 80;
static std::atomic<unsigned> s_vtcAddSampleSeq{ 0 };

// ============================================================================
//  Сэмплер VTC Add (диагностика «что вообще приходит в колбэк»)
// ============================================================================

void XamlVisualTreeWatcher::ResetVtcAddSampleCounter() noexcept
{
    s_vtcAddSampleSeq.store(0, std::memory_order_release);
}

void XamlVisualTreeWatcher::PrimeExplorerIslandsAttachPulse() noexcept
{
    PrimeExplorerIslandBlackOncePerPulse();
}

// ============================================================================
//  Конструктор / Деструктор
// ============================================================================

XamlVisualTreeWatcher::XamlVisualTreeWatcher(IXamlDiagnostics* diagnostics) noexcept
{
    m_xamlDiagnostics = diagnostics;
    if (m_xamlDiagnostics)
        m_xamlDiagnostics->AddRef();
}

XamlVisualTreeWatcher::~XamlVisualTreeWatcher()
{
    if (m_xamlDiagnostics)
        m_xamlDiagnostics->Release();
}

// ============================================================================
//  IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE XamlVisualTreeWatcher::QueryInterface(REFIID riid, void** ppvObject) noexcept
{
    if (!ppvObject)
        return E_POINTER;
    *ppvObject = nullptr;
    if (riid == IID_IUnknown || riid == IID_IVisualTreeServiceCallback || riid == IID_IVisualTreeServiceCallback2)
    {
        *ppvObject = static_cast<IVisualTreeServiceCallback2*>(this);
        AddRef();
        return S_OK;
    }
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE XamlVisualTreeWatcher::AddRef() noexcept
{
    return m_ref.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE XamlVisualTreeWatcher::Release() noexcept
{
    const ULONG c = m_ref.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (c == 0)
        delete this;
    return c;
}

// ============================================================================
//  RestoreAllFills — вызывается при shutdown
// ============================================================================

void XamlVisualTreeWatcher::RestoreAllFills() noexcept
{
    if (!m_xamlDiagnostics)
        return;
    XamlHostBarPatcher::RestoreExplorerChrome(
        m_xamlDiagnostics, XamlHostBarPatcher::ExplorerChromeRestoreScope::Full);
    XamlHostBarPatcher::ClearPatchedExplorerShellHwnds();
}

// ============================================================================
//  OnVisualTreeChange
// ============================================================================

HRESULT STDMETHODCALLTYPE XamlVisualTreeWatcher::OnVisualTreeChange(
    ParentChildRelation relation,
    VisualElement element,
    VisualMutationType mutationType) noexcept
{
    if (!m_xamlDiagnostics)
        return S_OK;

    GlassExplorerXamlDiag_OnVisualTreeCallback(GetCurrentThreadId());
    XamlTapSite::TryConsumePendingApplyHostBarChrome(m_xamlDiagnostics);
    XamlTapSite::TryConsumePendingRestoreHostBarChrome(m_xamlDiagnostics);
    XamlTapSite::TryConsumePendingRepatchTabElements(m_xamlDiagnostics);

    // Реентерабельность: SetProperty/IInspectable из колбэка могут рекурсивно вызвать OnVisualTreeChange.
    thread_local unsigned tl_onVtcDepth = 0;
    struct OnVtcDepthGuard
    {
        OnVtcDepthGuard() noexcept { ++tl_onVtcDepth; }
        ~OnVtcDepthGuard() noexcept { --tl_onVtcDepth; }
    } depthGuard;
    const bool allowHeavyMutations = (tl_onVtcDepth <= 1);

    if (mutationType == Add)
    {
        if (XamlHostBarPatcher::ShouldCountVtcAddFilteredSample(element.Type, element.Name))
        {
            /*const unsigned sampleIdx = s_vtcAddSampleSeq.fetch_add(1, std::memory_order_relaxed);
            if (sampleIdx < kVtcAddSampleMax)
            {
                DebugLog(
                    DBG_IMPORTANT,
                    L"[ExplorerCompat][XamlTree][VtcAddSample] #%u/%u type=%ls name=%ls parent=%llu children=%u depth=%u",
                    sampleIdx + 1u,
                    kVtcAddSampleMax,
                    element.Type ? element.Type : L"(null)",
                    element.Name ? element.Name : L"",
                    static_cast<unsigned long long>(relation.Parent),
                    static_cast<unsigned>(element.NumChildren),
                    static_cast<unsigned>(tl_onVtcDepth));
            }*/
        }

        // Запоминаем parent→child и type/name для repatch после live OFF.
        XamlHostBarPatcher::TrackParent(element.Handle, relation.Parent);
        XamlHostBarPatcher::TrackElementMetadata(element);

        if (!XamlHostBarPatcher::IsExplorerXamlPatchingAllowed())
            return S_OK;

        if (ShouldPrimeOnElementAdd(element))
            PrimeExplorerIslandBlackOncePerPulse();

        if (allowHeavyMutations)
        {
            (void)XamlHostBarPatcher::ApplyExplorerChrome(
                m_xamlDiagnostics,
                XamlHostBarPatcher::ExplorerChromeApplyScope::Incremental,
                &element,
                relation.Parent);
        }
        else if (element.Type && XamlHostBarPatcher::TypeOrNameHintsExplorerUi(element.Type, element.Name))
        {
            if (!XamlHostBarPatcher::IsExplorerXamlPatchingAllowed())
                return S_OK;
            // Реентрантность (depth>1): тяжёлые ветки отключены — узкий патч host bar.
            (void)XamlHostBarPatcher::ApplyExplorerChrome(
                m_xamlDiagnostics,
                XamlHostBarPatcher::ExplorerChromeApplyScope::Incremental,
                &element,
                relation.Parent);
        }

        return S_OK;
    }

    if (mutationType == Remove)
    {
        XamlHostBarPatcher::OnElementRemoved(m_xamlDiagnostics, element);
    }

    return S_OK;
}

// ============================================================================
//  OnElementStateChanged (stub)
// ============================================================================

HRESULT STDMETHODCALLTYPE XamlVisualTreeWatcher::OnElementStateChanged(
    InstanceHandle /*element*/,
    VisualElementState /*elementState*/,
    LPCWSTR /*context*/) noexcept
{
    return S_OK;
}