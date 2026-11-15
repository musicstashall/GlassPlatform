namespace GlassSettings.Services;

using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;

public static class ReflectionSettingsDialog
{
    public static async Task<ReflectionRenderSettings?> ShowAsync(string source)
    {
        var initial = ReflectionRenderState.Settings;
        var scale = Number("Reflection_SettingScale", 25, 200, initial.ScalePercent);
        var opacity = Number("Reflection_SettingOpacity", 0, 100, initial.OpacityPercent);
        var softness = Number("Reflection_SettingSoftness", 0, 3, initial.Softness);
        var noise = Number("Reflection_SettingNoise", 0, 100, initial.NoisePercent);
        var noiseSize = Number("Reflection_SettingNoiseSize", 1, 32, initial.NoiseSize);
        var preview = new Image { Height = 180, Stretch = Stretch.Uniform };
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap, Visibility = Visibility.Collapsed };
        var reset = new Button { Content = GlassSettingsResources.Get("Reflection_Reset") };
        var body = new StackPanel { Spacing = 12 };
        body.Children.Add(new TextBlock
        {
            Text = Path.GetFileNameWithoutExtension(source),
            TextTrimming = TextTrimming.CharacterEllipsis,
            Opacity = 0.7
        });
        body.Children.Add(new Border { Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 144, 144, 144)), Child = preview });
        body.Children.Add(Row("Reflection_SettingScale", scale));
        body.Children.Add(Row("Reflection_SettingOpacity", opacity));
        body.Children.Add(Row("Reflection_SettingSoftness", softness));
        body.Children.Add(Row("Reflection_SettingNoise", noise));
        body.Children.Add(Row("Reflection_SettingNoiseSize", noiseSize));
        body.Children.Add(reset); body.Children.Add(error);
        int revision = 0;
        bool closed = false;
        ReflectionRenderSettings Values() => new(
            double.IsFinite(scale.Value) ? (int)scale.Value : 100,
            double.IsFinite(opacity.Value) ? (int)opacity.Value : 100,
            double.IsFinite(softness.Value) ? (int)softness.Value : 0,
            double.IsFinite(noise.Value) ? (int)noise.Value : 0,
            double.IsFinite(noiseSize.Value) ? (int)noiseSize.Value : 1);
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
        noiseSize.ValueChanged += (_, _) => _ = RefreshAsync();
        reset.Click += (_, _) => { scale.Value = 100; opacity.Value = 100; softness.Value = 0; noise.Value = 0; noiseSize.Value = 1; };
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

    private static Grid Row(string label, NumberBox number)
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
