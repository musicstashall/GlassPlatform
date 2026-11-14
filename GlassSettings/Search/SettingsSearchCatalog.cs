using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using GlassSettings.Strings;

namespace GlassSettings.Search;

/// <summary>Небольшой двуязычный индекс: тексты ресурсов → обычный маршрут страницы.</summary>
public static class SettingsSearchCatalog
{
    private const string Russian = "ru-RU";
    private const string English = "en-US";
    private static readonly string[] ExcludedKeyMarkers =
    [
        "Desc", "Description", "Subtitle", "Action", "Tip", "Limit", "Info", "Message", "Body",
        "Format", "Placeholder", "Empty", "Error", "Notice", "Status", "Progress", "Failed",
        "Saved", "Path", "Tooltip", "Preview", "Warn", "Dialog", "Picker", "Version", "Installed",
        "Wizard", "SignatureActivated", "SignatureDeactivated"
    ];
    private static readonly Lazy<IReadOnlyList<IndexedEntry>> Index = new(BuildIndex);

    private static readonly SearchEntry[] Entries =
    [
        new("system", "System_RowShellExtension", "NavSystem", "system",
            ["System_RowShellExtension", "System_UpdateIntervalTitle", "System_RowOpenGlass", "System_RowGlassPlatform", "System_UpdateDaily", "System_UpdateWeekly"],
            ["system", "settings", "система", "настройки", "shell extension", "контекстное меню"]),
        new("processes", "Home_RowProcessList", "NavAttach", "attach",
            ["Home_RowProcessList", "Home_RowViewAllProcesses", "NavAttach"],
            ["process", "processes", "attach", "процесс", "процессы", "внедрение"]),
        new("glass", "NavGlass", "Breadcrumb_GlassSettings", "glass",
            ["Glass_GeneralTitle", "Glass_HubGeneral", "Glass_HubGeneralDesc"],
            ["glass", "стекло", "настройки стекла"]),
        new("root-windows", "Breadcrumb_RootSwca", "Breadcrumb_GlassSettings", "root-windows",
            ["Glass_HubRoot", "Flyout_EffectScopeRootTargets", "Glass_LabelEffectType", "Glass_LabelCornerRadius", "Flyout_Corners", "Flyout_Shadow"],
            ["root windows", "application windows", "окна приложений", "корневые окна", "blur", "размытие", "acrylic", "акрил", "mica", "transparency", "прозрачность"]),
        new("popup", "Breadcrumb_PopupWindows", "Breadcrumb_GlassSettings", "popup",
            ["Glass_HubPopup", "Glass_PopupDropDown", "Glass_PopupMenu", "Flyout_Animation", "Flyout_AnimationDesc", "Flyout_BorderColor", "Flyout_Corners", "Flyout_EffectType", "Flyout_GradientColor", "Flyout_Shadow", "ParamLabel_FadeInTime", "ParamLabel_FadeOutTime", "ParamLabel_PopInTime", "ParamLabel_PopInStyle"],
            ["popup", "pop-up", "menu", "dropdown", "drop down", "tooltip", "всплывающие окна", "меню", "подсказки", "анимация", "animation", "fade out", "fade in", "fadeouttime", "fadeintime", "blur", "размытие", "acrylic", "акрил", "mica", "opacity", "прозрачность", "corners", "углы", "shadow", "тень"]),
        new("taskbar", "Breadcrumb_Taskbar", "Breadcrumb_GlassSettings", "taskbar",
            ["Glass_HubTaskbar", "Flyout_EffectScopeTaskbarTarget", "Flyout_EffectType", "Flyout_Animation", "Flyout_BorderColor", "Flyout_Corners", "Flyout_GradientColor", "Flyout_Shadow", "Flyout_SeparateZones"],
            ["taskbar", "tray", "панель задач", "область уведомлений", "blur", "размытие", "acrylic", "акрил", "mica", "opacity", "прозрачность", "corners", "углы", "shadow", "тень"]),
        new("adjacent-windows", "Glass_AdjacentWindows", "Breadcrumb_Taskbar", "adjacent",
            ["Glass_AdjacentWindowsDesc", "Glass_Adjacent_TabPopup", "Glass_Adjacent_TabPreview", "Glass_Adjacent_TabMenu", "Glass_Adjacent_TabTooltip", "Glass_Adjacent_EffectDesc"],
            ["adjacent windows", "smoothed windows", "смежные окна", "popup", "preview", "menu", "tooltip"]),
        new("dwm", "NavDwm", "Breadcrumb_DwmSettings", "dwm",
            ["Dwm_HubColorization", "Dwm_HubGlass", "Dwm_HubTheme", "Dwm_System_GeneralTitle"],
            ["dwm", "openglass", "open glass", "композиция", "система"]),
        new("dwm-colorization", "Dwm_HubColorization", "Breadcrumb_DwmSettings", "dwm-colorization",
            ["Dwm_HubColorization", "Dwm_GlassTypeTitle", "Dwm_ActiveGlassColor", "Dwm_GlassColorWin7", "Dwm_CustomInactiveColor", "Dwm_AfterglowColor", "Dwm_BlurLevelTitle", "Dwm_BlurRadiusTitle", "Dwm_OpacityColorization", "Dwm_BaseColorSpoilerTitle", "Dwm_BaseOpacitySpoilerTitle", "Dwm_BaseOpacityActive", "Dwm_BaseOpacityInactive", "ParamLabel_GlassDrawThemeConstantAlpha"],
            ["colorization", "colorizationcolor", "glass color", "glass colour", "blur", "размытие", "цвет стекла", "прозрачность", "opacity", "alpha", "composition", "композиция", "acrylic", "акрил", "mica"]),
        new("dwm-glass", "Dwm_HubGlass", "Breadcrumb_DwmSettings", "dwm-glass",
            ["Dwm_HubGlass", "Dwm_AppearanceGeneral", "Dwm_AppearanceWindow", "Dwm_AppearanceCaption", "Dwm_BlurOptimization", "Dwm_UseDirect3DRenderer", "Dwm_GlassGeometryRadius", "Dwm_RoundRectProfileLabel", "Dwm_TextGlowMode", "Dwm_TextGlowSize", "Dwm_CaptionColorOverride"],
            ["glass appearance", "внешний вид стекла", "text glow", "свечение текста", "blur", "размытие", "shadow", "тень", "corners", "углы", "mica", "acrylic", "акрил", "enablecustomrendering", "custom rendering", "пользовательский рендеринг"]),
        new("dwm-theme", "Dwm_HubTheme", "Breadcrumb_DwmSettings", "dwm-theme",
            ["Dwm_HubTheme", "Dwm_ReflectionImage", "Dwm_ReflectionOpacity", "Dwm_ReflectionCollection", "Dwm_ReflectionParametersTitle", "Dwm_MaterialCollection", "Dwm_MaterialOpacity", "Dwm_ReflectionIntensity", "Dwm_ReflectionParallax", "Dwm_AtlasImage", "Dwm_AtlasCollection"],
            ["theme", "themes", "тема", "темы", "reflection", "отражение", "material", "материал", "atlas", "texture", "текстура"]),
        new("themes", "NavThemes", "Themes_CurrentSection", "themes",
            ["Themes_AllSection", "Themes_BackgroundCardTitle", "Themes_ColorCardTitle", "Themes_SignatureCardTitle", "Themes_ThemeFonts", "Home_RowVisualStyles", "Themes_RowGlassSmoothing"],
            ["themes", "visual styles", "тема", "темы", "визуальные стили"]),
        new("themes-background", "Themes_BackgroundCardTitle", "NavThemes", "themes-background",
            ["Themes_Bg_PersonalizeTitle", "Themes_Bg_ModeItems", "Themes_Bg_RecentImages", "Themes_Bg_ChoosePhoto", "Themes_Bg_ChooseFit", "Themes_Bg_SlideshowAlbum", "Themes_Bg_SlideshowInterval", "Themes_Bg_SlideshowShuffle", "Themes_BackgroundSpotlight", "Themes_BackgroundSolid"],
            ["background", "desktop background", "wallpaper", "обои", "фон рабочего стола", "слайд-шоу", "slideshow", "spotlight", "изображение"]),
        new("themes-colors", "Themes_ColorCardTitle", "NavThemes", "themes-colors",
            ["Themes_Colors_AccentModeItems", "Themes_Colors_AccentTitle", "Themes_Colors_AccentStartTaskbar", "Themes_Colors_AccentTitleBars", "Themes_Colors_AppsMode", "Themes_Colors_WindowsMode", "Themes_Colors_TransparencyTitle", "Themes_Colors_RecentColors"],
            ["colors", "colour", "accent", "акцент", "цвета", "прозрачность", "transparency"]),
        new("text", "NavText", "Text_RenderControlCardTitle", "text",
            ["Text_EffectsTitle", "Text_InkGammaBlendTitle", "Text_InkPseudoTitle", "Text_InkGammaLabel", "Text_InkBlendLabel", "Text_InkPseudoRadiusLabel", "Text_InkPseudoIntensityLabel", "Themes_SystemFontsTitle", "Themes_ApplySystemFonts"],
            ["text", "font", "fonts", "шрифт", "шрифты", "сглаживание", "text glow", "свечение текста", "gamma", "гамма"]),
    ];

    public static IReadOnlyList<SettingsSearchSuggestion> Search(string? query)
    {
        var normalizedQuery = Normalize(query ?? string.Empty);
        if (normalizedQuery.Length < 2)
            return [];

        var queryWords = normalizedQuery.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        var index = Index.Value;
        var exactMatches = FindMatches(index, queryWords, allowPrefix: false);
        var matches = exactMatches.Count > 0
            ? exactMatches
            : FindMatches(index, queryWords, allowPrefix: true);
        var activeLanguage = GetActiveLanguage();
        return matches
            .Select(item => new SettingsSearchSuggestion(
                item.Entry.Id,
                GetDisplayString(item.Entry.TitleKey, activeLanguage),
                GetDisplayString(item.Entry.LocationKey, activeLanguage),
                item.Entry.Destination,
                item.Entry.SearchText))
            .ToArray();
    }

    private static IReadOnlyList<IndexedEntry> BuildIndex()
    {
        var result = new List<IndexedEntry>(Entries.Length);
        foreach (var entry in Entries)
        {
            var localizedTitles = new[]
            {
                GlassSettingsResources.GetForLanguage(entry.TitleKey, Russian),
                GlassSettingsResources.GetForLanguage(entry.TitleKey, English),
                entry.Id
            };
            var texts = new List<string>(entry.ResourceKeys.Length * 2 + entry.Aliases.Length + 2);
            foreach (var key in entry.ResourceKeys
                         .Concat(SettingsSearchResourceKeys.ForDestination(entry.Destination))
                         .Append(entry.TitleKey)
                         .Append(entry.LocationKey)
                         .Distinct(StringComparer.Ordinal))
            {
                if (key != entry.TitleKey && key != entry.LocationKey && !IsSearchableResourceKey(key))
                    continue;

                AddIfNotEmpty(texts, GlassSettingsResources.GetForLanguage(key, Russian));
                AddIfNotEmpty(texts, GlassSettingsResources.GetForLanguage(key, English));
            }
            texts.AddRange(entry.Aliases);

            result.Add(new IndexedEntry(
                entry.Id,
                entry.TitleKey,
                entry.LocationKey,
                entry.Destination,
                Normalize(string.Join(" ", localizedTitles)),
                Normalize(string.Join(" ", texts))));
        }

        return result;
    }

    private static List<(IndexedEntry Entry, int Score)> FindMatches(
        IReadOnlyList<IndexedEntry> index,
        string[] queryWords,
        bool allowPrefix)
    {
        var matches = new List<(IndexedEntry Entry, int Score)>();
        foreach (var entry in index)
        {
            if (!WordsMatch(entry.SearchText, queryWords, allowPrefix))
                continue;

            var titleMatch = WordsMatch(entry.TitleSearchText, queryWords, allowPrefix);
            matches.Add((entry, (allowPrefix ? 4 : 12) + queryWords.Length + (titleMatch ? 20 : 0)));
        }

        return matches
            .OrderByDescending(item => item.Score)
            .ThenBy(item => item.Entry.Id, StringComparer.Ordinal)
            .ToList();
    }

    private static bool WordsMatch(string text, string[] queryWords, bool allowPrefix)
    {
        var indexedWords = text.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        return queryWords.All(queryWord => indexedWords.Any(indexedWord =>
            allowPrefix
                ? indexedWord.StartsWith(queryWord, StringComparison.Ordinal)
                : string.Equals(indexedWord, queryWord, StringComparison.Ordinal)));
    }

    private static string GetDisplayString(string key, string language)
    {
        var value = GlassSettingsResources.GetForLanguage(key, language);
        if (!string.IsNullOrWhiteSpace(value))
            return value;

        value = GlassSettingsResources.GetForLanguage(key, English);
        return string.IsNullOrWhiteSpace(value) ? key : value;
    }

    private static string GetActiveLanguage()
    {
        var language = GlassSettingsResources.ActiveLanguageTag;
        if (!string.IsNullOrWhiteSpace(language))
            return language.StartsWith("ru", StringComparison.OrdinalIgnoreCase) ? Russian : English;

        return CultureInfo.CurrentUICulture.Name.StartsWith("ru", StringComparison.OrdinalIgnoreCase)
            ? Russian
            : English;
    }

    private static bool IsSearchableResourceKey(string key) =>
        !ExcludedKeyMarkers.Any(marker => key.Contains(marker, StringComparison.OrdinalIgnoreCase));

    private static void AddIfNotEmpty(List<string> values, string value)
    {
        if (!string.IsNullOrWhiteSpace(value))
            values.Add(value);
    }

    private static string Normalize(string value)
    {
        value = value.ToLowerInvariant().Replace('ё', 'е');
        var decomposed = value.Normalize(NormalizationForm.FormD);
        var builder = new StringBuilder(decomposed.Length);
        foreach (var character in decomposed)
        {
            if (CharUnicodeInfo.GetUnicodeCategory(character) == UnicodeCategory.NonSpacingMark)
                continue;

            builder.Append(char.IsLetterOrDigit(character) ? character : ' ');
        }

        return Regex.Replace(builder.ToString(), @"\s+", " ").Trim();
    }

    private sealed record SearchEntry(
        string Id,
        string TitleKey,
        string LocationKey,
        string Destination,
        string[] ResourceKeys,
        string[] Aliases);

    private sealed record IndexedEntry(
        string Id,
        string TitleKey,
        string LocationKey,
        string Destination,
        string TitleSearchText,
        string SearchText);
}
