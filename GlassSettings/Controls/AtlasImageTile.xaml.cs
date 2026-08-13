using GlassSettings.Pages.Dwm;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace GlassSettings.Controls;

public sealed partial class AtlasImageTile : UserControl
{
    public static readonly DependencyProperty FilePathProperty =
        DependencyProperty.Register(
            nameof(FilePath),
            typeof(string),
            typeof(AtlasImageTile),
            new PropertyMetadata(string.Empty));

    public static readonly DependencyProperty ImageProperty =
        DependencyProperty.Register(
            nameof(Image),
            typeof(ImageSource),
            typeof(AtlasImageTile),
            new PropertyMetadata(null, OnImageChanged));

    public static readonly DependencyProperty PreviewStateProperty =
        DependencyProperty.Register(
            nameof(PreviewState),
            typeof(AtlasPreviewState),
            typeof(AtlasImageTile),
            new PropertyMetadata(AtlasPreviewState.Loading, OnPreviewStateChanged));

    public static readonly DependencyProperty TooltipTextProperty =
        DependencyProperty.Register(
            nameof(TooltipText),
            typeof(string),
            typeof(AtlasImageTile),
            new PropertyMetadata(string.Empty, OnTooltipTextChanged));

    public static readonly DependencyProperty IsSelectedProperty =
        DependencyProperty.Register(
            nameof(IsSelected),
            typeof(bool),
            typeof(AtlasImageTile),
            new PropertyMetadata(false, OnIsSelectedChanged));

    public AtlasImageTile()
    {
        InitializeComponent();
        CheckerboardImage.Source = TransparencyCheckerboard.CreateImage(178, 92);
        Loaded += (_, _) =>
        {
            UpdateSelectionVisualState();
            ApplyPreviewVisual();
        };
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

    public AtlasPreviewState PreviewState
    {
        get => (AtlasPreviewState)GetValue(PreviewStateProperty);
        set => SetValue(PreviewStateProperty, value);
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
        if (d is AtlasImageTile tile)
            tile.ApplyPreviewVisual();
    }

    private static void OnPreviewStateChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is AtlasImageTile tile)
            tile.ApplyPreviewVisual();
    }

    private static void OnTooltipTextChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is not AtlasImageTile tile)
            return;

        var text = e.NewValue as string ?? string.Empty;
        GlassSettingsToolTipService.SetToolTip(
            tile.RootBorder,
            string.IsNullOrEmpty(text) ? null : text);
    }

    private static void OnIsSelectedChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is AtlasImageTile tile)
            tile.UpdateSelectionVisualState();
    }

    private void ApplyPreviewVisual()
    {
        if (PreviewState == AtlasPreviewState.Failed)
        {
            PreviewContentHost.Visibility = Visibility.Collapsed;
            ErrorHost.Visibility = Visibility.Visible;
            return;
        }

        ErrorHost.Visibility = Visibility.Collapsed;
        PreviewContentHost.Visibility = Visibility.Visible;

        if (Image is not null)
        {
            PreviewImage.Source = Image;
            PreviewImage.Visibility = Visibility.Visible;
            return;
        }

        PreviewImage.Source = null;
        PreviewImage.Visibility = Visibility.Collapsed;
    }

    private void UpdateSelectionVisualState()
    {
        SelectionBorder.Visibility = IsSelected ? Visibility.Visible : Visibility.Collapsed;
        SelectionMark.Visibility = IsSelected ? Visibility.Visible : Visibility.Collapsed;
        VisualStateManager.GoToState(this, IsSelected ? "Active" : "Inactive", true);
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

    private void RootBorder_OnPointerEntered(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "PointerOver", true);

    private void RootBorder_OnPointerExited(object sender, PointerRoutedEventArgs e) =>
        VisualStateManager.GoToState(this, "Normal", true);
}
