#include "UiPch.h"
#include "xaml/XamlImageBrushRetry.hpp"

#include <DebugLog.h>

#include <algorithm>
#include <atomic>
#include <list>
#include <mutex>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

namespace UICompat
{
    namespace
    {
        namespace wux = winrt::Windows::UI::Xaml;

        struct ImageBrushWatchInfo
        {
            winrt::weak_ref<wux::Media::ImageBrush> brush{};
            winrt::hstring imageSource{};
            wux::Media::ImageBrush::ImageFailed_revoker imageFailedRevoker{};
            wux::Media::ImageBrush::ImageOpened_revoker imageOpenedRevoker{};
            // true только после ImageFailed — иначе retry ImageSource(nullptr)
            // сносит уже загруженную обложку альбома (MediaTransport Root/Thumbnail).
            bool needsRetry{ false };
        };

        struct ImageBrushesForThread
        {
            std::list<ImageBrushWatchInfo> watched{};
            winrt::Windows::UI::Core::CoreDispatcher dispatcher{ nullptr };
        };

        thread_local ImageBrushesForThread g_imageBrushesForThread{};

        std::mutex g_dispatcherRegistryMutex{};
        std::vector<winrt::weak_ref<winrt::Windows::UI::Core::CoreDispatcher>>
            g_dispatcherRegistry{};
        winrt::event_token g_networkStatusChangedToken{};
        std::atomic<bool> g_networkHandlerRegistered{ false };
        std::atomic<bool> g_shutdown{ false };

        void RemoveWatchedBrush(wux::Media::ImageBrush const& brush) noexcept
        {
            auto& list = g_imageBrushesForThread.watched;
            list.remove_if(
                [&brush](const ImageBrushWatchInfo& info) noexcept
                {
                    try
                    {
                        if (auto existing = info.brush.get())
                            return existing == brush;
                    }
                    catch (...)
                    {
                    }
                    return false;
                });
        }

        void MarkBrushNeedsRetry(wux::Media::ImageBrush const& brush) noexcept
        {
            for (auto& info : g_imageBrushesForThread.watched)
            {
                try
                {
                    if (auto existing = info.brush.get(); existing && existing == brush)
                    {
                        info.needsRetry = true;
                        return;
                    }
                }
                catch (...)
                {
                }
            }
        }

        void RetryFailedImageLoadsOnCurrentThread() noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire))
                return;

            auto& watched = g_imageBrushesForThread.watched;
            unsigned retryCount = 0;
            for (auto& info : watched)
            {
                if (!info.needsRetry)
                    continue;
                ++retryCount;
            }

            DebugLog(
                DBG_IMPORTANT,
                L"[UICompat][ImageBrush] retry failed loads count=%u watched=%zu",
                retryCount,
                watched.size());

            for (auto& info : watched)
            {
                if (!info.needsRetry)
                    continue;
                try
                {
                    auto brush = info.brush.get();
                    if (!brush)
                        continue;
                    brush.ImageSource(nullptr);
                    wux::Media::Imaging::BitmapImage bitmapImage;
                    bitmapImage.UriSource(winrt::Windows::Foundation::Uri(info.imageSource));
                    brush.ImageSource(bitmapImage);
                }
                catch (const winrt::hresult_error& ex)
                {
                    DebugLog(
                        DBG_WARNING,
                        L"[UICompat][ImageBrush] retry hr=0x%08X",
                        static_cast<unsigned>(ex.code()));
                }
                catch (...)
                {
                }
            }

            watched.remove_if(
                [](const ImageBrushWatchInfo& info) noexcept
                {
                    try
                    {
                        return !info.brush.get();
                    }
                    catch (...)
                    {
                        return true;
                    }
                });
        }

        void OnNetworkStatusChanged(winrt::Windows::Foundation::IInspectable const&) noexcept
        {
            if (g_shutdown.load(std::memory_order_acquire))
                return;

            std::vector<winrt::Windows::UI::Core::CoreDispatcher> dispatchers;
            {
                std::lock_guard lock(g_dispatcherRegistryMutex);
                for (auto& weakDispatcher : g_dispatcherRegistry)
                {
                    try
                    {
                        if (auto dispatcher = weakDispatcher.get())
                            dispatchers.push_back(dispatcher);
                    }
                    catch (...)
                    {
                    }
                }
                g_dispatcherRegistry.erase(
                    std::remove_if(
                        g_dispatcherRegistry.begin(),
                        g_dispatcherRegistry.end(),
                        [](const auto& weakDispatcher) noexcept
                        {
                            try
                            {
                                return !weakDispatcher.get();
                            }
                            catch (...)
                            {
                                return true;
                            }
                        }),
                    g_dispatcherRegistry.end());
            }

            for (auto& dispatcher : dispatchers)
            {
                try
                {
                    (void)dispatcher.RunAsync(
                        winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                        winrt::Windows::UI::Core::DispatchedHandler(
                            []() { RetryFailedImageLoadsOnCurrentThread(); }));
                }
                catch (...)
                {
                }
            }
        }

        void EnsureNetworkHandlerRegistered() noexcept
        {
            bool expected = false;
            if (!g_networkHandlerRegistered.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel))
                return;
            try
            {
                g_networkStatusChangedToken =
                    winrt::Windows::Networking::Connectivity::NetworkInformation::
                        NetworkStatusChanged(OnNetworkStatusChanged);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[UICompat][ImageBrush] NetworkStatusChanged registered");
            }
            catch (const winrt::hresult_error& ex)
            {
                g_networkHandlerRegistered.store(false, std::memory_order_release);
                DebugLog(
                    DBG_WARNING,
                    L"[UICompat][ImageBrush] NetworkStatusChanged hr=0x%08X",
                    static_cast<unsigned>(ex.code()));
            }
            catch (...)
            {
                g_networkHandlerRegistered.store(false, std::memory_order_release);
            }
        }

        void SetupImageBrushTracking(
            wux::Media::ImageBrush const& brush,
            winrt::hstring const& imageSourceUrl) noexcept
        {
            if (!brush || imageSourceUrl.empty() || g_shutdown.load(std::memory_order_acquire))
                return;

            RemoveWatchedBrush(brush);

            ImageBrushWatchInfo info{};
            info.brush = winrt::make_weak(brush);
            info.imageSource = imageSourceUrl;
            info.needsRetry = false;

            try
            {
                info.imageFailedRevoker = brush.ImageFailed(
                    winrt::auto_revoke,
                    [imageSourceUrl](
                        winrt::Windows::Foundation::IInspectable const& sender,
                        wux::ExceptionRoutedEventArgs const& e)
                    {
                        DebugLog(
                            DBG_WARNING,
                            L"[UICompat][ImageBrush] ImageFailed uri=%s err=%s",
                            imageSourceUrl.c_str(),
                            e.ErrorMessage().c_str());
                        try
                        {
                            if (const auto b = sender.try_as<wux::Media::ImageBrush>())
                                MarkBrushNeedsRetry(b);
                        }
                        catch (...)
                        {
                        }
                    });

                info.imageOpenedRevoker = brush.ImageOpened(
                    winrt::auto_revoke,
                    [brushWeak = winrt::make_weak(brush)](
                        winrt::Windows::Foundation::IInspectable const&,
                        wux::RoutedEventArgs const&)
                    {
                        try
                        {
                            if (auto b = brushWeak.get())
                                RemoveWatchedBrush(b);
                        }
                        catch (...)
                        {
                        }
                    });
            }
            catch (...)
            {
                return;
            }

            // В watched для жизни revoker'ов. В retry — только needsRetry после ImageFailed.
            g_imageBrushesForThread.watched.push_back(std::move(info));

            if (!g_imageBrushesForThread.dispatcher)
            {
                try
                {
                    if (const auto window = wux::Window::Current())
                    {
                        g_imageBrushesForThread.dispatcher = window.Dispatcher();
                        if (g_imageBrushesForThread.dispatcher)
                        {
                            std::lock_guard lock(g_dispatcherRegistryMutex);
                            g_dispatcherRegistry.push_back(
                                winrt::make_weak(g_imageBrushesForThread.dispatcher));
                        }
                    }
                }
                catch (...)
                {
                }
            }

            EnsureNetworkHandlerRegistered();
        }

        void MaybeTrackBrush(wux::Media::Brush const& brush) noexcept
        {
            try
            {
                const auto imageBrush = brush.try_as<wux::Media::ImageBrush>();
                if (!imageBrush)
                    return;
                const auto imageSource = imageBrush.ImageSource();
                if (!imageSource)
                    return;
                const auto bitmapImage = imageSource.try_as<wux::Media::Imaging::BitmapImage>();
                if (!bitmapImage)
                    return;
                const auto uriSource = bitmapImage.UriSource();
                if (!uriSource)
                    return;
                const winrt::hstring uriString = uriSource.ToString();
                if (uriString.size() < 7)
                    return;
                const wchar_t* s = uriString.c_str();
                const bool http = (_wcsnicmp(s, L"http://", 7) == 0)
                    || (_wcsnicmp(s, L"https://", 8) == 0);
                if (!http)
                    return;
                SetupImageBrushTracking(imageBrush, uriString);
            }
            catch (...)
            {
            }
        }

        void WalkTrackImageBrushes(wux::DependencyObject const& node, int depth) noexcept
        {
            if (!node || depth > 24)
                return;
            try
            {
                if (const auto fe = node.try_as<wux::FrameworkElement>())
                {
                    if (const auto panel = fe.try_as<wux::Controls::Panel>())
                        MaybeTrackBrush(panel.Background());
                    else if (const auto border = fe.try_as<wux::Controls::Border>())
                        MaybeTrackBrush(border.Background());
                    else if (const auto image = fe.try_as<wux::Controls::Image>())
                    {
                        // ThumbnailImage иногда Image, не Grid+ImageBrush — Source не трогаем.
                        (void)image;
                    }
                }

                const int n = wux::Media::VisualTreeHelper::GetChildrenCount(node);
                for (int i = 0; i < n; ++i)
                {
                    const auto child = wux::Media::VisualTreeHelper::GetChild(node, i);
                    WalkTrackImageBrushes(child, depth + 1);
                }
            }
            catch (...)
            {
            }
        }
    }

    void TrackMediaSubtreeImageBrushes(wux::Controls::Grid const& root) noexcept
    {
        if (!root || g_shutdown.load(std::memory_order_acquire))
            return;
        WalkTrackImageBrushes(root, 0);
    }

    void ShutdownImageBrushRetry() noexcept
    {
        g_shutdown.store(true, std::memory_order_release);
        try
        {
            if (g_networkHandlerRegistered.load(std::memory_order_acquire)
                && g_networkStatusChangedToken)
            {
                winrt::Windows::Networking::Connectivity::NetworkInformation::
                    NetworkStatusChanged(g_networkStatusChangedToken);
                g_networkStatusChangedToken = {};
            }
        }
        catch (...)
        {
        }
        {
            std::lock_guard lock(g_dispatcherRegistryMutex);
            g_dispatcherRegistry.clear();
        }
        g_imageBrushesForThread.watched.clear();
        g_imageBrushesForThread.dispatcher = nullptr;
    }
}
