namespace GlassSettings.Services;

using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

public static class ReflectionSettingsDialog
{
    // Navigation state belongs to this process, not the generated PNG recipe.
    private static int _selectedGroupIndex;

    public static async Task<ReflectionRenderSettings?> ShowAsync(string source)
    {
        var initial = ReflectionRenderState.Settings;
        var scale = Number("Reflection_SettingScale", 25, 200, initial.ScalePercent);
        var opacity = Number("Reflection_SettingOpacity", 0, 100, initial.OpacityPercent);
        var softness = Number("Reflection_SettingSoftness", 0, 3, initial.Softness);
        var noise = Number("Reflection_SettingNoise", 0, 100, initial.NoisePercent);
        var noiseDensity = Number("Reflection_SettingNoiseDensity", 0, 100, initial.NoiseDensity);
        var noiseType = new ComboBox { Width = 160, SelectedIndex = -1 };
        foreach (var key in new[] { "Soft", "Film", "Dots", "Blocks" })
            noiseType.Items.Add(GlassSettingsResources.Get("Reflection_Noise" + key));
        noiseType.SelectedIndex = initial.NoiseType;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(noiseType,
            GlassSettingsResources.Get("Reflection_SettingNoiseType"));
        var enabled = new ToggleSwitch
        {
            Header = GlassSettingsResources.Get("Reflection_EnableProcessing"), IsOn = initial.Enabled
        };
        var preview = new Image { Height = 144, Stretch = Stretch.Uniform };
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
        var reset = new Button { Content = GlassSettingsResources.Get("Reflection_Reset") };
        var body = new StackPanel { Spacing = 8, Margin = new Thickness(0, 12, 0, 0) };
        body.Children.Add(new TextBlock
        {
            Text = Path.GetFileNameWithoutExtension(source),
            TextTrimming = TextTrimming.CharacterEllipsis,
            Opacity = 0.7
        });
        body.Children.Add(PreviewCard(preview));
        body.Children.Add(enabled);
        var imageGroup = new StackPanel { Spacing = 12 };
        imageGroup.Children.Add(Row("Reflection_SettingScale", scale));
        imageGroup.Children.Add(Row("Reflection_SettingOpacity", opacity));
        imageGroup.Children.Add(Row("Reflection_SettingSoftness", softness));
        var noiseGroup = new StackPanel { Spacing = 12 };
        noiseGroup.Children.Add(Row("Reflection_SettingNoiseType", noiseType));
        noiseGroup.Children.Add(Row("Reflection_SettingNoise", noise));
        noiseGroup.Children.Add(Row("Reflection_SettingNoiseDensity", noiseDensity));
        var groups = new Pivot { Height = 176 };
        groups.Resources.Add(typeof(PivotHeaderItem), new Style(typeof(PivotHeaderItem))
        {
            BasedOn = (Style)Application.Current.Resources["GlassSettingsPivotHeaderItemStyle"]
        });
        groups.Items.Add(new PivotItem { Header = GlassSettingsResources.Get("Reflection_GroupImage"), Content = imageGroup });
        groups.Items.Add(new PivotItem { Header = GlassSettingsResources.Get("Reflection_GroupNoise"), Content = noiseGroup });
        groups.SelectedIndex = Math.Clamp(_selectedGroupIndex, 0, groups.Items.Count - 1);
        groups.SelectionChanged += (_, args) =>
        {
            if (ReferenceEquals(args.OriginalSource, groups) && groups.SelectedIndex >= 0)
                _selectedGroupIndex = groups.SelectedIndex;
        };
        groups.IsEnabled = enabled.IsOn;
        body.Children.Add(groups);
        body.Children.Add(reset); body.Children.Add(error);
        int revision = 0;
        bool closed = false;
        ReflectionRenderSettings Values() => new(
            double.IsFinite(scale.Value) ? (int)scale.Value : 100,
            double.IsFinite(opacity.Value) ? (int)opacity.Value : 100,
            double.IsFinite(softness.Value) ? (int)softness.Value : 0,
            double.IsFinite(noise.Value) ? (int)noise.Value : 0,
            1, Math.Max(0, noiseType.SelectedIndex),
            double.IsFinite(noiseDensity.Value) ? (int)noiseDensity.Value : 50, enabled.IsOn);
        async Task RefreshAsync()
        {
            int current = ++revision;
            await Task.Delay(180);
            if (closed || current != revision) return;
            try
            {
                var png = await ReflectionTextureService.RenderConfiguredAsync(source, 384, 216, Values());
                if (closed || current != revision) return;
                using var stream = new MemoryStream(png);
                using var random = stream.AsRandomAccessStream();
                var bitmap = new BitmapImage();
                await bitmap.SetSourceAsync(random);
                if (closed || current != revision) return;
                preview.Source = bitmap; error.Text = ""; error.Visibility = Visibility.Collapsed;
            }
            catch (Exception exception)
            {
                if (!closed && current == revision)
                {
                    error.Text = exception.Message;
                    error.Visibility = Visibility.Visible;
                }
            }
        }
        scale.ValueChanged += (_, _) => _ = RefreshAsync();
        opacity.ValueChanged += (_, _) => _ = RefreshAsync();
        softness.ValueChanged += (_, _) => _ = RefreshAsync();
        noise.ValueChanged += (_, _) => _ = RefreshAsync();
        noiseDensity.ValueChanged += (_, _) => _ = RefreshAsync();
        noiseType.SelectionChanged += (_, _) => _ = RefreshAsync();
        enabled.Toggled += (_, _) => { groups.IsEnabled = enabled.IsOn; _ = RefreshAsync(); };
        reset.Click += (_, _) => { enabled.IsOn = true; scale.Value = 100; opacity.Value = 100; softness.Value = 0; noise.Value = 0; noiseDensity.Value = 50; noiseType.SelectedIndex = 0; };
        _ = RefreshAsync();
        try
        {
            var result = await HostedDialogService.ShowAsync(new HostedDialogSpec
            {
                Title = GlassSettingsResources.Get("Reflection_Configure"), Body = HostedDialogLayout.WrapBodyWithContentPadding(body),
                MinWidth = 420, MaxWidth = 480, MaxHeight = 620,
                Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(GlassSettingsResources.Get("CmdApply"))
            });
            return result == HostedDialogResult.Primary ? Values().Normalize() : null;
        }
        finally { closed = true; revision++; }
    }

    private static Border PreviewCard(Image preview)
    {
        // Checkerboard is UI-only: never included in the encoded reflection PNG.
        var content = new Grid { Height = 144 };
        var white = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 255, 255, 255));
        var blueGrey = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 153, 170, 187));
        for (int x = 0; x < 32; x++) content.ColumnDefinitions.Add(new ColumnDefinition());
        for (int y = 0; y < 12; y++) content.RowDefinitions.Add(new RowDefinition());
        for (int y = 0; y < 12; y++)
        for (int x = 0; x < 32; x++)
        {
            var square = new Border
            {
                Background = (x + y) % 2 == 0 ? white : blueGrey,
                Opacity = 0.07 // Only the substrate is faint; leave the image untouched.
            };
            Grid.SetColumn(square, x); Grid.SetRow(square, y);
            content.Children.Add(square);
        }
        preview.Stretch = Stretch.UniformToFill;
        Grid.SetColumnSpan(preview, 32); Grid.SetRowSpan(preview, 12);
        content.Children.Add(preview);
        content.SizeChanged += (_, _) =>
        {
            var visual = Microsoft.UI.Xaml.Hosting.ElementCompositionPreview.GetElementVisual(content);
            var geometry = visual.Compositor.CreateRoundedRectangleGeometry();
            geometry.Size = new System.Numerics.Vector2((float)content.ActualWidth, (float)content.ActualHeight);
            geometry.CornerRadius = new System.Numerics.Vector2(8);
            visual.Clip = visual.Compositor.CreateGeometricClip(geometry);
        };
        return new Border
        {
            CornerRadius = new CornerRadius(8), BorderThickness = new Thickness(1),
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorDefaultBrush"],
            Child = content
        };
    }

    private static NumberBox Number(string label, double minimum, double maximum, double value)
    {
        var number = new NumberBox
        {
            Minimum = minimum, Maximum = maximum, Value = value, Width = 128,
            SmallChange = 1, LargeChange = 10, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(number, GlassSettingsResources.Get(label));
        return number;
    }

    private static Grid Row(string label, FrameworkElement number)
    {
        var row = new Grid { ColumnSpacing = 16 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        row.Children.Add(new TextBlock
        {
            Text = GlassSettingsResources.Get(label), VerticalAlignment = VerticalAlignment.Center,
            TextWrapping = TextWrapping.Wrap
        });
        Grid.SetColumn(number, 1);
        row.Children.Add(number);
        return row;
    }
}
