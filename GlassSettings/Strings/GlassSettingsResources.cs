using Microsoft.Windows.ApplicationModel.Resources;

namespace GlassSettings.Strings;

/// <summary>Единая точка доступа к строкам UI. MRT Core (WinApp SDK), без литералов в коде.</summary>
public static class GlassSettingsResources
{
    private static ResourceLoader? _loader;
    private static ResourceManager? _manager;
    private static ResourceContext? _explicitContext;
    private static string? _explicitLanguageTag;
    private static readonly Dictionary<(string Key, string Language), string> LanguageStringCache = new();
    private static readonly Dictionary<string, ResourceContext> LanguageContexts = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Явно выбранный язык интерфейса или null, если используется язык системы.</summary>
    public static string? ActiveLanguageTag => _explicitLanguageTag;

    private static ResourceLoader Loader => _loader ??= new ResourceLoader();

    /// <summary>Явный язык (BCP-47) или null — системный по MRT.</summary>
    public static void SetLanguageOverride(string? languageTag)
    {
        _explicitLanguageTag = languageTag;
        ResetLoader();
    }

    /// <summary>Сброс кэша после смены языка.</summary>
    public static void ResetLoader()
    {
        _loader = null;
        _manager = null;
        _explicitContext = null;
        LanguageStringCache.Clear();
        LanguageContexts.Clear();
    }

    /// <summary>Строка из заданной локали независимо от текущего языка интерфейса.</summary>
    public static string GetForLanguage(string key, string languageTag)
    {
        if (string.IsNullOrWhiteSpace(key) || string.IsNullOrWhiteSpace(languageTag))
            return string.Empty;

        var cacheKey = (key, languageTag);
        if (LanguageStringCache.TryGetValue(cacheKey, out var cached))
            return cached;

        _manager ??= new ResourceManager();
        if (!LanguageContexts.TryGetValue(languageTag, out var context))
        {
            context = _manager.CreateResourceContext();
            context.QualifierValues["Language"] = languageTag;
            LanguageContexts[languageTag] = context;
        }

        var map = _manager.MainResourceMap.GetSubtree("Resources");
        var value = map.TryGetValue(key, context)?.ValueAsString ?? string.Empty;
        LanguageStringCache[cacheKey] = value;
        return value;
    }

    public static string Get(string key)
    {
        var value = !string.IsNullOrEmpty(_explicitLanguageTag)
            ? GetExplicit(key)
            : Loader.GetString(key);

        if (string.IsNullOrEmpty(value))
            throw new InvalidOperationException($"Missing string resource: {key}");

        return value;
    }

    private static string GetExplicit(string key)
    {
        _manager ??= new ResourceManager();
        _explicitContext ??= _manager.CreateResourceContext();
        _explicitContext.QualifierValues["Language"] = _explicitLanguageTag!;

        var map = _manager.MainResourceMap.GetSubtree("Resources");
        var candidate = map.TryGetValue(key, _explicitContext);
        return candidate?.ValueAsString ?? string.Empty;
    }

    public static string Format(string key, params object[] args) =>
        string.Format(Get(key), args);

    /// <summary>Пункты списка комбобокса: одна строка ресурса, значения через «;» по порядку индексов.</summary>
    public static string[] GetSemicolonList(string key) =>
        Get(key).Split(';', StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries);

    /// <summary>Русская (и en-US) плюрализация: 1 / 2–4 / 5+.</summary>
    public static string CountForm(int count, string keyOne, string keyFew, string keyMany)
    {
        var key = count switch
        {
            1 => keyOne,
            >= 2 and <= 4 => keyFew,
            _ => keyMany
        };
        return count == 1 ? Get(keyOne) : Format(key, count);
    }
}
