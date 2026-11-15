namespace GlassSettings.Services;

/// <summary>Straight BGRA processing. Blur wraps on tiles and clamps on complete SVG canvases.</summary>
public static class ReflectionPixelEffects
{
    public static byte[] Resize(byte[] source, int sw, int sh, int width, int height, double zoom = 0)
    {
        if (zoom == 0 && sw == width && sh == height) return source;
        var result = new byte[checked(width * height * 4)];
        for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
        {
            double sx = zoom > 0 ? (x + .5 - width / 2.0) / zoom + sw / 2.0 - .5 : (x + .5) * sw / width - .5;
            double sy = zoom > 0 ? (y + .5 - height / 2.0) / zoom + sh / 2.0 - .5 : (y + .5) * sh / height - .5;
            int ix = (int)Math.Floor(sx), iy = (int)Math.Floor(sy);
            double fx = sx - ix, fy = sy - iy, alpha = 0;
            double b = 0, g = 0, r = 0;
            for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
            {
                int px = ix + dx, py = iy + dy;
                if (zoom > 0 && (px < 0 || py < 0 || px >= sw || py >= sh)) continue;
                px = (px % sw + sw) % sw; py = (py % sh + sh) % sh;
                int index = (py * sw + px) * 4;
                double weight = (dx == 0 ? 1 - fx : fx) * (dy == 0 ? 1 - fy : fy);
                double a = source[index + 3] * weight;
                alpha += a; b += source[index] * a; g += source[index + 1] * a; r += source[index + 2] * a;
            }
            int target = (y * width + x) * 4;
            if (alpha > 0)
            {
                result[target] = (byte)Math.Clamp((int)Math.Round(b / alpha), 0, 255);
                result[target + 1] = (byte)Math.Clamp((int)Math.Round(g / alpha), 0, 255);
                result[target + 2] = (byte)Math.Clamp((int)Math.Round(r / alpha), 0, 255);
                result[target + 3] = (byte)Math.Clamp((int)Math.Round(alpha), 0, 255);
            }
        }
        return result;
    }

    public static void ApplyAlpha(byte[] pixels, int width, int height, ReflectionRenderSettings settings, bool wrap)
    {
        int radius = settings.Softness;
        if (radius > 0)
        {
            var intermediate = new byte[pixels.Length];
            void Pass(byte[] input, byte[] output, bool horizontal)
            {
                int count = radius * 2 + 1;
                for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                {
                    int alpha = 0, b = 0, g = 0, r = 0;
                    for (int d = -radius; d <= radius; d++)
                    {
                        int sx = horizontal ? x + d : x, sy = horizontal ? y : y + d;
                        sx = wrap ? (sx % width + width) % width : Math.Clamp(sx, 0, width - 1);
                        sy = wrap ? (sy % height + height) % height : Math.Clamp(sy, 0, height - 1);
                        int index = (sy * width + sx) * 4, a = input[index + 3];
                        alpha += a; b += input[index] * a; g += input[index + 1] * a; r += input[index + 2] * a;
                    }
                    int target = (y * width + x) * 4;
                    output[target] = alpha == 0 ? (byte)0 : (byte)((b + alpha / 2) / alpha);
                    output[target + 1] = alpha == 0 ? (byte)0 : (byte)((g + alpha / 2) / alpha);
                    output[target + 2] = alpha == 0 ? (byte)0 : (byte)((r + alpha / 2) / alpha);
                    output[target + 3] = (byte)((alpha + count / 2) / count);
                }
            }
            // Filter premultiplied colors as well as alpha to avoid dark fringes on Lite SVG.
            Pass(pixels, intermediate, horizontal: true);
            Pass(intermediate, pixels, horizontal: false);
        }
        // Fixed-seed multiplicative alpha grain: preserves paired colors, transparent
        // areas and tile repetition. Zero noise is byte-for-byte unchanged.
        int grainSize = Math.Clamp(settings.NoiseSize, 1, 32);
        for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
        {
            // Hash grain coordinates, not individual pixels: each square fraction
            // has one attenuation. A texture's resulting cell is repeated as-is.
            uint seed;
            unchecked
            {
                seed = (uint)(x / grainSize) * 0x85EBCA6Bu
                    ^ (uint)(y / grainSize) * 0xC2B2AE35u ^ 0x9E3779B9u;
                seed ^= seed >> 16; seed *= 0x7FEB352Du;
                seed ^= seed >> 15; seed *= 0x846CA68Bu;
                seed ^= seed >> 16;
            }
            int i = (y * width + x) * 4 + 3;
            int attenuation = 100 - (int)(seed % 101) * settings.NoisePercent / 100;
            pixels[i] = (byte)((pixels[i] * attenuation * settings.OpacityPercent + 5000) / 10000);
        }
    }
}
