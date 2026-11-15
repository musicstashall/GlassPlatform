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
        var preview = new Image { Width = 384, Height = 216, Stretch = Stretch.Uniform };
        var error = new TextBlock { TextWrapping = TextWrapping.Wrap };
        var reset = new Button { Content = GlassSettingsResources.Get("Reflection_Reset") };
        var body = new StackPanel { Spacing = 12 };
        body.Children.Add(new TextBlock { Text = Path.GetFileNameWithoutExtension(source) });
        body.Children.Add(new Border { Background = new SolidColorBrush(Windows.UI.Color.FromArgb(255, 144, 144, 144)), Child = preview });
        body.Children.Add(scale); body.Children.Add(opacity); body.Children.Add(softness);
        body.Children.Add(reset); body.Children.Add(error);
        int revision = 0;
        bool closed = false;
        ReflectionRenderSettings Values() => new(
            double.IsFinite(scale.Value) ? (int)scale.Value : 100,
            double.IsFinite(opacity.Value) ? (int)opacity.Value : 100,
            double.IsFinite(softness.Value) ? (int)softness.Value : 0);
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
                preview.Source = bitmap; error.Text = "";
            }
            catch (Exception exception)
            {
                if (!closed && current == revision) error.Text = exception.Message;
            }
        }
        scale.ValueChanged += (_, _) => _ = RefreshAsync();
        opacity.ValueChanged += (_, _) => _ = RefreshAsync();
        softness.ValueChanged += (_, _) => _ = RefreshAsync();
        reset.Click += (_, _) => { scale.Value = 100; opacity.Value = 100; softness.Value = 0; };
        _ = RefreshAsync();
        try
        {
            var accepted = await HostedConfirmationDialogService.ConfirmAsync(new HostedConfirmationSpec
            {
                Title = GlassSettingsResources.Get("Reflection_Configure"), Body = body,
                MinWidth = 460, MaxWidth = 520,
                Footer = HostedConfirmationDialogService.CreateCancelConfirmFooter(GlassSettingsResources.Get("CmdApply"))
            });
            return accepted ? Values().Normalize() : null;
        }
        finally { closed = true; revision++; }
    }

    private static NumberBox Number(string label, double minimum, double maximum, double value) => new()
    {
        Header = GlassSettingsResources.Get(label), Minimum = minimum, Maximum = maximum, Value = value,
        SmallChange = 1, LargeChange = 10, SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact
    };
}
