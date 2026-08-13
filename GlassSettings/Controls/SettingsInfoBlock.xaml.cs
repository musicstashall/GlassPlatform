using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class SettingsInfoBlock : UserControl
{
    public static readonly DependencyProperty TitleProperty =
        DependencyProperty.Register(
            nameof(Title),
            typeof(string),
            typeof(SettingsInfoBlock),
            new PropertyMetadata(string.Empty, OnTitleChanged));

    public static readonly DependencyProperty MessageProperty =
        DependencyProperty.Register(
            nameof(Message),
            typeof(string),
            typeof(SettingsInfoBlock),
            new PropertyMetadata(string.Empty, OnMessageChanged));

    public static readonly DependencyProperty IconGlyphProperty =
        DependencyProperty.Register(
            nameof(IconGlyph),
            typeof(string),
            typeof(SettingsInfoBlock),
            new PropertyMetadata("\uE946", OnIconGlyphChanged));

    public SettingsInfoBlock()
    {
        InitializeComponent();
        UpdateTitleVisibility(Title);
    }

    /// <summary>Для rich-текста с гиперссылками (страница «Система»).</summary>
    public TextBlock MessageTextBlock => MessageText;

    public string Title
    {
        get => (string)GetValue(TitleProperty);
        set => SetValue(TitleProperty, value);
    }

    public string Message
    {
        get => (string)GetValue(MessageProperty);
        set => SetValue(MessageProperty, value);
    }

    public string IconGlyph
    {
        get => (string)GetValue(IconGlyphProperty);
        set => SetValue(IconGlyphProperty, value);
    }

    private static void OnTitleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsInfoBlock block)
        {
            var title = e.NewValue as string ?? string.Empty;
            block.TitleText.Text = title;
            block.UpdateTitleVisibility(title);
        }
    }

    private void UpdateTitleVisibility(string title)
    {
        TitleText.Visibility = string.IsNullOrWhiteSpace(title)
            ? Visibility.Collapsed
            : Visibility.Visible;
    }

    private static void OnMessageChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsInfoBlock block)
            block.MessageText.Text = e.NewValue as string ?? string.Empty;
    }

    private static void OnIconGlyphChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SettingsInfoBlock block)
            block.IconGlyphHost.Glyph = e.NewValue as string ?? string.Empty;
    }
}
