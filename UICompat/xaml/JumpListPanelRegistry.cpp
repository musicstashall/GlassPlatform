#include "UiPch.h"
#include "xaml/JumpListPanelRegistry.hpp"
#include "xaml/XamlDiagInit.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"

#include <TrayXamlPaint.h>
#include <DebugLog.h>

#include <cstdio>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace UICompat
{
    namespace
    {
        namespace wux = winrt::Windows::UI::Xaml;

        constexpr wchar_t const* kPropNames[4] = {
            L"Background",
            L"BorderBrush",
            L"BorderThickness",
            L"CornerRadius",
        };

        [[nodiscard]] winrt::Windows::UI::Color ColorFromArgb(DWORD argb) noexcept
        {
            return winrt::Windows::UI::Color{
                static_cast<uint8_t>((argb >> 24) & 0xFFu),
                static_cast<uint8_t>((argb >> 16) & 0xFFu),
                static_cast<uint8_t>((argb >> 8) & 0xFFu),
                static_cast<uint8_t>(argb & 0xFFu),
            };
        }

        [[nodiscard]] bool WantTintBorder(const Registry::Settings& settings) noexcept
        {
            // Tint не зависит от углов.
            return settings.patchBorder && !settings.noBorder && settings.borderArgb != 0;
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

        // copy_to_abi требует void*& — вернуть raw IInspectable* (+1 ref).
        [[nodiscard]] ::IInspectable* DetachInspectable(
            winrt::Windows::Foundation::IInspectable const& value) noexcept
        {
            if (!value)
                return nullptr;
            void* raw = nullptr;
            winrt::copy_to_abi(value, raw);
            return reinterpret_cast<::IInspectable*>(raw);
        }

        [[nodiscard]] HWND FindCoreWindowForTid(DWORD preferTid) noexcept
        {
            struct Ctx
            {
                DWORD pid{};
                DWORD preferTid{};
                HWND byTid{};
                HWND smallCore{};
                HWND anyCore{};
                LONG smallArea{ LONG_MAX };
            } ctx{ GetCurrentProcessId(), preferTid, nullptr, nullptr, nullptr, LONG_MAX };

            EnumWindows(
                [](HWND hwnd, LPARAM lp) -> BOOL
                {
                    auto* c = reinterpret_cast<Ctx*>(lp);
                    DWORD pid = 0;
                    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
                    if (pid != c->pid)
                        return TRUE;
                    wchar_t cls[64]{};
                    if (GetClassNameW(hwnd, cls, 64) <= 0)
                        return TRUE;
                    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0)
                        return TRUE;

                    if (!c->anyCore)
                        c->anyCore = hwnd;
                    if (c->preferTid != 0 && tid == c->preferTid && !c->byTid)
                        c->byTid = hwnd;

                    RECT rc{};
                    if (GetClientRect(hwnd, &rc))
                    {
                        const LONG w = rc.right - rc.left;
                        const LONG h = rc.bottom - rc.top;
                        const LONG area = (w > 0 && h > 0) ? (w * h) : 0;
                        // Jump List обычно меньше NC (≥600).
                        if (h > 0 && h < 600 && area > 0 && area < c->smallArea)
                        {
                            c->smallArea = area;
                            c->smallCore = hwnd;
                        }
                    }
                    return TRUE;
                },
                reinterpret_cast<LPARAM>(&ctx));

            if (ctx.byTid)
                return ctx.byTid;
            if (ctx.smallCore)
                return ctx.smallCore;
            return ctx.anyCore;
        }
    }

    JumpListPanelRegistry::JumpListPanelRegistry(
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

    unsigned JumpListPanelRegistry::PanelCount() const noexcept
    {
        std::lock_guard lock(m_mx);
        return static_cast<unsigned>(m_panels.size());
    }

    unsigned JumpListPanelRegistry::UnresolvedCount() const noexcept
    {
        return m_unresolved.load(std::memory_order_acquire);
    }

    bool JumpListPanelRegistry::SetPropByIndex(
        InstanceHandle element,
        unsigned propIndex,
        ::IInspectable* value) noexcept
    {
        if (!m_diagnostics || !m_vts3 || element == 0 || propIndex == UINT_MAX || !value)
            return false;
        InstanceHandle valueHandle = 0;
        const HRESULT hrHandle =
            m_diagnostics->GetHandleFromIInspectable(value, &valueHandle);
        if (FAILED(hrHandle) || valueHandle == 0)
            return false;
        const HRESULT hrSet = m_vts3->SetProperty(element, valueHandle, propIndex);
        return SUCCEEDED(hrSet);
    }

    void JumpListPanelRegistry::CaptureChrome(PanelSlot& slot) noexcept
    {
        if (slot.chromeCaptured || slot.handle == 0 || !m_diagnostics || !m_vts3)
            return;

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

        for (size_t i = 0; i < 4; ++i)
        {
            auto& original = slot.originals[i];
            unsigned propertyIndex = UINT_MAX;
            InstanceHandle propertyHandle = 0;
            ::IInspectable* inspectable = nullptr;
            HRESULT propertyHr = chainHr;

            if (SUCCEEDED(propertyHr) && properties)
            {
                for (unsigned p = 0; p < propertyCount; ++p)
                {
                    if (properties[p].PropertyName
                        && wcscmp(properties[p].PropertyName, kPropNames[i]) == 0)
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

        ReleasePropertyChain(sources, sourceCount, properties, propertyCount);
        // Только при успешном Background — иначе повторный Capture возможен.
        slot.chromeCaptured = (slot.originals[0].index != UINT_MAX);
    }

    unsigned JumpListPanelRegistry::RestoreChrome(PanelSlot& slot) noexcept
    {
        if (!slot.chromePatched || slot.handle == 0 || !m_diagnostics || !m_vts3)
        {
            slot.chromePatched = false;
            return 0;
        }

        unsigned restored = 0;
        for (size_t i = 0; i < slot.originals.size(); ++i)
        {
            auto& original = slot.originals[i];
            if (!original.captured || original.index == UINT_MAX)
                continue;
            // 0 = Background: ClearProperty → дыра (репорт Jump List theme→stop).
            // Neutral Acrylic текущей темы вместо stale SetProperty / пустоты.
            if (i == 0 && slot.clearBackgroundOnRestore)
            {
                (void)m_vts3->ClearProperty(slot.handle, original.index);
                if (const auto neut = CreateNeutralAcrylicBackdropBrush())
                {
                    ::IInspectable* brushInsp = DetachInspectable(
                        neut.as<winrt::Windows::Foundation::IInspectable>());
                    if (SetPropByIndex(slot.handle, original.index, brushInsp))
                        ++restored;
                    if (brushInsp)
                        brushInsp->Release();
                }
                else
                    ++restored;
                continue;
            }
            if (!original.value)
                continue;
            if (SetPropByIndex(slot.handle, original.index, original.value.get()))
                ++restored;
        }
        slot.chromePatched = false;
        return restored;
    }

    void JumpListPanelRegistry::RestoreMarshalThunk(void* param) noexcept
    {
        auto* job = static_cast<MarshalSlotJob*>(param);
        if (!job || !job->self || !job->slot)
            return;
        job->restored = job->self->RestoreChrome(*job->slot);
    }

    void JumpListPanelRegistry::ApplyMarshalThunk(void* param) noexcept
    {
        auto* job = static_cast<MarshalSlotJob*>(param);
        if (!job || !job->self || !job->slot)
            return;
        if (job->slot->kind == PanelKind::Companion)
            job->self->ApplyCompanionOnCurrentThread(job->slot);
        else
            job->self->ApplyPanelVisualsOnCurrentThread(job->slot);
    }

    void JumpListPanelRegistry::RestoreSlotOnUiThread(
        std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot || !slot->chromePatched)
            return;

        const DWORD curTid = GetCurrentThreadId();
        if (slot->uiTid != 0 && curTid == slot->uiTid)
        {
            (void)RestoreChrome(*slot);
            return;
        }

        const HWND hwnd = FindCoreWindowForTid(slot->uiTid);
        if (!hwnd)
        {
            (void)RestoreChrome(*slot);
            return;
        }

        MarshalSlotJob job{};
        job.self = this;
        job.slot = std::move(slot);
        const bool ok = RunFromWindowThread(
            hwnd, &JumpListPanelRegistry::RestoreMarshalThunk, &job);
        if (!ok && job.slot)
            (void)RestoreChrome(*job.slot);
    }

    void JumpListPanelRegistry::ApplyPanelVisuals(std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;

        // ReloadSettings идёт с NC marshal tid — VTS3 SetProperty на Jump List
        // STA часто no-op/fail → после DWM→XAML меню остаются без кастома.
        const DWORD curTid = GetCurrentThreadId();
        if (slot->uiTid == 0 || curTid == slot->uiTid)
        {
            if (slot->kind == PanelKind::Companion)
                ApplyCompanionOnCurrentThread(std::move(slot));
            else
                ApplyPanelVisualsOnCurrentThread(std::move(slot));
            return;
        }

        const HWND hwnd = FindCoreWindowForTid(slot->uiTid);
        if (!hwnd)
        {
            if (slot->kind == PanelKind::Companion)
                ApplyCompanionOnCurrentThread(std::move(slot));
            else
                ApplyPanelVisualsOnCurrentThread(std::move(slot));
            return;
        }

        MarshalSlotJob job{};
        job.self = this;
        job.slot = std::move(slot);
        const bool ok = RunFromWindowThread(
            hwnd, &JumpListPanelRegistry::ApplyMarshalThunk, &job);
        if (!ok && job.slot)
        {
            if (job.slot->kind == PanelKind::Companion)
                ApplyCompanionOnCurrentThread(std::move(job.slot));
            else
                ApplyPanelVisualsOnCurrentThread(std::move(job.slot));
        }
    }

    void JumpListPanelRegistry::ApplyCompanionOnCurrentThread(
        std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (slot->handle == 0 || !m_vts3 || !m_diagnostics)
        {
            slot->needsRetry = true;
            slot->appliedOk = false;
            return;
        }

        const Registry::Settings settings = [this]() noexcept
        {
            std::lock_guard lock(m_mx);
            return m_settings;
        }();

        if (!settings.paintPathXaml || settings.xamlEffectType == 0
            || settings.xamlEffectType == 5)
        {
            if (slot->chromePatched)
                (void)RestoreChrome(*slot);
            slot->ResetChromeCapture();
            slot->needsRetry = false;
            slot->appliedOk = true;
            return;
        }

        try
        {
            CaptureChrome(*slot);
            const unsigned bgIndex = slot->originals[0].index;
            if (bgIndex == UINT_MAX)
            {
                slot->needsRetry = true;
                slot->appliedOk = false;
                return;
            }

            winrt::Windows::UI::Color clear{};
            clear.A = 0;
            wux::Media::SolidColorBrush solid;
            solid.Color(clear);
            ::IInspectable* brushInsp = DetachInspectable(
                solid.as<winrt::Windows::Foundation::IInspectable>());
            const bool bgOk = SetPropByIndex(slot->handle, bgIndex, brushInsp);
            if (brushInsp)
                brushInsp->Release();

            if (!bgOk)
            {
                slot->needsRetry = true;
                slot->appliedOk = false;
                return;
            }

            slot->chromePatched = true;
            slot->appliedOk = true;
            slot->needsRetry = false;
            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][JumpList] companion clear ok handle=%llu",
                static_cast<unsigned long long>(slot->handle));
        }
        catch (...)
        {
            slot->needsRetry = true;
            slot->appliedOk = false;
        }
    }

    void JumpListPanelRegistry::ApplyPanelVisualsOnCurrentThread(
        std::shared_ptr<PanelSlot> slot) noexcept
    {
        if (!slot)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (slot->handle == 0 || !m_vts3 || !m_diagnostics)
        {
            slot->needsRetry = true;
            slot->appliedOk = false;
            return;
        }

        const Registry::Settings settings = [this]() noexcept
        {
            std::lock_guard lock(m_mx);
            return m_settings;
        }();

        // DWM / Disable: только restore уже запатченного. Не CaptureChrome —
        // иначе «оригинал» станет текущим (патченным) и меню залипнет.
        if (!settings.paintPathXaml)
        {
            if (slot->chromePatched)
                (void)RestoreChrome(*slot);
            // Сброс capture: при возврате на XAML перечитаем дефолтные props.
            slot->ResetChromeCapture();
            slot->needsRetry = false;
            slot->appliedOk = true;
            return;
        }

        try
        {
            // Blur → Acrylic: без GetElementVisual (AV на SEH surfaces).
            DWORD effect = settings.xamlEffectType;
            if (effect == 0 || effect == 5)
            {
                if (slot->chromePatched)
                    (void)RestoreChrome(*slot);
                slot->ResetChromeCapture();
                slot->needsRetry = false;
                slot->appliedOk = true;
                return;
            }
            if (effect == 3)
                effect = 4;

            CaptureChrome(*slot);

            // element=nullptr: Solid/Acrylic не требуют compositor element.
            wux::UIElement nullElement{ nullptr };
            const auto brush = CreateBackdropBrush(nullElement, effect, settings.tintArgb);
            if (!brush)
            {
                slot->needsRetry = true;
                slot->appliedOk = false;
                return;
            }

            const unsigned bgIndex = slot->originals[0].index;
            if (bgIndex == UINT_MAX)
            {
                slot->needsRetry = true;
                slot->appliedOk = false;
                return;
            }

            ::IInspectable* brushInsp = DetachInspectable(
                brush.as<winrt::Windows::Foundation::IInspectable>());
            const bool bgOk = SetPropByIndex(slot->handle, bgIndex, brushInsp);
            if (brushInsp)
                brushInsp->Release();

            if (!bgOk)
            {
                slot->needsRetry = true;
                slot->appliedOk = false;
                return;
            }

            const bool wantCorners =
                settings.cornersPresent && settings.cornerType != 0;
            const double trayH = Shared::TrayXamlPaint::QueryTaskbarHeightDip();
            // Без ActualHeight (WinRT) — MaxRound по высоте трея.
            const double r = wantCorners
                ? Shared::TrayXamlPaint::XamlPopupCornerRadiusDip(
                    settings.cornerType, trayH, trayH)
                : 0.0;

            if (wantCorners && slot->originals[3].index != UINT_MAX)
            {
                const wux::CornerRadius cr{ r, r, r, r };
                ::IInspectable* crInsp = DetachInspectable(winrt::box_value(cr));
                (void)SetPropByIndex(slot->handle, slot->originals[3].index, crInsp);
                if (crInsp)
                    crInsp->Release();
            }
            else if (
                !wantCorners
                && slot->originals[3].captured
                && slot->originals[3].value
                && slot->originals[3].index != UINT_MAX)
            {
                // System (CornerType=0): вернуть captured CR — иначе остаётся R=0 от Square.
                (void)SetPropByIndex(
                    slot->handle, slot->originals[3].index, slot->originals[3].value.get());
            }

            if (WantTintBorder(settings)
                && slot->originals[1].index != UINT_MAX
                && slot->originals[2].index != UINT_MAX)
            {
                wux::Media::SolidColorBrush borderBrush;
                borderBrush.Color(ColorFromArgb(settings.borderArgb));
                ::IInspectable* bbInsp = DetachInspectable(
                    borderBrush.as<winrt::Windows::Foundation::IInspectable>());
                (void)SetPropByIndex(slot->handle, slot->originals[1].index, bbInsp);
                if (bbInsp)
                    bbInsp->Release();

                const wux::Thickness th{ 1.0, 1.0, 1.0, 1.0 };
                ::IInspectable* thInsp = DetachInspectable(winrt::box_value(th));
                (void)SetPropByIndex(slot->handle, slot->originals[2].index, thInsp);
                if (thInsp)
                    thInsp->Release();
            }
            else if ((wantCorners || settings.patchBorder || settings.noBorder)
                && slot->originals[1].index != UINT_MAX
                && slot->originals[2].index != UINT_MAX)
            {
                const wux::Thickness th{ 0.0, 0.0, 0.0, 0.0 };
                ::IInspectable* thInsp = DetachInspectable(winrt::box_value(th));
                (void)SetPropByIndex(slot->handle, slot->originals[2].index, thInsp);
                if (thInsp)
                    thInsp->Release();
            }

            slot->chromePatched = true;
            slot->appliedOk = true;
            slot->needsRetry = false;

            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][JumpList] VTS3 apply ok handle=%llu effect=%u r=%.1f",
                static_cast<unsigned long long>(slot->handle),
                effect,
                r);
        }
        catch (const winrt::hresult_error&)
        {
            slot->needsRetry = true;
            slot->appliedOk = false;
        }
        catch (...)
        {
            slot->needsRetry = true;
            slot->appliedOk = false;
        }
    }

    void JumpListPanelRegistry::RefreshUnresolvedCount() noexcept
    {
        unsigned still = 0;
        {
            std::lock_guard lock(m_mx);
            for (auto& [h, s] : m_panels)
            {
                (void)h;
                if (s && (s->needsRetry || !s->appliedOk))
                    ++still;
            }
        }
        m_unresolved.store(still, std::memory_order_release);
    }

    void JumpListPanelRegistry::RegisterPanel(
        InstanceHandle handle,
        PanelKind kind) noexcept
    {
        if (handle == 0)
            return;
        if (m_state->shutdown.load(std::memory_order_acquire))
            return;
        if (!m_vts3 || !m_diagnostics)
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
                slot->handle = handle;
                slot->kind = kind;
                slot->uiTid = GetCurrentThreadId();
                if (slot->firstSeenTick == 0)
                    slot->firstSeenTick = GetTickCount64();
            }

            // VTC Add уже на UI-потоке Jump List.
            if (kind == PanelKind::Companion)
                ApplyCompanionOnCurrentThread(std::move(slot));
            else
                ApplyPanelVisualsOnCurrentThread(std::move(slot));
            RefreshUnresolvedCount();
        }
        catch (...)
        {
        }
    }

    void JumpListPanelRegistry::RetryUnresolvedPanels() noexcept
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
        {
            if (s->appliedOk && !s->needsRetry)
                continue;
            ApplyPanelVisuals(s);
        }
        RefreshUnresolvedCount();
    }

    void JumpListPanelRegistry::Unregister(InstanceHandle handle) noexcept
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
        if (!slot)
            return;
        try
        {
            // Remove обычно на UI-потоке Jump List — inline restore.
            if (slot->chromePatched)
                (void)RestoreChrome(*slot);
            slot->ClearWinrtRefs();
        }
        catch (...)
        {
        }
        RefreshUnresolvedCount();
    }

    void JumpListPanelRegistry::RestoreAll() noexcept
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

        unsigned restoredTotal = 0;
        for (auto& slot : snapshot)
        {
            try
            {
                if (slot->chromePatched)
                    RestoreSlotOnUiThread(slot);
                slot->ClearWinrtRefs();
                ++restoredTotal;
            }
            catch (...)
            {
            }
        }

        DebugLog(
            DBG_IMPORTANT,
            L"[UICompat][JumpList] RestoreAll VTS3 total=%u processed=%u",
            static_cast<unsigned>(snapshot.size()),
            restoredTotal);
    }

    void JumpListPanelRegistry::ReapplyPatchedBrushes() noexcept
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
                    // DWM→XAML: принудительно перепатчить даже «appliedOk» с DWM.
                    s->needsRetry = true;
                    s->appliedOk = false;
                    snapshot.push_back(s);
                }
            }
        }
        for (auto& s : snapshot)
            ApplyPanelVisuals(s);
        RefreshUnresolvedCount();
    }

    void JumpListPanelRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
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
            if (slot->chromePatched)
                (void)RestoreChrome(*slot);
            if (m_vts3 && slot->handle != 0)
            {
                for (auto& original : slot->originals)
                {
                    if (original.captured && original.index != UINT_MAX)
                        (void)m_vts3->ClearProperty(slot->handle, original.index);
                }
            }
            slot->ResetChromeCapture();
            slot->needsRetry = true;
            slot->appliedOk = false;
            ApplyPanelVisuals(slot);
        }
        RefreshUnresolvedCount();
        DebugLog(DBG_IMPORTANT, L"[UICompat][JumpList] RecaptureAfterThemeChange");
    }

    void JumpListPanelRegistry::RefreshBrushesAfterThemeChange(
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
                if (s)
                    s->clearBackgroundOnRestore = true;
            }
        }
        ReapplyPatchedBrushes();
        DebugLog(DBG_IMPORTANT, L"[UICompat][JumpList] RefreshBrushesAfterThemeChange");
    }

    void JumpListPanelRegistry::UpdateSettings(Registry::Settings settings) noexcept
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
