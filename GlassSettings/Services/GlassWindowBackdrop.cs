using GlassSettings.Core.Registry;
using Microsoft.UI.Composition;
using Microsoft.UI.Composition.SystemBackdrops;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Services;

/// <summary>
/// Единственный backdrop окна GlassSettings: Mica / MicaAlt / Desktop Acrylic поверх одного объекта.
///
/// Контракт (см. <c>GlassSettings/docs/GlassSettings-startup-window-plan.md</c>, Э1):
/// 1. объект присваивается <c>Window.SystemBackdrop</c> ровно один раз и до <c>Activate()</c>:
///    WinUI подключает цель при готовке острова, поэтому материал попадает в первый кадр.
///    Переприсваивание (XamlIsland::UpdateSystemBackdrop) рвёт связь с целью и выключает
///    прозрачность острова — отсюда «opaque→acrylic→opaque» в журналах старта;
/// 2. смена режима — <see cref="TryApplyMode"/>: пересоздаётся только контроллер внутри
///    уже подключённой цели;
/// 3. <c>Window.SystemBackdrop</c> не обнуляется, пока материал поддерживается (null вернул
///    бы остров непрозрачным);
/// 4. <see cref="MicaController.FallbackColor"/> задаётся явно: состояние Fallback (окно
///    неактивно, «эффекты прозрачности» выключены, Mica недоступна) не должно выглядеть серым дефектом.
///
/// Контроллер принадлежит только этому объекту и закрывается (<c>Dispose</c>) при отключении
/// цели либо при смене материала — как в деструкторе C++-аналога WinUI
/// (<c>RemoveSystemBackdropTarget</c> + <c>Close</c>).
/// </summary>
public sealed class GlassWindowBackdrop : SystemBackdrop
{
    private MicaController? _mica;
    private DesktopAcrylicController? _acrylic;
    private ICompositionSupportsSystemBackdrop? _target;
    private SystemBackdropConfiguration? _configuration;
    private AppWindowBackdropMode _mode;

    public GlassWindowBackdrop(AppWindowBackdropMode mode) => _mode = mode;

    public AppWindowBackdropMode Mode => _mode;

    /// <summary>Есть ли на этой ОС системный материал вообще (Mica или Desktop Acrylic).</summary>
    /// <remarks>
    /// На Win10 10.0.17763 нет ни того, ни другого: там окно сознательно получает непрозрачный
    /// solid-фон (см. <see cref="WindowBackdropService"/>), а не «дырку» в desktop.
    /// </remarks>
    public static bool IsAnyMaterialSupported()
    {
        try
        {
            return MicaController.IsSupported() || DesktopAcrylicController.IsSupported();
        }
        catch
        {
            return false;
        }
    }

    protected override void OnTargetConnected(ICompositionSupportsSystemBackdrop connectedTarget, XamlRoot xamlRoot)
    {
        base.OnTargetConnected(connectedTarget, xamlRoot);

        _target = connectedTarget;
        // Theme / IsInputActive / IsHighContrast ведёт фреймворк — конфигурация не наша.
        _configuration = GetDefaultSystemBackdropConfiguration(connectedTarget, xamlRoot);
        RebuildController();
        // Зонд Э7: материал активен с этого момента; если метка идёт позже first-present, кадр
        // может быть ещё без него (это и есть «стартует без акрила»).
        StartupPhaseTrace.Mark($"backdrop:target-connected {DescribeController()}");
    }

    protected override void OnTargetDisconnected(ICompositionSupportsSystemBackdrop disconnectedTarget)
    {
        base.OnTargetDisconnected(disconnectedTarget);

        ReleaseController();
        _target = null;
        _configuration = null;
    }

    protected override void OnDefaultSystemBackdropConfigurationChanged(ICompositionSupportsSystemBackdrop target, XamlRoot xamlRoot)
    {
        base.OnDefaultSystemBackdropConfigurationChanged(target, xamlRoot);

        // Явный FallbackColor отключает автоматическую смену тона контроллером по теме
        // (docs/api-specs/XamlBackdrop/xaml-backdrop-api.md) — синхронизируем вручную.
        ApplyThemeColors();
    }

    /// <summary>
    /// Пересчитать Fallback/Tint по текущей теме приложения. Фреймворк сам зовёт
    /// <see cref="OnDefaultSystemBackdropConfigurationChanged"/> при смене темы XamlRoot, но
    /// принудительная тема из реестра GlassSettings может не изменить «системную» конфигурацию —
    /// вызываем после RootGrid.ActualThemeChanged.
    /// </summary>
    public void RefreshThemeColors() => ApplyThemeColors();

    /// <summary>
    /// Сменить материал, не трогая <c>Window.SystemBackdrop</c>. false — ничего делать не
    /// нужно (режим тот же и контроллер жив уже или цель ещё не подключена).
    /// </summary>
    public bool TryApplyMode(AppWindowBackdropMode mode)
    {
        if (_mode == mode && (_mica is not null || _acrylic is not null || _target is null))
            return false;

        _mode = mode;
        RebuildController();
        StartupPhaseTrace.Mark($"backdrop:mode={mode} {DescribeController()}");
        return true;
    }

    private string DescribeController() =>
        _mica is not null ? ($"mica kind={_mica.Kind}")
        : _acrylic is not null ? "acrylic"
        : "none(solid)";

    private void RebuildController()
    {
        ReleaseController();

        if (_target is null || _configuration is null)
            return;

        // System → Mica, если доступен, иначе Desktop Acrylic (паритет прежнего WindowBackdropService).
        if (_mode is not AppWindowBackdropMode.Acrylic && SafeMicaSupported())
        {
            var mica = new MicaController
            {
                Kind = _mode == AppWindowBackdropMode.MicaAlt ? MicaKind.BaseAlt : MicaKind.Base
            };
            mica.AddSystemBackdropTarget(_target);
            mica.SetSystemBackdropConfiguration(_configuration);
            _mica = mica;
        }
        else if (SafeAcrylicSupported())
        {
            var acrylic = new DesktopAcrylicController();
            acrylic.AddSystemBackdropTarget(_target);
            acrylic.SetSystemBackdropConfiguration(_configuration);
            _acrylic = acrylic;
        }

        ApplyThemeColors();
    }

    private void ApplyThemeColors()
    {
        // Тон SolidBackgroundFillColorBase: Fallback совпадает с подложкой shell → не виден как артефакт.
        var dark = AppThemeVisualResolver.ResolveIsDarkMode();
        var fallback = dark
            ? Color.FromArgb(255, 0x20, 0x20, 0x20)
            : Color.FromArgb(255, 0xF3, 0xF3, 0xF3);

        try
        {
            if (_mica is not null)
                _mica.FallbackColor = fallback;
            if (_acrylic is not null)
                _acrylic.FallbackColor = fallback;
        }
        catch
        {
            /* кастомизация материала не влияет на корректность backdrop */
        }
    }

    private void ReleaseController()
    {
        DisposeController(_mica);
        _mica = null;
        DisposeController(_acrylic);
        _acrylic = null;
    }

    private static void DisposeController(object? controller)
    {
        if (controller is null)
            return;

        try
        {
            (controller as IDisposable)?.Dispose();
        }
        catch
        {
            /* teardown best-effort */
        }
    }

    private static bool SafeMicaSupported()
    {
        try
        {
            return MicaController.IsSupported();
        }
        catch
        {
            return false;
        }
    }

    private static bool SafeAcrylicSupported()
    {
        try
        {
            return DesktopAcrylicController.IsSupported();
        }
        catch
        {
            return false;
        }
    }
}
