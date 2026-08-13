using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls;

public sealed partial class AppSettingsPanel : UserControl
{
    private bool _loading;

    public AppSettingsPanel()
    {
        InitializeComponent();
        Loaded += OnLoaded;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        RefreshLocalizedContent();
    }

    public void RefreshLocalizedContent()
    {
        ApplyLocalizedStrings();
        PopulateCombos();
        LoadFromRegistry();
    }

    public void ApplyLocalizedStrings()
    {
        ApplySettingsCardHeader(ThemeCard, "AppSettings_ThemeHeader", "AppSettings_ThemeDescription");
        ThemeCombo.Header = GlassSettingsResources.Get("AppSettings_ThemeModeLabel");

        ApplySettingsCardHeader(BackdropCard, "AppSettings_BackdropHeader", "AppSettings_BackdropDescription");
        BackdropCombo.Header = GlassSettingsResources.Get("AppSettings_BackdropTypeLabel");

        ApplySettingsCardHeader(BusyOverlayCard, "AppSettings_BusyOverlayHeader", "AppSettings_BusyOverlayDescription");
        BusyOverlaySwitch.OnContent = GlassSettingsResources.Get("AppSettings_BusyOverlayModeAcrylic");
        BusyOverlaySwitch.OffContent = GlassSettingsResources.Get("AppSettings_BusyOverlayModeDim");

        LanguageLabel.Text = GlassSettingsResources.Get("AppSettings_LanguageLabel");
        LanguageDescription.Text = GlassSettingsResources.Get("AppSettings_LanguageDescription");
    }

    private static void ApplySettingsCardHeader(
        CommunityToolkit.WinUI.Controls.SettingsCard card,
        string headerKey,
        string descriptionKey)
    {
        card.Header = CreateCardHeader(GlassSettingsResources.Get(headerKey));
        card.Description = CreateCardDescription(GlassSettingsResources.Get(descriptionKey));
    }

    private static TextBlock CreateCardHeader(string text) => new()
    {
        Text = text,
        Style = (Style)Application.Current.Resources["BodyStrongTextBlockStyle"]
    };

    private static TextBlock CreateCardDescription(string text) => new()
    {
        Text = text,
        Style = (Style)Application.Current.Resources["GlassSettingsSpoilerSecondaryTextStyle"],
        TextWrapping = TextWrapping.Wrap
    };

    private void PopulateCombos()
    {
        ThemeCombo.Items.Clear();
        AddThemeItem(AppThemeMode.Light, GlassSettingsResources.Get("AppSettings_ThemeLight"));
        AddThemeItem(AppThemeMode.Dark, GlassSettingsResources.Get("AppSettings_ThemeDark"));
        AddThemeItem(AppThemeMode.System, GlassSettingsResources.Get("AppSettings_ThemeSystem"));

        BackdropCombo.Items.Clear();
        AddBackdropItem(AppWindowBackdropMode.Acrylic, GlassSettingsResources.Get("AppSettings_BackdropAcrylic"));
        AddBackdropItem(AppWindowBackdropMode.Mica, GlassSettingsResources.Get("Effect_Mica"));
        AddBackdropItem(AppWindowBackdropMode.MicaAlt, GlassSettingsResources.Get("Effect_MicaAlt"));
        AddBackdropItem(AppWindowBackdropMode.System, GlassSettingsResources.Get("AppSettings_BackdropSystem"));

        LanguageCombo.Items.Clear();
        AddLanguageItem(null, UiLanguageService.GetDisplayName(null));
        foreach (var tag in UiLanguageCatalog.GetLanguageTags())
            AddLanguageItem(tag, UiLanguageCatalog.GetDisplayName(tag));
    }

    private void LoadFromRegistry()
    {
        _loading = true;
        try
        {
            SelectTheme(GlassSettingsRegistry.ReadAppTheme());
            SelectBackdrop(GlassSettingsRegistry.ReadWindowBackdrop());
            SelectLanguage(GlassSettingsRegistry.ReadAppUiLanguage());
            BusyOverlaySwitch.IsOn =
                GlassSettingsRegistry.ReadWindowBusyOverlayMode() == WindowBusyOverlayMode.AcrylicChrome;
        }
        finally
        {
            _loading = false;
        }
    }

    private void ThemeCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || ThemeCombo.SelectedItem is not ComboBoxItem { Tag: AppThemeMode mode })
            return;

        GlassSettingsRegistry.WriteAppTheme(mode);
        AppPreferencesCoordinator.ApplyFromRegistry();
    }

    private void BackdropCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || BackdropCombo.SelectedItem is not ComboBoxItem { Tag: AppWindowBackdropMode mode })
            return;

        GlassSettingsRegistry.WriteWindowBackdrop(mode);
        AppPreferencesCoordinator.ApplyFromRegistry();
    }

    private void LanguageCombo_OnSelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_loading || LanguageCombo.SelectedItem is not ComboBoxItem item)
            return;

        var languageTag = item.Tag as string;
        if (LanguageTagsEqual(languageTag, GlassSettingsRegistry.ReadAppUiLanguage()))
            return;

        UiLanguageService.WriteAndApply(languageTag);
    }

    private void BusyOverlaySwitch_OnToggled(object sender, RoutedEventArgs e)
    {
        if (_loading)
            return;

        var mode = BusyOverlaySwitch.IsOn
            ? WindowBusyOverlayMode.AcrylicChrome
            : WindowBusyOverlayMode.Dim;
        GlassSettingsRegistry.WriteWindowBusyOverlayMode(mode);
        AppPreferencesCoordinator.ApplyFromRegistry();
    }

    private void SelectTheme(AppThemeMode mode)
    {
        foreach (var item in ThemeCombo.Items)
        {
            if (item is ComboBoxItem { Tag: AppThemeMode tag } && tag == mode)
            {
                ThemeCombo.SelectedItem = item;
                return;
            }
        }
    }

    private void SelectBackdrop(AppWindowBackdropMode mode)
    {
        foreach (var item in BackdropCombo.Items)
        {
            if (item is ComboBoxItem { Tag: AppWindowBackdropMode tag } && tag == mode)
            {
                BackdropCombo.SelectedItem = item;
                return;
            }
        }
    }

    private void SelectLanguage(string? languageTag)
    {
        foreach (var item in LanguageCombo.Items)
        {
            if (item is not ComboBoxItem comboItem)
                continue;

            var tag = comboItem.Tag as string;
            if (!LanguageTagsEqual(tag, languageTag))
                continue;

            LanguageCombo.SelectedItem = comboItem;
            return;
        }
    }

    private static bool LanguageTagsEqual(string? left, string? right) =>
        AppUiLanguage.IsSystem(left) && AppUiLanguage.IsSystem(right)
        || string.Equals(left, right, StringComparison.OrdinalIgnoreCase);

    private void AddThemeItem(AppThemeMode mode, string label) =>
        ThemeCombo.Items.Add(new ComboBoxItem { Content = label, Tag = mode });

    private void AddBackdropItem(AppWindowBackdropMode mode, string label) =>
        BackdropCombo.Items.Add(new ComboBoxItem { Content = label, Tag = mode });

    private void AddLanguageItem(string? languageTag, string label) =>
        LanguageCombo.Items.Add(new ComboBoxItem { Content = label, Tag = languageTag });
}
