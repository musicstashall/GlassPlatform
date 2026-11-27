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
            new PropertyMetadata(string.Empty, OnFilePathChanged));

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

    // Не Style: это имя занято FrameworkElement.Style, и XAML привязал бы базовое свойство.
    public static readonly DependencyProperty ImageStyleProperty =
        DependencyProperty.Register(
            nameof(ImageStyle),
            typeof(ThemeImageStyle),
            typeof(ReflectionImageTile),
            new PropertyMetadata(ThemeImageStyle.Catalog, OnImageStyleChanged));

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

    /// <summary>Откуда брать изображение: каталог, системное отражение или материал.</summary>
    public ThemeImageStyle ImageStyle
    {
        get => (ThemeImageStyle)GetValue(ImageStyleProperty);
        set => SetValue(ImageStyleProperty, value);
    }

    public bool IsSelected
    {
        get => (bool)GetValue(IsSelectedProperty);
        set => SetValue(IsSelectedProperty, value);
    }

    private int _imageRevision;

    // FilePath, Image and Style arrive as separate bindings and a recycled tile may keep
    // the previous picture; ApplyImage therefore always re-resolves from FilePath + Style.
    private static void OnFilePathChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ReflectionImageTile tile)
            tile.ApplyImage(tile.Image);
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

    private static void OnImageStyleChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is ReflectionImageTile tile)
            tile.ApplyImage(tile.Image);
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

    private async void ApplyImage(ImageSource? source)
    {
        var revision = ++_imageRevision;
        if (ImageStyle == ThemeImageStyle.DefaultReflection)
        {
            // Ресурс OpenGlass извлекается один раз; плитка только показывает готовое.
            source = ThemeImageCatalog.TryGetDefaultReflectionImage();
        }
        else if (ImageStyle == ThemeImageStyle.DefaultMaterial)
        {
            // Системный материал декодируется один раз и кешируется в памяти.
            source = await ThemeImageCatalog.TryGetDefaultMaterialImageAsync();
            if (revision != _imageRevision) return;
        }
        else if (SvgReflectionRenderer.IsPattern(FilePath) ||
            ReflectionTextureService.IsTexture(FilePath))
        {
            PreviewImage.Source = null;
            PreviewImage.Visibility = Visibility.Collapsed;
            PreviewFallback.Visibility = Visibility.Visible;
            // Cached per file version: revisiting a tab does not rasterize again.
            source = await ReflectionImageCache.GetTileAsync(FilePath);
            // A recycled tile may now refer to another image or path.
            if (revision != _imageRevision) return;
        }
        else if (source is null)
        {
            // Каталог не декодирует обложки: чтение идёт из общего кеша, а байты
            // читаются вне UI-потока, чтобы плитка не подвешивала прокрутку.
            source = await ReflectionImageCache.LoadAsync(FilePath);
            if (revision != _imageRevision) return;
        }
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
