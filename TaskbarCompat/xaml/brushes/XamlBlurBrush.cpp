#include "TaskbarPch.h"
#include "xaml/brushes/XamlBlurBrush.hpp"
#include "xaml/effects/CompositeEffect.hpp"
#include "xaml/effects/FloodEffect.hpp"
#include "xaml/effects/GaussianBlurEffect.hpp"

#include <DebugLog.h>

namespace TaskbarCompat
{
    namespace wuc = winrt::Windows::UI::Composition;
    namespace wfn = winrt::Windows::Foundation::Numerics;

    XamlBlurBrush::XamlBlurBrush(
        wuc::Compositor compositor,
        float blurAmount,
        wfn::float4 tint) noexcept
        : m_compositor(std::move(compositor))
        , m_blurAmount(blurAmount)
        , m_tint(tint)
    {
    }

    void XamlBlurBrush::OnConnected()
    {
        try
        {
            if (CompositionBrush())
                return;

            if (!m_compositor)
            {
                DebugLog(DBG_WARNING, L"[TaskbarCompat][XamlBlurBrush] OnConnected: compositor null");
                return;
            }

            auto backdropBrush = m_compositor.CreateBackdropBrush();

            auto blurEffect = winrt::make_self<Effects::GaussianBlurEffect>();
            blurEffect->Source = wuc::CompositionEffectSourceParameter(L"backdrop");
            blurEffect->BlurAmount = m_blurAmount;

            auto floodEffect = winrt::make_self<Effects::FloodEffect>();
            floodEffect->Color = m_tint;

            auto compositeEffect = winrt::make_self<Effects::CompositeEffect>();
            compositeEffect->Sources.push_back(*blurEffect);
            compositeEffect->Sources.push_back(*floodEffect);
            compositeEffect->Mode = D2D1_COMPOSITE_MODE_SOURCE_OVER;

            auto factory = m_compositor.CreateEffectFactory(*compositeEffect);
            auto blurBrush = factory.CreateBrush();
            blurBrush.SetSourceParameter(L"backdrop", backdropBrush);

            CompositionBrush(blurBrush);
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][XamlBlurBrush] OnConnected failed hr=0x%08X",
                static_cast<unsigned>(winrt::to_hresult()));
        }
    }

    void XamlBlurBrush::OnDisconnected()
    {
        try
        {
            if (const auto brush = CompositionBrush())
            {
                brush.Close();
                CompositionBrush(nullptr);
            }
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][XamlBlurBrush] OnDisconnected failed hr=0x%08X",
                static_cast<unsigned>(winrt::to_hresult()));
            try
            {
                CompositionBrush(nullptr);
            }
            catch (...)
            {
            }
        }
    }
}
