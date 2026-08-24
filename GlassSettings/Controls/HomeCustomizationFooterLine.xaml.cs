using GlassSettings.Services;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Documents;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

/// <summary>
/// Подстрока-футер карточки устройства на «Обзоре».
/// Левая часть — кастомизация, правая — несохранённые изменения (+ «Сохранить»).
/// При узком окне (обе части) — раскладка в две строки.
/// </summary>
public sealed partial class HomeCustomizationFooterLine : UserControl
{
    private const double StackBreakpoint = 480;

    private static readonly GridLength StarLength = new(1, GridUnitType.Star);
    private static readonly GridLength ZeroLength = new(0);

    public static readonly DependencyProperty TrailingContentProperty =
        DependencyProperty.Register(nameof(TrailingContent), typeof(object), typeof(HomeCustomizationFooterLine),
            new PropertyMetadata(null, (d, _) => ((HomeCustomizationFooterLine)d).UpdateLayoutMode()));

    public event EventHandler? CustomLinkClicked;
    public event EventHandler? UnsavedLinkClicked;

    private bool _hasLeft;
    private string _leftPrefix = string.Empty;
    private string _leftLink = string.Empty;
    private bool _hasRight;
    private string _rightPrefix = string.Empty;
    private string _rightLink = string.Empty;
    private string _rightPlain = string.Empty;

    public HomeCustomizationFooterLine()
    {
        InitializeComponent();
    }

    public object? TrailingContent
    {
        get => GetValue(TrailingContentProperty);
        set => SetValue(TrailingContentProperty, value);
    }

    /// <summary>Установить левую и правые части футера.</summary>
    public void SetParts(
        bool hasLeft,
        string leftPrefix,
        string? leftLink,
        bool hasRight,
        string rightPrefix,
        string? rightLink,
        string? rightPlain)
    {
        _hasLeft = hasLeft;
        _leftPrefix = leftPrefix;
        _leftLink = leftLink ?? string.Empty;
        _hasRight = hasRight;
        _rightPrefix = rightPrefix;
        _rightLink = rightLink ?? string.Empty;
        _rightPlain = rightPlain ?? string.Empty;

        RebuildSegments();
        UpdateLayoutMode();
    }

    /// <summary>Установить левую часть только.</summary>
    public void SetParts(bool hasLeft, string leftPrefix, string? leftLink)
        => SetParts(hasLeft, leftPrefix, leftLink, false, null, null, null);

    /// <summary>Установить правую часть только.</summary>
    public void SetParts(bool hasRight, string rightPrefix, string? rightLink, string? rightPlain)
        => SetParts(false, string.Empty, null, hasRight, rightPrefix, rightLink, rightPlain);

    public void RefreshAccentAppearance() => RebuildSegments();

    private void OnSizeChanged(object sender, SizeChangedEventArgs e) => UpdateLayoutMode();

    private void RebuildSegments()
    {
        CustomText.Inlines.Clear();
        if (_hasLeft)
        {
            AppendPrefix(CustomText, _leftPrefix);
            AppendLink(CustomText, _leftLink, CustomLinkClicked);
        }

        UnsavedText.Inlines.Clear();
        if (_hasRight)
        {
            AppendPrefix(UnsavedText, _rightPrefix);
            if (!string.IsNullOrEmpty(_rightLink))
                AppendLink(UnsavedText, _rightLink, UnsavedLinkClicked);
            else if (!string.IsNullOrEmpty(_rightPlain))
                UnsavedText.Inlines.Add(new Run { Text = _rightPlain });
        }
    }

    private void UpdateLayoutMode()
    {
        CustomText.Visibility = _hasLeft ? Visibility.Visible : Visibility.Collapsed;
        UnsavedRow.Visibility = _hasRight ? Visibility.Visible : Visibility.Collapsed;

        TrailingHost.Content = _hasRight ? TrailingContent : null;

        var hasContent = _hasLeft || _hasRight;
        var activeCount = (_hasLeft ? 1 : 0) + (_hasRight ? 1 : 0);

        var stack = activeCount >= 2 && ActualWidth > 0 && ActualWidth < StackBreakpoint;
        if (stack)
        {
            LeftColumn.Width = StarLength;
            RightColumn.Width = ZeroLength;

            var row = 0;
            if (_hasLeft)
            {
                Grid.SetColumn(CustomText, 0);
                Grid.SetRow(CustomText, row);
                Grid.SetColumnSpan(CustomText, 2);
                row++;
            }

            if (_hasRight)
            {
                Grid.SetColumn(UnsavedRow, 0);
                Grid.SetRow(UnsavedRow, row);
                Grid.SetColumnSpan(UnsavedRow, 2);
            }
            return;
        }

        Grid.SetRow(CustomText, 0);
        Grid.SetColumnSpan(CustomText, 1);
        Grid.SetRow(UnsavedRow, 0);
        Grid.SetColumnSpan(UnsavedRow, 1);
        Grid.SetColumn(CustomText, 0);
        Grid.SetColumn(UnsavedRow, _hasLeft ? 1 : 0);
    }

    private static void AppendPrefix(TextBlock target, string prefix)
    {
        if (!string.IsNullOrEmpty(prefix))
            target.Inlines.Add(new Run { Text = prefix });
    }

    private static void AppendLink(TextBlock target, string linkText, EventHandler? handler)
    {
        if (string.IsNullOrEmpty(linkText))
            return;

        var link = new Hyperlink { Foreground = AccentSubtitleBrushService.ResolveAccent() };
        link.Inlines.Add(new Run { Text = linkText, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold });
        if (handler is not null)
            link.Click += (_, _) => handler.Invoke(target, EventArgs.Empty);
        target.Inlines.Add(link);
    }
}