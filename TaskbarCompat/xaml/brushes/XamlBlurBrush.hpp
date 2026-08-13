#pragma once

// Порт XamlBlurBrush из TranslucentTB ExplorerTAP — XAML Composition brush (backdrop blur + tint).

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace TaskbarCompat
{
    class XamlBlurBrush : public winrt::Windows::UI::Xaml::Media::XamlCompositionBrushBaseT<XamlBlurBrush>
    {
    public:
        XamlBlurBrush(
            winrt::Windows::UI::Composition::Compositor compositor,
            float blurAmount,
            winrt::Windows::Foundation::Numerics::float4 tint) noexcept;

        void OnConnected();
        void OnDisconnected();

    private:
        winrt::Windows::UI::Composition::Compositor m_compositor{ nullptr };
        float m_blurAmount{ 3.0f };
        winrt::Windows::Foundation::Numerics::float4 m_tint{};
    };
}
