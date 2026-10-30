using System.Runtime.CompilerServices;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace GlassSettings.Services;

/// <summary>
/// Commit для слайдеров: Deferred (250 ms) или LiveImmediate (как wxEVT_SLIDER у OpenGlassGUI).
/// См. GlassSettings/docs/GlassSettings-slider-commit.md.
/// </summary>
public static class SettingsSliderCommit
{
    private const int DebounceMs = 250;
    public const int LivePreviewDebounceMs = 100;
    private static readonly ConditionalWeakTable<Slider, SliderBinding> Bindings = new();

    public enum CommitMode
    {
        /// <summary>Debounce 250 ms + flush на отпускании.</summary>
        Deferred,
        /// <summary>Реестр + notify на каждый tick; MarkDirty — в конце жеста.</summary>
        LiveImmediate
    }

    public static void Attach(
        Slider slider,
        Func<bool> canCommit,
        Action<double> onPreview,
        Action<double> onCommit,
        CommitMode mode = CommitMode.Deferred,
        Action? onGestureCommitted = null,
        int debounceMs = DebounceMs,
        int liveDebounceMs = 0)
    {
        var binding = new SliderBinding(slider, canCommit, onPreview, onCommit, mode, onGestureCommitted, debounceMs, liveDebounceMs);
        Bindings.Add(slider, binding);
        binding.Attach();
    }

    /// <summary>После Reload — сбросить baseline, чтобы программная смена Value не считалась мутацией.</summary>
    public static void SyncCommittedValue(Slider slider, double value)
    {
        if (Bindings.TryGetValue(slider, out var binding))
            binding.SyncCommitted(value);
    }

    private sealed class SliderBinding
    {
        private readonly Slider _slider;
        private readonly Func<bool> _canCommit;
        private readonly Action<double> _onPreview;
        private readonly Action<double> _onCommit;
        private readonly CommitMode _mode;
        private readonly Action? _onGestureCommitted;
        private readonly int _debounceMs;
        private readonly int _liveDebounceMs;
        private CancellationTokenSource? _debounceCts;
        private double _lastCommittedValue;
        private bool _gestureActive;
        private bool _hasPendingValue;
        private bool _gestureMutated;

        public SliderBinding(
            Slider slider,
            Func<bool> canCommit,
            Action<double> onPreview,
            Action<double> onCommit,
            CommitMode mode,
            Action? onGestureCommitted,
            int debounceMs,
            int liveDebounceMs)
        {
            _slider = slider;
            _canCommit = canCommit;
            _onPreview = onPreview;
            _onCommit = onCommit;
            _mode = mode;
            _onGestureCommitted = onGestureCommitted;
            _debounceMs = debounceMs;
            _liveDebounceMs = liveDebounceMs;
            _lastCommittedValue = slider.Value;
        }

        public void Attach()
        {
            _slider.ValueChanged += OnValueChanged;
            _slider.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler(OnPointerPressed), true);
            _slider.AddHandler(UIElement.PointerReleasedEvent, new PointerEventHandler(OnPointerReleased), true);
            _slider.AddHandler(UIElement.PointerCaptureLostEvent, new PointerEventHandler(OnPointerCaptureLost), true);
        }

        private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (!_gestureActive)
            {
                _gestureActive = true;
                _gestureMutated = false;
                SettingsInteractionDeferral.EnterSliderGesture();
            }
        }

        private void OnPointerReleased(object sender, PointerRoutedEventArgs e) =>
            EndGesture(flushPending: true);

        private void OnPointerCaptureLost(object sender, PointerRoutedEventArgs e)
        {
            if (_gestureActive)
                EndGesture(flushPending: true);
        }

        private void EndGesture(bool flushPending)
        {
            if (!_gestureActive)
                return;

            _gestureActive = false;
            _debounceCts?.Cancel();
            _debounceCts = null;

            if (_mode == CommitMode.Deferred && flushPending && _hasPendingValue)
                TryCommit(_slider.Value, force: true);

            if (_mode == CommitMode.LiveImmediate && flushPending && _hasPendingValue)
            {
                if (TryCommit(_slider.Value, force: true))
                    _gestureMutated = true;
            }

            if (_mode == CommitMode.LiveImmediate && _gestureMutated)
            {
                _onGestureCommitted?.Invoke();
                PageSessionNoticeState.InvalidateDismissOnMutation();
                HomeSessionNoticeState.InvalidateDismissOnMutation();
            }

            _hasPendingValue = false;
            SettingsInteractionDeferral.ExitSliderGesture(notifyShell: true, _slider);
        }

        private void OnValueChanged(object sender, Microsoft.UI.Xaml.Controls.Primitives.RangeBaseValueChangedEventArgs e)
        {
            _onPreview(e.NewValue);
            if (!_canCommit())
                return;

            if (_mode == CommitMode.LiveImmediate)
            {
                if (_liveDebounceMs > 0)
                {
                    _hasPendingValue = !ValuesEqual(e.NewValue, _lastCommittedValue);
                    RestartDebounce(e.NewValue, _liveDebounceMs);
                    return;
                }

                if (TryCommit(e.NewValue, force: false))
                    _gestureMutated = true;
                return;
            }

            _hasPendingValue = !ValuesEqual(e.NewValue, _lastCommittedValue);
            RestartDebounce(e.NewValue, _debounceMs);
        }

        private void RestartDebounce(double value, int debounceMs)
        {
            _debounceCts?.Cancel();
            _debounceCts = new CancellationTokenSource();
            var token = _debounceCts.Token;
            _ = WaitAndCommitAsync(value, debounceMs, token);
        }

        private async Task WaitAndCommitAsync(double staleValue, int debounceMs, CancellationToken token)
        {
            _ = staleValue;
            try
            {
                await Task.Delay(debounceMs, token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                return;
            }

            var snapshot = await ReadCommitSnapshotOnUiAsync(token).ConfigureAwait(false);
            if (!snapshot.Ok || !snapshot.CanCommit)
                return;

            var value = snapshot.Value;
            if (ValuesEqual(value, _lastCommittedValue))
            {
                _hasPendingValue = false;
                return;
            }

            _lastCommittedValue = value;
            _hasPendingValue = false;

            await Task.Run(() => _onCommit(value), token).ConfigureAwait(false);

            if (_mode == CommitMode.Deferred)
            {
                _ = _slider.DispatcherQueue.TryEnqueue(() =>
                {
                    PageSessionNoticeState.InvalidateDismissOnMutation();
                    HomeSessionNoticeState.InvalidateDismissOnMutation();
                });
            }

            _gestureMutated = true;
        }

        /// <summary>canCommit и Value — только UI-поток; после debounce перечитываем элемент.</summary>
        private async Task<(bool Ok, double Value, bool CanCommit)> ReadCommitSnapshotOnUiAsync(CancellationToken token)
        {
            var queue = _slider.DispatcherQueue;
            if (queue is null)
                return (false, 0, false);

            var tcs = new TaskCompletionSource<(double Value, bool CanCommit)>(
                TaskCreationOptions.RunContinuationsAsynchronously);
            if (!queue.TryEnqueue(() =>
                {
                    try
                    {
                        tcs.TrySetResult((_slider.Value, _canCommit()));
                    }
                    catch (Exception ex)
                    {
                        tcs.TrySetException(ex);
                    }
                }))
            {
                return (false, 0, false);
            }

            using var registration = token.Register(() => tcs.TrySetCanceled(token));
            try
            {
                var result = await tcs.Task.ConfigureAwait(false);
                return (true, result.Value, result.CanCommit);
            }
            catch (OperationCanceledException)
            {
                return (false, 0, false);
            }
        }

        private bool TryCommit(double value, bool force)
        {
            if (!_canCommit())
                return false;

            if (!force && ValuesEqual(value, _lastCommittedValue))
            {
                _hasPendingValue = false;
                return false;
            }

            _lastCommittedValue = value;
            _hasPendingValue = false;
            _ = Task.Run(() => _onCommit(value));

            if (_mode == CommitMode.Deferred)
            {
                PageSessionNoticeState.InvalidateDismissOnMutation();
                HomeSessionNoticeState.InvalidateDismissOnMutation();
            }

            return true;
        }

        public void SyncCommitted(double value)
        {
            _lastCommittedValue = value;
            _hasPendingValue = false;
            _gestureMutated = false;
            _debounceCts?.Cancel();
            _debounceCts = null;
        }

        private static bool ValuesEqual(double left, double right) =>
            Math.Abs(left - right) < 0.0001;
    }
}
