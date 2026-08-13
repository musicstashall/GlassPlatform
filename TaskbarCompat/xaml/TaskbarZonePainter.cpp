#include "TaskbarPch.h"
#include "xaml/TaskbarZonePainter.hpp"

#include <DebugLog.h>
#include <TrayXamlPaint.h>

#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace TaskbarCompat::Zones
{
    namespace wux = winrt::Windows::UI::Xaml;
    namespace wuxc = winrt::Windows::UI::Xaml::Controls;
    namespace wuxh = winrt::Windows::UI::Xaml::Hosting;
    namespace wuxm = winrt::Windows::UI::Xaml::Media;
    namespace wuxa = winrt::Windows::UI::Xaml::Automation;
    namespace wuxa_anim = winrt::Windows::UI::Xaml::Media::Animation;
    namespace wuc = winrt::Windows::UI::Composition;
    namespace wf = winrt::Windows::Foundation;
    namespace wfn = winrt::Windows::Foundation::Numerics;

    namespace
    {
        constexpr double kZoneGapDip = 8.0;
        constexpr double kZonePadY = 2.0;
        constexpr float kZonePadX = 6.0f;
        // MaxRound: чуть больше боковой «подушки», иначе R=h/2 подрезает иконки у краёв сегмента.
        constexpr float kZonePadXMaxRound = 12.0f;
        constexpr float kPanelEdgeEps = 2.0f;
        // Иконка TaskListButton ~44–72dip; шире — хост/призрак, раздувает center влево (лог: cX→left+gap).
        constexpr float kMaxTaskButtonWidthDip = 120.0f;

        [[nodiscard]] DWORD ReadExplorerAdvancedDword(const wchar_t* name, DWORD defaultValue) noexcept
        {
            DWORD v = defaultValue;
            DWORD cb = sizeof(v);
            const LSTATUS ls = RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                name,
                RRF_RT_REG_DWORD,
                nullptr,
                &v,
                &cb);
            return ls == ERROR_SUCCESS ? v : defaultValue;
        }

        [[nodiscard]] wux::FrameworkElement FindParentNamed(
            wux::FrameworkElement element,
            std::wstring_view name) noexcept
        {
            try
            {
                while (element)
                {
                    if (element.Name() == name)
                        return element;
                    element = wuxm::VisualTreeHelper::GetParent(element)
                        .try_as<wux::FrameworkElement>();
                }
            }
            catch (...)
            {
            }
            return nullptr;
        }

        [[nodiscard]] std::wstring TypeLeaf(wux::FrameworkElement const& fe) noexcept
        {
            try
            {
                const auto name = winrt::get_class_name(fe);
                const std::wstring s{ name };
                const auto pos = s.find_last_of(L'.');
                return pos == std::wstring::npos ? s : s.substr(pos + 1);
            }
            catch (...)
            {
                return {};
            }
        }

        void VisitDescendants(
            wux::DependencyObject const& root,
            const std::function<bool(wux::FrameworkElement const&)>& visitor) noexcept
        {
            try
            {
                std::deque<wux::DependencyObject> q;
                q.push_back(root);
                while (!q.empty())
                {
                    const auto cur = q.front();
                    q.pop_front();
                    if (auto fe = cur.try_as<wux::FrameworkElement>())
                    {
                        if (!visitor(fe))
                            return;
                    }
                    const int count = wuxm::VisualTreeHelper::GetChildrenCount(cur);
                    for (int i = 0; i < count; ++i)
                        q.push_back(wuxm::VisualTreeHelper::GetChild(cur, i));
                }
            }
            catch (...)
            {
            }
        }

        [[nodiscard]] bool IsDescendantOf(
            wux::FrameworkElement fe,
            wux::FrameworkElement const& ancestor) noexcept
        {
            if (!fe || !ancestor)
                return false;
            try
            {
                while (fe)
                {
                    if (fe == ancestor)
                        return true;
                    fe = wuxm::VisualTreeHelper::GetParent(fe).try_as<wux::FrameworkElement>();
                }
            }
            catch (...)
            {
            }
            return false;
        }

        [[nodiscard]] wf::Rect BoundsInRoot(
            wux::FrameworkElement const& fe,
            wux::FrameworkElement const& root) noexcept
        {
            try
            {
                if (!fe || !root || fe.ActualWidth() < 1.0 || fe.ActualHeight() < 1.0)
                    return {};
                const auto t = fe.TransformToVisual(root);
                return t.TransformBounds(wf::Rect{
                    0.0f,
                    0.0f,
                    static_cast<float>(fe.ActualWidth()),
                    static_cast<float>(fe.ActualHeight()) });
            }
            catch (...)
            {
                return {};
            }
        }

        [[nodiscard]] bool RectValid(wf::Rect const& r) noexcept
        {
            return r.Width > 1.0f && r.Height > 1.0f;
        }

        // TransformToVisual иногда даёт «призраков» далеко слева (лог: ccX=-10106) —
        // union раздувает center, при hasL=0 клип к x=0 «съезжает» к левому краю.
        [[nodiscard]] bool RectOnPanel(
            wf::Rect const& r,
            float rootW,
            float rootH) noexcept
        {
            if (!RectValid(r) || rootW < 2.0f)
                return false;
            constexpr float kSlop = 8.0f;
            if (r.X < -kSlop || r.X > rootW + kSlop)
                return false;
            if (r.Y < -kSlop || (rootH > 1.0f && r.Y > rootH + kSlop))
                return false;
            if (r.X + r.Width < -kSlop)
                return false;
            // Хост на всю панель — не chrome-кнопка.
            if (r.Width > rootW * 0.92f)
                return false;
            return true;
        }

        void ClampZoneToPanel(wf::Rect& r, float panelH) noexcept
        {
            if (!RectValid(r) || panelH < 2.0f)
                return;
            if (r.Height > panelH)
            {
                r.Y = 0.0f;
                r.Height = panelH;
            }
            else
            {
                if (r.Y < 0.0f)
                    r.Y = 0.0f;
                if (r.Y + r.Height > panelH)
                    r.Y = panelH - r.Height;
            }
        }

        void UnionRect(wf::Rect& acc, wf::Rect const& add) noexcept
        {
            if (!RectValid(add))
                return;
            if (!RectValid(acc))
            {
                acc = add;
                return;
            }
            const float left = (std::min)(acc.X, add.X);
            const float top = (std::min)(acc.Y, add.Y);
            const float right = (std::max)(acc.X + acc.Width, add.X + add.Width);
            const float bottom = (std::max)(acc.Y + acc.Height, add.Y + add.Height);
            acc = wf::Rect{ left, top, right - left, bottom - top };
        }

        [[nodiscard]] wf::Rect InflateRect(wf::Rect r, float dx, float dy) noexcept
        {
            if (!RectValid(r))
                return r;
            r.X -= dx;
            r.Y -= dy;
            r.Width += dx * 2.0f;
            r.Height += dy * 2.0f;
            if (r.X < 0)
            {
                r.Width += r.X;
                r.X = 0;
            }
            if (r.Y < 0)
            {
                r.Height += r.Y;
                r.Y = 0;
            }
            return r;
        }

        // Боковые отступы; края панели (x≈0 / x+w≈rootW) не расширяем.
        // Соседи: не забирать межзонный gap (иначе центр «раздувается» к left/tray).
        void ExpandZoneHorizontal(
            wf::Rect& r,
            float rootW,
            float pad,
            wf::Rect const* leftNeighbor,
            wf::Rect const* rightNeighbor,
            float gapKeep) noexcept
        {
            if (!RectValid(r) || rootW < 2.0f || pad <= 0.0f)
                return;
            const bool nearLeft = r.X <= kPanelEdgeEps;
            const bool nearRight = (r.X + r.Width) >= (rootW - kPanelEdgeEps);
            float newX = r.X;
            float newRight = r.X + r.Width;
            if (!nearLeft)
            {
                newX = r.X - pad;
                if (leftNeighbor && RectValid(*leftNeighbor))
                    newX = (std::max)(newX, leftNeighbor->X + leftNeighbor->Width + gapKeep);
                if (newX < 0.0f)
                    newX = 0.0f;
            }
            if (!nearRight)
            {
                newRight = r.X + r.Width + pad;
                if (rightNeighbor && RectValid(*rightNeighbor))
                    newRight = (std::min)(newRight, rightNeighbor->X - gapKeep);
                if (newRight > rootW)
                    newRight = rootW;
            }
            if (newRight <= newX + 1.0f)
                return;
            r.X = newX;
            r.Width = newRight - newX;
        }

        // Левый/правый сегмент не должны залезать в gap центра (симметрия ClipCenter).
        void ClipSideAgainstCenter(
            wf::Rect& side,
            wf::Rect const& center,
            bool sideIsLeft,
            float gap) noexcept
        {
            if (!RectValid(side) || !RectValid(center))
                return;
            if (sideIsLeft)
            {
                const float maxRight = center.X - gap;
                if (side.X + side.Width > maxRight)
                    side.Width = (std::max)(1.0f, maxRight - side.X);
            }
            else
            {
                const float minLeft = center.X + center.Width + gap;
                if (side.X < minLeft)
                {
                    const float right = side.X + side.Width;
                    side.X = minLeft;
                    side.Width = (std::max)(1.0f, right - minLeft);
                }
            }
        }

        [[nodiscard]] winrt::Windows::UI::Color ColorFromArgb(DWORD argb) noexcept
        {
            return winrt::Windows::UI::Color{
                static_cast<uint8_t>((argb >> 24) & 0xFFu),
                static_cast<uint8_t>((argb >> 16) & 0xFFu),
                static_cast<uint8_t>((argb >> 8) & 0xFFu),
                static_cast<uint8_t>(argb & 0xFFu),
            };
        }

        void CaptureAndClearRootGrid(FrameZoneState& state, wuxc::Grid const& grid) noexcept
        {
            try
            {
                if (!grid)
                    return;
                if (!state.rootChromeCaptured)
                {
                    state.rootGrid = grid;
                    state.originalRootBackground = grid.Background();
                    state.originalRootCornerRadius = grid.CornerRadius();
                    try
                    {
                        state.originalRootBorderBrush = grid.BorderBrush();
                        state.originalRootBorderThickness = grid.BorderThickness();
                    }
                    catch (...)
                    {
                        state.originalRootBorderBrush = nullptr;
                        state.originalRootBorderThickness = {};
                    }
                    state.rootChromeCaptured = true;
                }
                // Transparent (не nullptr): иначе пустые края сегментов не hit-test → нет RMB-меню.
                wuxm::SolidColorBrush hitBrush;
                hitBrush.Color(winrt::Windows::UI::Colors::Transparent());
                grid.Background(hitBrush);
                grid.CornerRadius({});
                grid.BorderBrush(nullptr);
                grid.BorderThickness({});
                state.rootChromeCleared = true;
            }
            catch (...)
            {
            }
        }

        void RestoreRootGridChrome(FrameZoneState& state) noexcept
        {
            if (!state.rootGrid || !state.rootChromeCaptured || !state.rootChromeCleared)
            {
                state.rootChromeCleared = false;
                return;
            }
            try
            {
                state.rootGrid.Background(state.originalRootBackground);
                state.rootGrid.CornerRadius(state.originalRootCornerRadius);
                state.rootGrid.BorderBrush(state.originalRootBorderBrush);
                state.rootGrid.BorderThickness(state.originalRootBorderThickness);
            }
            catch (...)
            {
            }
            state.rootChromeCleared = false;
        }

        void StopFrameStoryboard(FrameZoneState& state) noexcept;
        void StopLeftAppearStoryboard(FrameZoneState& state) noexcept;

        // Токен для async-завершений; при нехватке памяти возвращает пустой weak_ptr,
        // и колбэк просто ничего не делает.
        [[nodiscard]] std::weak_ptr<FrameZoneState*> AcquireZoneLifetime(
            FrameZoneState& state) noexcept
        {
            if (!state.lifetime || *state.lifetime != &state)
            {
                try
                {
                    state.lifetime = std::make_shared<FrameZoneState*>(&state);
                }
                catch (...)
                {
                    state.lifetime.reset();
                }
            }
            return state.lifetime;
        }

        [[nodiscard]] FrameZoneState* LockZoneState(
            std::weak_ptr<FrameZoneState*> const& token) noexcept
        {
            const auto locked = token.lock();
            return locked ? *locked : nullptr;
        }

        void RemoveOverlay(FrameZoneState& state, ZoneOverlay& overlay, wuxc::Grid const& root) noexcept
        {
            if (&overlay == &state.left)
            {
                StopLeftAppearStoryboard(state);
                // Slide-up только если left реально был показан (не мигание при старте).
                if (overlay.active)
                    state.leftAllowAppearAnim = true;
            }
            if (state.frameAnimRunning || overlay.inFrameAnim)
                StopFrameStoryboard(state);
            if (!overlay.host && !overlay.element)
            {
                overlay = {};
                return;
            }
            try
            {
                if (overlay.inserted && root)
                {
                    uint32_t index = 0;
                    const auto node = overlay.host ? overlay.host.as<wux::UIElement>()
                                                  : overlay.element.as<wux::UIElement>();
                    if (node && root.Children().IndexOf(node, index))
                        root.Children().RemoveAt(index);
                }
            }
            catch (...)
            {
            }
            overlay = {};
        }

        constexpr float kAnimEps = 2.0f;
        // Плавный retarget center/tray (не hard-snap каждый LayoutUpdated — иначе ←→ рывки).
        constexpr int kAnimDurationMs = 120;
        // Left appear: Composition KeyFrame (XAML Storyboard 150ms → Completed dtMs=1000).
        constexpr int kLeftAppearMs = 80;

        void SyncVisualFromElement(ZoneOverlay& overlay) noexcept
        {
            try
            {
                if (overlay.host && overlay.transform)
                {
                    const double w = overlay.host.Width();
                    const double sx = overlay.transform.ScaleX();
                    const double visualW = (w > 1.0 && !std::isnan(w) ? w : 0.0)
                        * (std::isnan(sx) || sx <= 0.0 ? 1.0 : sx);
                    if (visualW > 1.0)
                        overlay.visual.Width = static_cast<float>(visualW);
                    overlay.visual.X = static_cast<float>(overlay.transform.TranslateX());
                    overlay.visual.Y = static_cast<float>(
                        overlay.host.Margin().Top + overlay.transform.TranslateY());
                    const double h = overlay.host.Height();
                    if (h > 1.0 && !std::isnan(h))
                        overlay.visual.Height = static_cast<float>(h);
                    overlay.hasVisual = true;
                }
            }
            catch (...)
            {
            }
        }

        [[nodiscard]] bool RectNear(wf::Rect const& a, wf::Rect const& b) noexcept
        {
            return std::fabs(a.X - b.X) < kAnimEps && std::fabs(a.Y - b.Y) < kAnimEps
                && std::fabs(a.Width - b.Width) < kAnimEps
                && std::fabs(a.Height - b.Height) < kAnimEps;
        }

        void EnsureOverlayTransform(ZoneOverlay& overlay) noexcept
        {
            if (!overlay.host)
                return;
            if (!overlay.transform)
            {
                overlay.transform = wuxm::CompositeTransform{};
                // Origin снизу-слева: slide-up TranslateY и ScaleX от левого края.
                overlay.host.RenderTransformOrigin(wf::Point{ 0.0f, 1.0f });
                overlay.host.RenderTransform(overlay.transform);
            }
        }

        void SnapOverlayGeometry(ZoneOverlay& overlay, wf::Rect const& bounds) noexcept
        {
            if (!overlay.host)
                return;
            EnsureOverlayTransform(overlay);
            overlay.host.Width(static_cast<double>(bounds.Width));
            overlay.host.Height(static_cast<double>(bounds.Height));
            overlay.host.Margin(wux::Thickness{ 0.0, static_cast<double>(bounds.Y), 0.0, 0.0 });
            overlay.transform.TranslateX(static_cast<double>(bounds.X));
            overlay.transform.TranslateY(0.0);
            overlay.transform.ScaleX(1.0);
            overlay.visual = bounds;
            overlay.target = bounds;
            overlay.hasVisual = true;
            overlay.hasPending = false;
            overlay.queuedForAnim = false;
            overlay.inFrameAnim = false;
        }

        void ResetLeftAppearVisual(ZoneOverlay& overlay) noexcept
        {
            if (!overlay.host)
                return;
            try
            {
                const auto visual = wuxh::ElementCompositionPreview::GetElementVisual(overlay.host);
                visual.StopAnimation(L"Translation");
                visual.Properties().InsertVector3(L"Translation", wfn::float3{ 0.0f, 0.0f, 0.0f });
            }
            catch (...)
            {
            }
            try
            {
                if (overlay.transform)
                    overlay.transform.TranslateY(0.0);
            }
            catch (...)
            {
            }
        }

        void StopLeftAppearStoryboard(FrameZoneState& state) noexcept
        {
            ++state.leftAppearGeneration;
            ResetLeftAppearVisual(state.left);
            state.leftAppearRunning = false;
        }

        // Появление left: Composition Vector3KeyFrame Translation.Y (H→0).
        // XAML Storyboard Duration=150ms давал Completed ровно dtMs=1000 (тема/timeline).
        void BeginLeftSlideUpAppear(FrameZoneState& state, ZoneOverlay& overlay, wf::Rect const& bounds) noexcept
        {
            if (!overlay.host)
                return;
            if (state.leftAppearRunning)
                return;

            StopLeftAppearStoryboard(state);
            SnapOverlayGeometry(overlay, bounds);
            try
            {
                if (overlay.transform)
                    overlay.transform.TranslateY(0.0);

                wuxh::ElementCompositionPreview::SetIsTranslationEnabled(overlay.host, true);
                const auto visual = wuxh::ElementCompositionPreview::GetElementVisual(overlay.host);
                const auto compositor = visual.Compositor();

                const float fromY = (std::max)(bounds.Height, 1.0f);
                visual.Properties().InsertVector3(
                    L"Translation", wfn::float3{ 0.0f, fromY, 0.0f });

                wuc::Vector3KeyFrameAnimation anim = compositor.CreateVector3KeyFrameAnimation();
                anim.InsertKeyFrame(0.0f, wfn::float3{ 0.0f, fromY, 0.0f });
                anim.InsertKeyFrame(
                    1.0f,
                    wfn::float3{ 0.0f, 0.0f, 0.0f },
                    compositor.CreateCubicBezierEasingFunction(
                        wfn::float2{ 0.0f, 0.0f },
                        wfn::float2{ 0.0f, 1.0f }));
                anim.Duration(std::chrono::duration_cast<wf::TimeSpan>(
                    std::chrono::milliseconds{ kLeftAppearMs }));
                anim.IterationBehavior(wuc::AnimationIterationBehavior::Count);
                anim.IterationCount(1);

                auto batch = compositor.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
                state.leftAppearRunning = true;
                state.leftAllowAppearAnim = false;
                const auto token = AcquireZoneLifetime(state);
                const std::uint32_t generation = state.leftAppearGeneration;
                visual.StartAnimation(L"Translation", anim);
                batch.End();
                batch.Completed(
                    [token, generation](
                        wf::IInspectable const&,
                        wuc::CompositionBatchCompletedEventArgs const&) {
                        FrameZoneState* const live = LockZoneState(token);
                        if (!live || live->leftAppearGeneration != generation)
                            return;
                        try
                        {
                            live->leftAppearRunning = false;
                            ResetLeftAppearVisual(live->left);
                            if (live->left.host && live->left.hasVisual)
                            {
                                live->left.visual.Y =
                                    static_cast<float>(live->left.host.Margin().Top);
                            }
                        }
                        catch (...)
                        {
                            live->leftAppearRunning = false;
                        }
                    });
            }
            catch (...)
            {
                ResetLeftAppearVisual(overlay);
                SnapOverlayGeometry(overlay, bounds);
                state.leftAppearRunning = false;
            }
        }

        void NormalizeOverlayBaseline(ZoneOverlay& overlay) noexcept
        {
            if (!overlay.host || !overlay.transform)
                return;
            try
            {
                double w = overlay.host.Width();
                double sx = overlay.transform.ScaleX();
                if (!(w > 1.0) || std::isnan(w))
                    w = overlay.hasVisual ? overlay.visual.Width : 1.0;
                if (std::isnan(sx) || sx <= 0.0)
                    sx = 1.0;
                overlay.host.Width(w * sx);
                overlay.transform.ScaleX(1.0);
            }
            catch (...)
            {
            }
        }

        void AppendOverlayAnims(
            wuxa_anim::Storyboard const& sb,
            ZoneOverlay& overlay,
            wf::Rect const& to,
            wux::Duration const& dur) noexcept
        {
            EnsureOverlayTransform(overlay);
            NormalizeOverlayBaseline(overlay);

            overlay.target = to;
            overlay.hasPending = false;
            overlay.queuedForAnim = false;
            overlay.inFrameAnim = true;
            overlay.host.Height(static_cast<double>(to.Height));
            overlay.host.Margin(wux::Thickness{ 0.0, static_cast<double>(to.Y), 0.0, 0.0 });
            overlay.transform.TranslateY(0.0);

            const double fromX = overlay.transform.TranslateX();
            double fromW = overlay.host.Width();
            if (!(fromW > 1.0) || std::isnan(fromW))
                fromW = overlay.hasVisual ? overlay.visual.Width : to.Width;
            const double toScale = (fromW > 1.0) ? (static_cast<double>(to.Width) / fromW) : 1.0;

            wuxa_anim::CubicEase easeX;
            easeX.EasingMode(wuxa_anim::EasingMode::EaseOut);
            wuxa_anim::CubicEase easeS;
            easeS.EasingMode(wuxa_anim::EasingMode::EaseOut);

            wuxa_anim::DoubleAnimation animX;
            animX.From(fromX);
            animX.To(static_cast<double>(to.X));
            animX.Duration(dur);
            animX.EasingFunction(easeX);
            wuxa_anim::Storyboard::SetTarget(animX, overlay.transform);
            wuxa_anim::Storyboard::SetTargetProperty(animX, L"TranslateX");

            wuxa_anim::DoubleAnimation animS;
            animS.From(1.0);
            animS.To(toScale);
            animS.Duration(dur);
            animS.EasingFunction(easeS);
            wuxa_anim::Storyboard::SetTarget(animS, overlay.transform);
            wuxa_anim::Storyboard::SetTargetProperty(animS, L"ScaleX");

            sb.Children().Append(animX);
            sb.Children().Append(animS);
        }

        void BeginFrameStoryboard(FrameZoneState& state) noexcept;

        void FinishFrameStoryboard(FrameZoneState& state) noexcept
        {
            ZoneOverlay* zones[3] = { &state.left, &state.center, &state.tray };
            for (ZoneOverlay* z : zones)
            {
                if (!z->inFrameAnim)
                    continue;
                SnapOverlayGeometry(*z, z->target);
            }
            state.frameStoryboard = nullptr;
            state.frameAnimRunning = false;

            bool anyPending = false;
            for (ZoneOverlay* z : zones)
            {
                if (z->hasPending && z->host && RectValid(z->pending) && !RectNear(z->visual, z->pending))
                {
                    z->queuedForAnim = true;
                    z->target = z->pending;
                    z->hasPending = false;
                    anyPending = true;
                }
                else
                {
                    z->hasPending = false;
                    z->queuedForAnim = false;
                }
            }
            if (anyPending)
            {
                BeginFrameStoryboard(state);
            }
        }

        void BeginFrameStoryboard(FrameZoneState& state) noexcept
        {
            // Новый кадр обесценивает Completed предыдущего Storyboard.
            ++state.frameAnimGeneration;

            ZoneOverlay* zones[3] = { &state.left, &state.center, &state.tray };
            unsigned mask = 0;
            unsigned count = 0;
            for (unsigned i = 0; i < 3; ++i)
            {
                ZoneOverlay& z = *zones[i];
                if (!z.queuedForAnim || !z.host || !RectValid(z.target))
                {
                    z.queuedForAnim = false;
                    continue;
                }
                if (z.hasVisual && RectNear(z.visual, z.target))
                {
                    SnapOverlayGeometry(z, z.target);
                    continue;
                }
                mask |= (1u << i);
                ++count;
            }
            if (count == 0)
            {
                state.frameAnimRunning = false;
                return;
            }

            try
            {
                wuxa_anim::Storyboard sb;
                const wf::TimeSpan durTs =
                    std::chrono::duration_cast<wf::TimeSpan>(std::chrono::milliseconds{ kAnimDurationMs });
                const wux::Duration dur{ durTs };

                for (unsigned i = 0; i < 3; ++i)
                {
                    if ((mask & (1u << i)) == 0)
                        continue;
                    AppendOverlayAnims(sb, *zones[i], zones[i]->target, dur);
                }

                state.frameStoryboard = sb;
                state.frameAnimRunning = true;
                const auto token = AcquireZoneLifetime(state);
                const std::uint32_t generation = state.frameAnimGeneration;
                sb.Completed(
                    [token, generation](wf::IInspectable const&, wf::IInspectable const&) {
                        FrameZoneState* const live = LockZoneState(token);
                        if (!live || live->frameAnimGeneration != generation)
                            return;
                        FinishFrameStoryboard(*live);
                    });

                sb.Begin();
            }
            catch (const winrt::hresult_error&)
            {
                state.frameAnimRunning = false;
                state.frameStoryboard = nullptr;
                for (unsigned i = 0; i < 3; ++i)
                {
                    if ((mask & (1u << i)) == 0)
                        continue;
                    SnapOverlayGeometry(*zones[i], zones[i]->target);
                }
            }
            catch (...)
            {
                state.frameAnimRunning = false;
                state.frameStoryboard = nullptr;
                for (unsigned i = 0; i < 3; ++i)
                {
                    if ((mask & (1u << i)) == 0)
                        continue;
                    SnapOverlayGeometry(*zones[i], zones[i]->target);
                }
            }
        }

        void StopFrameStoryboard(FrameZoneState& state) noexcept
        {
            ++state.frameAnimGeneration;
            try
            {
                if (state.animArmTimer)
                    state.animArmTimer.Stop();
            }
            catch (...)
            {
            }
            try
            {
                if (state.frameStoryboard)
                    state.frameStoryboard.Stop();
            }
            catch (...)
            {
            }
            state.frameStoryboard = nullptr;
            state.frameAnimRunning = false;
            ZoneOverlay* zones[3] = { &state.left, &state.center, &state.tray };
            for (ZoneOverlay* z : zones)
            {
                z->inFrameAnim = false;
                z->queuedForAnim = false;
                z->hasPending = false;
                SyncVisualFromElement(*z);
                if (z->host && z->hasVisual)
                {
                    try
                    {
                        EnsureOverlayTransform(*z);
                        z->host.Width(static_cast<double>(z->visual.Width));
                        z->transform.ScaleX(1.0);
                        z->transform.TranslateX(static_cast<double>(z->visual.X));
                    }
                    catch (...)
                    {
                    }
                }
            }
        }

        void StopAllOverlayStoryboards(FrameZoneState& state) noexcept
        {
            StopFrameStoryboard(state);
        }

        void QueueOverlayAnim(FrameZoneState& state, ZoneOverlay& overlay, wf::Rect const& bounds) noexcept
        {
            if (!overlay.host)
                return;

            // Left slide-up: только X/W, TranslateY анимируется отдельно.
            if (state.leftAppearRunning && &overlay == &state.left)
            {
                try
                {
                    EnsureOverlayTransform(overlay);
                    overlay.host.Width(static_cast<double>(bounds.Width));
                    overlay.host.Height(static_cast<double>(bounds.Height));
                    overlay.host.Margin(
                        wux::Thickness{ 0.0, static_cast<double>(bounds.Y), 0.0, 0.0 });
                    overlay.transform.TranslateX(static_cast<double>(bounds.X));
                    overlay.transform.ScaleX(1.0);
                    overlay.visual.X = bounds.X;
                    overlay.visual.Width = bounds.Width;
                    overlay.visual.Height = bounds.Height;
                    overlay.target = bounds;
                    overlay.hasVisual = true;
                }
                catch (...)
                {
                }
                return;
            }

            // Center/tray: мгновенный snap (без ScaleX Storyboard).
            // Лог поиска: сначала cW↓ при том же cX, потом cX→ — двухфазный layout Windows;
            // наша анимация 120ms только отставала («задержка, потом прыжок»).
            if (state.frameAnimRunning)
            {
                try
                {
                    if (state.frameStoryboard)
                        state.frameStoryboard.Stop();
                }
                catch (...)
                {
                }
                for (ZoneOverlay* z : { &state.left, &state.center, &state.tray })
                {
                    z->inFrameAnim = false;
                    z->queuedForAnim = false;
                    z->hasPending = false;
                }
                state.frameStoryboard = nullptr;
                state.frameAnimRunning = false;
            }

            SnapOverlayGeometry(overlay, bounds);
        }

        void CommitFrameAnimations(FrameZoneState& state) noexcept
        {
            // Overlay geometry — snap; анимация только left slide-up.
            (void)state;
        }

        void EnsureOverlayThemeShadow(ZoneOverlay& overlay, wuxc::Grid const& root) noexcept
        {
            if (!overlay.element || !root)
                return;
            try
            {
                if (!overlay.element.Shadow())
                    overlay.element.Shadow(wuxm::ThemeShadow{});
                if (const auto shadow = overlay.element.Shadow().try_as<wuxm::ThemeShadow>())
                {
                    shadow.Receivers().Clear();
                    shadow.Receivers().Append(root);
                }
                // Лёгкий подъём без opaque plate (plate давала чёрную плитку).
                overlay.element.Translation(wfn::float3{ 0.0f, 0.0f, 24.0f });
            }
            catch (...)
            {
            }
        }

        void ApplyOverlayChrome(
            ZoneOverlay& overlay,
            wuxm::Brush const& brush,
            double radius,
            bool patchBorder,
            DWORD borderArgb) noexcept
        {
            if (!overlay.element || !brush)
                return;
            try
            {
                overlay.element.Background(brush);
                const auto cr = wux::CornerRadius{ radius, radius, radius, radius };
                overlay.element.CornerRadius(cr);

                if (patchBorder)
                {
                    wuxm::SolidColorBrush bb;
                    bb.Color(ColorFromArgb(borderArgb));
                    overlay.element.BorderBrush(bb);
                    overlay.element.BorderThickness(wux::Thickness{ 1.0, 1.0, 1.0, 1.0 });
                }
                else
                {
                    overlay.element.BorderBrush(nullptr);
                    overlay.element.BorderThickness({});
                }
            }
            catch (...)
            {
            }
        }

        void ApplyOverlay(
            ZoneOverlay& overlay,
            FrameZoneState& state,
            wuxc::Grid const& root,
            wf::Rect bounds,
            wuxm::Brush const& brush,
            double radius,
            bool patchBorder,
            DWORD borderArgb,
            int32_t zIndex,
            bool slideUpAppear = false) noexcept
        {
            if (!root || !brush || !RectValid(bounds))
                return;

            try
            {
                if (!overlay.host)
                {
                    overlay.host = wuxc::Grid{};
                    overlay.host.IsHitTestVisible(false);
                    overlay.host.HorizontalAlignment(wux::HorizontalAlignment::Left);
                    overlay.host.VerticalAlignment(wux::VerticalAlignment::Top);

                    overlay.element = wuxc::Border{};
                    overlay.element.IsHitTestVisible(false);
                    overlay.element.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
                    overlay.element.VerticalAlignment(wux::VerticalAlignment::Stretch);

                    overlay.host.Children().Append(overlay.element);

                    const uint32_t insertAt = root.Children().Size() > 0 ? 1u : 0u;
                    root.Children().InsertAt(insertAt, overlay.host);
                    overlay.inserted = true;
                    EnsureOverlayTransform(overlay);
                }

                // Отрицательный ZIndex: контент RootGrid (Z=0) поверх blur/тинтовых плиток.
                wuxc::Canvas::SetZIndex(overlay.host, zIndex);
                ApplyOverlayChrome(overlay, brush, radius, patchBorder, borderArgb);
                EnsureOverlayThemeShadow(overlay, root);

                const bool first = !overlay.hasVisual || !overlay.active;
                if (first && slideUpAppear)
                    BeginLeftSlideUpAppear(state, overlay, bounds);
                else if (first)
                    SnapOverlayGeometry(overlay, bounds);
                else
                    QueueOverlayAnim(state, overlay, bounds);

                overlay.host.Visibility(wux::Visibility::Visible);
                overlay.active = true;
            }
            catch (const winrt::hresult_error& ex)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][Zones] ApplyOverlay hr=0x%08X",
                    static_cast<unsigned>(ex.code()));
            }
            catch (...)
            {
            }
        }

        void ClipCenterAgainstNeighbor(
            wf::Rect& center,
            wf::Rect const& neighbor,
            bool neighborIsLeft,
            float gap) noexcept
        {
            if (!RectValid(center) || !RectValid(neighbor))
                return;
            if (neighborIsLeft)
            {
                const float edge = neighbor.X + neighbor.Width + gap;
                if (center.X < edge)
                {
                    const float right = center.X + center.Width;
                    center.X = edge;
                    center.Width = (std::max)(1.0f, right - edge);
                }
            }
            else
            {
                const float edge = neighbor.X - gap;
                if (center.X + center.Width > edge)
                    center.Width = (std::max)(1.0f, edge - center.X);
            }
        }

        // Гасим цельный слой TaskbarBackground (тинт на всю ширину под зонами).
        void SuppressFullBarLayer(
            wux::Shapes::Rectangle const& backgroundFill,
            FrameZoneState& state) noexcept
        {
            if (!backgroundFill)
                return;
            try
            {
                if (!state.fillCollapsed)
                {
                    state.originalFillVisibility = backgroundFill.Visibility();
                    state.fillCollapsed = true;
                }
                wuxm::SolidColorBrush transparent;
                transparent.Color(winrt::Windows::UI::Colors::Transparent());
                backgroundFill.Fill(transparent);
                backgroundFill.Visibility(wux::Visibility::Collapsed);

                const auto parent = wuxm::VisualTreeHelper::GetParent(backgroundFill)
                    .try_as<wux::FrameworkElement>();
                if (auto grid = parent.try_as<wuxc::Grid>())
                {
                    if (!state.fillParentCleared)
                    {
                        state.fillParentGrid = grid;
                        state.originalFillParentBackground = grid.Background();
                        state.fillParentCleared = true;
                    }
                    // Transparent hit-surface: nullptr убирает RMB по «пустым» краям сегментов.
                    wuxm::SolidColorBrush hitBrush;
                    hitBrush.Color(winrt::Windows::UI::Colors::Transparent());
                    grid.Background(hitBrush);
                }

                // Taskbar.TaskbarBackground — Visibility=Collapsed (Opacity=0 оставлял тинт в composition).
                wux::FrameworkElement climb = parent;
                while (climb)
                {
                    const auto leaf = TypeLeaf(climb);
                    if (leaf.find(L"TaskbarBackground") != std::wstring::npos
                        || climb.Name() == L"BackgroundControl")
                    {
                        if (!state.backgroundControlOpacityPatched)
                        {
                            state.backgroundControl = climb;
                            state.originalBackgroundControlOpacity = climb.Opacity();
                            try
                            {
                                state.originalBackgroundControlVisibility = climb.Visibility();
                            }
                            catch (...)
                            {
                                state.originalBackgroundControlVisibility = wux::Visibility::Visible;
                            }
                            state.backgroundControlOpacityPatched = true;
                        }
                        climb.Opacity(0.0);
                        climb.Visibility(wux::Visibility::Collapsed);
                        break;
                    }
                    if (climb.Name() == L"RootGrid")
                        break;
                    climb = wuxm::VisualTreeHelper::GetParent(climb)
                        .try_as<wux::FrameworkElement>();
                }
            }
            catch (...)
            {
            }
        }

        void RestoreFullBarLayer(
            wux::Shapes::Rectangle const& backgroundFill,
            FrameZoneState& state) noexcept
        {
            try
            {
                if (state.backgroundControl && state.backgroundControlOpacityPatched)
                {
                    state.backgroundControl.Visibility(state.originalBackgroundControlVisibility);
                    state.backgroundControl.Opacity(state.originalBackgroundControlOpacity);
                }
            }
            catch (...)
            {
            }
            state.backgroundControlOpacityPatched = false;

            try
            {
                if (state.fillParentGrid && state.fillParentCleared)
                    state.fillParentGrid.Background(state.originalFillParentBackground);
            }
            catch (...)
            {
            }
            state.fillParentCleared = false;

            try
            {
                if (backgroundFill && state.fillCollapsed)
                    backgroundFill.Visibility(state.originalFillVisibility);
            }
            catch (...)
            {
            }
            state.fillCollapsed = false;
        }

        void RestoreLegacyTrayHost(FrameZoneState& state) noexcept
        {
            if (!state.legacyTrayHost || !state.legacyTrayCaptured || !state.legacyTrayPatched)
            {
                state.legacyTrayPatched = false;
                return;
            }
            try
            {
                state.legacyTrayHost.Background(state.legacyTrayBackground);
                state.legacyTrayHost.CornerRadius(state.legacyTrayCorner);
                state.legacyTrayHost.BorderBrush(state.legacyTrayBorderBrush);
                state.legacyTrayHost.BorderThickness(state.legacyTrayBorderThickness);
            }
            catch (...)
            {
            }
            state.legacyTrayPatched = false;
        }

        // Снять прошлый HostChrome с tray (если остался от предыдущей сборки).
        void EnsureLegacyTrayRestored(wuxc::Grid const& trayGrid, FrameZoneState& state) noexcept
        {
            if (!trayGrid)
                return;
            // Если ранее патчили этот host — восстановить. Иначе запомнить originals на случай
            // чужого Background (не трогаем, только снимем наш legacy flag).
            if (state.legacyTrayPatched && state.legacyTrayHost == trayGrid)
                RestoreLegacyTrayHost(state);
        }

        [[nodiscard]] const char* BrushKindTag(wuxm::Brush const& brush) noexcept
        {
            if (!brush)
                return "null";
            try
            {
                if (brush.try_as<wuxm::SolidColorBrush>())
                    return "solid";
                if (brush.try_as<wuxm::AcrylicBrush>())
                    return "acrylic";
                return "other";
            }
            catch (...)
            {
                return "err";
            }
        }
    }

    double CornerRadiusDip(DWORD cornerType, double panelHeightDip) noexcept
    {
        return Shared::TrayXamlPaint::XamlCornerRadiusDip(cornerType, panelHeightDip);
    }

    void Apply(
        wux::Shapes::Rectangle const& backgroundFill,
        wux::Shapes::Rectangle const& backgroundStroke,
        FrameZoneState& state,
        Registry::Settings const& settings,
        wuxm::Brush const& brush) noexcept
    {
        if (!brush || !backgroundFill)
            return;

        try
        {
            const auto rootFe = FindParentNamed(backgroundFill, L"RootGrid");
            auto rootGrid = rootFe.try_as<wuxc::Grid>();
            if (!rootGrid)
            {
                return;
            }

            const auto searchRoot = [&]() -> wux::FrameworkElement {
                try
                {
                    if (const auto xr = backgroundFill.XamlRoot())
                    {
                        if (auto content = xr.Content().try_as<wux::FrameworkElement>())
                            return content;
                    }
                }
                catch (...)
                {
                }
                return rootFe;
            }();

            wux::FrameworkElement startBtn{ nullptr };
            wux::FrameworkElement searchBtn{ nullptr };
            wux::FrameworkElement taskViewBtn{ nullptr };
            wux::FrameworkElement widgetsBtn{ nullptr };
            wux::FrameworkElement trayDeep{ nullptr };
            wuxc::Grid trayGrid{ nullptr };
            std::vector<wux::FrameworkElement> taskButtons;
            unsigned skippedWideHosts = 0;

            VisitDescendants(searchRoot, [&](wux::FrameworkElement const& fe) {
                try
                {
                    const auto name = fe.Name();
                    const auto leaf = TypeLeaf(fe);
                    if (name == L"SystemTrayFrameGrid")
                    {
                        trayDeep = fe;
                        trayGrid = fe.try_as<wuxc::Grid>();
                    }
                    else if (!trayDeep
                        && (name == L"SystemTrayFrame" || leaf == L"SystemTrayFrame"))
                    {
                        trayDeep = fe;
                        trayGrid = fe.try_as<wuxc::Grid>();
                    }

                    const auto autoId = wuxa::AutomationProperties::GetAutomationId(fe);
                    if (autoId == L"StartButton")
                        startBtn = fe;
                    else if (autoId == L"WidgetsButton")
                        widgetsBtn = fe;
                    else if (
                        autoId == L"Search"
                        || autoId == L"SearchButton"
                        || autoId == L"StartMenuSearch"
                        || name == L"LaunchListButton"
                        || name == L"SearchBox")
                    {
                        searchBtn = fe;
                    }
                    else if (autoId == L"TaskViewButton" || name == L"TaskViewButton")
                    {
                        taskViewBtn = fe;
                    }

                    if (leaf == L"TaskListButton"
                        && name != L"LaunchListButton"
                        && autoId != L"StartButton"
                        && autoId != L"WidgetsButton"
                        && autoId != L"Search"
                        && autoId != L"SearchButton"
                        && autoId != L"TaskViewButton")
                    {
                        taskButtons.push_back(fe);
                    }
                    else if (leaf.find(L"TaskListButton") != std::wstring::npos)
                    {
                        ++skippedWideHosts;
                    }
                }
                catch (...)
                {
                }
                return true;
            });

            wf::Rect leftBounds{};
            wf::Rect centerBounds{};
            wf::Rect trayBounds{};
            wf::Rect startBounds{};
            wf::Rect searchBounds{};
            wf::Rect taskViewBounds{};

            const float rootWEarly = static_cast<float>(rootFe.ActualWidth());
            const float rootHEarly = static_cast<float>(
                rootFe.ActualHeight() > 1.0 ? rootFe.ActualHeight() : 48.0);

            if (trayDeep)
            {
                trayBounds = BoundsInRoot(trayDeep, rootFe);
                if (RectValid(trayBounds) && !RectOnPanel(trayBounds, rootWEarly, rootHEarly))
                {
                    // Tray почти на всю ширину — не режем по 0.92; только Y/X-slop.
                    if (trayBounds.X < -8.0f || trayBounds.Y < -8.0f
                        || trayBounds.Y > rootHEarly + 8.0f)
                    {
                        trayBounds = {};
                    }
                }
            }

            const DWORD taskbarDa = ReadExplorerAdvancedDword(L"TaskbarDa", 1);
            if (taskbarDa != 0 && widgetsBtn)
            {
                leftBounds = BoundsInRoot(widgetsBtn, rootFe);
                if (RectValid(leftBounds) && !RectOnPanel(leftBounds, rootWEarly, rootHEarly))
                    leftBounds = {};
            }

            // Пуск/ТV — узкие; Поиск может быть шире 120dip (иначе правый край «съезжает» влево).
            auto chromeBounds =
                [&](wux::FrameworkElement const& fe, float maxW) noexcept -> wf::Rect {
                wf::Rect r = BoundsInRoot(fe, rootFe);
                if (!RectValid(r))
                    return {};
                if (!RectOnPanel(r, rootWEarly, rootHEarly))
                    return {};
                if (r.Width > maxW)
                    r.Width = maxW;
                return r;
            };

            if (startBtn)
                startBounds = chromeBounds(startBtn, kMaxTaskButtonWidthDip);
            if (searchBtn)
                searchBounds = chromeBounds(searchBtn, (std::max)(kMaxTaskButtonWidthDip, rootWEarly * 0.45f));
            if (taskViewBtn)
                taskViewBounds = chromeBounds(taskViewBtn, kMaxTaskButtonWidthDip);

            std::vector<wf::Rect> taskRects;
            taskRects.reserve(taskButtons.size());

            for (const auto& btn : taskButtons)
            {
                try
                {
                    if (btn.Visibility() != wux::Visibility::Visible)
                        continue;
                    if (btn.Opacity() < 0.05)
                        continue;
                }
                catch (...)
                {
                }

                if (trayDeep && IsDescendantOf(btn, trayDeep))
                    continue;
                if (widgetsBtn && IsDescendantOf(btn, widgetsBtn))
                    continue;

                const wf::Rect br = BoundsInRoot(btn, rootFe);
                if (!RectValid(br))
                    continue;
                if (!RectOnPanel(br, rootWEarly, rootHEarly))
                    continue;

                if (br.Width > kMaxTaskButtonWidthDip)
                    continue;

                // Левее Пуск — призраки (ложный раздув center к left+gap).
                if (RectValid(startBounds) && (br.X + br.Width) <= (startBounds.X + 0.5f))
                    continue;

                taskRects.push_back(br);
            }

            // Кластер иконок у центра: только TaskListButton, не Пуск/Поиск.
            if (taskRects.size() >= 2u)
            {
                const float rootMid = static_cast<float>(rootFe.ActualWidth()) * 0.5f;
                size_t seed = 0;
                float bestDist = 1.0e9f;
                for (size_t i = 0; i < taskRects.size(); ++i)
                {
                    const float mid = taskRects[i].X + taskRects[i].Width * 0.5f;
                    const float d = std::fabs(mid - rootMid);
                    if (d < bestDist)
                    {
                        bestDist = d;
                        seed = i;
                    }
                }
                std::vector<char> keep(taskRects.size(), 0);
                keep[seed] = 1;
                bool grown = true;
                while (grown)
                {
                    grown = false;
                    for (size_t i = 0; i < taskRects.size(); ++i)
                    {
                        if (keep[i])
                            continue;
                        for (size_t j = 0; j < taskRects.size(); ++j)
                        {
                            if (!keep[j])
                                continue;
                            const float gap =
                                (taskRects[i].X > taskRects[j].X)
                                    ? (taskRects[i].X - (taskRects[j].X + taskRects[j].Width))
                                    : (taskRects[j].X - (taskRects[i].X + taskRects[i].Width));
                            if (gap <= 80.0f)
                            {
                                keep[i] = 1;
                                grown = true;
                                break;
                            }
                        }
                    }
                }
                std::vector<wf::Rect> clustered;
                clustered.reserve(taskRects.size());
                for (size_t i = 0; i < taskRects.size(); ++i)
                {
                    if (keep[i])
                        clustered.push_back(taskRects[i]);
                }
                taskRects.swap(clustered);
            }

            // Пуск / Поиск / Task View всегда в center (разрыв до иконок — норма, там Search).
            if (RectValid(startBounds))
                UnionRect(centerBounds, startBounds);
            if (RectValid(searchBounds))
                UnionRect(centerBounds, searchBounds);
            if (RectValid(taskViewBounds))
                UnionRect(centerBounds, taskViewBounds);

            for (const auto& br : taskRects)
                UnionRect(centerBounds, br);

            leftBounds = InflateRect(leftBounds, 0.0f, static_cast<float>(kZonePadY));
            centerBounds = InflateRect(centerBounds, 0.0f, static_cast<float>(kZonePadY));
            trayBounds = InflateRect(trayBounds, 0.0f, static_cast<float>(kZonePadY));
            ClampZoneToPanel(leftBounds, rootHEarly);
            ClampZoneToPanel(centerBounds, rootHEarly);
            ClampZoneToPanel(trayBounds, rootHEarly);

            // Полный gap (не half): half + односторонний clip оставлял left/tray «въехавшими» в центр.
            const float gapKeep = static_cast<float>(kZoneGapDip);
            ClipCenterAgainstNeighbor(centerBounds, leftBounds, true, gapKeep);
            ClipCenterAgainstNeighbor(centerBounds, trayBounds, false, gapKeep);

            const float rootW = static_cast<float>(rootFe.ActualWidth());
            const float padX =
                settings.cornerType == Shared::TrayXamlPaint::kCornerTypeMaxRoundXaml
                    ? kZonePadXMaxRound
                    : kZonePadX;
            // Сначала pad центра, затем стороны относительно уже расширенного центра —
            // иначе left expand съедает левый pad центра.
            const wf::Rect leftContent = leftBounds;
            const wf::Rect trayContent = trayBounds;
            ExpandZoneHorizontal(
                centerBounds, rootW, padX, &leftContent, &trayContent, gapKeep);
            ExpandZoneHorizontal(
                leftBounds, rootW, padX, nullptr, &centerBounds, gapKeep);
            ExpandZoneHorizontal(
                trayBounds, rootW, padX, &centerBounds, nullptr, gapKeep);
            // Не ClipCenter после expand: left/tray уже упираются в padded center+gap;
            // повторный clip снова ставит cX = left.Right+gap и съедает левый pad центра.
            ClipSideAgainstCenter(leftBounds, centerBounds, true, gapKeep);
            ClipSideAgainstCenter(trayBounds, centerBounds, false, gapKeep);

            // Shrink-hold: mid-anim measure уже, commit — после debounce settle.
            state.centerShrinkHoldPending = false;
            if (!state.centerShrinkCommit
                && state.center.hasVisual
                && RectValid(centerBounds)
                && RectValid(state.center.visual))
            {
                const wf::Rect& prev = state.center.visual;
                constexpr float kHoldEps = 2.0f;
                const float prevR = prev.X + prev.Width;
                const float newR = centerBounds.X + centerBounds.Width;
                if (centerBounds.Width + kHoldEps < prev.Width || newR + kHoldEps < prevR)
                {
                    const float left = (std::min)(centerBounds.X, prev.X);
                    const float right = (std::max)(newR, prevR);
                    centerBounds.X = left;
                    centerBounds.Width = (std::max)(1.0f, right - left);
                    state.centerShrinkHoldPending = true;
                    ClipSideAgainstCenter(leftBounds, centerBounds, true, gapKeep);
                    ClipSideAgainstCenter(trayBounds, centerBounds, false, gapKeep);
                }
            }

            const unsigned zoneCount =
                (RectValid(leftBounds) ? 1u : 0u)
                + (RectValid(centerBounds) ? 1u : 0u)
                + (RectValid(trayBounds) ? 1u : 0u);

            if (zoneCount < 2u)
            {
                Restore(backgroundFill, backgroundStroke, state);
                return;
            }

            const auto packU16 = [](float v) noexcept -> std::uint64_t {
                const int i = static_cast<int>(v + (v >= 0.0f ? 0.5f : -0.5f));
                const int clamped = i < 0 ? 0 : (i > 65535 ? 65535 : i);
                return static_cast<std::uint64_t>(clamped);
            };
            const std::uint64_t boundsSig =
                (packU16(leftBounds.X) << 48) | (packU16(leftBounds.Width) << 32)
                | (packU16(centerBounds.X) << 16) | packU16(centerBounds.Width)
                ^ ((packU16(trayBounds.X) << 16) | packU16(trayBounds.Width));
            if (boundsSig == state.lastBoundsSig
                && (state.left.active || state.center.active || state.tray.active))
            {
                // Геометрия та же — кисть/скругление всё равно обновляем (смена Effect/tint).
                const double panelHSkip = rootFe.ActualHeight() > 1.0
                    ? rootFe.ActualHeight()
                    : (backgroundFill.ActualHeight() > 1.0 ? backgroundFill.ActualHeight() : 48.0);
                const double radiusRefSkip = RectValid(centerBounds)
                    ? (std::min)(panelHSkip, static_cast<double>(centerBounds.Height))
                    : panelHSkip;
                const double radiusSkip = CornerRadiusDip(settings.cornerType, radiusRefSkip);
                const bool patchBorderSkip = settings.patchBorderStroke && settings.cornerType != 0;
                const DWORD borderArgbSkip = settings.borderColorArgb;
                if (state.left.active)
                    ApplyOverlayChrome(
                        state.left, brush, radiusSkip, patchBorderSkip, borderArgbSkip);
                if (state.center.active)
                    ApplyOverlayChrome(
                        state.center, brush, radiusSkip, patchBorderSkip, borderArgbSkip);
                if (state.tray.active)
                    ApplyOverlayChrome(
                        state.tray, brush, radiusSkip, patchBorderSkip, borderArgbSkip);
                return;
            }
            state.lastBoundsSig = boundsSig;

            CaptureAndClearRootGrid(state, rootGrid);
            SuppressFullBarLayer(backgroundFill, state);

            if (backgroundStroke)
            {
                if (!state.strokeCollapsed)
                {
                    state.originalStrokeVisibility = backgroundStroke.Visibility();
                    state.strokeCollapsed = true;
                }
                backgroundStroke.Visibility(wux::Visibility::Collapsed);
            }

            EnsureLegacyTrayRestored(trayGrid, state);

            const double panelH = rootFe.ActualHeight() > 1.0
                ? rootFe.ActualHeight()
                : (backgroundFill.ActualHeight() > 1.0 ? backgroundFill.ActualHeight() : 48.0);
            // MaxRound: R от высоты сегмента (не panelH), иначе при cH!=panelH центр радиуса «плывёт».
            const double radiusRefH = RectValid(centerBounds)
                ? (std::min)(panelH, static_cast<double>(centerBounds.Height))
                : panelH;
            const double radius = CornerRadiusDip(settings.cornerType, radiusRefH);
            const bool patchBorder = settings.patchBorderStroke && settings.cornerType != 0;
            const DWORD borderArgb = settings.borderColorArgb;

            // ZIndex < 0 — иначе blur-плитка перекрывает иконки (лог: left поверх контента).
            constexpr int32_t kZoneZ = -100;
            if (RectValid(centerBounds))
                ApplyOverlay(
                    state.center, state, rootGrid, centerBounds, brush, radius, patchBorder, borderArgb, kZoneZ);
            else
                RemoveOverlay(state, state.center, rootGrid);

            if (RectValid(leftBounds))
            {
                // Slide-up только после Remove left в сессии (лог: slide=1 на старте — не нужно).
                const bool slideUp = !state.left.active && state.leftAllowAppearAnim;
                ApplyOverlay(
                    state.left,
                    state,
                    rootGrid,
                    leftBounds,
                    brush,
                    radius,
                    patchBorder,
                    borderArgb,
                    kZoneZ,
                    slideUp);
            }
            else
                RemoveOverlay(state, state.left, rootGrid);

            if (RectValid(trayBounds))
                ApplyOverlay(
                    state.tray, state, rootGrid, trayBounds, brush, radius, patchBorder, borderArgb, kZoneZ);
            else
                RemoveOverlay(state, state.tray, rootGrid);

            // Один Begin на left+center+tray — синхронные края (лог Z5: solo center followup).
            CommitFrameAnimations(state);

        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][Zones] Apply hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][Zones] Apply unknown exception");
        }
    }

    void Restore(
        wux::Shapes::Rectangle const& backgroundFill,
        wux::Shapes::Rectangle const& backgroundStroke,
        FrameZoneState& state) noexcept
    {
        StopAllOverlayStoryboards(state);
        const auto root = state.rootGrid;
        RemoveOverlay(state, state.left, root);
        RemoveOverlay(state, state.center, root);
        RemoveOverlay(state, state.tray, root);
        RestoreLegacyTrayHost(state);
        RestoreFullBarLayer(backgroundFill, state);
        RestoreRootGridChrome(state);
        try
        {
            if (backgroundStroke && state.strokeCollapsed)
                backgroundStroke.Visibility(state.originalStrokeVisibility);
        }
        catch (...)
        {
        }
        state.strokeCollapsed = false;
        state.lastBoundsSig = 0;
    }
}
