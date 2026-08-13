using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Input;
using Windows.System;

namespace GlassSettings.Services;

/// <summary>Общая клавиатурная логика hosted-диалогов.</summary>
public static class HostedDialogKeyboardBehavior
{
    /// <summary>ESC → dismiss, если <paramref name="canDismiss"/> возвращает true.</summary>
    public static void AttachDismissOnEscape(UIElement host, Func<bool> canDismiss, Action dismiss)
    {
        void OnKeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key != VirtualKey.Escape || !canDismiss())
                return;

            e.Handled = true;
            dismiss();
        }

        host.AddHandler(UIElement.KeyDownEvent, (KeyEventHandler)OnKeyDown, handledEventsToo: true);
    }
}
