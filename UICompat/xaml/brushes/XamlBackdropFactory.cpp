#include "UiPch.h"
#include "xaml/brushes/XamlBackdropFactory.hpp"
#include "xaml/brushes/XamlBlurBrush.hpp"

#include <DebugLog.h>
#include <Helpers/ThemeHelper.h>

#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.Foundation.Numerics.h>

namespace UICompat
{
    namespace wux = winrt::Windows::UI::Xaml;
    namespace wuxm = winrt::Windows::UI::Xaml::Media;
    namespace wuxh = winrt::Windows::UI::Xaml::Hosting;
    namespace wfn = winrt::Windows::Foundation::Numerics;

    namespace
    {
        // ARGB DWORD → Windows.UI.Color (A,R,G,B).
        [[nodiscard]] winrt::Windows::UI::Color ArgbToColor(DWORD argb) noexcept
        {
            return winrt::Windows::UI::Color{
                static_cast<uint8_t>((argb >> 24) & 0xFFu),
                static_cast<uint8_t>((argb >> 16) & 0xFFu),
                static_cast<uint8_t>((argb >> 8) & 0xFFu),
                static_cast<uint8_t>(argb & 0xFFu),
            };
        }

        [[nodiscard]] wfn::float4 ColorToFloat4(winrt::Windows::UI::Color const& c) noexcept
        {
            return wfn::float4{
                c.R / 255.0f,
                c.G / 255.0f,
                c.B / 255.0f,
                c.A / 255.0f,
            };
        }

        // TTB: BlurRadius/3, default 9 → 3.0f.
        constexpr float kDefaultBlurAmount = 3.0f;

        [[nodiscard]] wuxm::Brush MakeSolid(winrt::Windows::UI::Color color) noexcept
        {
            wuxm::SolidColorBrush brush;
            brush.Color(color);
            return brush;
        }

        [[nodiscard]] wuxm::Brush MakeAcrylicBackdrop(winrt::Windows::UI::Color tint) noexcept
        {
            wuxm::AcrylicBrush acrylic;
            acrylic.BackgroundSource(wuxm::AcrylicBackgroundSource::Backdrop);
            acrylic.TintColor(tint);
            return acrylic;
        }
    }

    winrt::Windows::UI::Xaml::Media::Brush CreateBackdropBrush(
        winrt::Windows::UI::Xaml::UIElement const& elementForCompositor,
        DWORD effectType,
        DWORD tintArgb) noexcept
    {
        try
        {

            // 0 Disable / 5 System — кисть не подменяем (оригинал острова).
            if (effectType == 0 || effectType == 5)
                return nullptr;

            const auto tint = ArgbToColor(tintArgb);

            switch (effectType)
            {
            case 1: // Transparent
            {
                auto c = tint;
                c.A = 0;
                return MakeSolid(c);
            }

            case 2: // Solid
                return MakeSolid(tint);

            case 3: // Blur (XamlBlurBrush)
            {
                if (!elementForCompositor)
                {
                    DebugLog(DBG_WARNING, L"[UICompat][XamlBackdrop] Blur: element null");
                    return nullptr;
                }

                // Flood SOURCE_OVER: A≈1 полностью закрывает backdrop blur.
                auto blurTint = tint;
                if (blurTint.A > 0xE4)
                    blurTint.A = 0xE4;

                auto compositor =
                    wuxh::ElementCompositionPreview::GetElementVisual(elementForCompositor).Compositor();
                return winrt::make<XamlBlurBrush>(
                    std::move(compositor),
                    kDefaultBlurAmount,
                    ColorToFloat4(blurTint));
            }

            case 4: // Acrylic
                return MakeAcrylicBackdrop(tint);

            default:
                DebugLog(
                    DBG_WARNING,
                    L"[UICompat][XamlBackdrop] unknown effectType=%u",
                    static_cast<unsigned>(effectType));
                return nullptr;
            }
        }
        catch (...)
        {
            DebugLog(
                DBG_WARNING,
                L"[UICompat][XamlBackdrop] CreateBackdropBrush failed effectType=%u hr=0x%08X",
                static_cast<unsigned>(effectType),
                static_cast<unsigned>(winrt::to_hresult()));
            return nullptr;
        }
    }

    winrt::Windows::UI::Xaml::Media::Brush CreateNeutralAcrylicBackdropBrush() noexcept
    {
        try
        {
            // Лог theme→stop: белый Tint + Luminosity=0.85 → «кисть старой» светлой темы.
            // Tint по текущей Apps-теме (TrayXaml dark), без Glass registry tint.
            const bool dark = Shared::ThemeHelper::IsTrayXamlDarkMode();
            wuxm::AcrylicBrush acrylic;
            acrylic.BackgroundSource(wuxm::AcrylicBackgroundSource::Backdrop);
            acrylic.TintColor(
                dark ? winrt::Windows::UI::Color{ 255, 32, 32, 32 }
                     : winrt::Windows::UI::Color{ 255, 252, 252, 252 });
            acrylic.TintOpacity(dark ? 0.7 : 0.6);
            return acrylic;
        }
        catch (...)
        {
            return nullptr;
        }
    }
}
