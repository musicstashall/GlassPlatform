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
        // A separate source-over noise layer must also cover transparent pixels.
        // Infer its straight RGB from visible source pixels, never hidden RGB at alpha=0.
        long weight = 0, blue = 0, green = 0, red = 0;
        if (settings.NoisePercent > 0)
        {
            for (int i = 0; i < pixels.Length; i += 4)
            {
                int a = pixels[i + 3];
                weight += a; blue += pixels[i] * a;
                green += pixels[i + 1] * a; red += pixels[i + 2] * a;
            }
        }
        double nb = weight == 0 ? 0 : blue / (double)weight;
        double ng = weight == 0 ? 0 : green / (double)weight;
        double nr = weight == 0 ? 0 : red / (double)weight;
        int grainSize = Math.Clamp(settings.NoiseSize, 1, 32);
        for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
        {
            int i = (y * width + x) * 4 + 3;
            double grain = settings.NoisePercent == 0 ? 0
                : Grain(x, y, width, height, grainSize, settings.NoiseType, wrap);
            double noiseAlpha = grain * settings.NoisePercent / 100.0;
            double sourceAlpha = pixels[i] / 255.0;
            double combined = noiseAlpha + sourceAlpha * (1 - noiseAlpha);
            if (noiseAlpha > 0 && combined > 0)
            {
                double Mix(byte source, double noise) =>
                    (noise * noiseAlpha + source * sourceAlpha * (1 - noiseAlpha)) / combined;
                pixels[i - 3] = (byte)Math.Clamp((int)Math.Round(Mix(pixels[i - 3], nb)), 0, 255);
                pixels[i - 2] = (byte)Math.Clamp((int)Math.Round(Mix(pixels[i - 2], ng)), 0, 255);
                pixels[i - 1] = (byte)Math.Clamp((int)Math.Round(Mix(pixels[i - 1], nr)), 0, 255);
            }
            pixels[i] = (byte)Math.Clamp((int)Math.Round(combined * 255 * settings.OpacityPercent / 100.0), 0, 255);
        }
    }

    private static double Hash(int x, int y, int salt = 0)
    {
        unchecked
        {
            uint seed = (uint)x * 0x85EBCA6Bu ^ (uint)y * 0xC2B2AE35u
                ^ (uint)salt * 0x27D4EB2Du ^ 0x9E3779B9u;
            seed ^= seed >> 16; seed *= 0x7FEB352Du;
            seed ^= seed >> 15; seed *= 0x846CA68Bu;
            seed ^= seed >> 16;
            return seed / (double)uint.MaxValue;
        }
    }

    private static double Grain(int x, int y, int width, int height, int size, int type, bool wrap)
    {
        if (type == 3) return Hash(x / size, y / size); // Explicit block effect only.
        // Integer lattice periods keep the smooth fields periodic on repeated tiles.
        double Sample(double spacing, int salt, bool dots = false)
        {
            int nx = Math.Max(1, (int)Math.Ceiling(width / spacing));
            int ny = Math.Max(1, (int)Math.Ceiling(height / spacing));
            double px = wrap ? x * (double)nx / width : x / spacing;
            double py = wrap ? y * (double)ny / height : y / spacing;
            int ix = (int)Math.Floor(px), iy = (int)Math.Floor(py);
            double fx = px - ix, fy = py - iy;
            double H(int cx, int cy, int channel = 0) => Hash(
                wrap ? (cx % nx + nx) % nx : cx,
                wrap ? (cy % ny + ny) % ny : cy, salt + channel);
            if (dots)
            {
                double value = 0;
                // Jittered round grains, including neighbours across tile boundaries.
                for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    double cx = dx + .2 + .6 * H(ix + dx, iy + dy);
                    double cy = dy + .2 + .6 * H(ix + dx, iy + dy, 1);
                    double radius = .22 + .22 * H(ix + dx, iy + dy, 2);
                    double distance = Math.Sqrt((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy));
                    double t = Math.Clamp(1 - distance / radius, 0, 1);
                    value = Math.Max(value, t * t * (3 - 2 * t));
                }
                return value;
            }
            // Smoothstep interpolation, rather than constant square fractions.
            fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
            double top = H(ix, iy) * (1 - fx) + H(ix + 1, iy) * fx;
            double bottom = H(ix, iy + 1) * (1 - fx) + H(ix + 1, iy + 1) * fx;
            return top * (1 - fy) + bottom * fy;
        }
        return type switch
        {
            1 => .55 * Sample(size, 11) + .30 * Sample(Math.Max(1, size / 2.0), 29)
                + .15 * Sample(Math.Max(1, size / 4.0), 47),
            2 => Sample(size, 71, dots: true),
            _ => Sample(size, 0)
        };
    }
}
