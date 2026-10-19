using GlassSettings.Core.Platform;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;

namespace GlassSettings.Controls;

public sealed partial class HomeDualGlassPlatformRow : UserControl
{
    private bool _suppress;
    private bool _busyX86;
    private bool _busyX64;
    private GlassSettingsLabeledToggleSwitch? _gateX86;
    private GlassSettingsLabeledToggleSwitch? _gateX64;
    private string? _refreshFingerprint;

    public HomeDualGlassPlatformRow()
    {
        InitializeComponent();
        TitleX86.Text = GlassSettingsResources.Get("Home_RowGlassPlatformX86");
        TitleX64.Text = GlassSettingsResources.Get("Home_RowGlassPlatformX64");
        WireGates();
        Refresh();
    }

    public event EventHandler? NavigateRequested;

    public bool ShowDivider
    {
        get => Divider.Visibility == Visibility.Visible;
        set => Divider.Visibility = value ? Visibility.Visible : Visibility.Collapsed;
    }

    public void Refresh()
    {
        if (_suppress)
            return;

        // Один snapshot на arch до записи — fingerprint отсекает idle poll без thrash brush/DP.
        var x86Locked = GlassPlatformUiPolicy.IsX86DualUiControlsLocked();
        var uiX86 = HomeStatusSummary.QueryGlassPlatformUi(GlassPlatformArch.X86);
        var uiX64 = HomeStatusSummary.QueryGlassPlatformUi(GlassPlatformArch.X64);

        // Detect external lifecycle busy (menu start/stop) — preserve gate ON during startup only.
        var externalBusyX86 = uiX86.LifecycleBusy;
        var externalBusyX64 = uiX64.LifecycleBusy;
        var effectiveBusyX86 = _busyX86 || externalBusyX86;
        var effectiveBusyX64 = _busyX64 || externalBusyX64;

        // Старт: тумблер остаётся вкл, пока mutex ещё не Running.
        // Стоп: сразу выкл (Stopping/Residual или StopEvent).
        var preserveGateOnX86 = GlassPlatformService.IsStartInFlight(GlassPlatformArch.X86, uiX86.Life);
        var preserveGateOnX64 = GlassPlatformService.IsStartInFlight(GlassPlatformArch.X64, uiX64.Life);

        var fingerprint = BuildFingerprint(
            uiX86, uiX64, effectiveBusyX86, effectiveBusyX64, x86Locked, preserveGateOnX86, preserveGateOnX64);
        if (string.Equals(_refreshFingerprint, fingerprint, StringComparison.Ordinal))
            return;

        _refreshFingerprint = fingerprint;
        _suppress = true;
        try
        {
            // Текст/точка — всегда (в т.ч. «Выгружается» при своём stop); IsOn — не трогаем у busy-линии.
            ApplyLine(uiX86, SubtitleX86, DotX86, _gateX86, preserveGateOn: preserveGateOnX86, controlsLocked: x86Locked);
            ApplyLine(uiX64, SubtitleX64, DotX64, _gateX64, preserveGateOn: preserveGateOnX64);
        }
        finally
        {
            _suppress = false;
        }
    }

    private static string BuildFingerprint(
        HomeStatusSummary.GlassPlatformUiSnapshot x86,
        HomeStatusSummary.GlassPlatformUiSnapshot x64,
        bool busyX86,
        bool busyX64,
        bool x86Locked,
        bool preserveOnX86,
        bool preserveOnX64) =>
        $"{PackUi(x86)}|{PackUi(x64)}|{(busyX86 ? '1' : '0')}{(busyX64 ? '1' : '0')}{(x86Locked ? '1' : '0')}{(preserveOnX86 ? '1' : '0')}{(preserveOnX64 ? '1' : '0')}";

    private static string PackUi(HomeStatusSummary.GlassPlatformUiSnapshot ui) =>
        $"{(int)ui.Life}|{(int)ui.Autorun}|{(ui.IsUnloading ? 1 : 0)}|{(ui.LifecycleBusy ? 1 : 0)}|{ui.Line}|{PackColor(ui.DotColor)}";

    private static uint PackColor(Windows.UI.Color c) =>
        ((uint)c.A << 24) | ((uint)c.R << 16) | ((uint)c.G << 8) | c.B;

    private void WireGates()
    {
        _gateX86 = new GlassSettingsLabeledToggleSwitch();
        _gateX64 = new GlassSettingsLabeledToggleSwitch();
        _gateX86.Toggled += (_, _) => OnGateToggled(GlassPlatformArch.X86, _gateX86);
        _gateX64.Toggled += (_, _) => OnGateToggled(GlassPlatformArch.X64, _gateX64);
        GateX86Host.Content = _gateX86;
        GateX64Host.Content = _gateX64;
    }

    private static void ApplyLine(
        HomeStatusSummary.GlassPlatformUiSnapshot ui,
        TextBlock subtitle,
        Ellipse dot,
        GlassSettingsLabeledToggleSwitch? gate,
        bool preserveGateOn = false,
        bool controlsLocked = false)
    {
        if (controlsLocked)
        {
            var notInstalled = GlassSettingsResources.Get("Status_PlatformNotInstalled");
            if (!string.Equals(subtitle.Text, notInstalled, StringComparison.Ordinal))
                subtitle.Text = notInstalled;
            var dim = PlatformStatusColors.GlassPlatform(GlassAutorunUiState.NoTask);
            if (dot.Fill is not SolidColorBrush dimBrush || dimBrush.Color != dim)
                dot.Fill = new SolidColorBrush(dim);
            if (gate is not null)
            {
                if (gate.IsEnabled)
                    gate.IsEnabled = false;
                if (gate.IsOn)
                    gate.IsOn = false;
            }
            return;
        }

        if (!string.Equals(subtitle.Text, ui.Line, StringComparison.Ordinal))
            subtitle.Text = ui.Line;
        if (dot.Fill is not SolidColorBrush brush || brush.Color != ui.DotColor)
            dot.Fill = new SolidColorBrush(ui.DotColor);
        if (gate is null)
            return;

        var hasTask = ui.Autorun != GlassAutorunUiState.NoTask;
        var stopping = ui.Life is NativeHostInterop.InjectLifecycleUi.Stopping
            or NativeHostInterop.InjectLifecycleUi.Residual;
        var enabled = hasTask && !ui.LifecycleBusy && !preserveGateOn;
        if (gate.IsEnabled != enabled)
            gate.IsEnabled = enabled;
        // Gate position: OFF when stopping/residual (regardless of preserveGateOn),
        // otherwise based on autorun state.
        if (stopping)
        {
            if (gate.IsOn)
                gate.IsOn = false;
        }
        else if (!preserveGateOn)
        {
            var isOn = ui.Autorun == GlassAutorunUiState.Running;
            if (gate.IsOn != isOn)
                gate.IsOn = isOn;
        }
    }

    private async void OnGateToggled(GlassPlatformArch arch, GlassSettingsLabeledToggleSwitch gate)
    {
        if (_suppress || IsLocalBusy(arch))
            return;

        if (arch == GlassPlatformArch.X86 && GlassPlatformUiPolicy.IsX86DualUiControlsLocked())
        {
            _suppress = true;
            try
            {
                gate.IsOn = false;
                gate.IsEnabled = false;
            }
            finally
            {
                _suppress = false;
            }

            return;
        }

        // SoT как PlatformSchedulerRow: внешний stop/residual/starting — не Start/Stop.
        var life = NativeHostInterop.QueryInjectLifecycleUi(arch);
        if (life is NativeHostInterop.InjectLifecycleUi.Stopping
            or NativeHostInterop.InjectLifecycleUi.Residual
            or NativeHostInterop.InjectLifecycleUi.Starting)
        {
            _suppress = true;
            try
            {
                gate.IsOn = GlassPlatformService.QueryGlassAutorunUiState(arch)
                    == GlassAutorunUiState.Running;
                gate.IsEnabled = false;
            }
            finally
            {
                _suppress = false;
            }

            return;
        }

        var wantRun = gate.IsOn;
        var st = GlassPlatformService.QueryGlassAutorunUiState(arch);
        if (wantRun && st == GlassAutorunUiState.Running)
            return;
        if (!wantRun && st != GlassAutorunUiState.Running)
            return;

        SetLocalBusy(arch, true);
        gate.IsEnabled = false;
        try
        {
            if (wantRun)
            {
                if (st is GlassAutorunUiState.Ready or GlassAutorunUiState.Disabled)
                {
                    if (!await SystemPlatformRunActions.StartGlassAsync(arch).ConfigureAwait(true))
                    {
                        _suppress = true;
                        gate.IsOn = false;
                        _suppress = false;
                    }
                }
            }
            else if (st == GlassAutorunUiState.Running)
            {
                await SystemPlatformRunActions.StopGlassAsync(arch).ConfigureAwait(true);
            }
        }
        finally
        {
            SetLocalBusy(arch, false);
            Refresh();
            GateToggledFinished?.Invoke(this, EventArgs.Empty);
        }
    }

    private bool IsLocalBusy(GlassPlatformArch arch) =>
        arch == GlassPlatformArch.X86 ? _busyX86 : _busyX64;

    private void SetLocalBusy(GlassPlatformArch arch, bool value)
    {
        if (arch == GlassPlatformArch.X86)
            _busyX86 = value;
        else
            _busyX64 = value;
    }

    /// <summary>Home подписан: RefreshNotices + settle зеркала после старта/стопа.</summary>
    public event EventHandler? GateToggledFinished;

    private void RowBorder_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (e.OriginalSource is DependencyObject src
            && (IsUnder(GateX86Host, src) || IsUnder(GateX64Host, src)))
            return;

        NavigateRequested?.Invoke(this, EventArgs.Empty);
    }

    private static bool IsUnder(DependencyObject? ancestor, DependencyObject? node)
    {
        while (node is not null)
        {
            if (ReferenceEquals(node, ancestor))
                return true;
            node = VisualTreeHelper.GetParent(node);
        }

        return false;
    }

    private void RowBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e) =>
        RowBorder.Background = Application.Current.Resources["SubtleFillColorSecondaryBrush"] as Brush
            ?? RowBorder.Background;

    private void RowBorder_OnPointerExited(object sender, PointerRoutedEventArgs e) =>
        RowBorder.Background = null;
}
