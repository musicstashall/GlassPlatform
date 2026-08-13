#pragma once

// Порт FloodEffect из TranslucentTB ExplorerTAP — сплошная заливка (tint) для Composition EffectFactory.

#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <string_view>

namespace TaskbarCompat::Effects
{
    namespace wge = winrt::Windows::Graphics::Effects;
    namespace wfn = winrt::Windows::Foundation::Numerics;
    namespace awge = ABI::Windows::Graphics::Effects;

    struct FloodEffect : winrt::implements<
        FloodEffect,
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

        // RGBA в диапазоне 0..1 (как у Composition / D2D Flood).
        wfn::float4 Color{ 0.0f, 0.0f, 0.0f, 1.0f };

    private:
        winrt::hstring m_name{ L"FloodEffect" };
    };
}
