using Microsoft.Win32;

namespace GlassSettings.Core.Text;

/// <summary>Чтение/запись LOGFONT в HKCU\Control Panel\Desktop\WindowMetrics.</summary>
public static class WindowMetricsFontRegistry
{
    public static SystemFontsLogFont[] ReadAllFromRegistryOrSpi()
    {
        var result = new SystemFontsLogFont[SystemFontsCatalog.SlotCount];
        var anyFromRegistry = false;

        using (var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(SystemFontsCatalog.RegistrySubKey, false))
        {
            if (key is not null)
            {
                for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
                {
                    var raw = key.GetValue(SystemFontsCatalog.RegistryValueNames[i]) as byte[];
                    if (raw is { Length: >= 92 })
                    {
                        result[i] = SystemFontsLogFont.FromBytes(raw);
                        anyFromRegistry = true;
                    }
                }
            }
        }

        if (anyFromRegistry)
        {
            FillMissingFromSpi(result);
            return result;
        }

        return ReadAllFromSpi();
    }

    public static SystemFontsLogFont ReadSlot(int index)
    {
        if (index is < 0 or >= SystemFontsCatalog.SlotCount)
            return SystemFontsLogFont.Default;

        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(SystemFontsCatalog.RegistrySubKey, false);
        var raw = key?.GetValue(SystemFontsCatalog.RegistryValueNames[index]) as byte[];
        if (raw is { Length: >= 92 })
            return SystemFontsLogFont.FromBytes(raw);

        var spi = ReadAllFromSpi();
        return spi[index];
    }

    public static bool WriteSlot(int index, in SystemFontsLogFont font)
    {
        if (index is < 0 or >= SystemFontsCatalog.SlotCount)
            return false;

        using var key = global::Microsoft.Win32.Registry.CurrentUser.CreateSubKey(SystemFontsCatalog.RegistrySubKey, true);
        if (key is null)
            return false;

        var valueName = SystemFontsCatalog.RegistryValueNames[index];
        key.SetValue(valueName, SystemFontsLogFont.ToBytes(font), RegistryValueKind.Binary);

        var readBack = key.GetValue(valueName) as byte[];
        return readBack is { Length: >= SystemFontsLogFont.BinarySize };
    }

    public static bool WriteAll(IReadOnlyList<SystemFontsLogFont> fonts)
    {
        if (fonts.Count < SystemFontsCatalog.SlotCount)
            return false;

        using var key = global::Microsoft.Win32.Registry.CurrentUser.CreateSubKey(SystemFontsCatalog.RegistrySubKey, true);
        if (key is null)
            return false;

        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
        {
            key.SetValue(
                SystemFontsCatalog.RegistryValueNames[i],
                SystemFontsLogFont.ToBytes(fonts[i]),
                RegistryValueKind.Binary);
        }

        return true;
    }

    /// <summary>Запись только WorkCustom-слотов (остальные значения реестра не трогаем).</summary>
    public static bool WriteCustomSlots(SystemFontsModel model)
    {
        using var key = global::Microsoft.Win32.Registry.CurrentUser.CreateSubKey(SystemFontsCatalog.RegistrySubKey, true);
        if (key is null)
            return false;

        for (var i = 0; i < SystemFontsCatalog.SlotCount; i++)
        {
            if (!model.WorkCustom[i])
                continue;

            key.SetValue(
                SystemFontsCatalog.RegistryValueNames[i],
                SystemFontsLogFont.ToBytes(model.WorkLf[i]),
                RegistryValueKind.Binary);
        }

        return true;
    }

    public static byte[]? TryReadRawBytes(string valueName)
    {
        using var key = global::Microsoft.Win32.Registry.CurrentUser.OpenSubKey(SystemFontsCatalog.RegistrySubKey, false);
        return key?.GetValue(valueName) as byte[];
    }

    private static SystemFontsLogFont[] ReadAllFromSpi()
    {
        var result = new SystemFontsLogFont[SystemFontsCatalog.SlotCount];
        if (SystemFontsSpiCache.TryReadAllFontsFromCache(result))
            return result;

        return result;
    }

    private static void FillMissingFromSpi(SystemFontsLogFont[] result)
    {
        var spi = ReadAllFromSpi();
        for (var i = 0; i < result.Length; i++)
        {
            if (string.IsNullOrEmpty(result[i].lfFaceName))
                result[i] = spi[i];
        }
    }
}
