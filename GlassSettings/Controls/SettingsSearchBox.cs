using Microsoft.UI.Input;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>
/// AutoSuggestBox that uses the text-entry cursor when hovered.
/// ProtectedCursor is only accessible from a UIElement-derived class.
/// </summary>
public sealed class SettingsSearchBox : AutoSuggestBox
{
    public SettingsSearchBox()
    {
        ProtectedCursor = InputSystemCursor.Create(InputSystemCursorShape.IBeam);
    }
}
