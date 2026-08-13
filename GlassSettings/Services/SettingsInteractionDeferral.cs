using GlassSettings.Controls;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using VirtualKey = Windows.System.VirtualKey;

namespace GlassSettings.Services;

/// <summary>
/// Откладывает commit параметров до завершения жеста (ComboBox, слайдер и т.п.),
/// чтобы не блокировать UI и не дёргать notice на каждый кадр.
/// </summary>
public static class SettingsInteractionDeferral
{
    private static int _sliderGestureDepth;

    public static bool IsSliderGestureActive => _sliderGestureDepth > 0;

    public static void WhenComboCommit(ComboBox combo, Func<bool> canCommit, Action commit)
    {
        var indexAtOpen = combo.SelectedIndex;
        combo.DropDownOpened += (_, _) => indexAtOpen = combo.SelectedIndex;
        combo.DropDownClosed += (_, _) =>
        {
            if (!canCommit())
                return;

            if (combo.SelectedIndex < 0 || combo.SelectedIndex == indexAtOpen)
                return;

            RunAfterInputIdle(combo.DispatcherQueue, () =>
            {
                if (!canCommit())
                    return;

                commit();
                NotifyPageShellInteractionCompleted(combo);
            });
        };
    }

    /// <summary>
    /// Enter — коммит, Escape — отмена ручного ввода; затем возврат фокуса к панели.
    /// </summary>
    /// <param name="focusTarget">Куда перевести фокус; для SettingsParameterPanel — внутренний FocusSink.</param>
    public static void WhenNumberBoxCommitOnEnter(NumberBox box, UIElement? focusTarget = null)
    {
        void OnKey(KeyRoutedEventArgs e)
        {
            if (e.Key is not (VirtualKey.Enter or VirtualKey.Escape))
                return;

            e.Handled = true;
            if (e.Key == VirtualKey.Enter)
            {
                SettingsNumberBoxCommit.Flush(box);
                CommitAndUnfocusNumberBox(box, focusTarget);
            }
            else
            {
                SettingsNumberBoxCommit.CancelEdit(box);
                CancelAndUnfocusNumberBox(box, focusTarget);
            }
            NotifyPageShellInteractionCompleted(box);
        }

        box.PreviewKeyDown += (_, e) => OnKey(e);

        box.Loaded += (_, _) =>
        {
            ApplyNumberBoxTextChrome(box);
            if (FindDescendant<TextBox>(box) is TextBox input)
                input.PreviewKeyDown += (_, e) => OnKey(e);
        };

        box.GotFocus += (_, _) => ApplyNumberBoxTextChrome(box);
        box.LostFocus += (_, _) => ApplyNumberBoxTextChrome(box);
    }

    private static void CommitAndUnfocusNumberBox(NumberBox box, UIElement? focusTarget)
    {
        var queue = box.DispatcherQueue;
        queue?.TryEnqueue(DispatcherQueuePriority.Normal, () => UnfocusNumberBox(box, focusTarget));
        queue?.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            ApplyNumberBoxTextChrome(box);
            if (!double.IsNaN(box.Value))
                box.Value = box.Value;
        });
    }

    private static void CancelAndUnfocusNumberBox(NumberBox box, UIElement? focusTarget)
    {
        var queue = box.DispatcherQueue;
        if (queue is null)
        {
            UnfocusNumberBox(box, focusTarget);
            ApplyNumberBoxTextChrome(box);
            SettingsNumberBoxCommit.CompleteCancellation(box);
            return;
        }

        var unfocusQueued = queue.TryEnqueue(
            DispatcherQueuePriority.Normal,
            () => UnfocusNumberBox(box, focusTarget));
        if (!unfocusQueued)
            UnfocusNumberBox(box, focusTarget);

        if (queue.TryEnqueue(DispatcherQueuePriority.Low, () =>
            {
                ApplyNumberBoxTextChrome(box);
                if (!queue.TryEnqueue(
                    DispatcherQueuePriority.Low,
                    () => SettingsNumberBoxCommit.CompleteCancellation(box)))
                {
                    SettingsNumberBoxCommit.CompleteCancellation(box);
                }
            }))
        {
            return;
        }

        // Очередь закрывается: завершаем LostFocus синхронно, пока callback ещё подавлен.
        UnfocusNumberBox(box, focusTarget);
        ApplyNumberBoxTextChrome(box);
        SettingsNumberBoxCommit.CompleteCancellation(box);
    }

    private static void UnfocusNumberBox(NumberBox box, UIElement? focusTarget)
    {
        if (focusTarget is SettingsParameterPanel panel)
        {
            panel.RelinquishInputFocus();
            return;
        }

        if (focusTarget is not null && focusTarget.Focus(FocusState.Programmatic))
            return;

        if (box.XamlRoot?.Content is UIElement root && root.Focus(FocusState.Programmatic))
            return;

        if (FindAncestor<SettingsParameterPanel>(box) is { } ancestorPanel)
        {
            ancestorPanel.RelinquishInputFocus();
            return;
        }

        BlurInnerTextBox(box);
    }

    private static void BlurInnerTextBox(NumberBox box)
    {
        if (FindDescendant<TextBox>(box) is not TextBox input)
            return;

        input.IsEnabled = false;
        input.IsEnabled = true;
    }

    private static void ApplyNumberBoxTextChrome(NumberBox box)
    {
        if (FindDescendant<TextBox>(box) is TextBox input)
        {
            input.TextAlignment = TextAlignment.Left;
            input.HorizontalAlignment = HorizontalAlignment.Stretch;
        }

        if (box.FocusState == FocusState.Unfocused && FindDescendant<Button>(box) is Button deleteButton)
            deleteButton.Visibility = Visibility.Collapsed;
    }

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

    private static T? FindAncestor<T>(DependencyObject? child) where T : DependencyObject
    {
        while (child is not null)
        {
            if (child is T match)
                return match;

            child = VisualTreeHelper.GetParent(child);
        }

        return null;
    }

    internal static void EnterSliderGesture() => _sliderGestureDepth++;

    internal static void ExitSliderGesture(bool notifyShell, DependencyObject origin)
    {
        if (_sliderGestureDepth > 0)
            _sliderGestureDepth--;

        if (notifyShell)
            NotifyPageShellInteractionCompleted(origin);
    }

    internal static void NotifyPageShellInteractionCompleted(DependencyObject origin)
    {
        DependencyObject? current = origin;
        while (current is not null)
        {
            if (current is SettingsPageShell shell)
            {
                shell.NotifyInteractionCompleted();
                return;
            }

            current = VisualTreeHelper.GetParent(current);
        }
    }

    public static void RunAfterInputIdle(DispatcherQueue? queue, Action action)
    {
        if (queue is null)
        {
            action();
            return;
        }

        queue.TryEnqueue(DispatcherQueuePriority.Low, () => action());
    }

    /// <summary>Дать UI закрыть диалог / завершить анимацию перед тяжёлой работой.</summary>
    /// <remarks>Хвост <see cref="SettingsPageOperationSequence.RegistryMutationAsync"/> — не дублировать на страницах.</remarks>
    public static Task RunAfterUiSettledAsync(DispatcherQueue? queue = null)
    {
        queue ??= DispatcherQueue.GetForCurrentThread();
        if (queue is null)
            return Task.CompletedTask;

        var tcs = new TaskCompletionSource<bool>();
        queue.TryEnqueue(DispatcherQueuePriority.Low, () =>
        {
            queue.TryEnqueue(DispatcherQueuePriority.Low, () => tcs.TrySetResult(true));
        });
        return tcs.Task;
    }
}
