using System.Globalization;

namespace GlassSettings.Services;

/// <summary>Форматирование даты и размера для details-режима file picker.</summary>
public static class ExecutableFilePickerDisplayHelper
{
    public static string FormatDateModified(DateTime? utc) =>
        utc is { } value
            ? value.ToLocalTime().ToString("g", CultureInfo.CurrentCulture)
            : string.Empty;

    public static string FormatSize(long? bytes)
    {
        if (bytes is null or < 0)
            return string.Empty;

        var size = (double)bytes.Value;
        string[] units = ["B", "KB", "MB", "GB", "TB"];
        var unit = 0;
        while (size >= 1024 && unit < units.Length - 1)
        {
            size /= 1024;
            unit++;
        }

        var format = unit == 0 ? "0" : size >= 100 ? "0" : size >= 10 ? "0.0" : "0.00";
        return $"{size.ToString(format, CultureInfo.CurrentCulture)} {units[unit]}";
    }

    public static string GetTypeSortKey(ExecutableFilePickerItem item) =>
        item.IsFolder
            ? "\u0000"
            : item.TypeLabel ?? Path.GetExtension(item.FullPath);
}
