#pragma once

// Порт GaussianBlurEffect из TranslucentTB ExplorerTAP — D2D Gaussian Blur для Composition EffectFactory.

#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <string_view>

namespace TaskbarCompat::Effects
{
    namespace wge = winrt::Windows::Graphics::Effects;
    namespace awge = ABI::Windows::Graphics::Effects;

    struct GaussianBlurEffect : winrt::implements<
        GaussianBlurEffect,
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

        wge::IGraphicsEffectSource Source{};

        float BlurAmount = 3.0f;
        D2D1_GAUSSIANBLUR_OPTIMIZATION Optimization = D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED;
        D2D1_BORDER_MODE BorderMode = D2D1_BORDER_MODE_SOFT;

    private:
        winrt::hstring m_name{ L"GaussianBlurEffect" };
    };
}
