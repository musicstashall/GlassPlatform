#include "TaskbarPch.h"
#include "xaml/TrayMenuFlyoutPanelRegistry.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"

#include <TrayXamlPaint.h>

#include <DebugLog.h>

#include <vector>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace TaskbarCompat
{
    namespace
    {
        namespace wux = winrt::Windows::UI::Xaml;

        constexpr DWORD kRestoreWaitMs = 250;
        constexpr int kMaxResolveDepth = 4;

        [[nodiscard]] winrt::Windows::UI::Color ColorFromArgb(DWORD argb) noexcept
        {
            return winrt::Windows::UI::Color{
                static_cast<uint8_t>((argb >> 24) & 0xFFu),
                static_cast<uint8_t>((argb >> 16) & 0xFFu),
                static_cast<uint8_t>((argb >> 8) & 0xFFu),
                static_cast<uint8_t>(argb & 0xFFu),
            };
        }

        [[nodiscard]] bool ThicknessAnyPositive(const wux::Thickness& t) noexcept
        {
            return t.Left > 0.0 || t.Top > 0.0 || t.Right > 0.0 || t.Bottom > 0.0;
        }

        [[nodiscard]] double QueryVisibleHeightDip(wux::FrameworkElement const& fe) noexcept
        {
            if (!fe)
                return 0.0;
            try
            {
                const double h = fe.ActualHeight();
                return h > 0.0 ? h : 0.0;
            }
            catch (...)
            {
                return 0.0;
            }
        }

        [[nodiscard]] bool BorderLayoutReady(wux::Controls::Border const& border) noexcept
        {
            if (!border)
                return false;
            try
            {
                return border.ActualWidth() >= 1.0 && border.ActualHeight() >= 1.0;
            }
            catch (...)
            {
                return false;
            }
        }

        // Chrome MenuFlyout: первый Border. GetElementVisual только на нём (не на Presenter).
        [[nodiscard]] wux::Controls::Border FindChromeBorder(
            wux::DependencyObject root,
            int depth) noexcept
        {
            if (!root)
                return nullptr;
            try
            {
                const auto walk = [&](auto&& self, wux::DependencyObject node, int d)
                    -> wux::Controls::Border
                {
                    if (!node || d > kMaxResolveDepth)
                        return nullptr;
                    if (d >= 1)
                    {
                        if (const auto border = node.try_as<wux::Controls::Border>())
                            return border;
                    }
                    const int n = wux::Media::VisualTreeHelper::GetChildrenCount(node);
                    for (int i = 0; i < n; ++i)
                    {
                        if (const auto found =
                                self(self, wux::Media::VisualTreeHelper::GetChild(node, i), d + 1))
                            return found;
                    }
                    return nullptr;
                };
                return walk(walk, root, depth);
            }
            catch (...)
            {
                return nullptr;
            }
        }

        void RestoreControlBackground(
            wux::Controls::Control const& ctrl,
            wux::Media::Brush const& originalBackground,
            bool& hadOrig) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                ctrl.ClearValue(wux::Controls::Control::BackgroundProperty());
                if (hadOrig)
                    ctrl.Background(originalBackground);
                ctrl.InvalidateArrange();
                ctrl.InvalidateMeasure();
                ctrl.UpdateLayout();
            }
            catch (...)
            {
            }
        }

        void RestoreBorderBackground(
            wux::Controls::Border const& border,
            wux::Media::Brush const& originalBackground,
            bool& hadOrig) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                border.ClearValue(wux::Controls::Border::BackgroundProperty());
                if (hadOrig)
                    border.Background(originalBackground);
                border.InvalidateArrange();
                border.InvalidateMeasure();
                border.UpdateLayout();
            }
            catch (...)
            {
            }
        }

        void PinModuleIfPendingAsync(int pending) noexcept
        {
            if (pending <= 0)
                return;
            HMODULE self = nullptr;
            if (!GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(&PinModuleIfPendingAsync),
                    &self))
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][MenuFlyout] PIN failed GLE=%u pendingAsync=%d",
                    static_cast<unsigned>(GetLastError()),
                    pending);
                return;
            }
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][MenuFlyout] module PIN (pendingAsync=%d)",
                pending);
        }

    }

    TrayMenuFlyoutPanelRegistry::TrayMenuFlyoutPanelRegistry(Registry::Settings settings) noexcept
        : m_settings(settings)
    {
        m_state->paintPathXaml.store(settings.paintPathXaml, std::memory_order_release);
    }

    unsigned TrayMenuFlyoutPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    unsigned TrayMenuFlyoutPanelRegistry::UnresolvedCount() const noexcept
    {
        return m_unresolved.load(std::memory_order_acquire);
    }

    bool TrayMenuFlyoutPanelRegistry::ResolveVisuals(PanelSlot& slot) noexcept
    {
        if (!slot.root)
            return false;

        try
        {
            const auto found = FindChromeBorder(slot.root, 0);
            if (!found)
                return false;

            const bool borderChanged =
                !slot.chromeBorder
                || (winrt::get_abi(slot.chromeBorder) != winrt::get_abi(found));
            slot.chromeBorder = found;

            // После backgroundPatched Background() = наш brush — не писать в original.
            if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
            {
                if (!slot.originalPresenterBackground && !slot.backgroundPatched)
                    slot.originalPresenterBackground = ctrl.Background();
            }

            if (borderChanged)
            {
                slot.chromeCaptured = false;
                slot.appliedOk = false;
                slot.needsRetry = true;
                if (!slot.backgroundPatched)
                {
                    try
                    {
                        slot.originalBorderBackground = slot.chromeBorder.Background();
                    }
                    catch (...)
                    {
                    }
                }
            }
            else if (!slot.originalBorderBackground && !slot.backgroundPatched)
            {
                try
                {
                    slot.originalBorderBackground = slot.chromeBorder.Background();
                }
                catch (...)
                {
                }
            }

            CaptureChrome(slot);
            slot.visualsResolved = true;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void TrayMenuFlyoutPanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        if (!slot.chromeBorder || slot.chromeCaptured)
            return;
        try
        {
            slot.originalBorderBrush = slot.chromeBorder.BorderBrush();
            slot.originalBorderThickness = slot.chromeBorder.BorderThickness();
            slot.originalCornerRadius = slot.chromeBorder.CornerRadius();
            try
            {
                slot.originalShadow = slot.chromeBorder.Shadow();
            }
            catch (...)
            {
                slot.originalShadow = nullptr;
            }
            slot.chromeCaptured = true;
        }
        catch (...)
        {
            slot.originalBorderBrush = nullptr;
            slot.originalBorderThickness = {};
            slot.originalCornerRadius = {};
            slot.originalShadow = nullptr;
            slot.chromeCaptured = true;
        }
    }

    void TrayMenuFlyoutPanelRegistry::EnsureSizeHook(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->chromeBorder || slot->sizeHooked)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (BorderLayoutReady(slot->chromeBorder) && slot->appliedOk && !slot->needsRetry)
            return;

        try
        {
            std::weak_ptr<PanelSlot> weakSlot = slot;
            std::weak_ptr<RegistryState> weakState = m_state;
            TrayMenuFlyoutPanelRegistry* self = this;
            // Нельзя обнулять sizeRevoker внутри SizeChanged — CRT fail-fast (Overflow).
            slot->sizeRevoker = slot->chromeBorder.SizeChanged(
                winrt::auto_revoke,
                [weakSlot, weakState, self](
                    winrt::Windows::Foundation::IInspectable const&,
                    wux::SizeChangedEventArgs const&) noexcept
                {
                    try
                    {
                        auto state = weakState.lock();
                        auto s = weakSlot.lock();
                        if (!state || !self)
                            return;
                        if (state->shutdown.load(std::memory_order_acquire))
                            return;
                        if (!s || !s->chromeBorder)
                            return;
                        if (s->sizeApplyBusy || s->sizeApplyDone)
                            return;
                        if (!BorderLayoutReady(s->chromeBorder))
                            return;
                        s->sizeApplyBusy = true;
                        s->needsRetry = true;
                        self->ApplyPanelVisualsSync(*s);
                        if (s->appliedOk && BorderLayoutReady(s->chromeBorder))
                            s->sizeApplyDone = true;
                        s->sizeApplyBusy = false;
                    }
                    catch (...)
                    {
                        if (auto s = weakSlot.lock())
                            s->sizeApplyBusy = false;
                    }
                });
            slot->sizeHooked = true;
        }
        catch (...)
        {
        }
    }

    void TrayMenuFlyoutPanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (!slot.chromeBorder || !slot.chromeCaptured || !slot.chromePatched)
        {
            slot.chromePatched = false;
            return;
        }
        try
        {
            slot.chromeBorder.BorderBrush(slot.originalBorderBrush);
            slot.chromeBorder.BorderThickness(slot.originalBorderThickness);
            slot.chromeBorder.CornerRadius(slot.originalCornerRadius);
            try
            {
                slot.chromeBorder.Shadow(slot.originalShadow);
            }
            catch (...)
            {
            }
        }
        catch (...)
        {
        }
        slot.chromePatched = false;
    }

    void TrayMenuFlyoutPanelRegistry::RestorePanelVisuals(
        PanelSlot& slot,
        bool restoreBackground,
        bool& hadOrig) noexcept
    {
        hadOrig = false;
        RestoreChrome(slot);

        // ClearValue без backgroundPatched → прозрачность. С флагом —
        // ClearValue снимает наш local; original=null → theme.
        const bool restoreBg = restoreBackground && slot.backgroundPatched;

        const bool preferNeutral =
            slot.preferNeutralBackgroundOnRestore
            || m_themePreferNeutralRestore.load(std::memory_order_acquire);
        unsigned usedNeutral = 0;
        unsigned usedClear = 0;
        if (restoreBg && slot.chromeBorder)
        {
            // ClearValue всегда; Neutral только после theme epoch
            // (Neutral до theme липнет со старым tint).
            try
            {
                slot.chromeBorder.ClearValue(
                    wux::Controls::Border::BackgroundProperty());
                usedClear = 1;
                if (preferNeutral)
                {
                    if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                    {
                        slot.chromeBorder.Background(neut);
                        usedNeutral = 1;
                    }
                }
            }
            catch (...)
            {
            }
        }

        if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
        {
            if (restoreBg)
            {
                try
                {
                    ctrl.ClearValue(wux::Controls::Control::BackgroundProperty());
                    usedClear = 1;
                    if (preferNeutral)
                    {
                        if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                        {
                            ctrl.Background(neut);
                            usedNeutral = 1;
                        }
                    }
                }
                catch (...)
                {
                }
                hadOrig = usedNeutral != 0 || usedClear != 0;
            }

            if (slot.presenterCornerPatched && slot.presenterCornerCaptured)
            {
                try
                {
                    ctrl.CornerRadius(slot.originalPresenterCornerRadius);
                }
                catch (...)
                {
                }
                slot.presenterCornerPatched = false;
            }
        }
        if (restoreBg)
        {
            slot.backgroundPatched = false;
            slot.preferNeutralBackgroundOnRestore = false;
        }
    }

    HRESULT TrayMenuFlyoutPanelRegistry::RestoreSlotOnUiThread(
        std::shared_ptr<PanelSlot> slot,
        bool restoreBackground,
        bool& hadOrig,
        bool& usedDispatcher) noexcept
    {
        usedDispatcher = false;
        hadOrig = false;
        if (!slot)
            return S_FALSE;

        try
        {
            if (!slot->root && !slot->chromeBorder)
                return S_FALSE;

            const auto dispatcher = slot->root
                ? slot->root.Dispatcher()
                : (slot->chromeBorder ? slot->chromeBorder.Dispatcher() : nullptr);

            if (dispatcher && !dispatcher.HasThreadAccess())
            {
                usedDispatcher = true;
                const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!done)
                    return HRESULT_FROM_WIN32(GetLastError());

                struct Job
                {
                    std::shared_ptr<PanelSlot> slot;
                    std::shared_ptr<RegistryState> state;
                    HANDLE done{};
                    HRESULT hr{ S_OK };
                    bool hadOrig{ false };
                    bool restoreBackground{ true };
                    bool ownsEvent{ true };

                    ~Job() noexcept
                    {
                        if (ownsEvent && done)
                            CloseHandle(done);
                    }
                };

                auto job = std::make_shared<Job>();
                job->slot = std::move(slot);
                job->state = m_state;
                job->done = done;
                job->restoreBackground = restoreBackground;

                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    // Не дублировать SetValue original (старый inline игнорировал
                    // preferNeutral/epoch → старая кисть на Stop).
                    TrayMenuFlyoutPanelRegistry* self = this;
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [job, self]()
                            {
                                try
                                {
                                    if (job->slot && self)
                                    {
                                        bool had = false;
                                        self->RestorePanelVisuals(
                                            *job->slot, job->restoreBackground, had);
                                        job->hadOrig = had;
                                        job->slot->ClearWinrtRefs();
                                    }
                                }
                                catch (const winrt::hresult_error& ex)
                                {
                                    job->hr = ex.code();
                                }
                                catch (...)
                                {
                                    job->hr = E_FAIL;
                                }
                                if (job->state)
                                {
                                    job->state->pendingAsync.fetch_sub(
                                        1, std::memory_order_acq_rel);
                                }
                                if (job->done)
                                    SetEvent(job->done);
                            }));
                }
                catch (const winrt::hresult_error& ex)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                    return ex.code();
                }
                catch (...)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                    return E_FAIL;
                }

                const DWORD wr = WaitForSingleObject(done, kRestoreWaitMs);
                if (wr == WAIT_OBJECT_0)
                {
                    hadOrig = job->hadOrig;
                    const HRESULT hr = job->hr;
                    job->ownsEvent = false;
                    CloseHandle(done);
                    return hr;
                }

                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][MenuFlyout] Restore wait timeout (%u ms)",
                    kRestoreWaitMs);
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }

            RestorePanelVisuals(*slot, restoreBackground, hadOrig);
            slot->ClearWinrtRefs();
            return S_OK;
        }
        catch (const winrt::hresult_error& ex)
        {
            return ex.code();
        }
        catch (...)
        {
            return E_FAIL;
        }
    }

    void TrayMenuFlyoutPanelRegistry::ApplyPanelVisualsSync(PanelSlot& slot) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (!slot.root)
            return;

        const Registry::Settings settings = [this]() noexcept
        {
            std::lock_guard lock(m_mx);
            return m_settings;
        }();

        if (!ResolveVisuals(slot))
        {
            slot.needsRetry = true;
            return;
        }
        if (!slot.chromeBorder)
        {
            slot.needsRetry = true;
            return;
        }

        CaptureChrome(slot);

        if (!settings.paintPathXaml)
        {
            // Эталон NcMenuFlyout: restore только если уже патчили.
            if (slot.backgroundPatched || slot.chromePatched || slot.presenterCornerPatched)
            {
                bool hadOrig = false;
                RestorePanelVisuals(slot, true, hadOrig);
            }
            slot.needsRetry = false;
            slot.appliedOk = true;
            return;
        }

        // Blur/compositor — только на Border (GetElementVisual на Presenter ломает клики).
        wux::Media::Brush brush = CreateBackdropBrush(
            slot.chromeBorder,
            settings.xamlEffectType,
            settings.tintArgb);

        if (!brush)
        {
            slot.needsRetry = true;
            return;
        }

        try
        {
            slot.chromeBorder.Background(brush);
            slot.backgroundPatched = true;

            if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
            {
                winrt::Windows::UI::Color clear{};
                clear.A = 0;
                wux::Media::SolidColorBrush solid;
                solid.Color(clear);
                ctrl.Background(solid);
            }

            try
            {
                slot.chromeBorder.Shadow(nullptr);
            }
            catch (...)
            {
            }

            const bool wantCorners =
                settings.cornersPresent && settings.cornerType != 0;
            // Tint бордюра не зависит от углов (паритет bar Default stroke).
            const bool wantTintBorder =
                settings.patchBorderStroke && settings.borderColorArgb != 0;
            const double trayH = Shared::TrayXamlPaint::QueryTaskbarHeightDip();
            const double popupH = QueryVisibleHeightDip(slot.chromeBorder);
            double appliedR = 0.0;
            const bool layoutReady = BorderLayoutReady(slot.chromeBorder);

            if (wantCorners)
            {
                // Не ждать layout: иначе 0×0 → сотни VTC-retry и углы через 0.7–1.2 с.
                // SizeChanged (EnsureSizeHook) пересчитает R, когда появится ActualHeight.
                appliedR = Shared::TrayXamlPaint::XamlPopupCornerRadiusDip(
                    settings.cornerType, trayH, popupH);
                slot.chromeBorder.CornerRadius(
                    wux::CornerRadius{ appliedR, appliedR, appliedR, appliedR });

                // Presenter тоже — иначе clip/радиус шаблона остаётся системным.
                if (const auto ctrl = slot.root.try_as<wux::Controls::Control>())
                {
                    try
                    {
                        if (!slot.presenterCornerCaptured)
                        {
                            slot.originalPresenterCornerRadius = ctrl.CornerRadius();
                            slot.presenterCornerCaptured = true;
                        }
                        ctrl.CornerRadius(
                            wux::CornerRadius{ appliedR, appliedR, appliedR, appliedR });
                        slot.presenterCornerPatched = true;
                    }
                    catch (...)
                    {
                    }
                }
                slot.chromePatched = true;
            }
            else if (!wantTintBorder && !settings.patchBorderStroke)
            {
                bool hadOrig = false;
                RestorePanelVisuals(slot, false, hadOrig);
            }
            else if (slot.chromeCaptured)
            {
                // Системные углы + (tint|NoBorder): вернуть CR, бордюр ниже.
                try
                {
                    slot.chromeBorder.CornerRadius(slot.originalCornerRadius);
                }
                catch (...)
                {
                }
            }

            if (wantTintBorder)
            {
                wux::Media::SolidColorBrush borderBrush;
                borderBrush.Color(ColorFromArgb(settings.borderColorArgb));
                slot.chromeBorder.BorderBrush(borderBrush);
                const wux::Thickness th =
                    ThicknessAnyPositive(slot.originalBorderThickness)
                    ? slot.originalBorderThickness
                    : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                slot.chromeBorder.BorderThickness(th);
                slot.chromePatched = true;
            }
            else if (settings.patchBorderStroke || wantCorners)
            {
                slot.chromeBorder.BorderBrush(nullptr);
                slot.chromeBorder.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                slot.chromePatched = true;
            }

            const bool firstApply = !slot.appliedOk;
            slot.appliedOk = true;
            // Пока нет layout — ждём SizeChanged (не крутить VTC Apply сотни раз).
            slot.needsRetry = wantCorners && !layoutReady;

            if (firstApply && slot.firstSeenTick != 0 && slot.appliedOk)
            {
                const unsigned delayMs = static_cast<unsigned>(
                    GetTickCount64() - slot.firstSeenTick);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][MenuFlyout] applied delayMs=%u paintXaml=%u effect=%u "
                    L"corner=%u trayH=%.1f popupH=%.1f r=%.1f layout=%u",
                    delayMs,
                    settings.paintPathXaml ? 1u : 0u,
                    settings.xamlEffectType,
                    settings.cornerType,
                    trayH,
                    popupH,
                    appliedR,
                    layoutReady ? 1u : 0u);
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            slot.needsRetry = true;
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][MenuFlyout] ApplyPanelVisualsSync hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            slot.needsRetry = true;
        }
    }

    void TrayMenuFlyoutPanelRegistry::ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->root)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            const auto dispatcher = slot->root.Dispatcher();
            if (dispatcher && !dispatcher.HasThreadAccess())
            {
                // Не копировать settings в lambda — ApplyPanelVisualsSync читает live m_settings
                // / paintPathXaml (иначе stale XAML apply после XAML→DWM).
                std::weak_ptr<PanelSlot> weakSlot = slot;
                std::weak_ptr<RegistryState> weakState = m_state;
                TrayMenuFlyoutPanelRegistry* self = this;
                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [weakSlot, weakState, self]()
                            {
                                auto state = weakState.lock();
                                auto s = weakSlot.lock();
                                if (state)
                                {
                                    state->pendingAsync.fetch_sub(
                                        1, std::memory_order_acq_rel);
                                    if (state->shutdown.load(std::memory_order_acquire))
                                        return;
                                }
                                else
                                {
                                    return;
                                }
                                if (!self || !s || !s->root)
                                    return;
                                self->ApplyPanelVisualsSync(*s);
                            }));
                }
                catch (...)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                }
                return;
            }

            ApplyPanelVisualsSync(*slot);
        }
        catch (...)
        {
        }
    }

    void TrayMenuFlyoutPanelRegistry::RegisterPanel(
        InstanceHandle handle,
        wux::FrameworkElement root) noexcept
    {
        if (!root || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            std::shared_ptr<PanelSlot> slot;
            {
                std::lock_guard lock(m_mx);
                auto& ref = m_panels[handle];
                if (!ref)
                    ref = std::make_shared<PanelSlot>();
                slot = ref;
                slot->root = root;
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
                // Меню после theme Recapture (часто пустой snapshot) — Stop Neutral.
                if (m_themePreferNeutralRestore.load(std::memory_order_acquire))
                    slot->preferNeutralBackgroundOnRestore = true;
            }

            ApplyPanelVisualsSync(*slot);
            EnsureSizeHook(slot);
            if (slot->needsRetry)
                m_unresolved.store(1, std::memory_order_release);

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][MenuFlyout] Panel registered handle=%llu paintXaml=%u "
                L"effect=%u corner=%u border=%u resolved=%u retry=%u",
                static_cast<unsigned long long>(handle),
                m_settings.paintPathXaml ? 1u : 0u,
                m_settings.xamlEffectType,
                m_settings.cornerType,
                m_settings.patchBorderStroke ? 1u : 0u,
                slot->visualsResolved ? 1u : 0u,
                slot->needsRetry ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][MenuFlyout] RegisterPanel hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][MenuFlyout] RegisterPanel unknown exception");
        }
    }

    void TrayMenuFlyoutPanelRegistry::RetryUnresolvedPanels() noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        std::vector<std::shared_ptr<PanelSlot>> snapshot;
        {
            std::lock_guard lock(m_mx);
            if (m_panels.empty())
                return;
            snapshot.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    snapshot.push_back(s);
            }
        }
        if (snapshot.empty())
        {
            m_unresolved.store(0, std::memory_order_release);
            return;
        }

        unsigned still = 0;
        for (auto& s : snapshot)
        {
            // Border часто появляется только на retry — hook должен ставиться здесь,
            // иначе SizeChanged никогда не сработает (register при resolve_fail).
            if (s->appliedOk && !s->needsRetry && s->sizeHooked)
                continue;
            ApplyPanelVisualsSync(*s);
            EnsureSizeHook(s);
            if (s->needsRetry || !s->appliedOk)
                ++still;
        }
        m_unresolved.store(still, std::memory_order_release);
    }

    void TrayMenuFlyoutPanelRegistry::Unregister(InstanceHandle handle) noexcept
    {
        std::shared_ptr<PanelSlot> slot;
        {
            std::lock_guard lock(m_mx);
            const auto it = m_panels.find(handle);
            if (it == m_panels.end())
                return;
            slot = it->second;
            m_panels.erase(it);
        }

        try
        {
            if (!slot)
                return;
            const bool patched = slot->backgroundPatched || slot->chromePatched
                || slot->presenterCornerPatched;
            if (!patched)
                return;
            bool hadOrig = false;
            bool usedDisp = false;
            (void)RestoreSlotOnUiThread(std::move(slot), true, hadOrig, usedDisp);
        }
        catch (...)
        {
        }
    }

    void TrayMenuFlyoutPanelRegistry::RestoreAll() noexcept
    {
        m_state->shutdown.store(true, std::memory_order_release);

        std::vector<std::shared_ptr<PanelSlot>> snapshot;
        {
            std::lock_guard lock(m_mx);
            snapshot.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    snapshot.push_back(std::move(s));
            }
            m_panels.clear();
        }
        m_unresolved.store(0, std::memory_order_release);

        const unsigned total = static_cast<unsigned>(snapshot.size());
        unsigned ok = 0;
        unsigned fail = 0;
        unsigned noOrig = 0;
        unsigned viaDispatcher = 0;
        for (auto& slot : snapshot)
        {
            bool hadOrig = false;
            bool usedDispatcher = false;
            const HRESULT hr = RestoreSlotOnUiThread(slot, true, hadOrig, usedDispatcher);
            if (usedDispatcher)
                ++viaDispatcher;
            if (!hadOrig)
                ++noOrig;
            if (SUCCEEDED(hr))
                ++ok;
            else
                ++fail;
        }

        const int pending = m_state->pendingAsync.load(std::memory_order_acquire);
        PinModuleIfPendingAsync(pending);

        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][MenuFlyout] RestoreAll total=%u ok=%u fail=%u noOrig=%u "
            L"disp=%u pendingAsync=%d",
            total,
            ok,
            fail,
            noOrig,
            viaDispatcher,
            pending);
    }

    void TrayMenuFlyoutPanelRegistry::ReapplyPatchedBrushes() noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        std::vector<std::shared_ptr<PanelSlot>> snapshot;
        {
            std::lock_guard lock(m_mx);
            snapshot.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    snapshot.push_back(s);
            }
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void TrayMenuFlyoutPanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        m_state->paintPathXaml.store(settings.paintPathXaml, std::memory_order_release);

        // Лог: theme при закрытых меню → snapshot=0, слоты после open без flag.
        m_themePreferNeutralRestore.store(true, std::memory_order_release);

        std::vector<std::shared_ptr<PanelSlot>> snapshot;
        {
            std::lock_guard lock(m_mx);
            snapshot.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    snapshot.push_back(s);
            }
        }
        for (auto& slot : snapshot)
        {
            if (!slot)
                continue;
            // keepPrev старой темы → Stop «старая кисть». После theme —
            // originals null + preferNeutral → Neutral Acrylic текущей темы.
            try
            {
                if (slot->root)
                {
                    if (const auto presenter =
                            slot->root.try_as<wux::Controls::MenuFlyoutPresenter>())
                    {
                        presenter.ClearValue(wux::Controls::Control::BackgroundProperty());
                        presenter.ClearValue(wux::Controls::Control::CornerRadiusProperty());
                    }
                }
                if (slot->chromeBorder)
                {
                    slot->chromeBorder.ClearValue(wux::Controls::Border::BackgroundProperty());
                    slot->chromeBorder.ClearValue(wux::Controls::Border::BorderBrushProperty());
                    slot->chromeBorder.ClearValue(wux::Controls::Border::BorderThicknessProperty());
                    slot->chromeBorder.ClearValue(wux::Controls::Border::CornerRadiusProperty());
                }
            }
            catch (...)
            {
            }
            slot->originalPresenterBackground = nullptr;
            slot->originalBorderBackground = nullptr;
            slot->preferNeutralBackgroundOnRestore = true;
            slot->originalBorderBrush = nullptr;
            slot->originalShadow = nullptr;
            slot->chromeCaptured = false;
            slot->chromePatched = false;
            slot->backgroundPatched = false;
            slot->presenterCornerCaptured = false;
            slot->presenterCornerPatched = false;
            slot->appliedOk = false;
            slot->needsRetry = true;
            slot->visualsResolved = false;
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void TrayMenuFlyoutPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        m_state->paintPathXaml.store(settings.paintPathXaml, std::memory_order_release);
        ReapplyPatchedBrushes();
    }
}
