#include "TaskbarPch.h"
#include "xaml/InputSwitchPanelRegistry.hpp"
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

        [[nodiscard]] double InputSwitchCornerRadiusDip(DWORD cornerType) noexcept
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
            return r.TopLeft > 0.0 || r.TopRight > 0.0 || r.BottomRight > 0.0
                || r.BottomLeft > 0.0;
        }

        [[nodiscard]] wux::Controls::Grid FindContentGrid(
            wux::DependencyObject root,
            int depth) noexcept
        {
            wux::Controls::Grid best{ nullptr };
            double bestArea = -1.0;
            if (!root)
                return nullptr;
            try
            {
                const auto walk = [&](auto&& self, wux::DependencyObject node, int d) -> void
                {
                    if (!node || d > kMaxResolveDepth)
                        return;
                    if (d >= 1)
                    {
                        if (const auto grid = node.try_as<wux::Controls::Grid>())
                        {
                            double area = 0.0;
                            try
                            {
                                const double w = grid.ActualWidth();
                                const double h = grid.ActualHeight();
                                if (w > 0.0 && h > 0.0)
                                    area = w * h;
                            }
                            catch (...)
                            {
                            }
                            if (!best || area > bestArea)
                            {
                                best = grid;
                                bestArea = area;
                            }
                        }
                    }
                    const int n = wux::Media::VisualTreeHelper::GetChildrenCount(node);
                    for (int i = 0; i < n; ++i)
                        self(self, wux::Media::VisualTreeHelper::GetChild(node, i), d + 1);
                };
                walk(walk, root, depth);
            }
            catch (...)
            {
            }
            return best;
        }

        [[nodiscard]] wux::Controls::Grid FindInnerGrid(wux::Controls::Grid contentGrid) noexcept
        {
            if (!contentGrid)
                return nullptr;
            try
            {
                const int n = wux::Media::VisualTreeHelper::GetChildrenCount(contentGrid);
                for (int i = 0; i < n; ++i)
                {
                    const auto child = wux::Media::VisualTreeHelper::GetChild(contentGrid, i);
                    if (const auto g = child.try_as<wux::Controls::Grid>())
                        return g;
                }
            }
            catch (...)
            {
            }
            return nullptr;
        }

        void RestoreGridBackground(
            wux::Controls::Grid const& grid,
            wux::Media::Brush const& originalBackground,
            bool& hadOrig) noexcept
        {
            hadOrig = static_cast<bool>(originalBackground);
            try
            {
                // ClearValue сбрасывает local value (в т.ч. AcrylicBrush), иначе
                // Background(original) иногда не вытесняет composition residual.
                grid.ClearValue(wux::Controls::Panel::BackgroundProperty());
                if (hadOrig)
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
                    L"[TaskbarCompat][InputSwitch] PIN failed GLE=%u pendingAsync=%d",
                    static_cast<unsigned>(GetLastError()),
                    pending);
                return;
            }
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][InputSwitch] module PIN (pendingAsync=%d)",
                pending);
        }
    }

    InputSwitchPanelRegistry::InputSwitchPanelRegistry(Registry::Settings settings) noexcept
        : m_settings(settings)
    {
    }

    unsigned InputSwitchPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    unsigned InputSwitchPanelRegistry::UnresolvedCount() const noexcept
    {
        return m_unresolved.load(std::memory_order_acquire);
    }

    bool InputSwitchPanelRegistry::ResolveVisuals(PanelSlot& slot) noexcept
    {
        if (!slot.root)
            return false;

        try
        {
            // Всегда пере-ищем: шаблон InputSwitcher достраивается после первого Add
            // (ранний Grid 0×0 → applied_ok, затем реальный chrome без нашего патча).
            const auto found = FindContentGrid(slot.root, 0);
            if (!found)
                return false;

            const bool gridChanged =
                !slot.contentGrid || (winrt::get_abi(slot.contentGrid) != winrt::get_abi(found));
            slot.contentGrid = found;
            slot.innerGrid = FindInnerGrid(slot.contentGrid);

            if (gridChanged)
            {
                slot.chromeCaptured = false;
                slot.appliedOk = false;
                slot.needsRetry = true;
                // Не перезаписывать original нашей кистью при смене Grid.
                try
                {
                    if (slot.contentGrid)
                        slot.originalBackground = slot.contentGrid.Background();
                    if (slot.innerGrid)
                        slot.originalInnerBackground = slot.innerGrid.Background();
                }
                catch (...)
                {
                }
            }
            else
            {
                if (!slot.originalBackground && slot.contentGrid)
                    slot.originalBackground = slot.contentGrid.Background();
                if (slot.innerGrid && !slot.originalInnerBackground)
                    slot.originalInnerBackground = slot.innerGrid.Background();
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

    void InputSwitchPanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        if (!slot.contentGrid || slot.chromeCaptured)
            return;
        try
        {
            // Theme Recapture ClearValue / live Shadow(nullptr) → sample null.
            // Не затирать ранее захваченные system brush/shadow (как Overflow O1).
            const auto sampledBrush = slot.contentGrid.BorderBrush();
            if (sampledBrush || !slot.originalBorderBrush)
                slot.originalBorderBrush = sampledBrush;

            const auto sampledTh = slot.contentGrid.BorderThickness();
            if (ThicknessAnyPositive(sampledTh)
                || !ThicknessAnyPositive(slot.originalBorderThickness))
                slot.originalBorderThickness = sampledTh;

            const auto sampledCr = slot.contentGrid.CornerRadius();
            if (CornerRadiusAnyPositive(sampledCr)
                || !CornerRadiusAnyPositive(slot.originalCornerRadius))
                slot.originalCornerRadius = sampledCr;

            try
            {
                const auto sampledShadow = slot.contentGrid.Shadow();
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
            if (!slot.originalBorderBrush)
                slot.originalBorderBrush = nullptr;
            if (!ThicknessAnyPositive(slot.originalBorderThickness))
                slot.originalBorderThickness = {};
            if (!CornerRadiusAnyPositive(slot.originalCornerRadius))
                slot.originalCornerRadius = {};
            if (!slot.originalShadow)
                slot.originalShadow = nullptr;
            slot.chromeCaptured = true;
        }
    }

    void InputSwitchPanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (!slot.contentGrid || !slot.chromeCaptured || !slot.chromePatched)
        {
            slot.chromePatched = false;
            return;
        }
        try
        {
            slot.contentGrid.BorderBrush(slot.originalBorderBrush);
            slot.contentGrid.BorderThickness(slot.originalBorderThickness);
            slot.contentGrid.CornerRadius(slot.originalCornerRadius);
            try
            {
                slot.contentGrid.Shadow(slot.originalShadow);
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

    void InputSwitchPanelRegistry::RestorePanelVisuals(
        PanelSlot& slot,
        bool restoreBackground,
        bool& hadOrig) noexcept
    {
        hadOrig = false;
        RestoreChrome(slot);

        if (restoreBackground && slot.innerGrid)
        {
            bool innerHad = false;
            RestoreGridBackground(slot.innerGrid, slot.originalInnerBackground, innerHad);
        }

        if (restoreBackground && slot.contentGrid)
            RestoreGridBackground(slot.contentGrid, slot.originalBackground, hadOrig);
        else
            hadOrig = static_cast<bool>(slot.originalBackground);
    }

    HRESULT InputSwitchPanelRegistry::RestoreSlotOnUiThread(
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
            if (!slot->root && !slot->contentGrid)
                return S_FALSE;

            const auto dispatcher = slot->root
                ? slot->root.Dispatcher()
                : (slot->contentGrid ? slot->contentGrid.Dispatcher() : nullptr);

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
                                        if (s.contentGrid && s.chromeCaptured && s.chromePatched)
                                        {
                                            try
                                            {
                                                s.contentGrid.BorderBrush(s.originalBorderBrush);
                                                s.contentGrid.BorderThickness(
                                                    s.originalBorderThickness);
                                                s.contentGrid.CornerRadius(s.originalCornerRadius);
                                                try
                                                {
                                                    s.contentGrid.Shadow(s.originalShadow);
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
                                        if (job->restoreBackground && s.innerGrid)
                                        {
                                            bool innerHad = false;
                                            RestoreGridBackground(
                                                s.innerGrid,
                                                s.originalInnerBackground,
                                                innerHad);
                                        }
                                        if (job->restoreBackground && s.contentGrid)
                                        {
                                            RestoreGridBackground(
                                                s.contentGrid,
                                                s.originalBackground,
                                                job->hadOrig);
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
                    // Событие уже сигналено; закрываем здесь, job не должен CloseHandle.
                    job->ownsEvent = false;
                    CloseHandle(done);
                    return hr;
                }

                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][InputSwitch] Restore wait timeout (%u ms)",
                    kRestoreWaitMs);
                // Timeout: UI позже SetEvent + ~Job CloseHandle — без записи в стек.
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

    void InputSwitchPanelRegistry::ApplyPanelVisualsSync(PanelSlot& slot) noexcept
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
        if (!slot.contentGrid)
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
            slot.contentGrid,
            settings.xamlEffectType,
            settings.tintArgb);

        if (!brush)
        {
            slot.needsRetry = true;
            return;
        }

        try
        {
            slot.contentGrid.Background(brush);

            if (slot.innerGrid)
            {
                winrt::Windows::UI::Color clear{};
                clear.A = 0;
                wux::Media::SolidColorBrush solid;
                solid.Color(clear);
                slot.innerGrid.Background(solid);
            }

            const bool wantCorners =
                settings.cornersPresent && settings.cornerType != 0;
            const bool wantTintBorder =
                settings.patchBorderStroke && settings.borderColorArgb != 0;

            // ThemeShadow оставляем (live тень InputSwitch).

            if (wantCorners)
            {
                const double r = InputSwitchCornerRadiusDip(settings.cornerType);
                slot.contentGrid.CornerRadius(wux::CornerRadius{ r, r, r, r });
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
                    slot.contentGrid.CornerRadius(slot.originalCornerRadius);
                }
                catch (...)
                {
                }
            }

            if (wantTintBorder)
            {
                wux::Media::SolidColorBrush borderBrush;
                borderBrush.Color(ColorFromArgb(settings.borderColorArgb));
                slot.contentGrid.BorderBrush(borderBrush);
                const wux::Thickness th =
                    ThicknessAnyPositive(slot.originalBorderThickness)
                    ? slot.originalBorderThickness
                    : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                slot.contentGrid.BorderThickness(th);
                slot.chromePatched = true;
            }
            else if (settings.patchBorderStroke || wantCorners)
            {
                slot.contentGrid.BorderBrush(nullptr);
                slot.contentGrid.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                slot.chromePatched = true;
            }

            const bool firstApply = !slot.appliedOk;
            slot.appliedOk = true;
            slot.needsRetry = false;

            // Layout ещё 0×0 — оставляем dirty, иначе VTC перестанет ретраить.
            try
            {
                const double aw = slot.contentGrid.ActualWidth();
                const double ah = slot.contentGrid.ActualHeight();
                if (aw < 1.0 || ah < 1.0)
                {
                    slot.needsRetry = true;
                    slot.appliedOk = false;
                }
            }
            catch (...)
            {
            }

            if (firstApply && slot.firstSeenTick != 0 && slot.appliedOk)
            {
                const ULONGLONG now = GetTickCount64();
                const unsigned delayMs = static_cast<unsigned>(
                    now >= slot.firstSeenTick ? (now - slot.firstSeenTick) : 0);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][InputSwitch] applied delayMs=%u paintXaml=%u effect=%u",
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
                L"[TaskbarCompat][InputSwitch] ApplyPanelVisualsSync hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            slot.needsRetry = true;
        }
    }

    void InputSwitchPanelRegistry::ApplyPanelGuarded(std::shared_ptr<PanelSlot> slot) noexcept
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
                // Settings snapshot — без this в лямбде.
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
                                if (!s || !s->root)
                                    return;

                                // Inline apply без this: resolve + brush.
                                try
                                {
                                    if (!s->visualsResolved || !s->contentGrid)
                                    {
                                        s->contentGrid = FindContentGrid(s->root, 0);
                                        if (!s->contentGrid)
                                        {
                                            s->needsRetry = true;
                                            return;
                                        }
                                        s->innerGrid = FindInnerGrid(s->contentGrid);
                                        if (!s->originalBackground)
                                            s->originalBackground = s->contentGrid.Background();
                                        if (s->innerGrid && !s->originalInnerBackground)
                                            s->originalInnerBackground =
                                                s->innerGrid.Background();
                                        if (!s->chromeCaptured)
                                        {
                                            try
                                            {
                                                s->originalBorderBrush =
                                                    s->contentGrid.BorderBrush();
                                                s->originalBorderThickness =
                                                    s->contentGrid.BorderThickness();
                                                s->originalCornerRadius =
                                                    s->contentGrid.CornerRadius();
                                                try
                                                {
                                                    s->originalShadow = s->contentGrid.Shadow();
                                                }
                                                catch (...)
                                                {
                                                    s->originalShadow = nullptr;
                                                }
                                                s->chromeCaptured = true;
                                            }
                                            catch (...)
                                            {
                                                s->chromeCaptured = true;
                                            }
                                        }
                                        s->visualsResolved = true;
                                    }

                                    if (!settingsCopy.paintPathXaml)
                                    {
                                        if (s->contentGrid && s->chromeCaptured && s->chromePatched)
                                        {
                                            s->contentGrid.BorderBrush(s->originalBorderBrush);
                                            s->contentGrid.BorderThickness(
                                                s->originalBorderThickness);
                                            s->contentGrid.CornerRadius(s->originalCornerRadius);
                                            try
                                            {
                                                s->contentGrid.Shadow(s->originalShadow);
                                            }
                                            catch (...)
                                            {
                                            }
                                            s->chromePatched = false;
                                        }
                                        if (s->innerGrid)
                                        {
                                            bool innerHad = false;
                                            RestoreGridBackground(
                                                s->innerGrid,
                                                s->originalInnerBackground,
                                                innerHad);
                                        }
                                        if (s->contentGrid)
                                        {
                                            bool had = false;
                                            RestoreGridBackground(
                                                s->contentGrid, s->originalBackground, had);
                                        }
                                        s->needsRetry = false;
                                        s->appliedOk = true;
                                        return;
                                    }

                                    auto brush = CreateBackdropBrush(
                                        s->contentGrid,
                                        settingsCopy.xamlEffectType,
                                        settingsCopy.tintArgb);
                                    if (!brush)
                                    {
                                        s->needsRetry = true;
                                        return;
                                    }
                                    s->contentGrid.Background(brush);
                                    if (s->innerGrid)
                                    {
                                        winrt::Windows::UI::Color clear{};
                                        clear.A = 0;
                                        wux::Media::SolidColorBrush solid;
                                        solid.Color(clear);
                                        s->innerGrid.Background(solid);
                                    }
                                    const bool wantCorners = settingsCopy.cornersPresent
                                        && settingsCopy.cornerType != 0;
                                    // ThemeShadow оставляем (live тень InputSwitch).
                                    if (wantCorners)
                                    {
                                        const double r = InputSwitchCornerRadiusDip(
                                            settingsCopy.cornerType);
                                        s->contentGrid.CornerRadius(
                                            wux::CornerRadius{ r, r, r, r });
                                        if (settingsCopy.patchBorderStroke
                                            && settingsCopy.borderColorArgb != 0)
                                        {
                                            wux::Media::SolidColorBrush borderBrush;
                                            borderBrush.Color(
                                                ColorFromArgb(settingsCopy.borderColorArgb));
                                            s->contentGrid.BorderBrush(borderBrush);
                                            const wux::Thickness th =
                                                ThicknessAnyPositive(s->originalBorderThickness)
                                                ? s->originalBorderThickness
                                                : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                                            s->contentGrid.BorderThickness(th);
                                        }
                                        else
                                        {
                                            s->contentGrid.BorderBrush(nullptr);
                                            s->contentGrid.BorderThickness(
                                                wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                                        }
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

    void InputSwitchPanelRegistry::RegisterPanel(
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
            }

            ApplyPanelVisualsSync(*slot);
            if (slot->needsRetry)
                m_unresolved.store(1, std::memory_order_release);

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][InputSwitch] Panel registered handle=%llu paintXaml=%u "
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
                L"[TaskbarCompat][InputSwitch] RegisterPanel hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][InputSwitch] RegisterPanel unknown exception");
        }
    }

    void InputSwitchPanelRegistry::RetryUnresolvedPanels() noexcept
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
                // Все панели: шаблон может сменить Grid после appliedOk.
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
            ApplyPanelVisualsSync(*s);
            if (s->needsRetry || !s->appliedOk)
                ++still;
        }
        m_unresolved.store(still, std::memory_order_release);
    }

    void InputSwitchPanelRegistry::Unregister(InstanceHandle handle) noexcept
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
            bool hadOrig = false;
            bool usedDisp = false;
            (void)RestoreSlotOnUiThread(std::move(slot), true, hadOrig, usedDisp);
        }
        catch (...)
        {
        }
    }

    void InputSwitchPanelRegistry::RestoreAll() noexcept
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
            L"[TaskbarCompat][InputSwitch] RestoreAll total=%u ok=%u fail=%u noOrig=%u "
            L"disp=%u pendingAsync=%d",
            total,
            ok,
            fail,
            noOrig,
            viaDispatcher,
            pending);
    }

    void InputSwitchPanelRegistry::ReapplyPatchedBrushes() noexcept
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

    void InputSwitchPanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
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
            try
            {
                if (slot->contentGrid)
                    slot->contentGrid.ClearValue(wux::Controls::Panel::BackgroundProperty());
                if (slot->innerGrid)
                    slot->innerGrid.ClearValue(wux::Controls::Panel::BackgroundProperty());
                if (slot->contentGrid)
                {
                    slot->contentGrid.ClearValue(wux::Controls::Control::BorderBrushProperty());
                    slot->contentGrid.ClearValue(wux::Controls::Control::BorderThicknessProperty());
                    slot->contentGrid.ClearValue(wux::Controls::Control::CornerRadiusProperty());
                }
            }
            catch (...)
            {
            }
            slot->originalBackground = nullptr;
            slot->originalInnerBackground = nullptr;
            // Не null brush/shadow: Capture после ClearValue keepPrev (Overflow O1).
            slot->chromeCaptured = false;
            slot->chromePatched = false;
            slot->appliedOk = false;
            slot->needsRetry = true;
            slot->visualsResolved = false;
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void InputSwitchPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
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
