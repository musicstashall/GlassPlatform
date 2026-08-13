#include "TaskbarPch.h"
#include "xaml/TaskbarBackgroundRegistry.hpp"
#include "xaml/brushes/XamlBackdropFactory.hpp"
#include "xaml/TaskbarZonePainter.hpp"

#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <DebugLog.h>
#include <Helpers/ThemeHelper.h>
#include <OpenGlassDwmPolicy.h>
#include <OpenGlassHostState.h>
#include <SystemHelper.h>
#include <TrayBorderDrawMode.h>
#include <TrayXamlPaint.h>
#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>

namespace wux = winrt::Windows::UI::Xaml;

namespace TaskbarCompat
{
    namespace
    {
        std::atomic<unsigned> g_xamlPatchSeq{ 0 };

        enum class DebugXamlStrokeMode : DWORD
        {
            Auto   = 0xFFFFFFFFu,
            Fill   = 0,
            Stroke = 1,
            FillInset = 2,
        };

        [[nodiscard]] DWORD ReadTrayDebugDword(const wchar_t* valueName, DWORD defaultValue) noexcept
        {
            DWORD v = defaultValue;
            DWORD cb = sizeof(v);
            const LSTATUS ls = RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\StanislavKa\\BlackWindowManager\\Tray",
                valueName,
                RRF_RT_REG_DWORD,
                nullptr,
                &v,
                &cb);
            if (ls != ERROR_SUCCESS)
                return defaultValue;
            return v;
        }

        [[nodiscard]] bool ReadTrayDebugXamlRedrawWithFrame() noexcept
        {
            return ReadTrayDebugDword(L"DebugXamlRedrawWithFrame", 0) != 0;
        }

        [[nodiscard]] DebugXamlStrokeMode ReadTrayDebugXamlStrokeMode() noexcept
        {
            const DWORD v = ReadTrayDebugDword(L"DebugXamlStrokeMode", static_cast<DWORD>(DebugXamlStrokeMode::Auto));
            if (v > static_cast<DWORD>(DebugXamlStrokeMode::FillInset))
                return DebugXamlStrokeMode::Auto;
            return static_cast<DebugXamlStrokeMode>(v);
        }

        [[nodiscard]] DWORD ReadTrayDebugXamlStrokeInset(DWORD defaultInset) noexcept
        {
            const DWORD v = ReadTrayDebugDword(L"DebugXamlStrokeInset", defaultInset);
            return v;
        }

        [[nodiscard]] DWORD ReadTrayCornerTypeFromRegistry() noexcept
        {
            if (Shared::SystemHelper::g_buildNumber < 22000)
                return static_cast<DWORD>(DWMWCP_DONOTROUND);

            DWORD v = static_cast<DWORD>(DWMWCP_ROUND);
            DWORD cb = sizeof(v);
            const LSTATUS ls = RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\StanislavKa\\BlackWindowManager\\Tray",
                L"CornerType",
                RRF_RT_REG_DWORD,
                nullptr,
                &v,
                &cb);
            if (ls != ERROR_SUCCESS)
                return static_cast<DWORD>(DWMWCP_ROUND);
            return v;
        }

        [[nodiscard]] DWORD QueryEffectiveTrayCornerPref(HWND tray) noexcept
        {
            DWORD effective = static_cast<DWORD>(DWMWCP_DEFAULT);
            if (!tray || !IsWindow(tray))
                return effective;

            (void)DwmGetWindowAttribute(
                tray,
                DWMWA_WINDOW_CORNER_PREFERENCE,
                &effective,
                sizeof(effective));
            return effective;
        }

        /// Для XAML Stroke/Fill: явный ROUND/ROUNDSMALL из реестра; при DEFAULT — effective DWM на Shell_TrayWnd.
        [[nodiscard]] DWORD ResolveXamlStrokeCornerPref() noexcept
        {
            const DWORD raw = ReadTrayCornerTypeFromRegistry();
            if (raw == static_cast<DWORD>(DWMWCP_DONOTROUND))
                return raw;
            if (raw == static_cast<DWORD>(DWMWCP_ROUND)
                || raw == static_cast<DWORD>(DWMWCP_ROUNDSMALL))
            {
                return raw;
            }

            const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            return QueryEffectiveTrayCornerPref(tray);
        }

        [[nodiscard]] bool IsRoundedTrayCornerPreference(DWORD pref) noexcept
        {
            return pref == static_cast<DWORD>(DWMWCP_ROUND)
                || pref == static_cast<DWORD>(DWMWCP_ROUNDSMALL);
        }

        [[nodiscard]] DWORD DefaultHorizontalInsetForCorner(DWORD pref) noexcept
        {
            if (pref == static_cast<DWORD>(DWMWCP_ROUNDSMALL))
                return 4u;
            if (pref == static_cast<DWORD>(DWMWCP_ROUND))
                return 8u;
            return 0u;
        }

        [[nodiscard]] bool IsOpenGlassAffectingTrayBorders() noexcept
        {
            return Shared::OpenGlassHostState::IsPipelineAffectingBorders()
                || Shared::OpenGlassDwmPolicy::IsDwmGlassInjectedPipelinePresent();
        }

        void LogEffectiveCornerPrefOnTray() noexcept
        {
            const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
            if (!tray || !IsWindow(tray))
                return;

            const DWORD raw = ReadTrayCornerTypeFromRegistry();
            const DWORD effective = QueryEffectiveTrayCornerPref(tray);
            const DWORD strokePref = ResolveXamlStrokeCornerPref();
            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][BgRegistry] Tray corners registry=%u effective=%u xamlStrokePref=%u",
                raw,
                effective,
                strokePref);
        }

        [[nodiscard]] DWORD ReadTrayEnableDropShadowFromRegistry() noexcept
        {
            DWORD v = 0;
            DWORD cb = sizeof(v);
            (void)RegGetValueW(
                HKEY_CURRENT_USER,
                L"Software\\StanislavKa\\BlackWindowManager\\Tray",
                L"EnableDropShadow",
                RRF_RT_REG_DWORD,
                nullptr,
                &v,
                &cb);
            return v;
        }

        void LogTrayBorderDiagContext(unsigned seq, const wchar_t* stage) noexcept
        {
            const auto drawMode = Shared::TrayBorderDrawMode::ReadFromRegistry();
            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][BgRegistry] %s seq=%u BorderDrawMode=%u CornerType=%u "
                L"xamlStrokePref=%u EnableDropShadow=%u ogAffecting=%u",
                stage,
                seq,
                drawMode == Shared::TrayBorderDrawMode::Mode::Xaml ? 1u : 0u,
                ReadTrayCornerTypeFromRegistry(),
                ResolveXamlStrokeCornerPref(),
                ReadTrayEnableDropShadowFromRegistry(),
                IsOpenGlassAffectingTrayBorders() ? 1u : 0u);
        }

        [[nodiscard]] const wchar_t* StrokeModeLabel(DebugXamlStrokeMode mode) noexcept
        {
            switch (mode)
            {
            case DebugXamlStrokeMode::Fill: return L"Fill";
            case DebugXamlStrokeMode::Stroke: return L"Stroke";
            case DebugXamlStrokeMode::FillInset: return L"FillInset";
            default: return L"Auto";
            }
        }

        void LogBackgroundStrokeLayout(
            InstanceHandle           frameHandle,
            wux::Shapes::Rectangle   shape,
            unsigned                 seq,
            DebugXamlStrokeMode      mode,
            DWORD                    inset) noexcept
        {
            try
            {
                const wux::Thickness margin = shape.Margin();
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][BgRegistry] BackgroundStroke layout seq=%u frame=%llu "
                    L"mode=%ls inset=%u Height=%.2f ActualSize=%.2fx%.2f "
                    L"Margin L%.2f T%.2f R%.2f B%.2f VAlign=%d HAlign=%d StrokeTh=%.2f",
                    seq,
                    static_cast<unsigned long long>(frameHandle),
                    StrokeModeLabel(mode),
                    inset,
                    shape.Height(),
                    shape.ActualWidth(),
                    shape.ActualHeight(),
                    margin.Left,
                    margin.Top,
                    margin.Right,
                    margin.Bottom,
                    static_cast<int>(shape.VerticalAlignment()),
                    static_cast<int>(shape.HorizontalAlignment()),
                    shape.StrokeThickness());
            }
            catch (const winrt::hresult_error& ex)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][BgRegistry] BackgroundStroke layout log failed seq=%u hr=0x%08X",
                    seq,
                    static_cast<unsigned>(ex.code()));
            }
            catch (...)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][BgRegistry] BackgroundStroke layout log failed seq=%u (unknown)",
                    seq);
            }
        }
    } // namespace

    void RedrawShellTraysAfterTaskbarXamlPatch() noexcept
    {
        // Без RedrawWindow патч XAML есть в логе, но кадр трея может не обновиться (см. backup 2026-05-14).
        // При BorderDrawMode=XAML RDW_FRAME после патча может перерисовать DWM NC поверх BackgroundStroke (H2).
        constexpr UINT kRedrawBase =
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW;

        const bool xamlBorder =
            Shared::TrayBorderDrawMode::ReadFromRegistry() == Shared::TrayBorderDrawMode::Mode::Xaml;
        const bool useFrame = !xamlBorder || ReadTrayDebugXamlRedrawWithFrame();

        const UINT kRedraw = useFrame ? (kRedrawBase | RDW_FRAME) : kRedrawBase;

        if (xamlBorder)
        {
            DebugLog(
                DBG_NORMAL,
                L"[TaskbarCompat][BgRegistry] RedrawShellTrays: xaml=1 RDW_FRAME=%u (DebugXamlRedrawWithFrame=%u)",
                useFrame ? 1u : 0u,
                ReadTrayDebugXamlRedrawWithFrame() ? 1u : 0u);
        }

        if (const HWND primary = FindWindowW(L"Shell_TrayWnd", nullptr))
        {
            if (IsWindow(primary))
                RedrawWindow(primary, nullptr, nullptr, kRedraw);
        }

        (void)EnumWindows(
            [](HWND top, LPARAM lParam) -> BOOL {
                if (!IsWindow(top))
                    return TRUE;
                wchar_t cls[72]{};
                if (GetClassNameW(top, cls, static_cast<int>(std::size(cls))) <= 0)
                    return TRUE;
                if (wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0)
                    return TRUE;
                RedrawWindow(top, nullptr, nullptr, static_cast<UINT>(lParam));
                return TRUE;
            },
            static_cast<LPARAM>(kRedraw));
    }

    namespace
    {
        // ARGB DWORD → winrt::Windows::UI::Color (как у popup-политики).
        [[nodiscard]] winrt::Windows::UI::Color ColorFromArgb(DWORD argb) noexcept
        {
            const BYTE a = static_cast<BYTE>((argb >> 24) & 0xFF);
            const BYTE r = static_cast<BYTE>((argb >> 16) & 0xFF);
            const BYTE g = static_cast<BYTE>((argb >> 8) & 0xFF);
            const BYTE b = static_cast<BYTE>((argb)&0xFF);
            return winrt::Windows::UI::Color{ a, r, g, b };
        }

        void LogBackgroundStrokeSystemFill(
            InstanceHandle              frameHandle,
            wux::Media::Brush const&    fill,
            bool                        skipIfMatchesPatchedBorder,
            DWORD                       patchedBorderArgb) noexcept
        {
            const bool darkMode = Shared::ThemeHelper::IsTrayXamlDarkMode();
            try
            {
                if (const auto solid = fill.try_as<wux::Media::SolidColorBrush>())
                {
                    const winrt::Windows::UI::Color c = solid.Color();
                    const DWORD argb =
                        (static_cast<DWORD>(c.A) << 24)
                        | (static_cast<DWORD>(c.R) << 16)
                        | (static_cast<DWORD>(c.G) << 8)
                        | static_cast<DWORD>(c.B);
                    // Уже наш SolidColorBrush после патча — не травить CachedSystemStroke.
                    if (skipIfMatchesPatchedBorder
                        && patchedBorderArgb != 0
                        && argb == patchedBorderArgb)
                    {
                        DebugLog(
                            DBG_WARNING,
                            L"[TaskbarCompat][BgRegistry] skip NoteSystemStroke "
                            L"(matches patched border 0x%08X frame=%llu)",
                            argb,
                            static_cast<unsigned long long>(frameHandle));
                        return;
                    }
                    Registry::NoteSystemStrokeArgb(argb);
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[TaskbarCompat][BgRegistry] BackgroundStroke system Fill=0x%08X "
                        L"(frame=%llu, theme=%ls, brush=SolidColorBrush A=%u R=%u G=%u B=%u)",
                        argb,
                        static_cast<unsigned long long>(frameHandle),
                        darkMode ? L"dark" : L"light",
                        static_cast<unsigned>(c.A),
                        static_cast<unsigned>(c.R),
                        static_cast<unsigned>(c.G),
                        static_cast<unsigned>(c.B));
                    return;
                }

                winrt::hstring typeName;
                if (fill)
                    typeName = winrt::get_class_name(fill);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][BgRegistry] BackgroundStroke system Fill=(non-solid) "
                    L"(frame=%llu, theme=%ls, brushType=%ls)",
                    static_cast<unsigned long long>(frameHandle),
                    darkMode ? L"dark" : L"light",
                    typeName.empty() ? L"null" : typeName.c_str());
            }
            catch (const winrt::hresult_error& ex)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][BgRegistry] BackgroundStroke system Fill log failed hr=0x%08X",
                    static_cast<unsigned>(ex.code()));
            }
            catch (...)
            {
                DebugLog(DBG_WARNING,
                    L"[TaskbarCompat][BgRegistry] BackgroundStroke system Fill log unknown exception");
            }
        }
    } // namespace

    TaskbarBackgroundRegistry::TaskbarBackgroundRegistry(Registry::Settings settings) noexcept
        : m_settings(settings)
    {
        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][BgRegistry] created: patchStroke=%u BorderColor=0x%08X",
            m_settings.patchBorderStroke ? 1u : 0u,
            m_settings.borderColorArgb);
    }

    void TaskbarBackgroundRegistry::DetachBackgroundFillWatcher(TaskbarInfo& info) noexcept
    {
        try
        {
            if (info.background && info.backgroundFillWatcherRegistered)
            {
                info.background.UnregisterPropertyChangedCallback(
                    wux::Shapes::Shape::FillProperty(),
                    info.backgroundFillWatcherToken);
                info.backgroundFillWatcherRegistered = false;
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            info.backgroundFillWatcherRegistered = false;
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] Detach BackgroundFill watcher hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            info.backgroundFillWatcherRegistered = false;
            DebugLog(DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] Detach BackgroundFill watcher unknown exception");
        }
    }

    void TaskbarBackgroundRegistry::DetachBorderFillWatcher(TaskbarInfo& info) noexcept
    {
        try
        {
            if (info.border && info.borderFillWatcherRegistered)
            {
                info.border.UnregisterPropertyChangedCallback(
                    wux::Shapes::Shape::FillProperty(),
                    info.borderFillWatcherToken);
                info.borderFillWatcherRegistered = false;
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            info.borderFillWatcherRegistered = false;
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] Detach BackgroundStroke watcher hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            info.borderFillWatcherRegistered = false;
            DebugLog(DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] Detach BackgroundStroke watcher unknown exception");
        }
    }

    void TaskbarBackgroundRegistry::DetachAllFillWatchers(TaskbarInfo& info) noexcept
    {
        DetachBackgroundFillWatcher(info);
        DetachBorderFillWatcher(info);
    }

    void TaskbarBackgroundRegistry::OnTrackedFillPropertyChanged(
        InstanceHandle frameHandle,
        bool          patchBackgroundNotBorder) noexcept
    {
        try
        {
            const auto it = m_taskbars.find(frameHandle);
            if (it == m_taskbars.end())
                return;
            TaskbarInfo& info = it->second;
            if (info.programmaticFillNesting != 0)
                return;

            ProgrammaticFillScope guard(info);
            if (patchBackgroundNotBorder)
            {
                if (!info.background)
                    return;
                ApplyFillPolicyToFrame(frameHandle, info);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][BgRegistry] FillProperty (BackgroundFill) — reapply policy, frame=%llu",
                    static_cast<unsigned long long>(frameHandle));
            }
            else
            {
                if (!m_settings.patchBorderStroke)
                    return;
                const auto it = m_taskbars.find(frameHandle);
                if (it == m_taskbars.end())
                    return;
                ApplyStrokePolicyToFrame(frameHandle, it->second);
            }
            RedrawShellTraysAfterTaskbarXamlPatch();
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] OnTrackedFillPropertyChanged hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][BgRegistry] OnTrackedFillPropertyChanged unknown exception");
        }
    }

    void TaskbarBackgroundRegistry::RegisterBorderFillWatcher(
        InstanceHandle frameHandle,
        TaskbarInfo&   info) noexcept
    {
        if (!info.border || info.borderFillWatcherRegistered)
            return;

        try
        {
            const InstanceHandle fh = frameHandle;
            info.borderFillWatcherToken =
                info.border.RegisterPropertyChangedCallback(wux::Shapes::Shape::FillProperty(), [
                    registry = this,
                    fh
                ](wux::DependencyObject const&, wux::DependencyProperty const&) {
                    registry->OnTrackedFillPropertyChanged(fh, false);
                });
            info.borderFillWatcherRegistered = true;
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] Register BackgroundStroke watcher failed hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
    }

    void TaskbarBackgroundRegistry::CaptureBorderOriginalLayout(TaskbarInfo& info) noexcept
    {
        if (!info.border || info.borderLayoutCaptured)
            return;

        try
        {
            info.originalMargin = info.border.Margin();
            info.originalHeight = info.border.Height();
            info.originalStroke = info.border.Stroke();
            info.originalStrokeThickness = info.border.StrokeThickness();
            info.originalVerticalAlignment = info.border.VerticalAlignment();
            info.originalHorizontalAlignment = info.border.HorizontalAlignment();
            info.borderLayoutCaptured = true;
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] CaptureBorderOriginalLayout hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][BgRegistry] CaptureBorderOriginalLayout unknown exception");
        }
    }

    void TaskbarBackgroundRegistry::RestoreBorderOriginalLayout(TaskbarInfo& info) noexcept
    {
        if (!info.border || !info.borderLayoutCaptured)
            return;

        try
        {
            ProgrammaticFillScope prog(info);
            info.border.Margin(info.originalMargin);
            info.border.Height(info.originalHeight);
            info.border.Stroke(info.originalStroke);
            info.border.StrokeThickness(info.originalStrokeThickness);
            info.border.VerticalAlignment(info.originalVerticalAlignment);
            info.border.HorizontalAlignment(info.originalHorizontalAlignment);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] RestoreBorderOriginalLayout hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][BgRegistry] RestoreBorderOriginalLayout unknown exception");
        }
    }

    void TaskbarBackgroundRegistry::ApplyStrokePolicyToFrame(
        InstanceHandle frameHandle,
        TaskbarInfo&   info) noexcept
    {
        if (!info.border)
            return;

        // При раздельных зонах глобальный BackgroundStroke скрыт — не перебиваем Visibility.
        if (m_settings.separateZones)
        {
            try
            {
                ProgrammaticFillScope prog(info);
                info.border.Visibility(wux::Visibility::Collapsed);
            }
            catch (...)
            {
            }
            return;
        }

        CaptureBorderOriginalLayout(info);

        const unsigned seq = g_xamlPatchSeq.fetch_add(1u, std::memory_order_relaxed) + 1u;
        LogTrayBorderDiagContext(seq, L"ApplyStrokePolicyToFrame");

        try
        {
            if (!m_settings.patchBorderStroke)
            {
                DetachBorderFillWatcher(info);
                RestoreBorderOriginalLayout(info);
                RestoreFill(info.border, info.originalBorderFill);
                // Снять perimeter, если был.
                if (info.rootGrid && info.rootCornersCaptured)
                {
                    try
                    {
                        info.rootGrid.BorderBrush(info.originalRootBorderBrush);
                        info.rootGrid.BorderThickness(info.originalRootBorderThickness);
                    }
                    catch (...)
                    {
                    }
                }
                return;
            }

            ProgrammaticFillScope prog(info);

            // Политика: CornerType Default (0) → системный верхний stroke (только цвет).
            // CornerType 1–3 (прямоугольник/скругление) → бордюр вокруг на RootGrid, stroke скрыт.
            const bool perimeter =
                m_settings.paintPathXaml && m_settings.cornersPresent && m_settings.cornerType != 0;

            if (perimeter)
            {
                info.border.Visibility(wux::Visibility::Collapsed);
                // Perimeter рисуется в ApplySingleTileCorners (RootGrid.Border*).
                return;
            }

            if (info.border.Visibility() != wux::Visibility::Visible)
                info.border.Visibility(wux::Visibility::Visible);

            RestoreBorderOriginalLayout(info);
            wux::Media::SolidColorBrush brush;
            brush.Color(ColorFromArgb(m_settings.borderColorArgb));
            info.border.Fill(brush);

            LogBackgroundStrokeLayout(frameHandle, info.border, seq, DebugXamlStrokeMode::Fill, 0u);
            RegisterBorderFillWatcher(frameHandle, info);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] ApplyStrokePolicyToFrame hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][BgRegistry] ApplyStrokePolicyToFrame unknown exception");
        }
    }

    void TaskbarBackgroundRegistry::ApplyStrokePolicyToAllFrames() noexcept
    {
        if (m_taskbars.empty())
            return;

        for (auto& [handle, info] : m_taskbars)
            ApplyStrokePolicyToFrame(handle, info);

        LogEffectiveCornerPrefOnTray();
        RedrawShellTraysAfterTaskbarXamlPatch();
    }

    void TaskbarBackgroundRegistry::RegisterBackgroundFill(
        InstanceHandle frameHandle,
        wux::Shapes::Rectangle shape) noexcept
    {
        try
        {
            auto& info = m_taskbars[frameHandle];
            DetachBackgroundFillWatcher(info);

            info.background = shape;

            if (!shape)
                return;

            // Не затирать оригинальный system brush прозрачной заглушкой zones soft-path.
            const auto capturedFill = shape.Fill();
            bool captureAsOriginal = true;
            try
            {
                if (const auto solid = capturedFill.try_as<wux::Media::SolidColorBrush>())
                {
                    if (solid.Color().A < 8)
                    {
                        captureAsOriginal = false;
                        DebugLog(
                            DBG_WARNING,
                            L"[TaskbarCompat][BgRegistry] skip transparent Fill as originalBackgroundFill");
                    }
                }
            }
            catch (...)
            {
            }

            if (captureAsOriginal || !info.originalBackgroundFill)
            {
                if (captureAsOriginal)
                    info.originalBackgroundFill = capturedFill;
            }

            try
            {
                const auto brushForNote =
                    info.originalBackgroundFill ? info.originalBackgroundFill : capturedFill;
                if (const auto solid = brushForNote.try_as<wux::Media::SolidColorBrush>())
                {
                    const winrt::Windows::UI::Color c = solid.Color();
                    const DWORD argb =
                        (static_cast<DWORD>(c.A) << 24)
                        | (static_cast<DWORD>(c.R) << 16)
                        | (static_cast<DWORD>(c.G) << 8)
                        | static_cast<DWORD>(c.B);
                    Registry::NoteSystemFillArgb(argb);
                }
                else if (const auto acrylic = brushForNote.try_as<wux::Media::AcrylicBrush>())
                {
                    const winrt::Windows::UI::Color c = acrylic.TintColor();
                    const DWORD argb =
                        (static_cast<DWORD>(c.A) << 24)
                        | (static_cast<DWORD>(c.R) << 16)
                        | (static_cast<DWORD>(c.G) << 8)
                        | static_cast<DWORD>(c.B);
                    Registry::NoteSystemFillArgb(argb);
                }
                else
                {
                    DebugLog(
                        DBG_IMPORTANT,
                        L"[TaskbarCompat][BgRegistry] BackgroundFill system brush is non-solid");
                }
            }
            catch (...)
            {
            }

            // После capture system fill — tint/defaults могли обновиться без GS reload.
            m_settings = Registry::ReadFromRegistry();

            {
                ProgrammaticFillScope prog(info);
                ApplyFillPolicyToFrame(frameHandle, info);
            }

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][BgRegistry] BackgroundFill patched: frame=%llu paintXaml=%u effect=%u zones=%u",
                static_cast<unsigned long long>(frameHandle),
                m_settings.paintPathXaml ? 1u : 0u,
                m_settings.xamlEffectType,
                m_settings.separateZones ? 1u : 0u);

            try
            {
                const InstanceHandle fh = frameHandle;
                info.backgroundFillWatcherToken =
                    info.background.RegisterPropertyChangedCallback(wux::Shapes::Shape::FillProperty(), [
                        registry = this,
                        fh
                    ](wux::DependencyObject const&, wux::DependencyProperty const&) {
                        registry->OnTrackedFillPropertyChanged(fh, true);
                    });
                info.backgroundFillWatcherRegistered = true;
            }
            catch (const winrt::hresult_error& ex)
            {
                DebugLog(
                    DBG_WARNING,
                    L"[TaskbarCompat][BgRegistry] Register BackgroundFill watcher failed hr=0x%08X msg=%s",
                    static_cast<unsigned>(ex.code()),
                    ex.message().c_str());
            }
            RedrawShellTraysAfterTaskbarXamlPatch();
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][BgRegistry] RegisterBackgroundFill exception: frame=%llu hr=0x%08X msg=%s",
                static_cast<unsigned long long>(frameHandle),
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][BgRegistry] RegisterBackgroundFill unknown exception: frame=%llu",
                static_cast<unsigned long long>(frameHandle));
        }
    }

    void TaskbarBackgroundRegistry::RegisterBackgroundStroke(
        InstanceHandle frameHandle,
        wux::Shapes::Rectangle shape) noexcept
    {
        try
        {
            auto& info = m_taskbars[frameHandle];
            DetachBorderFillWatcher(info);

            info.border = shape;

            if (!shape)
                return;

            info.originalBorderFill = shape.Fill();

            LogBackgroundStrokeSystemFill(
                frameHandle,
                info.originalBorderFill,
                m_settings.patchBorderStroke,
                m_settings.borderColorArgb);

            // Capture sysStroke впервые включает patchBorderStroke — без GS Reload.
            const auto fresh = Registry::ReadFromRegistry();
            const bool strokeSettingsChanged =
                fresh.patchBorderStroke != m_settings.patchBorderStroke
                || fresh.borderColorArgb != m_settings.borderColorArgb;
            if (strokeSettingsChanged)
            {
                UpdateSettings(fresh);
                RedrawShellTraysAfterTaskbarXamlPatch();
                return;
            }

            if (m_settings.patchBorderStroke)
            {
                ApplyStrokePolicyToFrame(frameHandle, info);
                const unsigned seq = g_xamlPatchSeq.load(std::memory_order_relaxed);
                DebugLog(
                    DBG_IMPORTANT,
                    L"[TaskbarCompat][BgRegistry] BackgroundStroke patched: frame=%llu Fill=ARGB 0x%08X seq=%u",
                    static_cast<unsigned long long>(frameHandle),
                    m_settings.borderColorArgb,
                    seq);
            }
            else
            {
                DebugLog(
                    DBG_NORMAL,
                    L"[TaskbarCompat][BgRegistry] BackgroundStroke registered (no patch): frame=%llu",
                    static_cast<unsigned long long>(frameHandle));
            }

            if (!m_settings.patchBorderStroke)
                return;

            RedrawShellTraysAfterTaskbarXamlPatch();
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][BgRegistry] RegisterBackgroundStroke exception: frame=%llu hr=0x%08X msg=%s",
                static_cast<unsigned long long>(frameHandle),
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][BgRegistry] RegisterBackgroundStroke unknown exception: frame=%llu",
                static_cast<unsigned long long>(frameHandle));
        }
    }

    void TaskbarBackgroundRegistry::UpdateSettings(Registry::Settings settings) noexcept
    {
        m_settings = settings;
        DebugLog(
            DBG_IMPORTANT,
            L"[TaskbarCompat][BgRegistry] UpdateSettings: paintXaml=%u effect=%u zones=%u patchStroke=%u BorderColor=0x%08X",
            m_settings.paintPathXaml ? 1u : 0u,
            m_settings.xamlEffectType,
            m_settings.separateZones ? 1u : 0u,
            m_settings.patchBorderStroke ? 1u : 0u,
            m_settings.borderColorArgb);
        ApplyFillPolicyToAllFrames();
        ApplyStrokePolicyToAllFrames();
    }

    void TaskbarBackgroundRegistry::UnregisterFrame(InstanceHandle frameHandle) noexcept
    {
        try
        {
            const auto it = m_taskbars.find(frameHandle);
            if (it == m_taskbars.end())
                return;

            TaskbarInfo& info = it->second;
            DetachAllFillWatchers(info);
            RestoreFrameVisuals(info);
            m_taskbars.erase(it);

            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][BgRegistry] UnregisterFrame: frame=%llu",
                static_cast<unsigned long long>(frameHandle));
        }
        catch (...)
        {
            DebugLog(
                DBG_ERROR,
                L"[TaskbarCompat][BgRegistry] UnregisterFrame exception: frame=%llu",
                static_cast<unsigned long long>(frameHandle));
        }
    }

    void TaskbarBackgroundRegistry::RestoreAll() noexcept
    {
        try
        {
            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][BgRegistry] RestoreAll: count=%zu",
                m_taskbars.size());

            for (auto& [handle, info] : m_taskbars)
            {
                (void)handle;
                DetachAllFillWatchers(info);
                RestoreFrameVisuals(info);
            }

            m_taskbars.clear();
        }
        catch (...)
        {
            DebugLog(DBG_ERROR, L"[TaskbarCompat][BgRegistry] RestoreAll exception");
        }
    }

    void TaskbarBackgroundRegistry::ReapplyPatchedBrushes() noexcept
    {
        try
        {
            if (m_taskbars.empty())
                return;

            ApplyFillPolicyToAllFrames();
            ApplyStrokePolicyToAllFrames();

            DebugLog(
                DBG_NORMAL,
                L"[TaskbarCompat][BgRegistry] ReapplyPatchedBrushes: frames=%zu",
                m_taskbars.size());
            RedrawShellTraysAfterTaskbarXamlPatch();
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] ReapplyPatchedBrushes hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][BgRegistry] ReapplyPatchedBrushes unknown exception");
        }
    }

    void TaskbarBackgroundRegistry::CancelThemeSampleTimer(TaskbarInfo& info) noexcept
    {
        try
        {
            if (info.themeSampleTimer)
            {
                info.themeSampleTimer.Stop();
                info.themeSampleTimer = nullptr;
            }
        }
        catch (...)
        {
            info.themeSampleTimer = nullptr;
        }
        info.themeSampleRetries = 0;
    }

    bool TaskbarBackgroundRegistry::TrySampleOriginalsAfterThemeClear(
        InstanceHandle frameHandle,
        TaskbarInfo& info) noexcept
    {
        // После ClearValue: null / Solid A≈0 — ещё не system brush новой темы.
        auto usableFill = [](wux::Media::Brush const& fill) noexcept -> bool
        {
            if (!fill)
                return false;
            try
            {
                if (const auto solid = fill.try_as<wux::Media::SolidColorBrush>())
                    return solid.Color().A >= 8;
            }
            catch (...)
            {
                return false;
            }
            // Acrylic / composition brush — годится для Stop restore.
            return true;
        };

        bool bgOk = !info.background;
        bool borderOk = !info.border;
        try
        {
            if (info.background)
            {
                const auto fill = info.background.Fill();
                if (usableFill(fill))
                {
                    info.originalBackgroundFill = fill;
                    if (const auto solid = fill.try_as<wux::Media::SolidColorBrush>())
                    {
                        const auto c = solid.Color();
                        const DWORD argb =
                            (static_cast<DWORD>(c.A) << 24)
                            | (static_cast<DWORD>(c.R) << 16)
                            | (static_cast<DWORD>(c.G) << 8)
                            | static_cast<DWORD>(c.B);
                        Registry::NoteSystemFillArgb(argb);
                    }
                    bgOk = true;
                }
            }
            if (info.border)
            {
                const auto fill = info.border.Fill();
                if (usableFill(fill))
                {
                    info.originalBorderFill = fill;
                    LogBackgroundStrokeSystemFill(frameHandle, fill, false, 0);
                    borderOk = true;
                }
            }
        }
        catch (...)
        {
            return false;
        }
        return bgOk && borderOk;
    }

    void TaskbarBackgroundRegistry::ArmThemeSampleRetry(
        InstanceHandle frameHandle,
        TaskbarInfo& info) noexcept
    {
        CancelThemeSampleTimer(info);
        try
        {
            wux::DispatcherTimer timer;
            timer.Interval(std::chrono::milliseconds{ 80 });
            timer.Tick(
                [this, frameHandle](
                    winrt::Windows::Foundation::IInspectable const&,
                    winrt::Windows::Foundation::IInspectable const&) {
                    const auto it = m_taskbars.find(frameHandle);
                    if (it == m_taskbars.end())
                        return;
                    TaskbarInfo& live = it->second;
                    ++live.themeSampleRetries;
                    const unsigned retries = live.themeSampleRetries;
                    const bool sampled =
                        TrySampleOriginalsAfterThemeClear(frameHandle, live);
                    if (sampled || retries >= 8)
                    {
                        CancelThemeSampleTimer(live);
                        if (sampled)
                        {
                            m_settings = Registry::ReadFromRegistry();
                            DebugLog(
                                DBG_IMPORTANT,
                                L"[TaskbarCompat][BgRegistry] theme sample OK "
                                L"frame=%llu retries=%u",
                                static_cast<unsigned long long>(frameHandle),
                                retries);
                        }
                        else
                        {
                            DebugLog(
                                DBG_WARNING,
                                L"[TaskbarCompat][BgRegistry] theme sample give-up "
                                L"frame=%llu (Stop may ClearValue Fill)",
                                static_cast<unsigned long long>(frameHandle));
                        }
                    }
                });
            info.themeSampleTimer = timer;
            timer.Start();
        }
        catch (...)
        {
            info.themeSampleTimer = nullptr;
        }
    }

    void TaskbarBackgroundRegistry::RecaptureAfterThemeChange(Registry::Settings settings) noexcept
    {
        try
        {
            m_settings = settings;
            if (m_taskbars.empty())
                return;

            for (auto& [frameHandle, info] : m_taskbars)
            {
                CancelThemeSampleTimer(info);
                // Снять наш патч, затем ClearValue → кисти новой темы (не stale original*).
                // Если sample fail — не оставлять null: Stop RestoreFill→ClearValue = дыра.
                const auto prevBg = info.originalBackgroundFill;
                const auto prevBorder = info.originalBorderFill;
                RestoreFrameVisuals(info);
                try
                {
                    if (info.background)
                        info.background.ClearValue(wux::Shapes::Shape::FillProperty());
                    if (info.border)
                        info.border.ClearValue(wux::Shapes::Shape::FillProperty());
                    if (info.rootGrid)
                    {
                        info.rootGrid.ClearValue(wux::Controls::Panel::BackgroundProperty());
                        info.rootGrid.ClearValue(wux::Controls::Control::BorderBrushProperty());
                        info.rootGrid.ClearValue(wux::Controls::Control::BorderThicknessProperty());
                        info.rootGrid.ClearValue(wux::Controls::Control::CornerRadiusProperty());
                    }
                }
                catch (...)
                {
                }

                info.originalBackgroundFill = nullptr;
                info.originalBorderFill = nullptr;
                info.originalRootBackground = nullptr;
                info.originalRootBorderBrush = nullptr;
                info.rootCornersCaptured = false;
                info.rootCornersPatched = false;
                info.rootPaddingCaptured = false;
                info.borderLayoutCaptured = false;

                if (!TrySampleOriginalsAfterThemeClear(frameHandle, info))
                {
                    if (!info.originalBackgroundFill && prevBg)
                        info.originalBackgroundFill = prevBg;
                    if (!info.originalBorderFill && prevBorder)
                        info.originalBorderFill = prevBorder;
                    ArmThemeSampleRetry(frameHandle, info);
                }
            }

            // Settings могли обновиться после NoteSystemStroke (default border).
            m_settings = Registry::ReadFromRegistry();
            ApplyFillPolicyToAllFrames();
            ApplyStrokePolicyToAllFrames();
            DebugLog(
                DBG_IMPORTANT,
                L"[TaskbarCompat][BgRegistry] RecaptureAfterThemeChange: frames=%zu border=0x%08X",
                m_taskbars.size(),
                m_settings.borderColorArgb);
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] RecaptureAfterThemeChange hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][BgRegistry] RecaptureAfterThemeChange unknown");
        }
    }

    void TaskbarBackgroundRegistry::CancelZoneShrinkFollowUp(TaskbarInfo& info) noexcept
    {
        try
        {
            if (info.zoneShrinkFollowUpTimer)
            {
                info.zoneShrinkFollowUpTimer.Stop();
                info.zoneShrinkFollowUpTimer = nullptr;
            }
        }
        catch (...)
        {
            info.zoneShrinkFollowUpTimer = nullptr;
        }
    }

    void TaskbarBackgroundRegistry::DetachZoneLayoutHook(TaskbarInfo& info) noexcept
    {
        CancelZoneShrinkFollowUp(info);
        info.lastCenterWidth = -1.0f;
        info.zones.centerShrinkCommit = false;
        info.zones.centerShrinkHoldPending = false;
        try
        {
            info.zoneLayoutRevoker.revoke();
        }
        catch (...)
        {
        }
        info.zoneLayoutRevoker = {};
        info.zoneLayoutHookedGrid = nullptr;
        info.zoneLayoutApplyNesting = false;
    }

    void TaskbarBackgroundRegistry::EnsureZoneLayoutHook(
        InstanceHandle frameHandle,
        TaskbarInfo& info) noexcept
    {
        if (!m_settings.separateZones || !info.zones.rootGrid)
        {
            DetachZoneLayoutHook(info);
            return;
        }
        if (info.zoneLayoutRevoker)
        {
            if (info.zoneLayoutHookedGrid == info.zones.rootGrid)
                return;
            // RootGrid пересоздан — снять подписку со старого, иначе она живёт
            // до erase записи, а Apply идёт уже по новому дереву.
            DetachZoneLayoutHook(info);
        }

        try
        {
            info.zoneLayoutRevoker = info.zones.rootGrid.LayoutUpdated(
                winrt::auto_revoke,
                [this, frameHandle](
                    winrt::Windows::Foundation::IInspectable const&,
                    winrt::Windows::Foundation::IInspectable const&) {
                    OnZoneLayoutUpdated(frameHandle);
                });
            info.zoneLayoutHookedGrid = info.zones.rootGrid;
        }
        catch (const winrt::hresult_error&)
        {
            info.zoneLayoutRevoker = {};
            info.zoneLayoutHookedGrid = nullptr;
        }
        catch (...)
        {
            info.zoneLayoutRevoker = {};
            info.zoneLayoutHookedGrid = nullptr;
        }
    }

    void TaskbarBackgroundRegistry::ApplyZonesForFrame(
        InstanceHandle /*frameHandle*/,
        TaskbarInfo& info) noexcept
    {
        if (info.zoneLayoutApplyNesting || !info.zones.rootGrid)
            return;

        // Синхронно: иначе поиск on/off даёт «задержку, потом прыжок».
        info.zoneLayoutApplyNesting = true;
        try
        {
            wux::Media::Brush brush{ nullptr };
            if (info.zones.frameAnimRunning)
            {
                try
                {
                    if (info.zones.center.element)
                        brush = info.zones.center.element.Background();
                    else if (info.zones.tray.element)
                        brush = info.zones.tray.element.Background();
                }
                catch (...)
                {
                }
            }
            if (!brush)
                brush = MakeActiveBackdropBrush(info);
            if (brush)
            {
                Zones::Apply(
                    info.background,
                    info.border,
                    info.zones,
                    m_settings,
                    brush);
            }
        }
        catch (...)
        {
        }
        info.zoneLayoutApplyNesting = false;
    }

    void TaskbarBackgroundRegistry::ArmZoneShrinkSettle(
        InstanceHandle frameHandle,
        TaskbarInfo& info) noexcept
    {
        CancelZoneShrinkFollowUp(info);
        try
        {
            wux::DispatcherTimer timer;
            // Debounce: commit после паузы LayoutUpdated (без chase mid-anim).
            timer.Interval(std::chrono::milliseconds{ 200 });
            timer.Tick(
                [this, frameHandle](
                    winrt::Windows::Foundation::IInspectable const&,
                    winrt::Windows::Foundation::IInspectable const&) {
                    const auto it = m_taskbars.find(frameHandle);
                    if (it == m_taskbars.end())
                        return;
                    TaskbarInfo& live = it->second;
                    CancelZoneShrinkFollowUp(live);
                    if (!m_settings.separateZones || !live.zones.rootGrid)
                        return;
                    live.zones.centerShrinkCommit = true;
                    live.zones.lastBoundsSig = 0;
                    ApplyZonesForFrame(frameHandle, live);
                    live.zones.centerShrinkCommit = false;
                    live.zones.centerShrinkHoldPending = false;
                    if (live.zones.center.hasVisual)
                        live.lastCenterWidth = live.zones.center.visual.Width;
                });
            info.zoneShrinkFollowUpTimer = timer;
            timer.Start();
        }
        catch (...)
        {
            info.zoneShrinkFollowUpTimer = nullptr;
        }
    }

    void TaskbarBackgroundRegistry::OnZoneLayoutUpdated(InstanceHandle frameHandle) noexcept
    {
        if (!m_settings.separateZones)
            return;

        const auto it = m_taskbars.find(frameHandle);
        if (it == m_taskbars.end())
            return;

        TaskbarInfo& info = it->second;
        if (info.zoneLayoutApplyNesting || !info.zones.rootGrid)
            return;

        ApplyZonesForFrame(frameHandle, info);

        const float newW =
            (info.zones.center.active && info.zones.center.hasVisual)
                ? info.zones.center.visual.Width
                : -1.0f;
        // Hold: переармировать settle на каждый mid-anim shrink (без chase Apply).
        if (info.zones.centerShrinkHoldPending)
            ArmZoneShrinkSettle(frameHandle, info);

        info.lastCenterWidth = newW;
    }

    void TaskbarBackgroundRegistry::RestoreFrameVisuals(TaskbarInfo& info) noexcept
    {
        CancelThemeSampleTimer(info);
        DetachZoneLayoutHook(info);
        Zones::Restore(info.background, info.border, info.zones);
        RestoreSingleTileCorners(info);
        RestoreBorderOriginalLayout(info);
        RestoreFill(info.background, info.originalBackgroundFill);
        RestoreFill(info.border, info.originalBorderFill);
        try
        {
            if (info.background)
                info.background.Visibility(wux::Visibility::Visible);
            if (info.border)
                info.border.Visibility(wux::Visibility::Visible);
        }
        catch (...)
        {
        }
    }

    wux::Media::Brush TaskbarBackgroundRegistry::MakeActiveBackdropBrush(TaskbarInfo& info) noexcept
    {
        if (!info.background)
            return nullptr;
        if (!m_settings.paintPathXaml)
        {
            wux::Media::SolidColorBrush brush;
            brush.Color(winrt::Windows::UI::Colors::Transparent());
            return brush;
        }
        if (m_settings.xamlEffectType == 0)
            return nullptr;

        // Только EffectType=System (5) — оригинальный бэкдроп острова.
        // Blur/Acrylic: factory; тинт без GradientColor — clamped system fill / default (не A=0xFF).
        const bool useSystemBrush = m_settings.xamlEffectType == 5;
        if (useSystemBrush)
        {
            return info.originalBackgroundFill;
        }

        const auto brush =
            CreateBackdropBrush(info.background, m_settings.xamlEffectType, m_settings.tintArgb);
        return brush;
    }

    void TaskbarBackgroundRegistry::ApplySingleTileCorners(TaskbarInfo& info) noexcept
    {
        // BackgroundFill.RadiusX/Y на Taskbar.dll → AV (put_RadiusY). Скругление — через RootGrid.
        if (!info.background || !m_settings.paintPathXaml || !m_settings.cornersPresent
            || m_settings.separateZones)
        {
            return;
        }
        try
        {
            namespace wuxc = winrt::Windows::UI::Xaml::Controls;
            namespace wuxm = winrt::Windows::UI::Xaml::Media;

            if (!info.rootGrid)
            {
                wux::FrameworkElement cur = info.background;
                while (cur)
                {
                    if (cur.Name() == L"RootGrid")
                    {
                        info.rootGrid = cur.try_as<wuxc::Grid>();
                        break;
                    }
                    cur = wuxm::VisualTreeHelper::GetParent(cur).try_as<wux::FrameworkElement>();
                }
            }
            if (!info.rootGrid)
            {
                return;
            }

            if (!info.rootCornersCaptured)
            {
                info.originalRootBackground = info.rootGrid.Background();
                info.originalRootCornerRadius = info.rootGrid.CornerRadius();
                try
                {
                    info.originalRootBorderBrush = info.rootGrid.BorderBrush();
                    info.originalRootBorderThickness = info.rootGrid.BorderThickness();
                }
                catch (...)
                {
                    info.originalRootBorderBrush = nullptr;
                    info.originalRootBorderThickness = {};
                }
                info.rootCornersCaptured = true;
            }
            if (!info.rootPaddingCaptured)
            {
                try
                {
                    info.originalRootPadding = info.rootGrid.Padding();
                }
                catch (...)
                {
                    info.originalRootPadding = {};
                }
                info.rootPaddingCaptured = true;
            }

            const double panelH = info.rootGrid.ActualHeight() > 1.0
                ? info.rootGrid.ActualHeight()
                : (info.background && info.background.ActualHeight() > 1.0
                       ? info.background.ActualHeight()
                       : 48.0);
            const double r = Zones::CornerRadiusDip(m_settings.cornerType, panelH);
            const auto brush = MakeActiveBackdropBrush(info);
            if (brush)
                info.rootGrid.Background(brush);
            info.rootGrid.CornerRadius(wux::CornerRadius{ r, r, r, r });

            // MaxRound: CornerRadius клипит детей — чуть увеличить боковые Padding.
            if (m_settings.cornerType == Shared::TrayXamlPaint::kCornerTypeMaxRoundXaml)
            {
                const double side = (std::max)(8.0, r * 0.28);
                info.rootGrid.Padding(wux::Thickness{
                    side,
                    info.originalRootPadding.Top,
                    side,
                    info.originalRootPadding.Bottom });
            }
            else if (info.rootPaddingCaptured)
            {
                info.rootGrid.Padding(info.originalRootPadding);
            }

            // Perimeter-бордюр при CornerType ≠ Default (скругление / прямоугольник).
            const bool perimeter =
                m_settings.patchBorderStroke && m_settings.cornerType != 0;
            if (perimeter)
            {
                wux::Media::SolidColorBrush borderBrush;
                borderBrush.Color(ColorFromArgb(m_settings.borderColorArgb));
                info.rootGrid.BorderBrush(borderBrush);
                info.rootGrid.BorderThickness(wux::Thickness{ 1.0, 1.0, 1.0, 1.0 });
            }
            else
            {
                info.rootGrid.BorderBrush(info.originalRootBorderBrush);
                info.rootGrid.BorderThickness(info.originalRootBorderThickness);
            }

            info.rootCornersPatched = true;

            // Fill остаётся transparent (уже выставлен), чтобы не двоить фон.
        }
        catch (const winrt::hresult_error&)
        {
        }
        catch (...)
        {
        }
    }

    void TaskbarBackgroundRegistry::RestoreSingleTileCorners(TaskbarInfo& info) noexcept
    {
        if (!info.rootGrid || !info.rootCornersCaptured || !info.rootCornersPatched)
        {
            info.rootCornersPatched = false;
            return;
        }
        try
        {
            info.rootGrid.Background(info.originalRootBackground);
            info.rootGrid.CornerRadius(info.originalRootCornerRadius);
            info.rootGrid.BorderBrush(info.originalRootBorderBrush);
            info.rootGrid.BorderThickness(info.originalRootBorderThickness);
            if (info.rootPaddingCaptured)
                info.rootGrid.Padding(info.originalRootPadding);
        }
        catch (...)
        {
        }
        info.rootCornersPatched = false;
        // rootCornersCaptured оставляем — originals валидны до Unregister.
    }

    void TaskbarBackgroundRegistry::ApplyFillPolicyToFrame(
        InstanceHandle frameHandle,
        TaskbarInfo& info) noexcept
    {
        (void)frameHandle;
        if (!info.background)
            return;

        try
        {
            const bool wantZones = m_settings.paintPathXaml && m_settings.separateZones;
            const bool zonesLive =
                info.zones.left.active || info.zones.center.active || info.zones.tray.active;

            // Уже в зонах: НЕ Restore — иначе visual сбрасывается и lerp становится snap
            // (лог Z4: retarget vx==tx, settled за ~15ms).
            if (wantZones && zonesLive)
            {

                if (m_settings.xamlEffectType == 0)
                {
                    RestoreFill(info.background, info.originalBackgroundFill);
                    return;
                }

                wux::Media::Brush brush{ nullptr };
                if (info.zones.frameAnimRunning)
                {
                    try
                    {
                        if (info.zones.center.element)
                            brush = info.zones.center.element.Background();
                        else if (info.zones.tray.element)
                            brush = info.zones.tray.element.Background();
                    }
                    catch (...)
                    {
                    }
                }
                if (!brush)
                    brush = MakeActiveBackdropBrush(info);
                if (!brush)
                {
                    RestoreFill(info.background, info.originalBackgroundFill);
                    return;
                }

                {
                    wux::Media::SolidColorBrush transparent;
                    transparent.Color(winrt::Windows::UI::Colors::Transparent());
                    info.background.Fill(transparent);
                }
                Zones::Apply(info.background, info.border, info.zones, m_settings, brush);
                if (!info.zones.left.active && !info.zones.center.active && !info.zones.tray.active)
                {
                    DetachZoneLayoutHook(info);
                    Zones::Restore(info.background, info.border, info.zones);
                    info.background.Fill(brush);
                    ApplySingleTileCorners(info);
                }
                else
                {
                    EnsureZoneLayoutHook(frameHandle, info);
                }
                return;
            }

            // Смена режима / первый вход — полный tear-down.
            Zones::Restore(info.background, info.border, info.zones);
            RestoreSingleTileCorners(info);

            // После выхода из зон — Fill обязан быть Visible, single-tile chrome переприменится ниже.
            if (!m_settings.separateZones)
            {
                try
                {
                    if (info.background)
                        info.background.Visibility(wux::Visibility::Visible);
                }
                catch (...)
                {
                }
            }

            if (m_settings.paintPathXaml && m_settings.xamlEffectType == 0)
            {
                RestoreFill(info.background, info.originalBackgroundFill);
                return;
            }

            const auto brush = MakeActiveBackdropBrush(info);
            if (!m_settings.paintPathXaml)
            {
                // DWM/SWCA: RootGrid/зоны сняты выше; Fill transparent → виден SWCA.
                if (info.background.Visibility() != wux::Visibility::Visible)
                    info.background.Visibility(wux::Visibility::Visible);
                if (brush)
                    info.background.Fill(brush);
                else
                    RestoreFill(info.background, info.originalBackgroundFill);
                return;
            }

            if (!brush)
            {
                RestoreFill(info.background, info.originalBackgroundFill);
                return;
            }

            if (wantZones)
            {
                // Windhawk: полный BackgroundFill под зонами не оставляем (только Collapsed/transparent).
                {
                    wux::Media::SolidColorBrush transparent;
                    transparent.Color(winrt::Windows::UI::Colors::Transparent());
                    info.background.Fill(transparent);
                }
                Zones::Apply(info.background, info.border, info.zones, m_settings, brush);
                // Если зоны не нашлись — Zones::Apply вернул fill visible; красим single-tile.
                if (!info.zones.left.active && !info.zones.center.active && !info.zones.tray.active)
                {
                    DetachZoneLayoutHook(info);
                    info.background.Fill(brush);
                    ApplySingleTileCorners(info);
                }
                else
                {
                    EnsureZoneLayoutHook(frameHandle, info);
                }
                return;
            }

            DetachZoneLayoutHook(info);

            if (info.background.Visibility() != wux::Visibility::Visible)
                info.background.Visibility(wux::Visibility::Visible);

            // С углами: кисть+CornerRadius на RootGrid, Fill transparent (без RadiusX/Y).
            if (m_settings.paintPathXaml && m_settings.cornersPresent)
            {
                wux::Media::SolidColorBrush transparent;
                transparent.Color(winrt::Windows::UI::Colors::Transparent());
                info.background.Fill(transparent);
                ApplySingleTileCorners(info);
            }
            else
            {
                RestoreSingleTileCorners(info);
                info.background.Fill(brush);
            }
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] ApplyFillPolicyToFrame hr=0x%08X",
                static_cast<unsigned>(ex.code()));
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][BgRegistry] ApplyFillPolicyToFrame unknown exception");
        }
    }

    void TaskbarBackgroundRegistry::ApplyFillPolicyToAllFrames() noexcept
    {
        for (auto& [handle, info] : m_taskbars)
        {
            ProgrammaticFillScope guard(info);
            ApplyFillPolicyToFrame(handle, info);
        }
        RedrawShellTraysAfterTaskbarXamlPatch();
    }

    void TaskbarBackgroundRegistry::RestoreFill(
        wux::Shapes::Rectangle& shape,
        wux::Media::Brush& originalFill) noexcept
    {
        try
        {
            if (!shape)
                return;

            if (originalFill)
            {
                shape.Fill(originalFill);
                return;
            }

            // DWM-режим: original часто не захвачен (прозрачная заглушка).
            // ClearValue → system XAML-backdrop (Stop / restore-путь).
            shape.ClearValue(wux::Shapes::Shape::FillProperty());
        }
        catch (const winrt::hresult_error& ex)
        {
            DebugLog(
                DBG_WARNING,
                L"[TaskbarCompat][BgRegistry] RestoreFill exception: hr=0x%08X msg=%s",
                static_cast<unsigned>(ex.code()),
                ex.message().c_str());
        }
        catch (...)
        {
            DebugLog(DBG_WARNING, L"[TaskbarCompat][BgRegistry] RestoreFill unknown exception");
        }
    }
}
