using System.Globalization;
using System.Runtime.CompilerServices;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using VirtualKey = Windows.System.VirtualKey;

namespace GlassSettings.Services;

/// <summary>
/// Commit для NumberBox: ручной ввод — по Enter, spin-кнопки и клавиши шага — сразу.
/// </summary>
public static class SettingsNumberBoxCommit
{
    private static readonly ConditionalWeakTable<NumberBox, NumberBoxBinding> Bindings = new();

    public static void Attach(
        NumberBox box,
        Func<bool> canCommit,
        Action<double> onCommit)
    {
        ArgumentNullException.ThrowIfNull(box);
        ArgumentNullException.ThrowIfNull(canCommit);
        ArgumentNullException.ThrowIfNull(onCommit);

        var binding = new NumberBoxBinding(box, canCommit, onCommit);
        Bindings.Add(box, binding);
        binding.Attach();
    }

    /// <summary>Завершить отложенный ручной ввод, если он есть.</summary>
    public static bool Flush(NumberBox box) =>
        Bindings.TryGetValue(box, out var binding) && binding.Flush();

    /// <summary>Отменить текущий ручной ввод и восстановить значение до начала редактирования.</summary>
    public static void CancelEdit(NumberBox box)
    {
        if (Bindings.TryGetValue(box, out var binding))
            binding.CancelEdit();
    }

    internal static void CompleteCancellation(NumberBox box)
    {
        if (Bindings.TryGetValue(box, out var binding))
            binding.CompleteCancellation();
    }

    /// <summary>После Reload сбросить pending-ввод и принять значение как записанный baseline.</summary>
    public static void SyncCommittedValue(NumberBox box, double value)
    {
        if (Bindings.TryGetValue(box, out var binding))
            binding.SyncCommitted(value);
    }

    private sealed class NumberBoxBinding
    {
        private readonly NumberBox _box;
        private readonly Func<bool> _canCommit;
        private readonly Action<double> _onCommit;
        private TextBox? _input;
        private double _lastCommittedValue;
        private double _pendingValue = double.NaN;
        private double _focusEntryValue = double.NaN;
        private double _editStartValue = double.NaN;
        private double _canceledValue = double.NaN;
        private bool _hasCommittedValue;
        private bool _hasPendingValue;
        private bool _textEditActive;
        private bool _hasEditStartValue;
        private bool _directTextChange;
        private bool _spinPointerActive;
        private bool _keyboardStepActive;
        private bool _commitOnFocusLoss;
        private bool _cancelingEdit;
        private bool _settingNormalizedValue;

        public NumberBoxBinding(
            NumberBox box,
            Func<bool> canCommit,
            Action<double> onCommit)
        {
            _box = box;
            _canCommit = canCommit;
            _onCommit = onCommit;

            if (IsFinite(box.Value))
            {
                _lastCommittedValue = ClampToRange(box.Value);
                _hasCommittedValue = true;
            }
        }

        public void Attach()
        {
            _box.ValueChanged += OnValueChanged;
            _box.Loaded += OnLoaded;
            _box.Unloaded += OnUnloaded;
            _box.PreviewKeyDown += OnPreviewKeyDown;
            _box.AddHandler(UIElement.PointerPressedEvent, new PointerEventHandler(OnPointerPressed), true);
            _box.AddHandler(UIElement.PointerReleasedEvent, new PointerEventHandler(OnPointerReleased), true);
            _box.AddHandler(UIElement.PointerCaptureLostEvent, new PointerEventHandler(OnPointerCaptureLost), true);

            if (_box.IsLoaded)
                AttachInput();
        }

        private void OnLoaded(object sender, RoutedEventArgs e) => AttachInput();

        private void OnUnloaded(object sender, RoutedEventArgs e)
        {
            if (!_cancelingEdit)
                Flush();
        }

        private void AttachInput()
        {
            var input = FindDescendant<TextBox>(_box);
            if (ReferenceEquals(input, _input))
                return;

            if (_input is not null)
            {
                _input.BeforeTextChanging -= OnBeforeTextChanging;
                _input.GotFocus -= OnInputGotFocus;
                _input.LostFocus -= OnInputLostFocus;
            }

            _input = input;
            if (_input is null)
                return;

            _input.BeforeTextChanging += OnBeforeTextChanging;
            _input.GotFocus += OnInputGotFocus;
            _input.LostFocus += OnInputLostFocus;
        }

        private void OnInputGotFocus(object sender, RoutedEventArgs e)
        {
            if (!_textEditActive && !_cancelingEdit)
                _focusEntryValue = _box.Value;
        }

        private void OnBeforeTextChanging(TextBox sender, TextBoxBeforeTextChangingEventArgs args)
        {
            _ = args;
            if (_settingNormalizedValue
                || _cancelingEdit
                || _spinPointerActive
                || _keyboardStepActive
                || sender.FocusState == FocusState.Unfocused)
                return;

            BeginTextEdit();
            _hasPendingValue = true;
            _pendingValue = double.NaN;
            _directTextChange = true;
            _box.DispatcherQueue?.TryEnqueue(
                DispatcherQueuePriority.Low,
                () => _directTextChange = false);
        }

        private void OnInputLostFocus(object sender, RoutedEventArgs e)
        {
            if (_cancelingEdit)
                return;

            _commitOnFocusLoss = true;
            var queue = _box.DispatcherQueue;
            if (queue is null)
            {
                Flush();
                _commitOnFocusLoss = false;
                return;
            }

            if (!queue.TryEnqueue(DispatcherQueuePriority.Low, () =>
                {
                    Flush();
                    _commitOnFocusLoss = false;
                }))
            {
                Flush();
                _commitOnFocusLoss = false;
            }
        }

        private void OnPreviewKeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (!IsStepKey(e.Key))
                return;

            _keyboardStepActive = true;
            _box.DispatcherQueue?.TryEnqueue(
                DispatcherQueuePriority.Low,
                () => _keyboardStepActive = false);
        }

        private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            if (FindAncestor<RepeatButton>(e.OriginalSource as DependencyObject, _box) is not null)
                _spinPointerActive = true;
        }

        private void OnPointerReleased(object sender, PointerRoutedEventArgs e) =>
            EndSpinPointerInteraction();

        private void OnPointerCaptureLost(object sender, PointerRoutedEventArgs e) =>
            EndSpinPointerInteraction();

        private void EndSpinPointerInteraction()
        {
            _box.DispatcherQueue?.TryEnqueue(
                DispatcherQueuePriority.Low,
                () => _spinPointerActive = false);
        }

        private void OnValueChanged(NumberBox sender, NumberBoxValueChangedEventArgs args)
        {
            if (_settingNormalizedValue)
                return;

            if (_cancelingEdit)
            {
                if (IsFinite(_canceledValue))
                    SetNormalizedValue(_canceledValue);
                return;
            }

            if (_commitOnFocusLoss && IsFinite(args.NewValue))
            {
                DiscardPending();
                EndTextEdit();
                TryCommit(args.NewValue);
                return;
            }

            var isTextInput = _directTextChange
                || (_hasPendingValue
                    && !_spinPointerActive
                    && !_keyboardStepActive)
                || (!_spinPointerActive
                    && !_keyboardStepActive
                    && _input is not null
                    && _input.FocusState != FocusState.Unfocused);

            if (isTextInput)
            {
                BeginTextEdit();
                _hasPendingValue = true;
                _pendingValue = args.NewValue;
                return;
            }

            if (!IsFinite(args.NewValue))
                return;

            DiscardPending();
            EndTextEdit();
            TryCommit(args.NewValue);
        }

        public bool Flush()
        {
            if (!_hasPendingValue)
                return false;

            var pendingValue = TryReadInputValue(out var parsedValue)
                ? parsedValue
                : _pendingValue;
            _hasPendingValue = false;
            _pendingValue = double.NaN;
            EndTextEdit();
            if (!IsFinite(pendingValue))
            {
                RestoreCommittedValue();
                return false;
            }

            return TryCommit(pendingValue);
        }

        private bool TryReadInputValue(out double value)
        {
            if (_input is not null
                && double.TryParse(
                    _input.Text,
                    NumberStyles.Float | NumberStyles.AllowThousands,
                    CultureInfo.CurrentCulture,
                    out value)
                && IsFinite(value))
            {
                return true;
            }

            value = double.NaN;
            return false;
        }

        private bool TryCommit(double value)
        {
            if (!IsFinite(value))
                return false;

            var normalized = Normalize(value);
            SetNormalizedValue(normalized);
            if (!_canCommit())
                return false;

            if (_hasCommittedValue && ValuesEqual(normalized, _lastCommittedValue))
                return false;

            _lastCommittedValue = normalized;
            _hasCommittedValue = true;
            _onCommit(normalized);
            SettingsInteractionDeferral.NotifyPageShellInteractionCompleted(_box);
            return true;
        }

        public void SyncCommitted(double value)
        {
            DiscardPending();
            EndTextEdit();
            CompleteCancellation();
            if (!IsFinite(value))
            {
                _hasCommittedValue = false;
                return;
            }

            _lastCommittedValue = ClampToRange(value);
            _hasCommittedValue = true;
        }

        public void CancelEdit()
        {
            if (!_textEditActive && !_hasPendingValue)
                return;

            var restoreValue = _hasEditStartValue
                ? _editStartValue
                : _lastCommittedValue;

            DiscardPending();
            EndTextEdit();
            if (!IsFinite(restoreValue))
                return;

            _canceledValue = restoreValue;
            _cancelingEdit = true;
            SetNormalizedValue(restoreValue);
        }

        public void CompleteCancellation()
        {
            _cancelingEdit = false;
            _canceledValue = double.NaN;
        }

        private void BeginTextEdit()
        {
            if (_textEditActive)
                return;

            _textEditActive = true;
            var currentValue = IsFinite(_focusEntryValue)
                ? _focusEntryValue
                : IsFinite(_box.Value)
                ? _box.Value
                : _lastCommittedValue;
            if (!IsFinite(currentValue))
                return;

            _editStartValue = currentValue;
            _hasEditStartValue = true;
        }

        private void EndTextEdit()
        {
            _textEditActive = false;
            _hasEditStartValue = false;
            _focusEntryValue = double.NaN;
            _editStartValue = double.NaN;
        }

        private void DiscardPending()
        {
            if (!_hasPendingValue)
                return;

            _hasPendingValue = false;
            _pendingValue = double.NaN;
        }

        private void RestoreCommittedValue()
        {
            if (_hasCommittedValue)
                SetNormalizedValue(_lastCommittedValue);
        }

        private double Normalize(double value)
        {
            var minimum = EffectiveMinimum;
            var maximum = EffectiveMaximum;
            var clamped = Math.Clamp(value, minimum, maximum);
            var step = _box.SmallChange;
            if (!IsFinite(step) || step <= 0 || !IsFinite(minimum))
                return clamped;

            var steps = Math.Round(
                (clamped - minimum) / step,
                MidpointRounding.AwayFromZero);
            var snapped = minimum + steps * step;
            return Math.Clamp(snapped, minimum, maximum);
        }

        private double ClampToRange(double value) =>
            Math.Clamp(value, EffectiveMinimum, EffectiveMaximum);

        private double EffectiveMinimum =>
            IsFinite(_box.Minimum) ? _box.Minimum : double.MinValue;

        private double EffectiveMaximum =>
            IsFinite(_box.Maximum) ? _box.Maximum : double.MaxValue;

        private void SetNormalizedValue(double value)
        {
            if (ValuesEqual(_box.Value, value))
                return;

            _settingNormalizedValue = true;
            try
            {
                _box.Value = value;
            }
            finally
            {
                _settingNormalizedValue = false;
            }
        }

        private static bool IsStepKey(VirtualKey key) =>
            key is VirtualKey.Up or VirtualKey.Down or VirtualKey.PageUp or VirtualKey.PageDown;

        private static bool IsFinite(double value) =>
            !double.IsNaN(value) && !double.IsInfinity(value);

        private static bool ValuesEqual(double left, double right) =>
            IsFinite(left)
            && IsFinite(right)
            && Math.Abs(left - right) < 0.0001;

        private static T? FindDescendant<T>(DependencyObject root) where T : DependencyObject
        {
            var count = VisualTreeHelper.GetChildrenCount(root);
            for (var i = 0; i < count; i++)
            {
                var child = VisualTreeHelper.GetChild(root, i);
                if (child is T match)
                    return match;

                var nested = FindDescendant<T>(child);
                if (nested is not null)
                    return nested;
            }

            return null;
        }

        private static T? FindAncestor<T>(DependencyObject? child, DependencyObject stop)
            where T : DependencyObject
        {
            while (child is not null && !ReferenceEquals(child, stop))
            {
                if (child is T match)
                    return match;
                child = VisualTreeHelper.GetParent(child);
            }

            return null;
        }
    }
}
