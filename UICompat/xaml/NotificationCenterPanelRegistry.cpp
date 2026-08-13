#include "UiPch.h"
#include "xaml/NotificationCenterPanelRegistry.hpp"
#include "xaml/UiTapSite.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/XamlImageBrushRetry.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"
#include <TrayXamlPaint.h>

#include <DebugLog.h>
#include <RootBackdropPolicy.h>

#include <atomic>
#include <cstdio>
#include <vector>
#include <string_view>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

namespace UICompat
{
    namespace
    {
        namespace wux = winrt::Windows::UI::Xaml;

        // Win32-памп живого ControlCenterWindow надёжнее CoreDispatcher закрытого QS.
        constexpr DWORD kRestoreWaitMs = 1'000;
        constexpr std::uint32_t kMaxBackdropRetries = 3;

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

        [[nodiscard]] double CoreWindowCornerRadiusDip(DWORD cornerType) noexcept
        {
            return Shared::TrayXamlPaint::XamlCornerRadiusDip(
                cornerType, QueryTaskbarHeightDip());
        }

        [[nodiscard]] bool ThicknessAnyPositive(const wux::Thickness& t) noexcept
        {
            return t.Left > 0.0 || t.Top > 0.0 || t.Right > 0.0 || t.Bottom > 0.0;
        }

        [[nodiscard]] winrt::Windows::Foundation::IInspectable ReadLocalValueWithWorkaround(
            wux::DependencyObject const& element,
            wux::DependencyProperty const& property) noexcept
        {
            try
            {
                const auto value = element.ReadLocalValue(property);
                if (!value)
                    return value;
                const auto className = winrt::get_class_name(value);
                if (className == L"Windows.UI.Xaml.Data.BindingExpressionBase"
                    || className == L"Windows.UI.Xaml.Data.BindingExpression")
                {
                    return element.GetAnimationBaseValue(property);
                }
                return value;
            }
            catch (...)
            {
                return nullptr;
            }
        }

        [[nodiscard]] bool IsBindingExpressionLocal(
            wux::DependencyObject const& element,
            wux::DependencyProperty const& property) noexcept
        {
            try
            {
                const auto raw = element.ReadLocalValue(property);
                if (!raw)
                    return false;
                if (raw == wux::DependencyProperty::UnsetValue())
                    return false;
                const auto className = winrt::get_class_name(raw);
                return className == L"Windows.UI.Xaml.Data.BindingExpressionBase"
                    || className == L"Windows.UI.Xaml.Data.BindingExpression";
            }
            catch (...)
            {
                return false;
            }
        }

        void CaptureLocalProp(
            NcLocalPropCapture& cap,
            wux::DependencyObject const& element,
            wux::DependencyProperty const& property) noexcept
        {
            if (cap.captured)
                return;
            try
            {
                const auto raw = element.ReadLocalValue(property);
                if (!raw || raw == wux::DependencyProperty::UnsetValue())
                {
                    cap.clearOnRestore = true;
                    cap.value = nullptr;
                }
                else if (IsBindingExpressionLocal(element, property))
                {
                    // Binding рвётся local SetValue — restore = ClearValue.
                    cap.clearOnRestore = true;
                    cap.value = ReadLocalValueWithWorkaround(element, property);
                }
                else
                {
                    cap.clearOnRestore = false;
                    cap.value = raw;
                }
                cap.captured = true;
            }
            catch (...)
            {
                cap.clearOnRestore = true;
                cap.value = nullptr;
                cap.captured = true;
            }
        }

        void RestoreLocalProp(
            wux::DependencyObject const& element,
            wux::DependencyProperty const& property,
            NcLocalPropCapture const& cap) noexcept
        {
            if (!cap.captured || !element)
                return;
            try
            {
                if (cap.clearOnRestore || !cap.value)
                {
                    element.ClearValue(property);
                    return;
                }
                element.SetValue(property, cap.value);
            }
            catch (...)
            {
                try
                {
                    element.ClearValue(property);
                }
                catch (...)
                {
                }
            }
        }

        [[nodiscard]] wux::Controls::Border FindL1OuterBorder(
            wux::Controls::Grid panel) noexcept
        {
            if (!panel)
                return nullptr;
            try
            {
                wux::DependencyObject cur = panel;
                for (int depth = 0; depth < 16 && cur; ++depth)
                {
                    if (const auto fe = cur.try_as<wux::FrameworkElement>())
                    {
                        if (fe.Name() == L"L1Grid")
                        {
                            if (const auto g = fe.try_as<wux::Controls::Grid>())
                            {
                                const int n =
                                    wux::Media::VisualTreeHelper::GetChildrenCount(g);
                                for (int i = 0; i < n; ++i)
                                {
                                    const auto child =
                                        wux::Media::VisualTreeHelper::GetChild(g, i);
                                    if (const auto b = child.try_as<wux::Controls::Border>())
                                        return b;
                                }
                            }
                            return nullptr;
                        }
                    }
                    cur = wux::Media::VisualTreeHelper::GetParent(cur);
                }
            }
            catch (...)
            {
            }
            return nullptr;
        }

        [[nodiscard]] bool IsMediaTransportPanelName(std::wstring_view name) noexcept
        {
            return name == L"MediaTransportControlsRegion"
                || name == L"MediaTransportControlsRoot";
        }

        [[nodiscard]] winrt::hstring TryReadHttpImageBrushUri(
            wux::Media::Brush const& brush) noexcept
        {
            try
            {
                const auto imageBrush = brush.try_as<wux::Media::ImageBrush>();
                if (!imageBrush)
                    return {};
                const auto imageSource = imageBrush.ImageSource();
                if (!imageSource)
                    return {};
                const auto bitmap = imageSource.try_as<wux::Media::Imaging::BitmapImage>();
                if (!bitmap)
                    return {};
                const auto uri = bitmap.UriSource();
                if (!uri)
                    return {};
                const winrt::hstring uriString = uri.ToString();
                if (uriString.size() < 7)
                    return {};
                const wchar_t* s = uriString.c_str();
                if ((_wcsnicmp(s, L"http://", 7) != 0)
                    && (_wcsnicmp(s, L"https://", 8) != 0))
                    return {};
                return uriString;
            }
            catch (...)
            {
                return {};
            }
        }

        void CaptureMediaRootAlbumUri(
            wux::Controls::Grid const& grid,
            winrt::hstring& outUri) noexcept
        {
            if (!grid || !outUri.empty())
                return;
            try
            {
                if (grid.Name() != L"MediaTransportControlsRoot")
                    return;
                outUri = TryReadHttpImageBrushUri(grid.Background());
            }
            catch (...)
            {
            }
        }

        void RehydrateMediaRootAlbum(
            wux::Controls::Grid const& grid,
            winrt::hstring const& albumUri,
            NcLocalPropCapture const& originalBackground) noexcept
        {
            if (!grid || albumUri.empty())
                return;
            try
            {
                if (grid.Name() != L"MediaTransportControlsRoot")
                    return;

                wux::Media::ImageBrush imageBrush{ nullptr };
                if (const auto current = grid.Background())
                    imageBrush = current.try_as<wux::Media::ImageBrush>();
                if (!imageBrush && originalBackground.value)
                    imageBrush = originalBackground.value.try_as<wux::Media::ImageBrush>();
                if (!imageBrush)
                    return;

                wux::Media::Imaging::BitmapImage bitmapImage;
                bitmapImage.UriSource(winrt::Windows::Foundation::Uri(albumUri));
                imageBrush.ImageSource(bitmapImage);
                grid.Background(imageBrush);
            }
            catch (...)
            {
            }
        }

        void RelayoutNcGrid(wux::Controls::Grid const& grid) noexcept
        {
            if (!grid)
                return;
            try
            {
                grid.InvalidateArrange();
                grid.InvalidateMeasure();
                grid.UpdateLayout();
            }
            catch (...)
            {
            }
        }

        void RestoreGridBackground(
            wux::Controls::Grid const& grid,
            NcLocalPropCapture const& originalBackground,
            bool& hadOrig) noexcept
        {
            hadOrig = originalBackground.captured
                && !originalBackground.clearOnRestore
                && static_cast<bool>(originalBackground.value);
            try
            {
                RestoreLocalProp(
                    grid, wux::Controls::Panel::BackgroundProperty(), originalBackground);
                RelayoutNcGrid(grid);
            }
            catch (...)
            {
                try
                {
                    grid.ClearValue(wux::Controls::Panel::BackgroundProperty());
                    RelayoutNcGrid(grid);
                }
                catch (...)
                {
                }
            }
        }

        [[nodiscard]] HWND FindNcSurfaceHwndInProcess() noexcept
        {
            // SEH: несколько CoreWindow (toast / NC). Первый попавшийся часто toast →
            // RunFromWindowThread на чужой tid → restore NC не доходит (кастом после Stop).
            struct Ctx
            {
                DWORD pid{};
                DWORD preferTid{};
                HWND controlCenter{};
                HWND byShellTid{};
                HWND largeCore{};
                HWND anyCore{};
                LONG largeArea{};
            } ctx{ GetCurrentProcessId(), GetShellSurfaceThreadId(),
                   nullptr, nullptr, nullptr, nullptr, 0 };

            EnumWindows(
                [](HWND hwnd, LPARAM lp) -> BOOL
                {
                    auto* c = reinterpret_cast<Ctx*>(lp);
                    DWORD pid = 0;
                    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
                    if (pid != c->pid)
                        return TRUE;
                    wchar_t cls[256]{};
                    if (GetClassNameW(hwnd, cls, 256) <= 0)
                        return TRUE;
                    if (_wcsicmp(cls, L"ControlCenterWindow") == 0)
                    {
                        c->controlCenter = hwnd;
                        return FALSE;
                    }
                    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0)
                        return TRUE;

                    if (!c->anyCore)
                        c->anyCore = hwnd;
                    if (c->preferTid != 0 && tid == c->preferTid && !c->byShellTid)
                        c->byShellTid = hwnd;

                    RECT rc{};
                    if (GetWindowRect(hwnd, &rc))
                    {
                        const LONG w = rc.right - rc.left;
                        const LONG h = rc.bottom - rc.top;
                        const LONG area = (w > 0 && h > 0) ? (w * h) : 0;
                        // NC-sized (как IXDE defer cy>=600); toast обычно меньше.
                        if (h >= 600 && area > c->largeArea)
                        {
                            c->largeArea = area;
                            c->largeCore = hwnd;
                        }
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&ctx));

            HWND picked = nullptr;
            const char* how = "none";
            if (ctx.controlCenter)
            {
                picked = ctx.controlCenter;
                how = "cc";
            }
            else if (ctx.byShellTid)
            {
                picked = ctx.byShellTid;
                how = "shell_tid";
            }
            else if (ctx.largeCore)
            {
                picked = ctx.largeCore;
                how = "large_cw";
            }
            else if (ctx.anyCore)
            {
                picked = ctx.anyCore;
                how = "any_cw";
            }

            return picked;
        }
    }

    NotificationCenterPanelRegistry::NotificationCenterPanelRegistry(
        Registry::Settings settings) noexcept
        : m_settings(settings)
    {
    }

    unsigned NotificationCenterPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    void NotificationCenterPanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        if (slot.chromeCaptured)
            return;
        if (slot.kind == SlotKind::ToastViewClear)
        {
            slot.chromeCaptured = true;
            return;
        }
        try
        {
            if (slot.kind == SlotKind::ToastBorder && slot.border)
            {
                CaptureLocalProp(
                    slot.originalBorderBrush,
                    slot.border,
                    wux::Controls::Border::BorderBrushProperty());
                CaptureLocalProp(
                    slot.originalBorderThickness,
                    slot.border,
                    wux::Controls::Border::BorderThicknessProperty());
                CaptureLocalProp(
                    slot.originalCornerRadius,
                    slot.border,
                    wux::Controls::Border::CornerRadiusProperty());
                try
                {
                    slot.originalBorderThicknessValue = slot.border.BorderThickness();
                }
                catch (...)
                {
                    slot.originalBorderThicknessValue = {};
                }
                slot.chromeCaptured = true;
                return;
            }

            if (!slot.grid)
                return;

            CaptureLocalProp(
                slot.originalBorderBrush,
                slot.grid,
                wux::Controls::Grid::BorderBrushProperty());
            CaptureLocalProp(
                slot.originalBorderThickness,
                slot.grid,
                wux::Controls::Grid::BorderThicknessProperty());
            CaptureLocalProp(
                slot.originalCornerRadius,
                slot.grid,
                wux::Controls::Grid::CornerRadiusProperty());

            if (slot.originalBorderThickness.value)
            {
                try
                {
                    slot.originalBorderThicknessValue =
                        winrt::unbox_value<wux::Thickness>(slot.originalBorderThickness.value);
                }
                catch (...)
                {
                    slot.originalBorderThicknessValue = slot.grid.BorderThickness();
                }
            }
            else
            {
                try
                {
                    slot.originalBorderThicknessValue = slot.grid.BorderThickness();
                }
                catch (...)
                {
                    slot.originalBorderThicknessValue = {};
                }
            }

            slot.l1OuterBorder = FindL1OuterBorder(slot.grid);
            if (slot.l1OuterBorder)
                slot.originalL1Visibility = slot.l1OuterBorder.Visibility();
            slot.chromeCaptured = true;
        }
        catch (...)
        {
            slot.originalBorderBrush = {};
            slot.originalBorderThickness = {};
            slot.originalCornerRadius = {};
            slot.l1OuterBorder = nullptr;
            slot.chromeCaptured = true;
        }
    }

    void NotificationCenterPanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (!slot.chromeCaptured || !slot.chromePatched)
        {
            slot.chromePatched = false;
            slot.l1Patched = false;
            return;
        }
        try
        {
            if (slot.kind == SlotKind::ToastBorder && slot.border)
            {
                RestoreLocalProp(
                    slot.border,
                    wux::Controls::Border::BorderBrushProperty(),
                    slot.originalBorderBrush);
                RestoreLocalProp(
                    slot.border,
                    wux::Controls::Border::BorderThicknessProperty(),
                    slot.originalBorderThickness);
                RestoreLocalProp(
                    slot.border,
                    wux::Controls::Border::CornerRadiusProperty(),
                    slot.originalCornerRadius);
            }
            else if (slot.grid)
            {
                // ClearValue только при Unset/Binding. Concrete local — SetValue без
                // предварительного ClearValue (на MediaTransport* always-ClearValue
                // схлопывал layout после Stop).
                RestoreLocalProp(
                    slot.grid,
                    wux::Controls::Grid::BorderBrushProperty(),
                    slot.originalBorderBrush);
                RestoreLocalProp(
                    slot.grid,
                    wux::Controls::Grid::BorderThicknessProperty(),
                    slot.originalBorderThickness);
                RestoreLocalProp(
                    slot.grid,
                    wux::Controls::Grid::CornerRadiusProperty(),
                    slot.originalCornerRadius);

                if (slot.l1Patched && slot.l1OuterBorder)
                    slot.l1OuterBorder.Visibility(slot.originalL1Visibility);
            }
        }
        catch (...)
        {
        }
        slot.chromePatched = false;
        slot.l1Patched = false;
    }

    void NotificationCenterPanelRegistry::RestorePanelVisuals(
        PanelSlot& slot,
        bool restoreBackground,
        bool& hadOrig) noexcept
    {
        hadOrig = false;
        bool mediaPanel = false;
        try
        {
            if (slot.grid)
                mediaPanel = IsMediaTransportPanelName(slot.grid.Name().c_str());
        }
        catch (...)
        {
        }

        RestoreChrome(slot);

        // Как MenuFlyout: ClearValue Background без backgroundPatched травит theme/Binding
        // (медиа-карточка QS после Stop — дыра вместо acrylic).
        const bool restoreBg = restoreBackground && slot.backgroundPatched;
        if (!restoreBg)
        {
            hadOrig = slot.originalBackground.captured
                && !slot.originalBackground.clearOnRestore
                && static_cast<bool>(slot.originalBackground.value);
            if (mediaPanel && slot.grid)
                RelayoutNcGrid(slot.grid);
            return;
        }

        // Debug BgChanged: не считать наш Restore чужим overwrite.
        ++slot.programmaticBgNesting;
        struct NestGuard
        {
            PanelSlot& s;
            ~NestGuard() noexcept
            {
                if (s.programmaticBgNesting > 0)
                    --s.programmaticBgNesting;
            }
        } nestGuard{ slot };

        if (slot.kind == SlotKind::ToastBorder && slot.border)
        {
            try
            {
                RestoreLocalProp(
                    slot.border,
                    wux::Controls::Border::BackgroundProperty(),
                    slot.originalBackground);
                hadOrig = slot.originalBackground.captured
                    && !slot.originalBackground.clearOnRestore
                    && static_cast<bool>(slot.originalBackground.value);
            }
            catch (...)
            {
            }
            slot.backgroundPatched = false;
            return;
        }

        if (slot.kind == SlotKind::ToastViewClear && slot.element)
        {
            try
            {
                if (const auto border = slot.element.try_as<wux::Controls::Border>())
                {
                    RestoreLocalProp(
                        border,
                        wux::Controls::Border::BackgroundProperty(),
                        slot.originalBackground);
                    if (slot.chromePatched)
                        border.Visibility(slot.originalL1Visibility);
                }
                else if (const auto panel = slot.element.try_as<wux::Controls::Panel>())
                {
                    RestoreLocalProp(
                        panel,
                        wux::Controls::Panel::BackgroundProperty(),
                        slot.originalBackground);
                }
                else
                {
                    RestoreLocalProp(
                        slot.element,
                        wux::Controls::Control::BackgroundProperty(),
                        slot.originalBackground);
                }
                hadOrig = slot.originalBackground.captured
                    && !slot.originalBackground.clearOnRestore
                    && static_cast<bool>(slot.originalBackground.value);
            }
            catch (...)
            {
            }
            slot.chromePatched = false;
            slot.backgroundPatched = false;
            return;
        }

        if (slot.grid)
        {
            RestoreGridBackground(slot.grid, slot.originalBackground, hadOrig);
            bool isMediaRegion = false;
            bool isControlCenter = false;
            try
            {
                const auto n = slot.grid.Name();
                isMediaRegion = (n == L"MediaTransportControlsRegion");
                isControlCenter = (n == L"ControlCenterRegion");
            }
            catch (...)
            {
            }
            // QS main card: Binding ClearValue → дыра (start→stop, медиа при этом OK).
            // Media Region / themeClear: нейтральный Acrylic вместо Glass tint / пустоты.
            if ((isMediaRegion || isControlCenter)
                && slot.originalBackground.captured
                && slot.originalBackground.clearOnRestore)
            {
                try
                {
                    if (isMediaRegion)
                    {
                        slot.grid.ClearValue(
                            wux::Controls::Panel::BackgroundProperty());
                    }
                    wux::Media::Brush cur{ nullptr };
                    try
                    {
                        cur = slot.grid.Background();
                    }
                    catch (...)
                    {
                    }
                    bool needFallback = !cur;
                    unsigned curKind = 0;
                    unsigned solidA = 0;
                    if (!needFallback)
                    {
                        if (const auto solid =
                                cur.try_as<wux::Media::SolidColorBrush>())
                        {
                            curKind = 1;
                            try
                            {
                                solidA = solid.Color().A;
                                needFallback = (solidA == 0);
                            }
                            catch (...)
                            {
                                needFallback = true;
                            }
                        }
                        else if (cur.try_as<wux::Media::AcrylicBrush>())
                            curKind = 2;
                        else
                            curKind = 3;
                    }
                    unsigned fallbackOk = 0;
                    if (needFallback)
                    {
                        if (isControlCenter || slot.themeClearBackgroundOnRestore)
                        {
                            if (auto fb = CreateNeutralAcrylicBackdropBrush())
                            {
                                slot.grid.Background(fb);
                                fallbackOk = 1;
                            }
                        }
                        else if (auto fb = CreateBackdropBrush(
                                     slot.grid,
                                     4,
                                     Registry::ReadFromRegistry().tintArgb))
                        {
                            slot.grid.Background(fb);
                            fallbackOk = 1;
                        }
                    }
                    if (isControlCenter && fallbackOk)
                        RelayoutNcGrid(slot.grid);
                }
                catch (...)
                {
                }
            }
            // MediaTransport*: после RestoreChrome — только relayout.
            if (mediaPanel)
            {
                try
                {
                    // Обложка Root: ImageSource мог быть сброшен ImageBrush-retry.
                    RehydrateMediaRootAlbum(
                        slot.grid, slot.mediaAlbumImageUri, slot.originalBackground);
                    RelayoutNcGrid(slot.grid);
                    if (const auto parent =
                            wux::Media::VisualTreeHelper::GetParent(slot.grid))
                    {
                        if (const auto parentGrid = parent.try_as<wux::Controls::Grid>())
                            RelayoutNcGrid(parentGrid);
                        // Sibling Root/Region — оба grid в медиа-карточке.
                        const int childCount =
                            wux::Media::VisualTreeHelper::GetChildrenCount(parent);
                        for (int i = 0; i < childCount; ++i)
                        {
                            const auto child =
                                wux::Media::VisualTreeHelper::GetChild(parent, i);
                            if (const auto g = child.try_as<wux::Controls::Grid>())
                            {
                                try
                                {
                                    const auto gName = g.Name();
                                    if (gName == L"MediaTransportControlsRegion"
                                        || gName == L"MediaTransportControlsRoot")
                                        RelayoutNcGrid(g);
                                }
                                catch (...)
                                {
                                }
                            }
                        }
                    }
                }
                catch (...)
                {
                }
            }
        }
        slot.backgroundPatched = false;
    }

    void NotificationCenterPanelRegistry::RestoreMarshalThunk(void* param) noexcept
    {
        auto* j = static_cast<MarshalRestoreJob*>(param);
        if (!j || !j->slot)
            return;
        try
        {
            RestorePanelVisuals(*j->slot, j->restoreBackground, j->hadOrig);
            if (j->clearRefs)
                j->slot->ClearWinrtRefs();
        }
        catch (const winrt::hresult_error& ex)
        {
            j->hr = ex.code();
        }
        catch (...)
        {
            j->hr = E_FAIL;
        }
    }

    HRESULT NotificationCenterPanelRegistry::RestoreSlotOnUiThread(
        std::shared_ptr<PanelSlot> slot,
        bool restoreBackground,
        bool& hadOrig,
        bool& usedDispatcher,
        bool clearRefs) noexcept
    {
        usedDispatcher = false;
        hadOrig = false;
        if (!slot)
            return S_FALSE;

        wux::FrameworkElement fe{ nullptr };
        if (slot->grid)
            fe = slot->grid;
        else if (slot->border)
            fe = slot->border;
        else
            fe = slot->element;
        if (!fe)
            return S_FALSE;

        HRESULT hrOut = S_OK;

        try
        {
            const auto dispatcher = fe.Dispatcher();
            if (dispatcher && dispatcher.HasThreadAccess())
            {
                RestorePanelVisuals(*slot, restoreBackground, hadOrig);
                if (clearRefs)
                    slot->ClearWinrtRefs();
                hrOut = S_OK;
                return hrOut;
            }

            // Предпочитаем Win32-памп HWND (ControlCenterWindow) — CoreDispatcher
            // закрытого QS не качает очередь.
            if (const HWND hwnd = FindNcSurfaceHwndInProcess())
            {
                auto job = std::make_shared<MarshalRestoreJob>();
                job->slot = slot;
                job->restoreBackground = restoreBackground;
                job->clearRefs = clearRefs;
                const bool marshaled = RunFromWindowThread(
                    hwnd,
                    &NotificationCenterPanelRegistry::RestoreMarshalThunk,
                    job.get());
                if (marshaled)
                {
                    usedDispatcher = true;
                    hadOrig = job->hadOrig;
                    hrOut = job->hr;
                    return hrOut;
                }
            }

            // Fallback: CoreDispatcher с коротким таймаутом.
            if (dispatcher)
            {
                usedDispatcher = true;
                const HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!done)
                {
                    hrOut = HRESULT_FROM_WIN32(GetLastError());
                    return hrOut;
                }

                struct Job
                {
                    MarshalRestoreJob restore{};
                    std::shared_ptr<RegistryState> state;
                    HANDLE done{};
                    bool ownsEvent{ true };

                    ~Job() noexcept
                    {
                        if (ownsEvent && done)
                            CloseHandle(done);
                    }
                };

                auto job = std::make_shared<Job>();
                job->restore.slot = std::move(slot);
                job->restore.restoreBackground = restoreBackground;
                job->restore.clearRefs = clearRefs;
                job->state = m_state;
                job->done = done;

                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            [job]()
                            {
                                RestoreMarshalThunk(&job->restore);
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
                    hrOut = ex.code();
                    return hrOut;
                }
                catch (...)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                    hrOut = E_FAIL;
                    return hrOut;
                }

                const DWORD wr = WaitForSingleObject(done, kRestoreWaitMs);
                if (wr == WAIT_OBJECT_0)
                {
                    hadOrig = job->restore.hadOrig;
                    hrOut = job->restore.hr;
                    job->ownsEvent = false;
                    CloseHandle(done);
                    return hrOut;
                }

                DebugLog(
                    DBG_WARNING,
                    L"[UICompat][NC] Restore wait timeout (%u ms)",
                    kRestoreWaitMs);
                hrOut = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                return hrOut;
            }

            // Нет диспетчера — best-effort inline.
            RestorePanelVisuals(*slot, restoreBackground, hadOrig);
            if (clearRefs && slot)
                slot->ClearWinrtRefs();
            hrOut = S_OK;
            return hrOut;
        }
        catch (const winrt::hresult_error& ex)
        {
            hrOut = ex.code();
            return hrOut;
        }
        catch (...)
        {
            hrOut = E_FAIL;
            return hrOut;
        }
    }

    void NotificationCenterPanelRegistry::ApplyPanelVisuals(
        PanelSlot& slot,
        const Registry::Settings& settings) noexcept
    {
        (void)ApplyPanelVisualsEx(slot, settings);
    }

    bool NotificationCenterPanelRegistry::ApplyPanelVisualsEx(
        PanelSlot& slot,
        const Registry::Settings& settings) noexcept
    {
        if (slot.kind == SlotKind::NcGrid && !slot.grid)
            return false;
        if (slot.kind == SlotKind::ToastBorder && !slot.border)
            return false;
        if (slot.kind == SlotKind::ToastViewClear && !slot.element)
            return false;

        // DWM / Disable: restore уже запатченного — без CaptureChrome (как JumpList).
        if (!settings.paintPathXaml)
        {
            if (slot.backgroundPatched || slot.chromePatched || slot.l1Patched)
            {
                bool hadOrig = false;
                RestorePanelVisuals(slot, true, hadOrig);
            }
            return false;
        }

        CaptureChrome(slot);

        // Toast view / AcrylicHighlight: Transparent BG; overlay — Collapsed.
        if (slot.kind == SlotKind::ToastViewClear)
        {
            wchar_t elName[80]{};
            try
            {
                if (slot.element)
                    wcsncpy_s(elName, slot.element.Name().c_str(), _TRUNCATE);
            }
            catch (...)
            {
            }
            const bool acrylicOverlay =
                _wcsicmp(elName, L"ToastAcrylicHightlightBorder") == 0
                || _wcsicmp(elName, L"ToastAcrylicHighlightBorder") == 0;
            try
            {
                winrt::Windows::UI::Color clear{};
                clear.A = 0;
                wux::Media::SolidColorBrush solid;
                solid.Color(clear);
                if (const auto border = slot.element.try_as<wux::Controls::Border>())
                {
                    border.Background(solid);
                    if (acrylicOverlay)
                    {
                        if (!slot.chromePatched)
                        {
                            try
                            {
                                slot.originalL1Visibility = border.Visibility();
                            }
                            catch (...)
                            {
                                slot.originalL1Visibility = wux::Visibility::Visible;
                            }
                        }
                        border.Visibility(wux::Visibility::Collapsed);
                        slot.chromePatched = true;
                    }
                }
                else if (const auto panel = slot.element.try_as<wux::Controls::Panel>())
                    panel.Background(solid);
                else if (const auto control = slot.element.try_as<wux::Controls::Control>())
                    control.Background(solid);
                slot.backgroundPatched = true;
            }
            catch (...)
            {
            }
            return false;
        }

        wchar_t panelName[128]{};
        try
        {
            if (slot.border)
                wcsncpy_s(panelName, slot.border.Name().c_str(), _TRUNCATE);
            else if (slot.grid)
                wcsncpy_s(panelName, slot.grid.Name().c_str(), _TRUNCATE);
        }
        catch (...)
        {
        }
        const bool mediaRootClear =
            _wcsicmp(panelName, L"MediaTransportControlsRoot") == 0;
        const bool mediaRegion =
            _wcsicmp(panelName, L"MediaTransportControlsRegion") == 0;
        const bool toastBorder = slot.kind == SlotKind::ToastBorder;

        if ((mediaRootClear || mediaRegion) && slot.grid)
            TrackMediaSubtreeImageBrushes(slot.grid);

        // Повторный Register/Reload создавал новый AcrylicBrush и сбрасывал
        // compositor connect (лог: apply 0/1.2/3.1 с, кастом ~10 с без событий).
        // Media Region: не keep — shell часто перетирает BG без watcher
        // (лог: Region apply→keep_bg, кастом на карточке нет).
        const bool keepExistingBg = slot.backgroundPatched
            && slot.kind == SlotKind::NcGrid
            && !mediaRootClear
            && !mediaRegion
            && !toastBorder
            && slot.appliedRequestedEffect == settings.xamlEffectType
            && slot.appliedTintArgb == settings.tintArgb;


        if (!keepExistingBg)
        {
            wux::UIElement brushElement{ nullptr };
            if (slot.border)
                brushElement = slot.border;
            else if (slot.grid)
                brushElement = slot.grid;

            wux::Media::Brush brush{ nullptr };
            // Toast border:
            // H19: Acrylic (4) → isolid=0 → мгновенный crash-loop.
            // H22: Blur/XamlBlurBrush (3) → isolid=1, exit ok, но SEH умирает ~1с
            //      (OnConnected/composition). Пока только реальный SolidColorBrush.
            DWORD effectForBrush = settings.xamlEffectType;
            if (toastBorder)
            {
                if (effectForBrush == 0 || effectForBrush == 5)
                    effectForBrush = 1; // Transparent solid
                else if (effectForBrush == 3 || effectForBrush == 4)
                    effectForBrush = 2; // Solid tint (не XamlBlurBrush / не AcrylicBrush)
            }

            // Toast Solid: tint A=0x40 (лог H28 tint=1089729523) почти не отличим от stock.
            DWORD toastTintArgb = settings.tintArgb;
            if (toastBorder)
            {
                const DWORD a = (toastTintArgb >> 24) & 0xFFu;
                if (a < 0xB0u)
                    toastTintArgb = (toastTintArgb & 0x00FFFFFFu) | 0xB0000000u;
            }

            if (mediaRootClear)
            {
                winrt::Windows::UI::Color clear{};
                clear.A = 0;
                wux::Media::SolidColorBrush solid;
                solid.Color(clear);
                brush = solid;
            }
            else
            {
                brush = CreateBackdropBrush(
                    brushElement,
                    effectForBrush,
                    toastBorder ? toastTintArgb : settings.tintArgb);
            }

            if (toastBorder && brush
                && brush.try_as<wux::Media::ISolidColorBrush>() == nullptr)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[UICompat][NC] ToastBorder brush without ISolidColorBrush — Solid fallback");
                brush = CreateBackdropBrush(brushElement, 2, settings.tintArgb);
                if (brush && brush.try_as<wux::Media::ISolidColorBrush>() == nullptr)
                    brush = nullptr;
            }

            if (!brush)
            {
                if ((settings.xamlEffectType == 3 || settings.xamlEffectType == 4)
                    && slot.backdropRetryCount < kMaxBackdropRetries)
                {
                    ++slot.backdropRetryCount;
                    return true; // нужен отложенный ретрай
                }
                bool hadOrig = false;
                RestorePanelVisuals(slot, true, hadOrig);
                return false;
            }

            ++slot.programmaticBgNesting;
            try
            {
                if (slot.border)
                    slot.border.Background(brush);
                else
                    slot.grid.Background(brush);
            }
            catch (...)
            {
            }
            if (slot.programmaticBgNesting > 0)
                --slot.programmaticBgNesting;
            slot.backgroundPatched = true;
            slot.appliedRequestedEffect = settings.xamlEffectType;
            slot.appliedEffectType = effectForBrush;
            slot.appliedTintArgb =
                toastBorder ? toastTintArgb : settings.tintArgb;
            if (mediaRootClear)
            {
                slot.appliedEffectType = 1;
                slot.appliedTintArgb = 0;
            }
            slot.backdropRetryCount = 0;
        }

        if (slot.kind == SlotKind::NcGrid)
        {
            try
            {
                if (slot.grid && !slot.bgWatchRegistered
                    && (_wcsicmp(panelName, L"NotificationCenterGrid") == 0
                        || _wcsicmp(panelName, L"CalendarCenterGrid") == 0
                        || _wcsicmp(panelName, L"ControlCenterRegion") == 0
                        || _wcsicmp(panelName, L"MediaTransportControlsRegion") == 0))
                {
                    PanelSlot* raw = &slot;
                    slot.bgWatchToken = slot.grid.RegisterPropertyChangedCallback(
                        wux::Controls::Panel::BackgroundProperty(),
                        [raw](wux::DependencyObject const&, wux::DependencyProperty const&)
                        {
                            if (!raw || raw->programmaticBgNesting > 0)
                                return;
                            // Лог QS pid=8272: overwrite +16ms → keep_bg держал чужую кисть.
                            // Theme: чужой Background → sample; флаг themeClear сохраняем
                            // (иначе CaptureLocalProp снимает clearOnRestore → Stop SetValue Glass).
                            try
                            {
                                if (raw->grid)
                                {
                                    const bool keepThemeClear =
                                        raw->themeClearBackgroundOnRestore;
                                    raw->originalBackground = {};
                                    CaptureLocalProp(
                                        raw->originalBackground,
                                        raw->grid,
                                        wux::Controls::Panel::BackgroundProperty());
                                    if (keepThemeClear)
                                    {
                                        raw->themeClearBackgroundOnRestore = true;
                                        raw->originalBackground.clearOnRestore = true;
                                    }
                                }
                            }
                            catch (...)
                            {
                            }
                            raw->backgroundPatched = false;
                            raw->appliedRequestedEffect = 0xFFFFFFFFu;
                            raw->reassertArmed = false;
                            UiTapSite::PostReapplyIfActive();
                        });
                    slot.bgWatchRegistered = true;
                }
            }
            catch (...)
            {
            }
        }

        // H27: полный chrome (BorderThickness/ClearValue) валил SEH.
        // H28/H30/H37: ToastAcrylicHightlight часто не регистрируется (no_fe) →
        // stock acrylic поверх Solid — «кастома нет». Collapse siblings по имени.
        if (toastBorder)
        {
            const bool wantCornersToast =
                settings.cornersPresent && settings.cornerType != 0;
            if (wantCornersToast)
            {
                try
                {
                    const double r = CoreWindowCornerRadiusDip(settings.cornerType);
                    slot.border.CornerRadius(wux::CornerRadius{ r, r, r, r });
                    slot.chromePatched = true;
                }
                catch (...)
                {
                }
            }
            unsigned collapsedOverlay = 0;
            try
            {
                if (const auto parent =
                        wux::Media::VisualTreeHelper::GetParent(slot.border))
                {
                    const int n = wux::Media::VisualTreeHelper::GetChildrenCount(parent);
                    for (int i = 0; i < n; ++i)
                    {
                        const auto child =
                            wux::Media::VisualTreeHelper::GetChild(parent, i);
                        const auto fe = child.try_as<wux::FrameworkElement>();
                        if (!fe)
                            continue;
                        wchar_t childName[80]{};
                        try
                        {
                            wcsncpy_s(childName, fe.Name().c_str(), _TRUNCATE);
                        }
                        catch (...)
                        {
                            continue;
                        }
                        if (_wcsicmp(childName, L"ToastAcrylicHightlightBorder") != 0
                            && _wcsicmp(childName, L"ToastAcrylicHighlightBorder") != 0)
                            continue;
                        fe.Visibility(wux::Visibility::Collapsed);
                        ++collapsedOverlay;
                    }
                }
            }
            catch (...)
            {
            }
            return false;
        }

        const bool wantCorners =
            settings.cornersPresent && settings.cornerType != 0;
        // Root: только Transparent Background (Windhawk). CornerRadius/Border на Root
        // после Stop схлопывали контент медиа-карточки; рамка — на Region.
        const bool wantTintBorder =
            wantCorners && settings.patchBorder && !settings.noBorder
            && settings.borderArgb != 0;

        if (mediaRootClear)
        {
            return false;
        }

        if (wantCorners)
        {
            const double r = CoreWindowCornerRadiusDip(settings.cornerType);
            const auto radius = wux::CornerRadius{ r, r, r, r };
            if (slot.border)
                slot.border.CornerRadius(radius);
            else
                slot.grid.CornerRadius(radius);

            if (wantTintBorder)
            {
                wux::Media::SolidColorBrush borderBrush;
                borderBrush.Color(ColorFromArgb(settings.borderArgb));
                const wux::Thickness th = ThicknessAnyPositive(slot.originalBorderThicknessValue)
                    ? slot.originalBorderThicknessValue
                    : wux::Thickness{ 1.0, 1.0, 1.0, 1.0 };
                if (slot.border)
                {
                    slot.border.BorderBrush(borderBrush);
                    slot.border.BorderThickness(th);
                }
                else
                {
                    slot.grid.BorderBrush(borderBrush);
                    slot.grid.BorderThickness(th);
                }
            }
            else if ((mediaRegion || toastBorder)
                && (slot.originalBorderThickness.clearOnRestore
                    || slot.originalBorderBrush.clearOnRestore))
            {
                // Не писать Thickness{0} поверх Unset/Binding — на Stop ClearValue
                // после concrete 0 схлопывал MediaTransport layout (H15 bgClear).
                try
                {
                    if (slot.border)
                    {
                        slot.border.ClearValue(wux::Controls::Border::BorderBrushProperty());
                        slot.border.ClearValue(
                            wux::Controls::Border::BorderThicknessProperty());
                    }
                    else
                    {
                        slot.grid.ClearValue(wux::Controls::Grid::BorderBrushProperty());
                        slot.grid.ClearValue(wux::Controls::Grid::BorderThicknessProperty());
                    }
                }
                catch (...)
                {
                }
            }
            else if (slot.border)
            {
                slot.border.BorderBrush(nullptr);
                slot.border.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
            }
            else
            {
                slot.grid.BorderBrush(nullptr);
                slot.grid.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
            }

            const bool skipL1 = mediaRegion || toastBorder || !slot.grid;
            if (!skipL1)
            {
                if (!slot.l1OuterBorder)
                    slot.l1OuterBorder = FindL1OuterBorder(slot.grid);
                if (slot.l1OuterBorder)
                {
                    if (!slot.l1Patched)
                        slot.originalL1Visibility = slot.l1OuterBorder.Visibility();
                    slot.l1OuterBorder.Visibility(wux::Visibility::Collapsed);
                    slot.l1Patched = true;
                }
            }

            slot.chromePatched = true;
        }
        else
        {
            bool hadOrig = false;
            RestorePanelVisuals(slot, false, hadOrig);
        }

        return false;
    }

    void NotificationCenterPanelRegistry::ScheduleDeferredReassert(
        std::shared_ptr<PanelSlot> slot,
        std::shared_ptr<RegistryState> state,
        Registry::Settings settings) noexcept
    {
        if (!slot || !state || slot->reassertArmed)
            return;
        if (slot->kind != SlotKind::NcGrid || !slot->grid)
            return;
        if (state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            const auto dispatcher = slot->grid.Dispatcher();
            if (!dispatcher)
                return;

            slot->reassertArmed = true;
            std::weak_ptr<PanelSlot> weakSlot = slot;
            std::weak_ptr<RegistryState> weakState = state;
            state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
            (void)dispatcher.RunAsync(
                winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                winrt::Windows::UI::Core::DispatchedHandler(
                    [weakSlot, weakState, settings]()
                    {
                        auto st = weakState.lock();
                        auto s = weakSlot.lock();
                        if (st)
                        {
                            st->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                            if (st->shutdown.load(std::memory_order_acquire))
                                return;
                        }
                        else
                        {
                            return;
                        }
                        if (!s || !s->grid)
                            return;
                        try
                        {
                            NotificationCenterPanelRegistry stub{ settings };
                            (void)stub.ApplyPanelVisualsEx(*s, settings);
                        }
                        catch (...)
                        {
                        }
                    }));
        }
        catch (...)
        {
            state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
            slot->reassertArmed = false;
        }
    }

    void NotificationCenterPanelRegistry::ApplyPanelGuarded(
        std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot)
            return;
        wux::FrameworkElement fe{ nullptr };
        if (slot->grid)
            fe = slot->grid;
        else if (slot->border)
            fe = slot->border;
        else
            fe = slot->element;
        if (!fe)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        const Registry::Settings settingsCopy = [this]() noexcept
        {
            std::lock_guard lock(m_mx);
            return m_settings;
        }();

        // XAML→DWM: тот же RestoreSlotOnUiThread (Win32 HWND), не CoreDispatcher.RunAsync.
        if (!settingsCopy.paintPathXaml)
        {
            if (!(slot->backgroundPatched || slot->chromePatched || slot->l1Patched))
                return;
            bool hadOrig = false;
            bool usedDisp = false;
            (void)RestoreSlotOnUiThread(slot, true, hadOrig, usedDisp, /*clearRefs=*/false);
            return;
        }

        try
        {
            const auto dispatcher = fe.Dispatcher();
            if (dispatcher && !dispatcher.HasThreadAccess())
            {
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
                                // Toast-слоты без grid: раньше silent return → нет chrome.
                                if (!s)
                                    return;
                                if (!s->grid && s->kind == SlotKind::NcGrid)
                                    return;
                                if (!s->grid && !s->border && !s->element)
                                    return;

                                try
                                {
                                    NotificationCenterPanelRegistry stub{ settingsCopy };
                                    const bool needRetry =
                                        stub.ApplyPanelVisualsEx(*s, settingsCopy);

                                    if (!needRetry)
                                    {
                                        stub.ScheduleDeferredReassert(s, state, settingsCopy);
                                        return;
                                    }

                                    if (state->shutdown.load(std::memory_order_acquire))
                                        return;

                                    const auto disp = s->grid.Dispatcher();
                                    if (!disp)
                                        return;

                                    struct RetryOnce
                                    {
                                        static void Arm(
                                            winrt::Windows::UI::Core::CoreDispatcher const& disp,
                                            std::weak_ptr<PanelSlot> wSlot,
                                            std::weak_ptr<RegistryState> wState,
                                            Registry::Settings settings) noexcept
                                        {
                                            auto st = wState.lock();
                                            if (!st
                                                || st->shutdown.load(std::memory_order_acquire))
                                                return;
                                            st->pendingAsync.fetch_add(
                                                1, std::memory_order_acq_rel);
                                            try
                                            {
                                                (void)disp.RunAsync(
                                                    winrt::Windows::UI::Core::
                                                        CoreDispatcherPriority::Normal,
                                                    winrt::Windows::UI::Core::DispatchedHandler(
                                                        [wSlot, wState, settings]()
                                                        {
                                                            auto st2 = wState.lock();
                                                            auto p = wSlot.lock();
                                                            if (st2)
                                                            {
                                                                st2->pendingAsync.fetch_sub(
                                                                    1,
                                                                    std::memory_order_acq_rel);
                                                                if (st2->shutdown.load(
                                                                        std::memory_order_acquire))
                                                                    return;
                                                            }
                                                            else
                                                            {
                                                                return;
                                                            }
                                                            if (!p || !p->grid)
                                                                return;
                                                            try
                                                            {
                                                                NotificationCenterPanelRegistry
                                                                    stub2{ settings };
                                                                if (stub2.ApplyPanelVisualsEx(
                                                                        *p, settings))
                                                                {
                                                                    Arm(
                                                                        p->grid.Dispatcher(),
                                                                        wSlot,
                                                                        wState,
                                                                        settings);
                                                                }
                                                                else
                                                                {
                                                                    stub2.ScheduleDeferredReassert(
                                                                        p, st2, settings);
                                                                }
                                                            }
                                                            catch (...)
                                                            {
                                                            }
                                                        }));
                                            }
                                            catch (...)
                                            {
                                                st->pendingAsync.fetch_sub(
                                                    1, std::memory_order_acq_rel);
                                            }
                                        }
                                    };
                                    RetryOnce::Arm(disp, weakSlot, weakState, settingsCopy);
                                }
                                catch (...)
                                {
                                }
                            }));
                }
                catch (...)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                }
                return;
            }

            const bool needRetry = ApplyPanelVisualsEx(*slot, settingsCopy);
            if (!needRetry)
            {
                ScheduleDeferredReassert(slot, m_state, settingsCopy);
                return;
            }
            if (needRetry && slot->grid)
            {
                try
                {
                    const auto dispatcher2 = slot->grid.Dispatcher();
                    if (dispatcher2)
                    {
                        std::weak_ptr<PanelSlot> weakSlot = slot;
                        std::weak_ptr<RegistryState> weakState = m_state;
                        m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                        (void)dispatcher2.RunAsync(
                            winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
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
                                    if (!s || !s->grid)
                                        return;
                                    try
                                    {
                                        NotificationCenterPanelRegistry stub{ settingsCopy };
                                        if (!stub.ApplyPanelVisualsEx(*s, settingsCopy))
                                            stub.ScheduleDeferredReassert(s, state, settingsCopy);
                                    }
                                    catch (...)
                                    {
                                    }
                                }));
                    }
                }
                catch (...)
                {
                    m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                }
            }
        }
        catch (...)
        {
        }
    }

    void NotificationCenterPanelRegistry::RegisterPanel(
        InstanceHandle handle,
        wux::Controls::Grid grid) noexcept
    {
        if (!grid || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            UiTapSite::EnsureMarshalWindowOnUiThread();

            std::shared_ptr<PanelSlot> slot;
            {
                std::lock_guard lock(m_mx);
                auto& ref = m_panels[handle];
                if (!ref)
                    ref = std::make_shared<PanelSlot>();
                slot = ref;
                slot->kind = SlotKind::NcGrid;
                slot->grid = grid;
                slot->border = nullptr;
                slot->element = grid;
                if (!slot->originalBackground.captured)
                {
                    CaptureLocalProp(
                        slot->originalBackground,
                        grid,
                        wux::Controls::Panel::BackgroundProperty());
                    CaptureMediaRootAlbumUri(grid, slot->mediaAlbumImageUri);
                }
            }

            CaptureChrome(*slot);
            ApplyPanelGuarded(slot);

            const Registry::Settings settingsCopy = [this]() noexcept
            {
                std::lock_guard lock(m_mx);
                return m_settings;
            }();

            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][NC] Panel registered handle=%llu paintXaml=%u effect=%u "
                L"corner=%u patchBorder=%u",
                static_cast<unsigned long long>(handle),
                settingsCopy.paintPathXaml ? 1u : 0u,
                settingsCopy.xamlEffectType,
                settingsCopy.cornerType,
                settingsCopy.patchBorder ? 1u : 0u);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][NC] RegisterPanel hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[UICompat][NC] RegisterPanel unknown exception");
        }
    }

    void NotificationCenterPanelRegistry::RegisterToastBorder(
        InstanceHandle handle,
        wux::Controls::Border border) noexcept
    {
        if (!border || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            UiTapSite::EnsureMarshalWindowOnUiThread();

            std::shared_ptr<PanelSlot> slot;
            {
                std::lock_guard lock(m_mx);
                auto& ref = m_panels[handle];
                if (!ref)
                    ref = std::make_shared<PanelSlot>();
                slot = ref;
                slot->kind = SlotKind::ToastBorder;
                slot->border = border;
                slot->element = border;
                slot->grid = nullptr;
                if (!slot->originalBackground.captured)
                {
                    CaptureLocalProp(
                        slot->originalBackground,
                        border,
                        wux::Controls::Border::BackgroundProperty());
                }
            }

            CaptureChrome(*slot);
            ApplyPanelGuarded(slot);
            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][NC] ToastBorder registered handle=%llu name=%s",
                static_cast<unsigned long long>(handle),
                border.Name().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[UICompat][NC] RegisterToastBorder failed");
        }
    }

    void NotificationCenterPanelRegistry::RegisterToastViewClear(
        InstanceHandle handle,
        wux::FrameworkElement element) noexcept
    {
        if (!element || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            UiTapSite::EnsureMarshalWindowOnUiThread();

            std::shared_ptr<PanelSlot> slot;
            {
                std::lock_guard lock(m_mx);
                auto& ref = m_panels[handle];
                if (!ref)
                    ref = std::make_shared<PanelSlot>();
                slot = ref;
                slot->kind = SlotKind::ToastViewClear;
                slot->element = element;
                slot->grid = nullptr;
                slot->border = nullptr;
                if (!slot->originalBackground.captured)
                {
                    if (const auto border = element.try_as<wux::Controls::Border>())
                    {
                        CaptureLocalProp(
                            slot->originalBackground,
                            border,
                            wux::Controls::Border::BackgroundProperty());
                    }
                    else if (const auto panel = element.try_as<wux::Controls::Panel>())
                    {
                        CaptureLocalProp(
                            slot->originalBackground,
                            panel,
                            wux::Controls::Panel::BackgroundProperty());
                    }
                    else if (const auto control = element.try_as<wux::Controls::Control>())
                    {
                        CaptureLocalProp(
                            slot->originalBackground,
                            control,
                            wux::Controls::Control::BackgroundProperty());
                    }
                }
            }

            CaptureChrome(*slot);
            ApplyPanelGuarded(slot);
            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][NC] ToastViewClear registered handle=%llu name=%s",
                static_cast<unsigned long long>(handle),
                element.Name().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[UICompat][NC] RegisterToastViewClear failed");
        }
    }

    void NotificationCenterPanelRegistry::Unregister(InstanceHandle handle) noexcept
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
            if (!(slot->backgroundPatched || slot->chromePatched || slot->l1Patched))
                return;
            bool hadOrig = false;
            bool usedDisp = false;
            (void)RestoreSlotOnUiThread(std::move(slot), true, hadOrig, usedDisp);
        }
        catch (...)
        {
        }
    }

    void NotificationCenterPanelRegistry::RestoreAll() noexcept
    {
        m_state->shutdown.store(true, std::memory_order_release);
        ShutdownImageBrushRetry();

        // НЕ Sleep(pendingAsync) здесь: Shutdown часто на UI/marshal-потоке —
        // Sleep блокирует pump → pending не дренируется + ClearWinrtRefs после
        // timeout убивал слот до Restore (NC оставался с кастомом после Stop).

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

        const unsigned total = static_cast<unsigned>(snapshot.size());
        unsigned ok = 0;
        unsigned fail = 0;
        unsigned noOrig = 0;
        unsigned viaDispatcher = 0;
        for (auto& slot : snapshot)
        {
            bool hadOrig = false;
            bool usedDispatcher = false;
            // ClearWinrtRefs — внутри RestoreSlotOnUiThread / RestoreMarshalThunk.
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

        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][NC] RestoreAll total=%u ok=%u fail=%u noOrig=%u disp=%u pendingAsync=%d",
            total,
            ok,
            fail,
            noOrig,
            viaDispatcher,
            m_state->pendingAsync.load(std::memory_order_acquire));
    }

    void NotificationCenterPanelRegistry::ReapplyPatchedBrushes() noexcept
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
                    s->backdropRetryCount = 0;
                    snapshot.push_back(s);
                }
            }
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
    }

    void NotificationCenterPanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
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
            bool hadOrig = false;
            RestorePanelVisuals(*slot, true, hadOrig);
            try
            {
                if (slot->grid)
                {
                    slot->grid.ClearValue(wux::Controls::Panel::BackgroundProperty());
                    slot->grid.ClearValue(wux::Controls::Control::BorderBrushProperty());
                    slot->grid.ClearValue(wux::Controls::Control::BorderThicknessProperty());
                    slot->grid.ClearValue(wux::Controls::Control::CornerRadiusProperty());
                }
                if (slot->border)
                {
                    slot->border.ClearValue(wux::Controls::Border::BackgroundProperty());
                    slot->border.ClearValue(wux::Controls::Border::BorderBrushProperty());
                    slot->border.ClearValue(wux::Controls::Border::BorderThicknessProperty());
                    slot->border.ClearValue(wux::Controls::Border::CornerRadiusProperty());
                }
            }
            catch (...)
            {
            }
            slot->originalBackground = {};
            slot->originalBorderBrush = {};
            slot->originalBorderThickness = {};
            slot->originalCornerRadius = {};
            slot->chromeCaptured = false;
            slot->chromePatched = false;
            slot->backgroundPatched = false;
            slot->backdropRetryCount = 0;
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
        DebugLog(DBG_IMPORTANT, L"[UICompat][NC] RecaptureAfterThemeChange");
    }

    void NotificationCenterPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        ReapplyPatchedBrushes();
    }

    void NotificationCenterPanelRegistry::RefreshBrushesAfterThemeChange(
        Registry::Settings settings) noexcept
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
        // Live: сброс keepExistingBg → новый Glass brush (без ClearValue сейчас).
        // Stop: themeClearBackgroundOnRestore → всегда ClearValue BG (не SetValue
        // stale / не Glass fallback на Media Region).
        unsigned forcedClear = 0;
        for (auto& slot : snapshot)
        {
            if (!slot)
                continue;
            slot->themeClearBackgroundOnRestore = true;
            slot->originalBackground.clearOnRestore = true;
            if (slot->originalBackground.captured)
                ++forcedClear;
            if (slot->originalBorderBrush.captured)
                slot->originalBorderBrush.clearOnRestore = true;
            slot->backgroundPatched = false;
            slot->appliedRequestedEffect = 0xFFFFFFFFu;
            slot->backdropRetryCount = 0;
            slot->reassertArmed = false;
        }
        for (auto& s : snapshot)
            ApplyPanelGuarded(s);
        DebugLog(DBG_IMPORTANT, L"[UICompat][NC] RefreshBrushesAfterThemeChange");
    }
}
