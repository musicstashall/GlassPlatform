using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

/// <summary>Строка таблицы «Модуль | Версия» с ThemeResource для колонки версии.</summary>
public sealed partial class HomeComponentTableRow : UserControl
{
    public HomeComponentTableRow() => InitializeComponent();

    public void SetValues(string name, string value)
    {
        if (!string.Equals(NameText.Text, name, StringComparison.Ordinal))
            NameText.Text = name;
        if (!string.Equals(ValueText.Text, value, StringComparison.Ordinal))
            ValueText.Text = value;
    }
}
