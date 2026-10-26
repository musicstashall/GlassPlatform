using GlassSettings.Core.Platform;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Windows.Foundation;

namespace GlassSettings.Controls;

/// <summary>
/// Split — HeaderActions (Spacing 16, эталон зазора).
/// Gate — TrailingContent, как у OpenGlass: тумблер и шеврон в одной рейке (16 px).
/// Шаг тумблеров подгоняется под шаг кнопок, чтобы строки совпали.
/// </summary>
public sealed partial class SystemDualGlassPlatformControls : UserControl
{
    private readonly StackPanel _trailingGates;
    private readonly ContentPresenter _gateX86Host;
    private readonly ContentPresenter _gateX64Host;

    public SystemDualGlassPlatformControls()
    {
        InitializeComponent();

        _trailingGates = new StackPanel
        {
            Spacing = 16,
            VerticalAlignment = VerticalAlignment.Center,
            HorizontalAlignment = HorizontalAlignment.Right
        };
        _gateX86Host = new ContentPresenter { VerticalAlignment = VerticalAlignment.Center };
        _gateX64Host = new ContentPresenter { VerticalAlignment = VerticalAlignment.Center };
        _trailingGates.Children.Add(_gateX86Host);
        _trailingGates.Children.Add(_gateX64Host);

        RowX86 = new PlatformSchedulerRow
        {
            Kind = PlatformSchedulerKind.Glass,
            GlassArch = GlassPlatformArch.X86
        };
        RowX64 = new PlatformSchedulerRow
        {
            Kind = PlatformSchedulerKind.Glass,
            GlassArch = GlassPlatformArch.X64
        };

        RowX86.AttachToDualControlRow(ActionsX86Host, _gateX86Host);
        RowX64.AttachToDualControlRow(ActionsX64Host, _gateX64Host);

        RowX86.StateChanged += (_, _) => StateChanged?.Invoke(this, EventArgs.Empty);
        RowX64.StateChanged += (_, _) => StateChanged?.Invoke(this, EventArgs.Empty);

        Loaded += (_, _) => SyncTogglePitchToButtons();
        ActionsStack.SizeChanged += (_, _) => SyncTogglePitchToButtons();
    }

    private bool IsAnyInstallGateBusy =>
        RowX86.IsInstallGateBusy || RowX64.IsInstallGateBusy;

    public PlatformSchedulerRow RowX86 { get; }

    public PlatformSchedulerRow RowX64 { get; }

    public UIElement TrailingGates => _trailingGates;

    public event EventHandler? StateChanged;

    /// <summary>pitch кнопок (высота + Spacing) = pitch тумблеров.</summary>
    private void SyncTogglePitchToButtons()
    {
        if (IsAnyInstallGateBusy)
            return;

        ActionsX86Host.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        _gateX86Host.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));

        var buttonH = ActionsX86Host.DesiredSize.Height;
        var gateH = _gateX86Host.DesiredSize.Height;
        if (buttonH <= 0 || gateH <= 0)
            return;

        var spacing = buttonH + ActionsStack.Spacing - gateH;
        if (spacing < 0)
            spacing = 0;

        if (Math.Abs(_trailingGates.Spacing - spacing) > 0.5)
            _trailingGates.Spacing = spacing;
    }
}
