#pragma once

#include <windows.h>

#include <windows.ui.xaml.media.h>
#include <xamlOM.h>

namespace XamlHostBarPatcher::Detail
{
    [[nodiscard]] HRESULT CreateSolidColorBrushArgb(
        BYTE a,
        BYTE r,
        BYTE g,
        BYTE b,
        ABI::Windows::UI::Xaml::Media::ISolidColorBrush** out) noexcept;

    [[nodiscard]] HRESULT TrySetBackgroundViaComInterface(
        IInspectable* insp,
        BYTE a,
        BYTE r,
        BYTE g,
        BYTE b) noexcept;

    [[nodiscard]] HRESULT CreateAcrylicBrushArgb(
        BYTE a,
        BYTE r,
        BYTE g,
        BYTE b,
        float tintOpacity,
        ABI::Windows::UI::Xaml::Media::IAcrylicBrush** out) noexcept;

    [[nodiscard]] HRESULT TrySetBackgroundViaComInterfaceAcrylic(
        IInspectable* insp,
        BYTE a,
        BYTE r,
        BYTE g,
        BYTE b,
        float tintOpacity) noexcept;

    void ReleaseDiagnosticPropertyChain(
        PropertyChainSource* sources,
        unsigned sourceCount,
        PropertyChainValue* values,
        unsigned propCount) noexcept;
}
