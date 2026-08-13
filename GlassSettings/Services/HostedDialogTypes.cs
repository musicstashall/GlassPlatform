using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Services;

/// <summary>Результат закрытия hosted-диалога.</summary>
public enum HostedDialogResult
{
    Dismissed,
    Primary
}

/// <summary>Спецификация hosted-диалога (штора + панель по центру окна).</summary>
public sealed class HostedDialogSpec
{
    public required string Title { get; init; }

    public required UIElement Body { get; init; }

    public HostedDialogFooterSpec? Footer { get; init; }

    public double MinWidth { get; init; } = 480;

    public double MaxWidth { get; init; } = 560;

    public double? MaxHeight { get; init; }

    /// <summary>Если false — шторой управляет вызывающий код (Push до ShowAsync, Pop после).</summary>
    public bool ManageOverlay { get; init; } = true;

    /// <summary>Primary не закрывает диалог автоматически — обработчик в configure.</summary>
    public bool DeferPrimaryCloseToHost { get; init; }

    /// <summary>Body сам скроллит контент (TreeView/ListView); внешний ScrollViewer без вертикальной полосы.</summary>
    public bool BodyHandlesScroll { get; init; }

    /// <summary>Разделитель footer без горизонтальных inset (edge-to-edge на chrome grid).</summary>
    public bool FooterDividerFullWidth { get; init; }

    /// <summary>Отступ над разделителем footer (между body и линией).</summary>
    public double FooterDividerTopMargin { get; init; } = 8;

    /// <summary>
    /// Нижний margin у центрированной панели. При VerticalAlignment=Center
    /// поднимает диалог примерно на половину значения по Y.
    /// </summary>
    public double CenterBottomMargin { get; init; }
}

/// <summary>Body hosted-диалога с адаптивной разметкой по ширине shell (паттерн спойлеров).</summary>
public interface IHostedDialogAdaptiveBody
{
    void OnHostedDialogBoundsChanged(double width, double height);

    /// <summary>Переопределение MinWidth shell; null — из <see cref="HostedDialogSpec.MinWidth"/>.</summary>
    double? GetHostedDialogMinWidth() => null;
}

/// <summary>Нижняя кнопка действия hosted-диалога (без «Закрыть» — только крестик).</summary>
public sealed class HostedDialogFooterSpec
{
    public string? SecondaryButtonText { get; init; }

    public double SecondaryButtonWidth { get; init; } = 88;

    public required string PrimaryButtonText { get; init; }

    public double PrimaryButtonWidth { get; init; } = 88;

    /// <summary>Интервал между кнопками footer (px).</summary>
    public double ButtonSpacing { get; init; } = 12;

    public bool IsPrimaryEnabled { get; init; } = true;
}

/// <summary>Контекст настройки hosted-диалога после построения UI.</summary>
public sealed class HostedDialogHostContext
{
    internal HostedDialogHostContext(
        Button primaryButton,
        Grid? footerRow,
        Action refreshBounds,
        Action<HostedDialogResult> close,
        Func<HostedDialogSpec, Task<HostedDialogResult>>? showChildDialog = null)
    {
        PrimaryButton = primaryButton;
        FooterRow = footerRow;
        _refreshBounds = refreshBounds;
        _close = close;
        _showChildDialog = showChildDialog;
    }

    public Button PrimaryButton { get; }

    /// <summary>Горизонтальная полоса footer (32px): col0 — combo, col1 — кнопки справа.</summary>
    public Grid? FooterRow { get; }

    private readonly Action _refreshBounds;
    private readonly Action<HostedDialogResult> _close;
    private readonly Func<HostedDialogSpec, Task<HostedDialogResult>>? _showChildDialog;

    public void SetPrimaryEnabled(bool enabled) => PrimaryButton.IsEnabled = enabled;

    public void RequestBoundsRefresh() => _refreshBounds();

    public Task<HostedDialogResult> ShowChildDialogAsync(HostedDialogSpec spec) =>
        _showChildDialog?.Invoke(spec) ?? Task.FromResult(HostedDialogResult.Dismissed);

    public void ClosePrimary() => _close(HostedDialogResult.Primary);

    public void CloseDismissed() => _close(HostedDialogResult.Dismissed);
}

/// <summary>Общие правила layout hosted-диалога (padding body, edge-to-edge).</summary>
public static class HostedDialogLayout
{
    /// <summary>Горизонтальный margin из <c>ContentDialogPadding</c>.</summary>
    public static Thickness GetContentDialogPadding() =>
        Application.Current.Resources["ContentDialogPadding"] as Thickness? ?? new Thickness(24);

    /// <summary>Grid-обёртка с горизонтальным margin для body обычного диалога.</summary>
    public static Grid WrapBodyWithContentPadding(UIElement body)
    {
        ApplyBodyStretch(body);
        var pad = GetContentDialogPadding();
        var host = new Grid
        {
            Margin = new Thickness(pad.Left, 0, pad.Right, 0),
            HorizontalAlignment = HorizontalAlignment.Stretch
        };
        host.Children.Add(body);
        return host;
    }

    /// <summary>Растянуть body по ширине (и высоте при star-layout) внутри hosted-диалога.</summary>
    public static void ApplyBodyStretch(UIElement body)
    {
        if (body is not FrameworkElement element)
            return;

        element.HorizontalAlignment = HorizontalAlignment.Stretch;
        if (element is Panel or UserControl or ContentControl)
            element.VerticalAlignment = VerticalAlignment.Stretch;
    }
}
