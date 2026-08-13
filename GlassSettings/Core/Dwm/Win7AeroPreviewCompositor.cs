namespace GlassSettings.Core.Dwm;

/// <summary>
/// Win7 blur/glow-слои миниатюры. Tint — flat SolidColorBrush в UI (паритет пикера).
/// blurBalance — linear 0..1, как AeroColorizationEffect.hlsl / GlassRenderer.
/// </summary>
public static class Win7AeroPreviewCompositor
{
    private const float GrayR = 0.2126f;
    private const float GrayG = 0.7152f;
    private const float GrayB = 0.0722f;

    /// <summary>
    /// blurBalance / afterglowBalance — 0..1 (linear из реестра).
    /// blurredBgra — BGRA, полный гаусс по BlurDeviation.
    /// </summary>
    public static byte[] BuildBlurStage(
        ReadOnlySpan<byte> blurredBgra,
        int width,
        int height,
        float blurBalance,
        float afterglowBalance)
    {
        blurBalance = Math.Clamp(blurBalance, 0f, 1f);
        afterglowBalance = Math.Clamp(afterglowBalance, 0f, 1f);

        var pixelCount = width * height;
        var blurOut = new byte[pixelCount * 4];

        // blurBalance=0 → вклад backdrop = (0,0,0), opaque (как OG, alpha=1 в шейдере).
        if (blurBalance <= 0f)
        {
            for (var i = 3; i < blurOut.Length; i += 4)
                blurOut[i] = 255;

            return blurOut;
        }

        for (var i = 0; i < pixelCount; i++)
        {
            var offset = i * 4;
            var b = blurredBgra[offset] / 255f;
            var g = blurredBgra[offset + 1] / 255f;
            var r = blurredBgra[offset + 2] / 255f;

            WriteBgra(
                blurOut,
                offset,
                b * blurBalance,
                g * blurBalance,
                r * blurBalance);
        }

        return blurOut;
    }

    /// <summary>
    /// Afterglow-слой: (1 − luminance(full gaussian)) × afterglow RGB (уже × balance).
    /// OG literal: luminance × afterglow; превью — darkWeight (темнее фон → ярче glow).
    /// Premultiplied BGRA: при нулевом вкладе pixel прозрачен.
    /// </summary>
    public static byte[] BuildAfterglowStage(
        ReadOnlySpan<byte> blurredBgra,
        int width,
        int height,
        float afterglowR,
        float afterglowG,
        float afterglowB)
    {
        afterglowR = Math.Clamp(afterglowR, 0f, 1f);
        afterglowG = Math.Clamp(afterglowG, 0f, 1f);
        afterglowB = Math.Clamp(afterglowB, 0f, 1f);

        var pixelCount = width * height;
        var glowOut = new byte[pixelCount * 4];

        for (var i = 0; i < pixelCount; i++)
        {
            var offset = i * 4;
            var b = blurredBgra[offset] / 255f;
            var g = blurredBgra[offset + 1] / 255f;
            var r = blurredBgra[offset + 2] / 255f;
            var luminance = (r * GrayR) + (g * GrayG) + (b * GrayB);
            var darkWeight = 1f - Math.Clamp(luminance, 0f, 1f);

            var br = darkWeight * afterglowB;
            var gr = darkWeight * afterglowG;
            var rr = darkWeight * afterglowR;
            var a = Math.Clamp(Math.Max(br, Math.Max(gr, rr)), 0f, 1f);

            WriteBgraPremul(glowOut, offset, br, gr, rr, a);
        }

        return glowOut;
    }

    private static void WriteBgra(byte[] buffer, int offset, float b, float g, float r)
    {
        buffer[offset] = ToByte(b);
        buffer[offset + 1] = ToByte(g);
        buffer[offset + 2] = ToByte(r);
        buffer[offset + 3] = 255;
    }

    private static void WriteBgraPremul(byte[] buffer, int offset, float b, float g, float r, float a)
    {
        buffer[offset] = ToByte(b);
        buffer[offset + 1] = ToByte(g);
        buffer[offset + 2] = ToByte(r);
        buffer[offset + 3] = ToByte(a);
    }

    private static byte ToByte(float value)
    {
        if (value <= 0f)
            return 0;
        if (value >= 1f)
            return 255;
        return (byte)Math.Round(value * 255f);
    }
}
