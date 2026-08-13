namespace GlassSettings.Core.Text;

/// <summary>Константы системных шрифтов WindowMetrics (эталон страницы «Текст»).</summary>
public static class SystemFontsCatalog
{
    public const int SlotCount = 6;

    public const string RegistrySubKey = @"Control Panel\Desktop\WindowMetrics";

    public static readonly string[] RegistryValueNames =
    [
        "CaptionFont",
        "SmCaptionFont",
        "MenuFont",
        "StatusFont",
        "MessageFont",
        "IconFont"
    ];

    /// <summary>tmschema TMT ids для GetThemeSysFont.</summary>
    public static readonly int[] ThemeMetricIds = [801, 802, 803, 804, 805, 806];

    public static int IndexOfValueName(string valueName)
    {
        for (var i = 0; i < RegistryValueNames.Length; i++)
        {
            if (string.Equals(RegistryValueNames[i], valueName, StringComparison.OrdinalIgnoreCase))
                return i;
        }

        return -1;
    }
}
