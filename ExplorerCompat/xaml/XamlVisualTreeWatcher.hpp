#pragma once
#include <xamlOM.h>
#include <unknwn.h>
#include <atomic>

struct IXamlDiagnostics;

// IVisualTreeServiceCallback2: лог дерева + подмена Fill у Rectangle (IShape).
class XamlVisualTreeWatcher final : public IVisualTreeServiceCallback2
{
public:
    explicit XamlVisualTreeWatcher(IXamlDiagnostics* diagnostics) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) noexcept override;
    ULONG STDMETHODCALLTYPE AddRef() noexcept override;
    ULONG STDMETHODCALLTYPE Release() noexcept override;

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation relation,
        VisualElement element,
        VisualMutationType mutationType) noexcept override;

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle element,
        VisualElementState elementState,
        LPCWSTR context) noexcept override;

    void RestoreAllFills() noexcept;

    // Сброс сэмплера логов Add (вызывать после успешного Advise — см. XamlDiagTiming).
    static void ResetVtcAddSampleCounter() noexcept;

    // GDI prime островов — тот же путь, что при VTC Add (attach pulse).
    static void PrimeExplorerIslandsAttachPulse() noexcept;

private:
    ~XamlVisualTreeWatcher();

    std::atomic<ULONG> m_ref{ 1 };
    IXamlDiagnostics* m_xamlDiagnostics{ nullptr };
};
