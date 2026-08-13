using GlassSettings.Core.Registry;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class DwmHiveScopeBanner : UserControl
{
    public static readonly DependencyProperty HiveProperty =
        DependencyProperty.Register(
            nameof(Hive),
            typeof(DwmRegistryHive),
            typeof(DwmHiveScopeBanner),
            new PropertyMetadata(DwmRegistryHive.Hkcu, OnBannerStateChanged));

    public DwmHiveScopeBanner()
    {
        InitializeComponent();
        UpdateText();
    }

    public DwmRegistryHive Hive
    {
        get => (DwmRegistryHive)GetValue(HiveProperty);
        set => SetValue(HiveProperty, value);
    }

    private static void OnBannerStateChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is DwmHiveScopeBanner banner)
            banner.UpdateText();
    }

    private void UpdateText()
    {
        Visibility = Hive == DwmRegistryHive.Hklm
            ? Visibility.Visible
            : Visibility.Collapsed;
        if (Hive != DwmRegistryHive.Hklm)
            return;

        var body = GlassSettingsResources.Get("Dwm_HklmInfoBlockMessage");

        InfoBlock.Title = string.Empty;
        InfoBlock.Message = body;
    }
}
