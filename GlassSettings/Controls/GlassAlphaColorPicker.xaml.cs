using GlassSettings.Core.Flyout;
using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Controls;

public sealed partial class GlassAlphaColorPicker : UserControl
{
    public static readonly DependencyProperty UseLivePreviewProperty =
        DependencyProperty.Register(
            nameof(UseLivePreview),
            typeof(bool),
            typeof(GlassAlphaColorPicker),
            new PropertyMetadata(false));

    public static readonly DependencyProperty IsAlphaVisibleProperty =
        DependencyProperty.Register(
            nameof(IsAlphaVisible),
            typeof(bool),
            typeof(GlassAlphaColorPicker),
            new PropertyMetadata(true, OnIsAlphaVisibleChanged));

    private bool _lock;
    private bool _pointerActive;
    private bool _liveGestureActive;
    private uint? _lastCommittedArgb;
    private uint? _lastLivePreviewArgb;
    private bool _gestureRenderPollActive;

    public event EventHandler<uint>? ColorChanged;
    public event EventHandler<uint>? ColorLivePreview;
    public event EventHandler? LiveGestureStarted;
    public event EventHandler? LiveGestureEnded;

    public bool UseLivePreview
    {
        get => (bool)GetValue(UseLivePreviewProperty);
        set => SetValue(UseLivePreviewProperty, value);
    }

    /// <summary>Показывать альфа-канал в пикере (слайдер + ввод). По умолчанию true.</summary>
    public bool IsAlphaVisible
    {
        get => (bool)GetValue(IsAlphaVisibleProperty);
        set => SetValue(IsAlphaVisibleProperty, value);
    }

    private static void OnIsAlphaVisibleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is GlassAlphaColorPicker self)
            self.ApplyAlphaVisible((bool)e.NewValue);
    }

    private void ApplyAlphaVisible(bool visible)
    {
        Picker.IsAlphaEnabled = visible;
        Picker.IsAlphaSliderVisible = visible;
        Picker.IsAlphaTextInputVisible = visible;
    }

    public GlassAlphaColorPicker()
    {
        InitializeComponent();
        Picker.ColorChanged += (_, _) => OnPickerColorChanged();
        AddPointerHandlers(Picker);
        AddPointerHandlers(this);
    }

    public void SetColor(uint argb)
    {
        _lock = true;
        ArgbColor.Unpack(argb, out var a, out var r, out var g, out var b);
        Picker.Color = Color.FromArgb(a, r, g, b);
        _lock = false;
        _lastCommittedArgb = argb;
        _lastLivePreviewArgb = argb;
    }

    public uint GetColorArgb()
    {
        var c = Picker.Color;
        return NormalizeCommittedArgb(ArgbColor.Pack(c.A, c.R, c.G, c.B));
    }

    private uint NormalizeCommittedArgb(uint argb) =>
        IsAlphaVisible ? argb : (argb & 0x00FFFFFF) | 0xFF000000;

    private void AddPointerHandlers(UIElement element)
    {
        element.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler(OnPointerPressed), true);
        element.AddHandler(UIElement.PointerMovedEvent, new PointerEventHandler(OnPointerMoved), true);
        element.AddHandler(UIElement.PointerReleasedEvent, new PointerEventHandler(OnPointerReleased), true);
        element.AddHandler(UIElement.PointerCanceledEvent, new PointerEventHandler(OnPointerReleased), true);
        element.AddHandler(UIElement.PointerCaptureLostEvent, new PointerEventHandler(OnPointerReleased), true);
    }

    private void BeginLiveGestureIfNeeded()
    {
        if (!UseLivePreview || _liveGestureActive)
            return;

        _liveGestureActive = true;
        SettingsInteractionDeferral.EnterSliderGesture();
        LiveGestureStarted?.Invoke(this, EventArgs.Empty);
    }

    private void EndLiveGestureAndCommit()
    {
        StopGestureRenderPoll();
        if (UseLivePreview && _liveGestureActive)
        {
            _liveGestureActive = false;
            SettingsInteractionDeferral.ExitSliderGesture(notifyShell: true, this);
            LiveGestureEnded?.Invoke(this, EventArgs.Empty);
        }

        _pointerActive = false;
        _lastLivePreviewArgb = null;
        CommitColor();
    }

    private void StartGestureRenderPoll()
    {
        if (_gestureRenderPollActive)
            return;

        _gestureRenderPollActive = true;
        CompositionTarget.Rendering += OnGestureCompositionRendering;
    }

    private void StopGestureRenderPoll()
    {
        if (!_gestureRenderPollActive)
            return;

        _gestureRenderPollActive = false;
        CompositionTarget.Rendering -= OnGestureCompositionRendering;
    }

    private void OnGestureCompositionRendering(object? sender, object e)
    {
        if (!_pointerActive && !_liveGestureActive)
        {
            StopGestureRenderPoll();
            return;
        }

        NotifyPickerColorIfChanged("render-poll");
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_pointerActive && !_liveGestureActive)
            return;

        NotifyPickerColorIfChanged("pointer-move");
    }

    private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        _pointerActive = true;
        _lastLivePreviewArgb = null;
        BeginLiveGestureIfNeeded();
        StartGestureRenderPoll();
        NotifyPickerColorIfChanged("pointer-press");
    }

    private void OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (!_pointerActive && !_liveGestureActive)
            return;

        EndLiveGestureAndCommit();
    }

    private void OnPickerColorChanged()
    {
        NotifyPickerColorIfChanged("color-changed");
    }

    private void NotifyPickerColorIfChanged(string source)
    {
        if (_lock)
            return;

        var argb = GetColorArgb();
        if (UseLivePreview)
        {
            BeginLiveGestureIfNeeded();
            if (_lastLivePreviewArgb == argb)
                return;

            _lastLivePreviewArgb = argb;
            ColorLivePreview?.Invoke(this, argb);
            return;
        }

        CommitColor();
    }

    private void CommitColor()
    {
        var argb = GetColorArgb();
        if (_lastCommittedArgb == argb)
            return;

        _lastCommittedArgb = argb;
        ColorChanged?.Invoke(this, argb);
    }
}
