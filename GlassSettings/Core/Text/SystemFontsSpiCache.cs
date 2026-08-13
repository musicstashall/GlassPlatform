using GlassSettings.Services;

namespace GlassSettings.Core.Text;

/// <summary>Кэш SPI_GET при старте GS (нативный NONCLIENTMETRICSW).</summary>
internal static class SystemFontsSpiCache
{
    private static byte[]? _nonClientMetrics;
    private static byte[]? _iconTitleLogFont;
    private static int _nonClientMetricsByteSize;

    public static bool HasNonClientMetrics => _nonClientMetrics is not null;
    public static bool HasIconTitleLogFont => _iconTitleLogFont is not null;
    public static int NonClientMetricsByteSize => _nonClientMetricsByteSize;

    /// <summary>SPI_GET один раз при старте GlassSettings.</summary>
    public static void LoadAtStartup()
    {
        var ncmOk = NativeHostInterop.LoadSystemFontsSpiCache();

        _nonClientMetricsByteSize = ncmOk
            ? TryCopyCachedNcm(out _nonClientMetrics)
            : 0;

        var icon = new byte[SystemFontsLogFont.BinarySize];
        var iconOk = NativeHostInterop.TryCopyCachedIconLogFont(icon);
        _iconTitleLogFont = iconOk ? icon : null;
    }

    public static bool TryReadAllFontsFromCache(SystemFontsLogFont[] result)
    {
        if (result.Length < SystemFontsCatalog.SlotCount)
            return false;

        if (_nonClientMetrics is not null)
        {
            const int captionOffset = sizeof(int) + sizeof(int) * 5;
            for (var i = 0; i < 5; i++)
            {
                var offset = captionOffset + i * SystemFontsLogFont.BinarySize;
                if (_nonClientMetrics.Length >= offset + SystemFontsLogFont.BinarySize)
                    result[i] = SystemFontsLogFont.FromBytes(_nonClientMetrics.AsSpan(offset, SystemFontsLogFont.BinarySize));
            }
        }

        if (_iconTitleLogFont is not null)
            result[5] = SystemFontsLogFont.FromBytes(_iconTitleLogFont);

        return _nonClientMetrics is not null || _iconTitleLogFont is not null;
    }

    private static int TryCopyCachedNcm(out byte[]? buffer)
    {
        var scratch = new byte[512];
        if (!NativeHostInterop.TryCopyCachedNonClientMetrics(scratch))
        {
            buffer = null;
            return 0;
        }

        var cbSize = BitConverter.ToInt32(scratch, 0);
        if (cbSize <= 0 || cbSize > scratch.Length)
            cbSize = scratch.Length;

        buffer = new byte[cbSize];
        Buffer.BlockCopy(scratch, 0, buffer, 0, cbSize);
        _nonClientMetricsByteSize = cbSize;
        return cbSize;
    }
}
