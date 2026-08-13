#pragma once

// Порт CompositeEffect из TranslucentTB ExplorerTAP — композиция источников (blur + flood).

#include <d2d1_1.h>
#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <string_view>
#include <vector>

namespace TaskbarCompat::Effects
{
    namespace wge = winrt::Windows::Graphics::Effects;
    namespace awge = ABI::Windows::Graphics::Effects;

    struct CompositeEffect : winrt::implements<
        CompositeEffect,
        wge::IGraphicsEffect,
        wge::IGraphicsEffectSource,
        awge::IGraphicsEffectD2D1Interop>
    {
        // IGraphicsEffectD2D1Interop
        HRESULT STDMETHODCALLTYPE GetEffectId(GUID* id) noexcept override;
        HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(
            LPCWSTR name,
            UINT* index,
            awge::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override;
        HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override;
        HRESULT STDMETHODCALLTYPE GetProperty(
            UINT index,
            ABI::Windows::Foundation::IPropertyValue** value) noexcept override;
        HRESULT STDMETHODCALLTYPE GetSource(
            UINT index,
            awge::IGraphicsEffectSource** source) noexcept override;
        HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override;

        // IGraphicsEffect
        winrt::hstring Name();
        void Name(winrt::hstring name);

        std::vector<wge::IGraphicsEffectSource> Sources;
        D2D1_COMPOSITE_MODE Mode = D2D1_COMPOSITE_MODE_SOURCE_OVER;

    private:
        winrt::hstring m_name{ L"CompositeEffect" };
    };
}
