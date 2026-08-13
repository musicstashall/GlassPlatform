#include "UiPch.h"
#include "xaml/ShellSurfacePanelRegistry.hpp"
#include "xaml/UiTapSite.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"

#include <TrayXamlPaint.h>
#include <DebugLog.h>
#include <Helpers/ThemeHelper.h>

#include <cstdio>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace UICompat
{
    namespace
    {
        namespace wux = winrt::Windows::UI::Xaml;

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

        [[nodiscard]] double ShellSurfaceCornerRadiusDip(DWORD cornerType) noexcept
        {
            return Shared::TrayXamlPaint::XamlCornerRadiusDip(
                cornerType, QueryTaskbarHeightDip());
        }

        [[nodiscard]] bool ThicknessAnyPositive(const wux::Thickness& t) noexcept
        {
            return t.Left > 0.0 || t.Top > 0.0 || t.Right > 0.0 || t.Bottom > 0.0;
        }

        void ReleasePropertyChain(
            PropertyChainSource* sources,
            unsigned sourceCount,
            PropertyChainValue* values,
            unsigned valueCount) noexcept
        {
            if (sources)
            {
                for (unsigned i = 0; i < sourceCount; ++i)
                {
                    SysFreeString(sources[i].TargetType);
                    SysFreeString(sources[i].Name);
                }
                CoTaskMemFree(sources);
            }
            if (values)
            {
                for (unsigned i = 0; i < valueCount; ++i)
                {
                    SysFreeString(values[i].Type);
                    SysFreeString(values[i].DeclaringType);
                    SysFreeString(values[i].ValueType);
                    SysFreeString(values[i].ItemType);
                    SysFreeString(values[i].Value);
                    SysFreeString(values[i].PropertyName);
                }
                CoTaskMemFree(values);
            }
        }

    }

    ShellSurfacePanelRegistry::ShellSurfacePanelRegistry(
        IXamlDiagnostics* diagnostics,
        Registry::Settings settings) noexcept
        : m_settings(settings)
    {
        if (!diagnostics)
            return;
        m_diagnostics.copy_from(diagnostics);
        (void)diagnostics->QueryInterface(
            __uuidof(IVisualTreeService3), m_vts3.put_void());
    }

    unsigned ShellSurfacePanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    ShellSurfacePanelRegistry::Role ShellSurfacePanelRegistry::ClassifyBorder(
        wux::Controls::Border const& border) noexcept
    {
        try
        {
            const auto name = border.Name();
            if (name == L"AcrylicOverlay")
                return Role::AcrylicOverlay;
            if (name == L"LayerBorder")
                return Role::LayerBorder;
            if (name == L"dropshadow"
                || name == L"DropShadow"
                || name == L"RootGridDropShadow"
                || name == L"StartDropShadow")
                return Role::DropShadow;
            if (name == L"AppBorder")
                return Role::AppBorder;
            // Оставшаяся подтверждённая цель allow-list — AcrylicBorder.
            return Role::AcrylicBorder;
        }
        catch (...)
        {
            return Role::AcrylicBorder;
        }
    }

    void ShellSurfacePanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        if (!slot.border || slot.chromeCaptured)
            return;
        // Без VTS3 нельзя сохранить originals → не помечаем captured
        // (иначе Apply патчит, а Restore не откатит Background).
        if (!m_diagnostics || !m_vts3 || slot.handle == 0)
            return;

        const wchar_t* names[4]{};
        size_t nameCount = 0;
        switch (slot.role)
        {
        case Role::AcrylicOverlay:
        case Role::LayerBorder:
            names[0] = L"Visibility";
            nameCount = 1;
            break;
        case Role::DropShadow:
            names[0] = L"Opacity";
            nameCount = 1;
            break;
        case Role::AcrylicBorder:
        case Role::AppBorder:
            names[0] = L"Background";
            names[1] = L"BorderBrush";
            names[2] = L"BorderThickness";
            names[3] = L"CornerRadius";
            nameCount = 4;
            break;
        }

        unsigned sourceCount = 0;
        unsigned propertyCount = 0;
        PropertyChainSource* sources = nullptr;
        PropertyChainValue* properties = nullptr;
        const HRESULT chainHr = m_vts3->GetPropertyValuesChain(
            slot.handle,
            &sourceCount,
            &sources,
            &propertyCount,
            &properties);

        for (size_t i = 0; i < nameCount && i < slot.originals.size(); ++i)
        {
            auto& original = slot.originals[i];
            if (original.captured)
                continue;

            unsigned propertyIndex = UINT_MAX;
            InstanceHandle propertyHandle = 0;
            ::IInspectable* inspectable = nullptr;
            HRESULT propertyHr = chainHr;

            if (SUCCEEDED(propertyHr) && properties)
            {
                for (unsigned p = 0; p < propertyCount; ++p)
                {
                    if (properties[p].PropertyName
                        && wcscmp(properties[p].PropertyName, names[i]) == 0)
                    {
                        propertyIndex = properties[p].Index;
                        break;
                    }
                }
                if (propertyIndex == UINT_MAX)
                    propertyHr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            }
            if (SUCCEEDED(propertyHr) && propertyIndex != UINT_MAX)
            {
                propertyHr = m_vts3->GetProperty(
                    slot.handle, propertyIndex, &propertyHandle);
            }
            if (SUCCEEDED(propertyHr) && propertyHandle != 0)
            {
                propertyHr = m_diagnostics->GetIInspectableFromHandle(
                    propertyHandle, &inspectable);
            }
            if (SUCCEEDED(propertyHr) && inspectable)
            {
                original.index = propertyIndex;
                original.value.attach(inspectable);
                original.captured = true;
            }
            else if (inspectable)
            {
                inspectable->Release();
            }
        }

        ReleasePropertyChain(
            sources, sourceCount, properties, propertyCount);

        // AppBorder/AcrylicBorder: минимум Background; overlay/shadow — Visibility/Opacity.
        // Неполный capture → chromeCaptured=false → повтор в Prepare/Apply.
        switch (slot.role)
        {
        case Role::AcrylicOverlay:
        case Role::LayerBorder:
        case Role::DropShadow:
            slot.chromeCaptured = slot.originals[0].captured;
            break;
        case Role::AcrylicBorder:
        case Role::AppBorder:
            slot.chromeCaptured = slot.originals[0].captured;
            break;
        }

    }

    void ShellSurfacePanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (!slot.border || !slot.chromePatched)
        {
            slot.chromePatched = false;
            return;
        }

        // После MenuFlyout VTS3 SetProperty часто S_OK, но WinRT-local
        // (Background/CR/Border*), выставленные Apply, остаются. Restore тем же
        // каналом, что Apply — WinRT с captured IInspectable.
        unsigned winrtRestored = 0;
        try
        {
            auto asInsp = [](winrt::com_ptr<::IInspectable> const& value)
                -> winrt::Windows::Foundation::IInspectable
            {
                winrt::Windows::Foundation::IInspectable obj{ nullptr };
                if (value)
                    winrt::copy_from_abi(obj, value.get());
                return obj;
            };

            switch (slot.role)
            {
            case Role::AcrylicOverlay:
            case Role::LayerBorder:
                slot.border.Visibility(wux::Visibility::Visible);
                ++winrtRestored;
                break;

            case Role::DropShadow:
                slot.border.Opacity(1.0);
                ++winrtRestored;
                break;

            case Role::AcrylicBorder:
            case Role::AppBorder:
                // originals[]: Background, BorderBrush, BorderThickness, CornerRadius
                // Theme→Stop: VTS3 ClearProperty = дыра Search. После theme —
                // нейтральный Acrylic (не stale Glass tint / не ClearProperty).
                {
                    bool usedNeutral = false;
                    if (slot.clearBackgroundOnRestore)
                    {
                        if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                        {
                            slot.border.Background(neut);
                            ++winrtRestored;
                            usedNeutral = true;
                        }
                    }
                    if (!usedNeutral
                        && slot.originals[0].captured
                        && slot.originals[0].value)
                    {
                        const auto obj = asInsp(slot.originals[0].value);
                        if (const auto brush = obj.try_as<wux::Media::Brush>())
                        {
                            // Не ClearValue: штатные кисти Пуска/Поиска — локальные;
                            // ClearValue ломает дерево (пустой WebView).
                            slot.border.Background(brush);
                            ++winrtRestored;
                        }
                    }
                }
                if (slot.originals[1].captured && slot.originals[1].value)
                {
                    const auto obj = asInsp(slot.originals[1].value);
                    if (const auto brush = obj.try_as<wux::Media::Brush>())
                    {
                        slot.border.BorderBrush(brush);
                        ++winrtRestored;
                    }
                    else
                    {
                        slot.border.BorderBrush(nullptr);
                        ++winrtRestored;
                    }
                }
                if (slot.originals[2].captured && slot.originals[2].value)
                {
                    const auto obj = asInsp(slot.originals[2].value);
                    if (const auto ref =
                            obj.try_as<winrt::Windows::Foundation::IReference<wux::Thickness>>())
                    {
                        slot.border.BorderThickness(ref.Value());
                        ++winrtRestored;
                    }
                }
                if (slot.originals[3].captured && slot.originals[3].value)
                {
                    const auto obj = asInsp(slot.originals[3].value);
                    if (const auto ref =
                            obj.try_as<winrt::Windows::Foundation::IReference<wux::CornerRadius>>())
                    {
                        slot.border.CornerRadius(ref.Value());
                        ++winrtRestored;
                    }
                }
                break;
            }
        }
        catch (...)
        {
        }

        // Fallback Visibility/Opacity если WinRT не сработал (не ClearValue фона).
        if (winrtRestored == 0)
        {
            try
            {
                if (slot.role == Role::AcrylicOverlay || slot.role == Role::LayerBorder)
                    slot.border.Visibility(wux::Visibility::Visible);
                else if (slot.role == Role::DropShadow)
                    slot.border.Opacity(1.0);
            }
            catch (...)
            {
            }
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][ShellSurface] Restore role=%u winrt=%u "
            L"capBg=%u capBrush=%u capTh=%u capCr=%u",
            static_cast<unsigned>(slot.role),
            winrtRestored,
            slot.originals[0].captured ? 1u : 0u,
            slot.originals[1].captured ? 1u : 0u,
            slot.originals[2].captured ? 1u : 0u,
            slot.originals[3].captured ? 1u : 0u);

        slot.chromePatched = false;
    }

    void ShellSurfacePanelRegistry::RestorePanelVisuals(PanelSlot& slot) noexcept
    {
        RestoreChrome(slot);
    }

    void ShellSurfacePanelRegistry::ApplyPanelVisuals(
        PanelSlot& slot,
        const Registry::Settings& settings) noexcept
    {
        if (!slot.border)
            return;

        if (!settings.paintPathXaml)
        {
            RestorePanelVisuals(slot);
            return;
        }

        try
        {
            if (!slot.chromeCaptured)
                CaptureChrome(slot);

            switch (slot.role)
            {
            case Role::AcrylicOverlay:
                // Без original Visibility Stop не восстановит слой.
                if (!slot.originals[0].captured)
                {
                    return;
                }
                slot.border.Visibility(wux::Visibility::Collapsed);
                slot.chromePatched = true;
                return;

            case Role::LayerBorder:
                // Иначе штатный acrylic и его углы видны за AppBorder.
                if (!slot.originals[0].captured)
                {
                    return;
                }
                slot.border.Visibility(wux::Visibility::Collapsed);
                slot.chromePatched = true;
                return;

            case Role::DropShadow:
                if (!slot.originals[0].captured)
                {
                    return;
                }
                slot.border.Opacity(0.0);
                slot.chromePatched = true;
                return;

            case Role::AcrylicBorder:
            case Role::AppBorder:
                // Без captured Background Stop оставляет кастомный brush.
                if (!slot.originals[0].captured)
                {
                    return;
                }
                break;
            }

            // Blur (3): XamlBlurBrush = паритет WindhawkBlur на Start/Search.
            // При fail (GetElementVisual) — fallback Acrylic (не форсировать 3→4 заранее).
            const DWORD effectRaw = settings.xamlEffectType;
            DWORD effect = effectRaw;
            auto brush = CreateBackdropBrush(slot.border, effect, settings.tintArgb);
            if (!brush && effectRaw == 3)
            {
                effect = 4;
                brush = CreateBackdropBrush(slot.border, effect, settings.tintArgb);
            }

            if (!brush)
            {
                // Disable/System или полный fail — вернуть оригинал (паритет NC).
                if (slot.chromePatched || effectRaw == 0 || effectRaw == 5)
                    RestorePanelVisuals(slot);
                return;
            }

            slot.border.Background(brush);
            // Сразу: если CornerRadius бросит — Stop всё равно откатит Background.
            slot.chromePatched = true;

            const bool wantCorners =
                settings.cornersPresent && settings.cornerType != 0;
            const bool wantTintBorder =
                wantCorners && settings.patchBorder && !settings.noBorder
                && settings.borderArgb != 0;

            if (wantCorners)
            {
                const double r = ShellSurfaceCornerRadiusDip(settings.cornerType);
                slot.border.CornerRadius(wux::CornerRadius{ r, r, r, r });

                if (wantTintBorder)
                {
                    wux::Media::SolidColorBrush borderBrush;
                    borderBrush.Color(ColorFromArgb(settings.borderArgb));
                    slot.border.BorderBrush(borderBrush);
                    slot.border.BorderThickness(wux::Thickness{ 1.0, 1.0, 1.0, 1.0 });
                }
                else
                {
                    slot.border.BorderBrush(nullptr);
                    slot.border.BorderThickness(wux::Thickness{ 0.0, 0.0, 0.0, 0.0 });
                }
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][ShellSurface] ApplyPanel hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[UICompat][ShellSurface] ApplyPanel unknown exception");
        }
    }

    void ShellSurfacePanelRegistry::PrepareAndApplyDeferred(
        std::shared_ptr<PanelSlot> slot,
        const Registry::Settings& settings) noexcept
    {
        if (!slot || !slot->border)
            return;

        try
        {
            slot->role = ClassifyBorder(slot->border);
            // WinRT-геттеры этих свойств небезопасны; исходные значения
            // сохраняются через IVisualTreeService3 handles.
            CaptureChrome(*slot);
            ApplyPanelVisuals(*slot, settings);
            // Первый AcrylicBrush иногда fail (patched:0) — сразу retry.
            if (!slot->chromePatched && settings.paintPathXaml)
                ApplyPanelVisuals(*slot, settings);
            // Не PostShellPrepare отсюда: FlushPending(flushing=true) съест вложенный Post.
            if (!slot->chromePatched && settings.paintPathXaml)
                slot->pendingPrepare = true;
        }
        catch (...)
        {
            if (slot && settings.paintPathXaml)
                slot->pendingPrepare = true;
        }
    }

    void ShellSurfacePanelRegistry::FlushPendingPrepares() noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (m_state->flushing.exchange(true, std::memory_order_acq_rel))
            return;

        struct FlushGuard
        {
            std::atomic<bool>& flag;
            ~FlushGuard() noexcept
            {
                flag.store(false, std::memory_order_release);
            }
        } guard{ m_state->flushing };

        Registry::Settings settingsCopy{};
        std::vector<std::shared_ptr<PanelSlot>> pending;
        {
            std::lock_guard lock(m_mx);
            settingsCopy = m_settings;
            pending.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s && s->pendingPrepare && s->border)
                {
                    s->pendingPrepare = false;
                    pending.push_back(s);
                }
            }
        }

        for (auto& slot : pending)
        {
            try
            {
                const auto dispatcher = slot->border.Dispatcher();
                // CatchUp/SHOW уже на UI-потоке — apply sync до первого paint.
                if (!dispatcher || dispatcher.HasThreadAccess())
                {
                    PrepareAndApplyDeferred(slot, settingsCopy);
                    continue;
                }
                std::weak_ptr<PanelSlot> weakSlot = slot;
                std::weak_ptr<RegistryState> weakState = m_state;
                auto diagnostics = m_diagnostics;
                m_state->pendingAsync.fetch_add(1, std::memory_order_acq_rel);
                (void)dispatcher.RunAsync(
                    winrt::Windows::UI::Core::CoreDispatcherPriority::High,
                    winrt::Windows::UI::Core::DispatchedHandler(
                        [weakSlot, weakState, diagnostics, settingsCopy]()
                        {
                            auto state = weakState.lock();
                            auto s = weakSlot.lock();
                            if (state)
                            {
                                state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                                if (state->shutdown.load(std::memory_order_acquire))
                                    return;
                            }
                            else
                            {
                                return;
                            }
                            if (!s || !s->border)
                                return;
                            ShellSurfacePanelRegistry stub{
                                diagnostics.get(), settingsCopy };
                            stub.PrepareAndApplyDeferred(s, settingsCopy);
                        }));
            }
            catch (...)
            {
                m_state->pendingAsync.fetch_sub(1, std::memory_order_acq_rel);
                PrepareAndApplyDeferred(slot, settingsCopy);
            }
        }

        // После снятия flushing — один Post на оставшиеся fail (не storm из Prepare).
        bool needRepost = false;
        {
            std::lock_guard lock(m_mx);
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s && s->pendingPrepare)
                {
                    needRepost = true;
                    break;
                }
            }
        }
        if (needRepost)
            UiTapSite::PostShellPrepareIfActive();
    }

    void ShellSurfacePanelRegistry::RegisterBorder(
        InstanceHandle handle,
        wux::Controls::Border border) noexcept
    {
        if (!border || handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        try
        {
            // На стеке Advise/VTC — только сохранить Border. Никаких Name()/Background()/
            // EnsureMarshal/Composition: это может сорвать Advise через SEH.
            {
                std::lock_guard lock(m_mx);
                auto& ref = m_panels[handle];
                if (!ref)
                    ref = std::make_shared<PanelSlot>();
                // Тот же handle, новый Border ABI — штатный rebuild Пуска после SHOW.
                // Старый chromePatched блокировал pending → первый показ без патча.
                const bool borderChanged =
                    ref->border
                    && winrt::get_abi(ref->border) != winrt::get_abi(border);
                if (borderChanged)
                {
                    // Advise: без RestoreChrome (crash-loop на QI).
                    // Лог: Apply → border_changed_recap → Capture уже нашего
                    // Background → Stop «восстанавливает» кастом. Поэтому
                    // originals/chromeCaptured первого (до Apply) capture
                    // сохраняем; только сбрасываем chromePatched и re-Apply.
                    ref->chromePatched = false;
                }
                ref->border = border;
                ref->handle = handle;
                ref->pendingPrepare = true;
            }

            // Post на marshal HWND (уже создан в SetSite) — выполнится после возврата из Advise.
            UiTapSite::PostShellPrepareIfActive();

            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][ShellSurface] Border queued handle=%llu",
                static_cast<unsigned long long>(handle));
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][ShellSurface] RegisterBorder hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[UICompat][ShellSurface] RegisterBorder unknown exception");
        }
    }

    void ShellSurfacePanelRegistry::Unregister(InstanceHandle handle) noexcept
    {
        std::shared_ptr<PanelSlot> slot;
        {
            std::lock_guard lock(m_mx);
            const auto it = m_panels.find(handle);
            if (it == m_panels.end())
                return;
            slot = std::move(it->second);
            m_panels.erase(it);
        }
        if (slot)
            RestorePanelVisuals(*slot);
    }

    void ShellSurfacePanelRegistry::RestoreAll() noexcept
    {
        m_state->shutdown.store(true, std::memory_order_release);

        std::map<InstanceHandle, std::shared_ptr<PanelSlot>> copy;
        {
            std::lock_guard lock(m_mx);
            copy.swap(m_panels);
        }

        for (auto& [handle, slot] : copy)
        {
            (void)handle;
            if (slot)
            {
                RestorePanelVisuals(*slot);
                slot->ClearWinrtRefs();
            }
        }
        DebugLog(DBG_IMPORTANT, L"[UICompat][ShellSurface] RestoreAll");
    }

    void ShellSurfacePanelRegistry::ReapplyPatchedBrushes() noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        std::vector<std::shared_ptr<PanelSlot>> slots;
        Registry::Settings settings{};
        {
            std::lock_guard lock(m_mx);
            settings = m_settings;
            slots.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    slots.push_back(s);
            }
        }
        for (auto& slot : slots)
            ApplyPanelVisuals(*slot, settings);
    }

    void ShellSurfacePanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }

        std::vector<std::shared_ptr<PanelSlot>> slots;
        {
            std::lock_guard lock(m_mx);
            slots.reserve(m_panels.size());
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s)
                    slots.push_back(s);
            }
        }


        for (auto& slot : slots)
        {
            if (!slot)
                continue;
            if (slot->chromePatched)
                RestorePanelVisuals(*slot);
            try
            {
                if (slot->border
                    && (slot->role == Role::AcrylicBorder || slot->role == Role::AppBorder))
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
            for (auto& original : slot->originals)
            {
                original.value = nullptr;
                original.captured = false;
                original.index = UINT_MAX;
            }
            slot->chromeCaptured = false;
            slot->chromePatched = false;
            CaptureChrome(*slot);
            ApplyPanelVisuals(*slot, settings);
        }
        DebugLog(DBG_IMPORTANT, L"[UICompat][ShellSurface] RecaptureAfterThemeChange");
    }

    void ShellSurfacePanelRegistry::RefreshBrushesAfterThemeChange(
        Registry::Settings settings) noexcept
    {
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s
                    && (s->role == Role::AcrylicBorder || s->role == Role::AppBorder))
                {
                    s->clearBackgroundOnRestore = true;
                }
            }
        }
        // Apply всегда CreateBackdropBrush — без live ClearValue Background.
        ReapplyPatchedBrushes();
        DebugLog(DBG_IMPORTANT, L"[UICompat][ShellSurface] RefreshBrushesAfterThemeChange");
    }

    void ShellSurfacePanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
    {
        {
            std::lock_guard lock(m_mx);
            m_settings = settings;
        }
        ReapplyPatchedBrushes();
    }
}
