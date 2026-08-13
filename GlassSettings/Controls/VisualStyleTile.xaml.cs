using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;

namespace GlassSettings.Controls;

public sealed partial class VisualStyleTile : UserControl
{
    public static readonly DependencyProperty PreviewImagePathProperty =
        DependencyProperty.Register(
            nameof(PreviewImagePath),
            typeof(string),
            typeof(VisualStyleTile),
            new PropertyMetadata(null, OnPreviewImagePathChanged));

    public static readonly DependencyProperty TooltipTextProperty =
        DependencyProperty.Register(
            nameof(TooltipText),
            typeof(string),
            typeof(VisualStyleTile),
            new PropertyMetadata(string.Empty, OnTooltipTextChanged));

    public static readonly DependencyProperty IsActiveProperty =
        DependencyProperty.Register(
            nameof(IsActive),
            typeof(bool),
            typeof(VisualStyleTile),
            new PropertyMetadata(false, OnIsActiveChanged));

    public static readonly DependencyProperty IsLargeProperty =
        DependencyProperty.Register(
            nameof(IsLarge),
            typeof(bool),
            typeof(VisualStyleTile),
            new PropertyMetadata(false, OnIsLargeChanged));

    public static readonly DependencyProperty EntryProperty =
        DependencyProperty.Register(
            nameof(Entry),
            typeof(VisualStyleThemeEntry),
            typeof(VisualStyleTile),
            new PropertyMetadata(null, OnEntryChanged));

    public event EventHandler<VisualStyleThemeEntry>? ThemeClicked;
    public event EventHandler<VisualStyleThemeEntry>? DeleteRequested;

    public VisualStyleTile()
    {
        InitializeComponent();
        Loaded += (_, _) => UpdateActiveVisualState();
        UpdateTileSize();
    }

    public string? PreviewImagePath
    {
        get => (string?)GetValue(PreviewImagePathProperty);
        set => SetValue(PreviewImagePathProperty, value);
    }

    public string TooltipText
    {
        get => (string)GetValue(TooltipTextProperty);
        set => SetValue(TooltipTextProperty, value);
    }

    public bool IsActive
    {
        get => (bool)GetValue(IsActiveProperty);
        set => SetValue(IsActiveProperty, value);
    }

    public bool IsLarge
    {
        get => (bool)GetValue(IsLargeProperty);
        set => SetValue(IsLargeProperty, value);
    }

    public VisualStyleThemeEntry? Entry
    {
        get => (VisualStyleThemeEntry?)GetValue(EntryProperty);
        set => SetValue(EntryProperty, value);
    }

    private static void OnEntryChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not VisualStyleTile tile)
            return;

        tile.PreviewChrome.ApplyTheme(e.NewValue as VisualStyleThemeEntry);
    }

    private static void OnIsLargeChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is VisualStyleTile tile)
            tile.UpdateTileSize();
    }

    private static void OnTooltipTextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not VisualStyleTile tile)
            return;

        var text = e.NewValue as string ?? string.Empty;
        GlassSettingsToolTipService.SetToolTip(
            tile.RootBorder,
            string.IsNullOrEmpty(text) ? null : text);
    }

    private static void OnPreviewImagePathChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is VisualStyleTile tile)
            tile.ApplyPreviewImage(e.NewValue as string);
    }

    private static void OnIsActiveChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is VisualStyleTile tile)
            tile.UpdateActiveVisualState();
    }

    private void UpdateTileSize()
    {
        RootBorder.Width = IsLarge ? 380 : 168;
        RootBorder.Height = IsLarge ? 214 : 94;
        PreviewHost.Height = IsLarge ? 214 : 94;
    }

    private void ApplyPreviewImage(string? path)
    {
        var bitmap = ThemeImageUriHelper.TryCreateBitmap(path);
        if (bitmap is not null)
        {
            PreviewImage.Source = bitmap;
            PreviewImage.Visibility = Visibility.Visible;
            PreviewFallback.Visibility = Visibility.Collapsed;
            return;
        }

        PreviewImage.Source = null;
        PreviewImage.Visibility = Visibility.Collapsed;
        PreviewFallback.Visibility = Visibility.Visible;
    }

    private void UpdateActiveVisualState()
    {
        VisualStateManager.GoToState(this, IsActive ? "Active" : "Inactive", true);
    }

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "PointerOver", true);

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, IsActive ? "Active" : "Normal", true);

    private void RootBorder_OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        if (Entry is not null)
            ThemeClicked?.Invoke(this, Entry);
    }

    private void RootBorder_OnRightTapped(object sender, RightTappedRoutedEventArgs e)
    {
        var entry = Entry;
        if (entry is null)
            return;

        e.Handled = true;
        var canDelete = ThemeDeleteService.CanDelete(entry);
        var deleteItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Themes_DeleteMenu"),
            IsEnabled = canDelete
        };
        if (canDelete)
        {
            deleteItem.Click += (_, _) =>
            {
                if (Entry is not null)
                    DeleteRequested?.Invoke(this, Entry);
            };
        }

        var flyout = new MenuFlyout();
        flyout.Items.Add(deleteItem);
        flyout.ShowAt(RootBorder, e.GetPosition(RootBorder));
    }
}
