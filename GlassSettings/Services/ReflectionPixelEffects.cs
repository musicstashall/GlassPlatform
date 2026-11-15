namespace GlassSettings.Services;

/// <summary>Straight BGRA processing. Blur wraps on tiles and clamps on complete SVG canvases.</summary>
public static class ReflectionPixelEffects
{
    private static readonly Lazy<double[]> NoiseGains = new(CalibrateNoise);

    // Match RMS visible contrast on a neutral substrate at full density. With
    // source-over, contrast is proportional to alpha * grain, hence grain^4.
    // Attenuate stronger types only, never amplify or clip weaker distributions.
    private static double[] CalibrateNoise()
    {
        var contrast = new double[4];
        for (int type = 0; type < 4; type++)
        {
            double sum = 0;
            for (int y = 0; y < 96; y++)
            for (int x = 0; x < 96; x++)
            {
                double g = Grain(x, y, 96, 96, type, 1, false);
                sum += g * g * g * g;
            }
            contrast[type] = Math.Sqrt(sum / (96 * 96));
        }
        double target = contrast.Min();
        return contrast.Select(value => value > 0 ? target / value : 1).ToArray();
    }

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
        if (!settings.Enabled) return;
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
        // Bipolar luminance grain, independent of source RGB. Both light and dark
        // grains cover transparent areas; near-neutral samples fade out instead of
        // adding a uniform grey veil. Composite straight BGRA using source-over.
        double density = Math.Clamp(settings.NoiseDensity, 0, 100) / 100.0;
        for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
        {
            int i = (y * width + x) * 4 + 3;
            double grain = settings.NoisePercent == 0 || density == 0 ? 0
                : Grain(x, y, width, height, settings.NoiseType, density, wrap);
            double noiseAlpha = Math.Abs(grain) * settings.NoisePercent / 100.0
                * .35 * (grain == 0 ? 1 : NoiseGains.Value[Math.Clamp(settings.NoiseType, 0, 3)]);
            double luminance = 127.5 * (1 + grain);
            double sourceAlpha = pixels[i] / 255.0;
            double combined = noiseAlpha + sourceAlpha * (1 - noiseAlpha);
            if (noiseAlpha > 0 && combined > 0)
            {
                double Mix(byte source, double noise) =>
                    (noise * noiseAlpha + source * sourceAlpha * (1 - noiseAlpha)) / combined;
                pixels[i - 3] = (byte)Math.Clamp((int)Math.Round(Mix(pixels[i - 3], luminance)), 0, 255);
                pixels[i - 2] = (byte)Math.Clamp((int)Math.Round(Mix(pixels[i - 2], luminance)), 0, 255);
                pixels[i - 1] = (byte)Math.Clamp((int)Math.Round(Mix(pixels[i - 1], luminance)), 0, 255);
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

    private static double Grain(int x, int y, int width, int height, int type, double density, bool wrap)
    {
        // Fixed spatial scales: density selects grains; it never enlarges them.
        const int size = 2;
        if (type == 3) return Hash(x / 2, y / 2, 991) < density
            ? 2 * Hash(x / 2, y / 2) - 1 : 0;
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
            double L(int cx, int cy)
            {
                if (H(cx, cy, 991) >= density) return 0;
                if (type != 1) return 2 * H(cx, cy) - 1;
                // Box–Muller Gaussian: moderate grains dominate, high contrast is
                // rare. Separate hash channels for radius and phase.
                double u = Math.Max(1e-9, H(cx, cy, 101));
                double z = Math.Sqrt(-2 * Math.Log(u)) * Math.Cos(2 * Math.PI * H(cx, cy, 211));
                return Math.Clamp(z / 2.5, -1, 1);
            }
            if (dots)
            {
                double value = 0;
                // Jittered round grains, including neighbours across tile boundaries.
                for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                {
                    if (H(ix + dx, iy + dy, 991) >= density) continue;
                    double cx = dx + .25 + .5 * H(ix + dx, iy + dy);
                    double cy = dy + .25 + .5 * H(ix + dx, iy + dy, 1);
                    double radius = .38 + .12 * H(ix + dx, iy + dy, 2);
                    double coverage = 0;
                    // Four subpixel samples give small dots stable coverage instead
                    // of letting most of them fall between pixel sample positions.
                    for (int sy = 0; sy < 2; sy++)
                    for (int sx = 0; sx < 2; sx++)
                    {
                        double ox = fx + (sx == 0 ? -.25 : .25) / spacing - cx;
                        double oy = fy + (sy == 0 ? -.25 : .25) / spacing - cy;
                        double t = Math.Clamp((radius - Math.Sqrt(ox * ox + oy * oy)) * spacing + .5, 0, 1);
                        coverage += t * t * (3 - 2 * t) / 4;
                    }
                    double sign = H(ix + dx, iy + dy, 3) < .5 ? -1 : 1;
                    double contrast = .5 + .5 * H(ix + dx, iy + dy, 4);
                    double mark = sign * contrast * coverage;
                    if (Math.Abs(mark) > Math.Abs(value)) value = mark;
                }
                return value;
            }
            // Smoothstep interpolation, rather than constant square fractions.
            fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
            double top = L(ix, iy) * (1 - fx) + L(ix + 1, iy) * fx;
            double bottom = L(ix, iy + 1) * (1 - fx) + L(ix + 1, iy + 1) * fx;
            return top * (1 - fy) + bottom * fy;
        }
        return type switch
        {
            1 => .55 * Sample(size, 11) + .30 * Sample(Math.Max(1, size / 2.0), 29)
                + .15 * Sample(Math.Max(1, size / 4.0), 47),
            2 => Sample(3, 71, dots: true),
            _ => Sample(size, 0)
        };
    }
}
