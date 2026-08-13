using System.Globalization;

namespace GlassSettings.Core.Registry;

/// <summary>Коды языка UI GlassSettings. Реестр хранит BCP-47; отсутствие ключа — System.</summary>
public static class AppUiLanguage
{
    /// <summary>Системный язык (ключ реестра отсутствует).</summary>
    public const string System = "";

    /// <summary>BCP-47: русский.</summary>
    public const string RuRu = "ru-RU";

    /// <summary>BCP-47: English (United States).</summary>
    public const string EnUs = "en-US";

    /// <summary>LANGID ru-RU (1049).</summary>
    public const ushort LangIdRuRu = 0x0419;

    /// <summary>LANGID en-US (1033).</summary>
    public const ushort LangIdEnUs = 0x0409;

    public static bool IsSystem(string? tag) => string.IsNullOrWhiteSpace(tag);

    /// <summary>Нормализованный BCP-47 или null для System.</summary>
    public static string? Normalize(string? tag)
    {
        if (IsSystem(tag))
            return null;

        return tag!.Trim();
    }

    public static ushort ToLangId(string bcp47Tag) =>
        (ushort)CultureInfo.GetCultureInfo(bcp47Tag).LCID;

    public static string FromLangId(ushort langId) => langId switch
    {
        LangIdRuRu => RuRu,
        LangIdEnUs => EnUs,
        _ => System
    };
}
