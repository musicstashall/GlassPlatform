#include "TaskbarPch.h"
#include "xaml/TrayOverflowPanelRegistry.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"

#include <TrayXamlPaint.h>

#include <DebugLog.h>

#include <vector>
#include <string_view>

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

        [[nodiscard]] winrt::Windows::UI::Color ColorFromArgb(DWORD argb) noexcept
        {
            return winrt::Windows::UI::Color{
                static_cast<uint8_t>((argb >> 24) & 0xFFu),
                static_cast<uint8_t>((argb >> 16) & 0xFFu),
                static_cast<uint8_t>((argb >> 8) & 0xFFu),
                static_cast<uint8_t>(argb & 0xFFu),
            };
        }

        [[nodiscard]] double QueryTaskbarHeightDip() noexcept
        {
            HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (!tray)
                tray = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
            if (!tray)
                return 48.0;
            RECT rc{};
            if (!GetClientRect(tray, &rc))
                return 48.0;
            const int h = rc.bottom - rc.top;
            return h > 1 ? static_cast<double>(h) : 48.0;
        }

        [[nodiscard]] double OverflowCornerRadiusDip(DWORD cornerType) noexcept
        {
            return Shared::TrayXamlPaint::XamlCornerRadiusDip(
                cornerType, QueryTaskbarHeightDip());
        }

        [[nodiscard]] bool ThicknessAnyPositive(const wux::Thickness& t) noexcept
        {
            return t.Left > 0.0 || t.Top > 0.0 || t.Right > 0.0 || t.Bottom > 0.0;
        }

        [[nodiscard]] bool CornerRadiusAnyPositive(const wux::CornerRadius& r) noexcept
        {
            return r.TopLeft > 0.5 || r.TopRight > 0.5 || r.BottomLeft > 0.5
                || r.BottomRight > 0.5;
        }

        [[nodiscard]] wux::FrameworkElement FindDescendantByName(
            wux::DependencyObject root,
            std::wstring_view name,
            int depth) noexcept
        {
            if (!root || name.empty())
                return nullptr;
            try
            {
                const auto walk = [&](auto&& self, wux::DependencyObject node, int d)
                    -> wux::FrameworkElement
                {
                    if (!node || d > kMaxResolveDepth)
                        return nullptr;
                    if (d >= 1)
                    {
                        if (const auto fe = node.try_as<wux::FrameworkElement>())
                        {
                            try
                            {
                                if (fe.Name() == name)
                                    return fe;
                            }
                            catch (...)
                            {
                            }
                        }
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

        [[nodiscard]] wux::Controls::Grid ResolveOverflowRootGrid(
            wux::Controls::Border const& border) noexcept
        {
            if (!border)
                return nullptr;
            try
            {
                const auto parent = wux::Media::VisualTreeHelper::GetParent(border)
                    .try_as<wux::Controls::Grid>();
                if (!parent)
                    return nullptr;
                try
                {
                    if (parent.Name() != L"OverflowRootGrid")
                        return nullptr;
                }
                catch (...)
                {
                    return nullptr;
                }
                return parent;
            }
            catch (...)
            {
                return nullptr;
            }
        }

        void RestoreBorderBackground(
            wux::Controls::Border const& border,
            wux::Media::Brush const& originalBackground,
            bool& hadOrig,
            bool preferClear) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                // preferClear (theme epoch): ClearValue alone = дыра (лог B).
                // Neutral Acrylic текущей темы — как TrayMenu after epoch.
                border.ClearValue(wux::Controls::Border::BackgroundProperty());
                if (preferClear)
                {
                    if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                    {
                        border.Background(neut);
                        hadOrig = true;
                    }
                }
                else if (hadOrig)
                {
                    border.Background(originalBackground);
                }
                border.InvalidateArrange();
                border.InvalidateMeasure();
                border.UpdateLayout();
            }
            catch (...)
            {
            }
        }

        void RestoreGridBackground(
            wux::Controls::Grid const& grid,
            wux::Media::Brush const& originalBackground,
            bool& hadOrig,
            bool preferClear) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                // OverflowRootGrid: только Transparent / ClearValue.
                // Neutral на корне = backdrop на весь остров (тени + невидимая
                // область) поверх Border#OverflowFlyoutBackgroundBorder (лог O4).
                grid.ClearValue(wux::Controls::Panel::BackgroundProperty());
                if (!preferClear && hadOrig)
                    grid.Background(originalBackground);
                grid.InvalidateArrange();
                grid.InvalidateMeasure();
                grid.UpdateLayout();
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
                    L"[TaskbarCompat][Overflow] PIN failed GLE=%u pendingAsync=%d",
                    static_cast<unsigned>(GetLastError()),
                    pending);
                return;
            }
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][Overflow] module PIN (pendingAsync=%d)",
                pending);
        }
    }

    TrayOverflowPanelRegistry::TrayOverflowPanelRegistry(Registry::Settings settings) noexcept
        : m_settings(settings)
    {
    }

    unsigned TrayOverflowPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    unsigned TrayOverflowPanelRegistry::UnresolvedCount() const noexcept
    {
        return m_unresolved.load(std::memory_order_acquire);
    }

    bool TrayOverflowPanelRegistry::ResolveVisuals(PanelSlot& slot) noexcept
    {
        try
        {
            wux::Controls::Border found{ nullptr };

            if (slot.root)
            {
                const auto fe = FindDescendantByName(
                    slot.root, L"OverflowFlyoutBackgroundBorder", 0);
                found = fe ? fe.try_as<wux::Controls::Border>() : nullptr;
            }
            if (!found && slot.backgroundBorder)
                found = slot.backgroundBorder;
            if (!found)
                return false;

            const bool borderChanged =
                !slot.backgroundBorder
                || (winrt::get_abi(slot.backgroundBorder) != winrt::get_abi(found));
            slot.backgroundBorder = found;
            slot.rootGrid = ResolveOverflowRootGrid(slot.backgroundBorder);

            if (borderChanged)
            {
                slot.chromeCaptured = false;
                slot.appliedOk = false;
                slot.needsRetry = true;
                try
                {
                    if (slot.backgroundBorder)
                        slot.originalBackground = slot.backgroundBorder.Background();
                    if (slot.rootGrid)
                        slot.originalRootGridBackground = slot.rootGrid.Background();
                }
                catch (...)
                {
                }
            }
            else
            {
                if (!slot.originalBackground && slot.backgroundBorder)
                    slot.originalBackground = slot.backgroundBorder.Background();
                if (slot.rootGrid && !slot.originalRootGridBackground)
                    slot.originalRootGridBackground = slot.rootGrid.Background();
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

    void TrayOverflowPanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        if (!slot.backgroundBorder || slot.chromeCaptured)
            return;
        try
        {
            // CornerRadius на 0×0 Border → AV / fail-fast в Windows.UI.Xaml.
            // Читаем chrome только после layout.
            if (!BorderLayoutReady(slot.backgroundBorder))
                return;

            // Theme Recapture ClearValue / Shadow(nullptr) → sample null.
            // Не затирать ранее захваченные system brush/shadow.
            const auto sampledBrush = slot.backgroundBorder.BorderBrush();
            if (sampledBrush || !slot.originalBorderBrush)
                slot.originalBorderBrush = sampledBrush;

            const auto sampledTh = slot.backgroundBorder.BorderThickness();
            if (ThicknessAnyPositive(sampledTh)
                || !ThicknessAnyPositive(slot.originalBorderThickness))
                slot.originalBorderThickness = sampledTh;

            try
            {
                // Theme Recapture ClearValue(CR) → sample 0 → Stop прямые углы.
                // Нулевой sample не затирает ранее захваченный system CR.
                const auto sampled = slot.backgroundBorder.CornerRadius();
                if (CornerRadiusAnyPositive(sampled)
                    || !CornerRadiusAnyPositive(slot.originalCornerRadius))
                    slot.originalCornerRadius = sampled;
            }
            catch (...)
            {
                if (!CornerRadiusAnyPositive(slot.originalCornerRadius))
                    slot.originalCornerRadius = {};
            }
            try
            {
                const auto sampledShadow = slot.backgroundBorder.Shadow();
                if (sampledShadow || !slot.originalShadow)
                    slot.originalShadow = sampledShadow;
            }
            catch (...)
            {
                if (!slot.originalShadow)
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

    void TrayOverflowPanelRegistry::EnsureSizeHook(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->backgroundBorder || slot->sizeHooked)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (BorderLayoutReady(slot->backgroundBorder))
            return;

        try
        {
            std::weak_ptr<PanelSlot> weakSlot = slot;
            std::weak_ptr<RegistryState> weakState = m_state;
            TrayOverflowPanelRegistry* self = this;
            // Нельзя делать sizeRevoker={} внутри SizeChanged — winrt/CRT fail-fast
            // (0xC0000409 FAST_FAIL_FATAL_APP_EXIT). Снимаем хук только в ClearWinrtRefs.
            slot->sizeRevoker = slot->backgroundBorder.SizeChanged(
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
                        if (!s || !s->backgroundBorder)
                            return;
                        if (s->sizeApplyDone || s->sizeApplyBusy)
                            return;
                        if (s->appliedOk && !s->needsRetry)
                        {
                            s->sizeApplyDone = true;
                            return;
                        }
                        if (!BorderLayoutReady(s->backgroundBorder))
                            return;
                        s->sizeApplyBusy = true;
                        s->needsRetry = true;
                        s->appliedOk = false;
                        self->ApplyPanelVisualsSync(*s);
                        if (s->appliedOk)
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

    void TrayOverflowPanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (!slot.backgroundBorder || !slot.chromeCaptured || !slot.chromePatched)
        {
            slot.chromePatched = false;
            return;
        }
        try
        {
            // ClearValue Border* после theme снимал углы/бордюр (система не ребиндит).
            // Shadow/Border*/CR — SetValue kept originals.
            slot.backgroundBorder.BorderBrush(slot.originalBorderBrush);
            slot.backgroundBorder.BorderThickness(slot.originalBorderThickness);
            slot.backgroundBorder.CornerRadius(slot.originalCornerRadius);
            try
            {
                slot.backgroundBorder.Shadow(slot.originalShadow);
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

    void TrayOverflowPanelRegistry::RestorePanelVisuals(
        PanelSlot& slot,
        bool restoreBackground,
        bool& hadOrig) noexcept
    {
        hadOrig = false;
        const bool preferClear = m_themePreferClearRestore;
        RestoreChrome(slot);

        if (restoreBackground && slot.rootGrid)
        {
            bool gridHad = false;
            RestoreGridBackground(
                slot.rootGrid, slot.originalRootGridBackground, gridHad, preferClear);
        }

        if (restoreBackground && slot.backgroundBorder)
        {
            RestoreBorderBackground(
                slot.backgroundBorder, slot.originalBackground, hadOrig, preferClear);
        }
        else
            hadOrig = static_cast<bool>(slot.originalBackground);
    }

    HRESULT TrayOverflowPanelRegistry::RestoreSlotOnUiThread(
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
            if (!slot->root && !slot->backgroundBorder)
                return S_FALSE;

            const auto dispatcher = slot->root
                ? slot->root.Dispatcher()
                : (slot->backgroundBorder ? slot->backgroundBorder.Dispatcher() : nullptr);

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
                    bool preferClear{ false };
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
                job->preferClear = m_themePreferClearRestore;

                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [job]()
                            {
                                try
                                {
                                    if (job->slot)
                                    {
                                        auto& s = *job->slot;
                                        if (s.backgroundBorder && s.chromeCaptured && s.chromePatched)
                                        {
                                            try
                                            {
                                                s.backgroundBorder.BorderBrush(
                                                    s.originalBorderBrush);
                                                s.backgroundBorder.BorderThickness(
                                                    s.originalBorderThickness);
                                                s.backgroundBorder.CornerRadius(
                                                    s.originalCornerRadius);
                                                try
                                                {
                                                    s.backgroundBorder.Shadow(s.originalShadow);
                                                }
                                                catch (...)
                                                {
                                                }
                                            }
                                            catch (...)
                                            {
                                            }
                                            s.chromePatched = false;
                                        }
                                        if (job->restoreBackground && s.rootGrid)
                                        {
                                            bool gridHad = false;
                                            RestoreGridBackground(
                                                s.rootGrid,
                                                s.originalRootGridBackground,
                                                gridHad,
                                                job->preferClear);
                                        }
                                        if (job->restoreBackground && s.backgroundBorder)
                                        {
                                            RestoreBorderBackground(
                                                s.backgroundBorder,
                                                s.originalBackground,
                                                job->hadOrig,
                                                job->preferClear);
                                        }
                                        else
                                        {
                                            job->hadOrig =
                                                static_cast<bool>(s.originalBackground);
                                        }
                                        s.ClearWinrtRefs();
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
                    L"[TaskbarCompat][Overflow] Restore wait timeout (%u ms)",
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

    void TrayOverflowPanelRegistry::ApplyPanelVisualsSync(PanelSlot& slot) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (!slot.root && !slot.backgroundBorder)
            return;

        // Уже применено (типично: RegisterPanel + сразу RegisterBackgroundBorder).
        if (slot.appliedOk && !slot.needsRetry)
        {
            return;
        }

        const Registry::Settings settings = [this]() noexcept
        {
            std::lock_guard lock(m_mx);
            return m_settings;
        }();

        if (!ResolveVisuals(slot))
        {
            slot.needsRetry = true;
            if (!slot.resolveFailLogged)
            {
                slot.resolveFailLogged = true;
                DebugLog(
                    DBG_NORMAL,
                    L"[TaskbarCompat][Overflow] resolve failed: border=%u rootGrid=%u",
                    slot.backgroundBorder ? 1u : 0u,
                    slot.rootGrid ? 1u : 0u);
            }
            return;
        }
        if (!slot.backgroundBorder)
        {
            slot.needsRetry = true;
            return;
        }

        CaptureChrome(slot);

        if (!settings.paintPathXaml)
        {
            bool hadOrig = false;
            RestorePanelVisuals(slot, true, hadOrig);
            slot.needsRetry = false;
            slot.appliedOk = true;
            return;
        }

        wux::Media::Brush brush = CreateBackdropBrush(
            slot.backgroundBorder,
            settings.xamlEffectType,
            settings.tintArgb);

        // Disable/System — не патчим (оставить оригинал); если раньше патчили — restore.
        if (!brush)
        {
            bool hadOrig = false;
            RestorePanelVisuals(slot, true, hadOrig);
            slot.needsRetry = false;
            slot.appliedOk = true;
            return;
        }

        try
        {
            slot.backgroundBorder.Background(brush);

            if (slot.rootGrid)
            {
                winrt::Windows::UI::Color clear{};
                clear.A = 0;
                wux::Media::SolidColorBrush solid;
                solid.Color(clear);
                slot.rootGrid.Background(solid);
            }

            const bool wantCorners =
                settings.cornersPresent && settings.cornerType != 0;
            const bool wantTintBorder =
                settings.patchBorderStroke && settings.borderColorArgb != 0;
            const bool layoutReady = BorderLayoutReady(slot.backgroundBorder);

            if (wantCorners && !layoutReady)
            {
                // Фон уже выставлен; углы/Shadow — после SizeChanged / повторного Add.
                slot.appliedOk = false;
                slot.needsRetry = true;
                return;
            }

            // ThemeShadow оставляем: кастомный CR + system shadow (юзер: нет тени live).
            // HoverFlyout гасит Shadow при CornerType≠0; Overflow — нет.

            if (wantCorners || wantTintBorder || settings.patchBorderStroke)
            {
                CaptureChrome(slot);
                if (!slot.chromeCaptured && wantCorners)
                {
                    slot.needsRetry = true;
                    slot.appliedOk = false;
                    return;
                }
            }

            if (wantCorners)
            {
                const double r = OverflowCornerRadiusDip(settings.cornerType);
                try
                {
                    slot.backgroundBorder.CornerRadius(wux::CornerRadius{ r, r, r, r });
                }
                catch (...)
                {
                    slot.needsRetry = true;
                    slot.appliedOk = false;
                    return;
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
                try
                {
                    slot.backgroundBorder.CornerRadius(slot.originalCornerRadius);
                }
                catch (...)
                {
                }
            }

            if (wantTintBorder)
            {
                wux::Media::SolidColorBrush borderBrush;
                borderBrush.Color(ColorFromArgb(settings.borderColorArgb));
                slot.backgroundBorder.BorderBrush(borderBrush);
                const wux::Thickness th =
                    ThicknessAnyPositive(slot.originalBorderThickness)
                    ? slot.originalBorderThickness
                    : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                slot.backgroundBorder.BorderThickness(th);
                slot.chromePatched = true;
            }
            else if (settings.patchBorderStroke || wantCorners)
            {
                slot.backgroundBorder.BorderBrush(nullptr);
                slot.backgroundBorder.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                slot.chromePatched = true;
            }

            const bool firstApply = !slot.appliedOk;
            slot.appliedOk = true;
            slot.needsRetry = false;


            if (firstApply && slot.firstSeenTick != 0)
            {
                const ULONGLONG now = GetTickCount64();
                const unsigned delayMs = static_cast<unsigned>(
                    now >= slot.firstSeenTick ? (now - slot.firstSeenTick) : 0);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][Overflow] applied delayMs=%u paintXaml=%u effect=%u",
                    delayMs,
                    settings.paintPathXaml ? 1u : 0u,
                    settings.xamlEffectType);
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            slot.needsRetry = true;
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][Overflow] ApplyPanelVisualsSync hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            slot.needsRetry = true;
        }
    }

    void TrayOverflowPanelRegistry::ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || (!slot->root && !slot->backgroundBorder))
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            const auto dispatcher = slot->root
                ? slot->root.Dispatcher()
                : slot->backgroundBorder.Dispatcher();
            if (dispatcher && !dispatcher.HasThreadAccess())
            {
                const Registry::Settings settingsCopy = [this]() noexcept
                {
                    std::lock_guard lock(m_mx);
                    return m_settings;
                }();

                std::weak_ptr<PanelSlot> weakSlot = slot;
                std::weak_ptr<RegistryState> weakState = m_state;
                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [weakSlot, weakState, settingsCopy]()
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
                                if (!s || (!s->root && !s->backgroundBorder))
                                    return;

                                try
                                {
                                    if (!s->visualsResolved || !s->backgroundBorder)
                                    {
                                        if (s->root)
                                        {
                                            const auto fe = FindDescendantByName(
                                                s->root,
                                                L"OverflowFlyoutBackgroundBorder",
                                                0);
                                            s->backgroundBorder =
                                                fe ? fe.try_as<wux::Controls::Border>() : nullptr;
                                        }
                                        if (!s->backgroundBorder)
                                        {
                                            s->needsRetry = true;
                                            return;
                                        }
                                        s->rootGrid =
                                            ResolveOverflowRootGrid(s->backgroundBorder);
                                        if (!s->originalBackground)
                                            s->originalBackground =
                                                s->backgroundBorder.Background();
                                        if (s->rootGrid && !s->originalRootGridBackground)
                                            s->originalRootGridBackground =
                                                s->rootGrid.Background();
                                        // CornerRadius capture — только при layout > 0.
                                        if (!s->chromeCaptured
                                            && BorderLayoutReady(s->backgroundBorder))
                                        {
                                            try
                                            {
                                                s->originalBorderBrush =
                                                    s->backgroundBorder.BorderBrush();
                                                s->originalBorderThickness =
                                                    s->backgroundBorder.BorderThickness();
                                                s->originalCornerRadius =
                                                    s->backgroundBorder.CornerRadius();
                                                try
                                                {
                                                    s->originalShadow =
                                                        s->backgroundBorder.Shadow();
                                                }
                                                catch (...)
                                                {
                                                    s->originalShadow = nullptr;
                                                }
                                                s->chromeCaptured = true;
                                            }
                                            catch (...)
                                            {
                                                s->chromeCaptured = false;
                                            }
                                        }
                                        s->visualsResolved = true;
                                    }

                                    if (!settingsCopy.paintPathXaml)
                                    {
                                        if (s->backgroundBorder && s->chromeCaptured
                                            && s->chromePatched)
                                        {
                                            s->backgroundBorder.BorderBrush(
                                                s->originalBorderBrush);
                                            s->backgroundBorder.BorderThickness(
                                                s->originalBorderThickness);
                                            s->backgroundBorder.CornerRadius(
                                                s->originalCornerRadius);
                                            try
                                            {
                                                s->backgroundBorder.Shadow(s->originalShadow);
                                            }
                                            catch (...)
                                            {
                                            }
                                            s->chromePatched = false;
                                        }
                                        if (s->rootGrid)
                                        {
                                            bool gridHad = false;
                                            RestoreGridBackground(
                                                s->rootGrid,
                                                s->originalRootGridBackground,
                                                gridHad,
                                                false);
                                        }
                                        if (s->backgroundBorder)
                                        {
                                            bool had = false;
                                            RestoreBorderBackground(
                                                s->backgroundBorder,
                                                s->originalBackground,
                                                had,
                                                false);
                                        }
                                        s->needsRetry = false;
                                        s->appliedOk = true;
                                        return;
                                    }

                                    auto brush = CreateBackdropBrush(
                                        s->backgroundBorder,
                                        settingsCopy.xamlEffectType,
                                        settingsCopy.tintArgb);
                                    if (!brush)
                                    {
                                        if (s->backgroundBorder && s->chromeCaptured
                                            && s->chromePatched)
                                        {
                                            s->backgroundBorder.BorderBrush(
                                                s->originalBorderBrush);
                                            s->backgroundBorder.BorderThickness(
                                                s->originalBorderThickness);
                                            s->backgroundBorder.CornerRadius(
                                                s->originalCornerRadius);
                                            try
                                            {
                                                s->backgroundBorder.Shadow(s->originalShadow);
                                            }
                                            catch (...)
                                            {
                                            }
                                            s->chromePatched = false;
                                        }
                                        if (s->rootGrid)
                                        {
                                            bool gridHad = false;
                                            RestoreGridBackground(
                                                s->rootGrid,
                                                s->originalRootGridBackground,
                                                gridHad,
                                                false);
                                        }
                                        if (s->backgroundBorder)
                                        {
                                            bool had = false;
                                            RestoreBorderBackground(
                                                s->backgroundBorder,
                                                s->originalBackground,
                                                had,
                                                false);
                                        }
                                        s->needsRetry = false;
                                        s->appliedOk = true;
                                        return;
                                    }

                                    s->backgroundBorder.Background(brush);
                                    if (s->rootGrid)
                                    {
                                        winrt::Windows::UI::Color clear{};
                                        clear.A = 0;
                                        wux::Media::SolidColorBrush solid;
                                        solid.Color(clear);
                                        s->rootGrid.Background(solid);
                                    }
                                    const bool wantCorners = settingsCopy.cornersPresent
                                        && settingsCopy.cornerType != 0;
                                    const bool wantTintBorder = settingsCopy.patchBorderStroke
                                        && settingsCopy.borderColorArgb != 0;
                                    // ThemeShadow оставляем (live тень Overflow).
                                    if (wantCorners
                                        && !BorderLayoutReady(s->backgroundBorder))
                                    {
                                        s->needsRetry = true;
                                        s->appliedOk = false;
                                        return;
                                    }
                                    // (Shadow не гасим)
                                    if (wantCorners || wantTintBorder
                                        || settingsCopy.patchBorderStroke)
                                    {
                                        if (!s->chromeCaptured)
                                        {
                                            try
                                            {
                                                s->originalBorderBrush =
                                                    s->backgroundBorder.BorderBrush();
                                                s->originalBorderThickness =
                                                    s->backgroundBorder.BorderThickness();
                                                s->originalCornerRadius =
                                                    s->backgroundBorder.CornerRadius();
                                                s->chromeCaptured = true;
                                            }
                                            catch (...)
                                            {
                                                if (wantCorners)
                                                {
                                                    s->needsRetry = true;
                                                    s->appliedOk = false;
                                                    return;
                                                }
                                            }
                                        }
                                    }
                                    if (wantCorners)
                                    {
                                        const double r =
                                            OverflowCornerRadiusDip(settingsCopy.cornerType);
                                        s->backgroundBorder.CornerRadius(
                                            wux::CornerRadius{ r, r, r, r });
                                        s->chromePatched = true;
                                    }
                                    else if (s->chromeCaptured
                                        && (wantTintBorder || settingsCopy.patchBorderStroke))
                                    {
                                        try
                                        {
                                            s->backgroundBorder.CornerRadius(
                                                s->originalCornerRadius);
                                        }
                                        catch (...)
                                        {
                                        }
                                    }
                                    if (wantTintBorder)
                                    {
                                        wux::Media::SolidColorBrush borderBrush;
                                        borderBrush.Color(
                                            ColorFromArgb(settingsCopy.borderColorArgb));
                                        s->backgroundBorder.BorderBrush(borderBrush);
                                        const wux::Thickness th =
                                            ThicknessAnyPositive(s->originalBorderThickness)
                                            ? s->originalBorderThickness
                                            : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                                        s->backgroundBorder.BorderThickness(th);
                                        s->chromePatched = true;
                                    }
                                    else if (settingsCopy.patchBorderStroke || wantCorners)
                                    {
                                        s->backgroundBorder.BorderBrush(nullptr);
                                        s->backgroundBorder.BorderThickness(
                                            wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                                        s->chromePatched = true;
                                    }
                                    s->appliedOk = true;
                                    s->needsRetry = false;
                                }
                                catch (...)
                                {
                                    if (s)
                                        s->needsRetry = true;
                                }
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

    void TrayOverflowPanelRegistry::RegisterPanel(
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
                // Border мог зарегистрироваться раньше под своим handle — сливаем.
                for (auto it = m_panels.begin(); it != m_panels.end();)
                {
                    if (it->first == handle)
                    {
                        ++it;
                        continue;
                    }
                    auto& other = it->second;
                    if (other && other->backgroundBorder && !other->root)
                    {
                        slot = other;
                        it = m_panels.erase(it);
                        m_panels[handle] = slot;
                        break;
                    }
                    ++it;
                }
                if (!slot)
                {
                    auto& ref = m_panels[handle];
                    if (!ref)
                        ref = std::make_shared<PanelSlot>();
                    slot = ref;
                }
                slot->root = root;
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
            }

            ApplyPanelVisualsSync(*slot);
            if (slot->needsRetry)
            {
                m_unresolved.store(1, std::memory_order_release);
                EnsureSizeHook(slot);
            }

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][Overflow] Panel registered handle=%llu paintXaml=%u "
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
                L"[TaskbarCompat][Overflow] RegisterPanel hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][Overflow] RegisterPanel unknown exception");
        }
    }

    void TrayOverflowPanelRegistry::RegisterBackgroundBorder(
        InstanceHandle handle,
        wux::Controls::Border border) noexcept
    {
        if (!border || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {

            std::shared_ptr<PanelSlot> slot;
            bool alreadyApplied = false;
            {
                std::lock_guard lock(m_mx);
                // Слить со слотом от NotificationAreaOverflow. ABI FromHandle и
                // FindDescendant часто РАЗНЫЕ указатели на один визуальный Border —
                // сравнение get_abi недостаточно; иначе второй Apply → 0xC0000409.
                for (auto& [h, s] : m_panels)
                {
                    (void)h;
                    if (!s)
                        continue;
                    if (s->backgroundBorder
                        && winrt::get_abi(s->backgroundBorder) == winrt::get_abi(border))
                    {
                        slot = s;
                        break;
                    }
                    if (s->root)
                    {
                        const auto fe = FindDescendantByName(
                            s->root, L"OverflowFlyoutBackgroundBorder", 0);
                        if (fe)
                        {
                            slot = s;
                            break;
                        }
                    }
                    if (s->borderHandle == handle)
                    {
                        slot = s;
                        break;
                    }
                }
                // Один overflow на процесс: переиспользовать единственный слот.
                if (!slot && m_panels.size() == 1)
                    slot = m_panels.begin()->second;

                if (!slot)
                {
                    auto& ref = m_panels[handle];
                    if (!ref)
                        ref = std::make_shared<PanelSlot>();
                    slot = ref;
                }

                alreadyApplied = slot->appliedOk && !slot->needsRetry;
                slot->borderHandle = handle;
                // Не подменять уже успешно пропатченный backgroundBorder на FromHandle.
                if (!slot->backgroundBorder)
                {
                    slot->backgroundBorder = border;
                    slot->rootGrid = ResolveOverflowRootGrid(border);
                }
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
            }

            if (alreadyApplied)
            {
                return;
            }

            ApplyPanelVisualsSync(*slot);
            if (slot->needsRetry)
            {
                m_unresolved.store(1, std::memory_order_release);
                EnsureSizeHook(slot);
            }

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][Overflow] Border registered handle=%llu paintXaml=%u "
                L"effect=%u resolved=%u retry=%u",
                static_cast<unsigned long long>(handle),
                m_settings.paintPathXaml ? 1u : 0u,
                m_settings.xamlEffectType,
                slot->visualsResolved ? 1u : 0u,
                slot->needsRetry ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][Overflow] RegisterBackgroundBorder hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][Overflow] RegisterBackgroundBorder unknown exception");
        }
    }

    void TrayOverflowPanelRegistry::RetryUnresolvedPanels() noexcept
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
                // Только нерешённые: appliedOk панели не трогаем на каждом VTC Add.
                if (s && (s->needsRetry || !s->appliedOk || !s->visualsResolved))
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
            ApplyPanelVisualsSync(*s);
            if (s->needsRetry || !s->appliedOk)
                ++still;
        }
        m_unresolved.store(still, std::memory_order_release);
    }

    void TrayOverflowPanelRegistry::Unregister(InstanceHandle handle) noexcept
    {
        std::shared_ptr<PanelSlot> slot;
        {
            std::lock_guard lock(m_mx);
            const auto it = m_panels.find(handle);
            if (it != m_panels.end())
            {
                slot = it->second;
                m_panels.erase(it);
            }
            else
            {
                // Remove мог прийти по Border handle, пока слот keyed по Overflow.
                for (auto pit = m_panels.begin(); pit != m_panels.end(); ++pit)
                {
                    if (pit->second && pit->second->borderHandle == handle)
                    {
                        slot = pit->second;
                        m_panels.erase(pit);
                        break;
                    }
                }
            }
        }
        if (!slot)
            return;

        try
        {
            bool hadOrig = false;
            bool usedDisp = false;
            (void)RestoreSlotOnUiThread(std::move(slot), true, hadOrig, usedDisp);
        }
        catch (...)
        {
        }
    }

    void TrayOverflowPanelRegistry::RestoreAll() noexcept
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
            L"[TaskbarCompat][Overflow] RestoreAll total=%u ok=%u fail=%u noOrig=%u "
            L"disp=%u pendingAsync=%d",
            total,
            ok,
            fail,
            noOrig,
            viaDispatcher,
            pending);
        m_themePreferClearRestore = false;
    }

    void TrayOverflowPanelRegistry::ReapplyPatchedBrushes() noexcept
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
                {
                    s->appliedOk = false;
                    s->needsRetry = true;
                    snapshot.push_back(s);
                }
            }
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void TrayOverflowPanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
            m_themePreferClearRestore = true;
        }

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
            // Keep CornerRadius: ClearValue+Capture может дать 0 → Stop прямые углы.
            // BorderBrush/Shadow не null'им — Capture после ClearValue keepPrev.
            const auto prevCR = slot->originalCornerRadius;
            try
            {
                if (slot->backgroundBorder)
                {
                    slot->backgroundBorder.ClearValue(
                        wux::Controls::Border::BackgroundProperty());
                    slot->backgroundBorder.ClearValue(
                        wux::Controls::Border::BorderBrushProperty());
                    slot->backgroundBorder.ClearValue(
                        wux::Controls::Border::BorderThicknessProperty());
                    slot->backgroundBorder.ClearValue(
                        wux::Controls::Border::CornerRadiusProperty());
                }
                if (slot->rootGrid)
                {
                    slot->rootGrid.ClearValue(wux::Controls::Panel::BackgroundProperty());
                }
            }
            catch (...)
            {
            }
            slot->originalBackground = nullptr;
            slot->originalRootGridBackground = nullptr;
            slot->originalCornerRadius = prevCR;
            slot->chromeCaptured = false;
            slot->chromePatched = false;
            slot->appliedOk = false;
            slot->needsRetry = true;
            slot->visualsResolved = false;
        }

        for (auto& s : snapshot)
        {
            ApplyPanelGuarded(s);
            // CR capture только при layout > 0; иначе SizeChanged доведёт chromeCaptured.
            if (s && s->needsRetry)
            {
                m_unresolved.store(1, std::memory_order_release);
                EnsureSizeHook(s);
            }
        }
    }

    void TrayOverflowPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        ReapplyPatchedBrushes();
    }
}
