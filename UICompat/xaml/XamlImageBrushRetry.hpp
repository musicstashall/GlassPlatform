#pragma once

#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>

namespace UICompat
{
    // Windhawk-style: трекинг ImageBrush с http(s) ImageSource + ретрай по NetworkStatusChanged.
    // Обложка media-карточки (Grid#ThumbnailImage и др.) не должна пропадать после патча.
    void TrackMediaSubtreeImageBrushes(
        winrt::Windows::UI::Xaml::Controls::Grid const& root) noexcept;

    void ShutdownImageBrushRetry() noexcept;
}
