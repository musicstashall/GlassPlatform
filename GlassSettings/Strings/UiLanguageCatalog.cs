using System.Globalization;
using GlassSettings.Core.Registry;
using Microsoft.Windows.ApplicationModel.Resources;

namespace GlassSettings.Strings;

/// <summary>Каталог языков UI из PRI (по <c>Strings/&lt;tag&gt;/Resources.resw</c>).</summary>
public static partial class UiLanguageCatalog
{
    private static IReadOnlyList<string>? _resolved;

    /// <summary>BCP-47 теги, для которых в PRI есть строковые ресурсы.</summary>
    public static IReadOnlyList<string> GetLanguageTags() =>
        _resolved ??= ResolveAvailable();

    public static void InvalidateCache() => _resolved = null;

    public static string GetDisplayName(string bcp47Tag) =>
        CultureInfo.GetCultureInfo(bcp47Tag).NativeName;

    public static bool IsSupported(string? tag) =>
        !AppUiLanguage.IsSystem(tag)
        && GetLanguageTags().Contains(tag!, StringComparer.OrdinalIgnoreCase);

    private static IReadOnlyList<string> ResolveAvailable()
    {
        var result = new List<string>(EmbeddedLanguageTags.Count);
        foreach (var tag in EmbeddedLanguageTags)
        {
            if (HasResourceBundle(tag))
                result.Add(tag);
        }

        return result;
    }

    private static bool HasResourceBundle(string tag)
    {
        try
        {
            var manager = new ResourceManager();
            var context = manager.CreateResourceContext();
            context.QualifierValues["Language"] = tag;
            var map = manager.MainResourceMap.GetSubtree("Resources");
            var candidate = map.TryGetValue(ProbeResourceKey, context);
            return !string.IsNullOrEmpty(candidate?.ValueAsString);
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Ключ, который есть во всех локалях.</summary>
    private const string ProbeResourceKey = "AppTitle";
}
