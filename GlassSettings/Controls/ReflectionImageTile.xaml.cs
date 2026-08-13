using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;

namespace GlassSettings.Controls;

public sealed partial class ReflectionImageTile : UserControl
{
    public static readonly DependencyProperty FilePathProperty =
        DependencyProperty.Register(
            nameof(FilePath),
            typeof(string),
            typeof(ReflectionImageTile),
            new PropertyMetadata(string.Empty));

    public static readonly DependencyProperty ImageProperty =
        DependencyProperty.Register(
            nameof(Image),
            typeof(ImageSource),
            typeof(ReflectionImageTile),
            new PropertyMetadata(null, OnImageChanged));

    public static readonly DependencyProperty TooltipTextProperty =
        DependencyProperty.Register(
            nameof(TooltipText),
            typeof(string),
            typeof(ReflectionImageTile),
            new PropertyMetadata(string.Empty, OnTooltipTextChanged));

    public static readonly DependencyProperty IsSelectedProperty =
        DependencyProperty.Register(
            nameof(IsSelected),
            typeof(bool),
            typeof(ReflectionImageTile),
            new PropertyMetadata(false, OnIsSelectedChanged));

    public ReflectionImageTile()
    {
        InitializeComponent();
        Loaded += (_, _) => UpdateSelectionVisualState();
        RootBorder.ContextRequested += OnRootContextRequested;
    }

    public string FilePath
    {
        get => (string)GetValue(FilePathProperty);
        set => SetValue(FilePathProperty, value);
    }

    public ImageSource? Image
    {
        get => (ImageSource?)GetValue(ImageProperty);
        set => SetValue(ImageProperty, value);
    }

    public string TooltipText
    {
        get => (string)GetValue(TooltipTextProperty);
        set => SetValue(TooltipTextProperty, value);
    }

    public bool IsSelected
    {
        get => (bool)GetValue(IsSelectedProperty);
        set => SetValue(IsSelectedProperty, value);
    }

    private static void OnImageChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ReflectionImageTile tile)
            tile.ApplyImage(e.NewValue as ImageSource);
    }

    private static void OnTooltipTextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not ReflectionImageTile tile)
            return;

        var text = e.NewValue as string ?? string.Empty;
        GlassSettingsToolTipService.SetToolTip(
            tile.RootBorder,
            string.IsNullOrEmpty(text) ? null : text);
    }

    private static void OnIsSelectedChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ReflectionImageTile tile)
            tile.UpdateSelectionVisualState();
    }

    private void OnRootContextRequested(UIElement sender, ContextRequestedEventArgs args)
    {
        if (!args.TryGetPosition(RootBorder, out var position))
            return;

        var flyout = new MenuFlyout();
        var deleteItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Attach_MenuDelete")
        };
        deleteItem.Click += (_, _) =>
        {
            if (Tag is IThemeImageTileModel model)
                model.RequestDelete();
        };
        flyout.Items.Add(deleteItem);
        flyout.ShowAt(RootBorder, position);
        args.Handled = true;
    }

    private void ApplyImage(ImageSource? source)
    {
        if (source is not null)
        {
            PreviewImage.Source = source;
            PreviewImage.Visibility = Visibility.Visible;
            PreviewFallback.Visibility = Visibility.Collapsed;
            return;
        }

        PreviewImage.Source = null;
        PreviewImage.Visibility = Visibility.Collapsed;
        PreviewFallback.Visibility = Visibility.Visible;
    }

    private void UpdateSelectionVisualState()
    {
        SelectionBorder.Visibility = IsSelected ? Visibility.Visible : Visibility.Collapsed;
        SelectionMark.Visibility = IsSelected ? Visibility.Visible : Visibility.Collapsed;
        VisualStateManager.GoToState(this, IsSelected ? "Active" : "Inactive", true);
    }

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "PointerOver", true);

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "Normal", true);
}
