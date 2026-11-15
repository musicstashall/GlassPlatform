using GlassSettings.Services;

static void Check(bool value, string description)
{
    if (!value) throw new Exception(description);
    Console.WriteLine("PASS: " + description);
}

byte[] original = [160, 160, 160, 200, 160, 160, 160, 50];
var defaults = (byte[])original.Clone();
ReflectionPixelEffects.ApplyAlpha(defaults, 2, 1, new(), wrap: true);
Check(defaults.SequenceEqual(original), "default settings preserve pixels");
Check(ReflectionPixelEffects.Resize(defaults, 2, 1, 2, 1).SequenceEqual(original), "100% resize preserves pixels");
var faded = (byte[])original.Clone();
ReflectionPixelEffects.ApplyAlpha(faded, 2, 1, new(100, 50), wrap: true);
Check(faded[3] == 100 && faded[7] == 25 && faded[0] == 160, "opacity changes alpha, not color");
byte[] single = [160, 160, 160, 100];
ReflectionPixelEffects.ApplyAlpha(single, 1, 1, new(100, 100, 3), wrap: true);
Check(single.SequenceEqual(new byte[] { 160, 160, 160, 100 }), "blur radius larger than cell wraps safely");
byte[] edge = [160, 160, 160, 255, 0, 0, 0, 0, 0, 0, 0, 0];
ReflectionPixelEffects.ApplyAlpha(edge, 3, 1, new(100, 100, 1), wrap: true);
Check(edge[3] == 85 && edge[7] == 85 && edge[11] == 85, "toroidal blur crosses tile seam");
Check(edge[0] == 160 && edge[4] == 160 && edge[8] == 160, "premultiplied filtering avoids dark Lite fringes");
var scaled = ReflectionPixelEffects.Resize(new byte[] { 160, 160, 160, 255 }, 1, 1, 5, 5);
Check(scaled.Length == 100 && scaled.Where((_, i) => i % 4 == 3).All(a => a == 255), "resampling uniform cell remains uniform");
var bounded = new ReflectionRenderSettings(0, 300, -4).Normalize();
Check(bounded == new ReflectionRenderSettings(25, 100, 0), "registry/UI values normalized");
Console.WriteLine("Registry and WinUI are not exercised by these pixel tests.");
var grainA = Enumerable.Repeat((byte)160, 256 * 4).ToArray();
var grainB = (byte[])grainA.Clone();
ReflectionPixelEffects.ApplyAlpha(grainA, 256, 1, new(100, 100, 0, 75), wrap: true);
ReflectionPixelEffects.ApplyAlpha(grainB, 256, 1, new(100, 100, 0, 75), wrap: true);
Check(grainA.SequenceEqual(grainB), "noise is deterministic");
Check(Enumerable.Range(0, 256).All(i => grainA[i * 4] == grainA[i * 4 + 1]
    && grainA[i * 4 + 1] == grainA[i * 4 + 2]), "grain stays achromatic");
Check(grainA.Where((_, i) => i % 4 == 3).Distinct().Count() > 1, "noise modulates alpha");
Check(new ReflectionRenderSettings(100, 100, 0, 999).Normalize().NoisePercent == 100, "noise clamped");
Check(new ReflectionRenderSettings(NoiseDensity: 999).Normalize().NoiseDensity == 100, "density clamped");
Check(new ReflectionRenderSettings(NoiseSize: 32).Normalize().NoiseSize == 1, "legacy grain size ignored");

var variants = new List<byte[]>();
for (int type = 0; type < 5; type++)
{
    var pixels = Enumerable.Repeat((byte)200, 32 * 32 * 4).ToArray();
    ReflectionPixelEffects.ApplyAlpha(pixels, 32, 32, new(100, 100, 0, 100, 8, type), wrap: true);
    var repeat = Enumerable.Repeat((byte)200, 32 * 32 * 4).ToArray();
    ReflectionPixelEffects.ApplyAlpha(repeat, 32, 32, new(100, 100, 0, 100, 8, type), wrap: true);
    Check(pixels.SequenceEqual(repeat), "grain type deterministic: " + type);
    Check(pixels.Where((_, i) => i % 4 != 3).Any(v => v != 200), "grain varies brightness: " + type);
    variants.Add(pixels);
}
Check(!variants[0].SequenceEqual(variants[1]) && !variants[1].SequenceEqual(variants[2])
    && !variants[2].SequenceEqual(variants[3]), "grain types produce distinct results");
Check(Enumerable.Range(0, 8).Select(x => variants[0][x * 4 + 3]).Distinct().Count() > 2,
    "soft grain varies within a former square block");
for (int type = 0; type < 5; type++)
{
    var transparent = new byte[32 * 32 * 4];
    ReflectionPixelEffects.ApplyAlpha(transparent, 32, 32, new(100, 100, 0, 75, 4, type), wrap: false);
    Check(transparent.Where((_, i) => i % 4 == 3).Any(a => a > 0),
        "noise fills fully transparent canvas: " + type);
    var lite = new byte[32 * 32 * 4];
    lite[0] = lite[1] = lite[2] = 160; lite[3] = 255;
    ReflectionPixelEffects.ApplyAlpha(lite, 32, 32, new(100, 100, 0, 75, 4, type), wrap: false);
    Check(Enumerable.Range(1, 1023).Any(i => lite[i * 4 + 3] > 0 && lite[i * 4] != 160),
        "noise brightness is independent of Lite source: " + type);
    var disabled = new byte[32 * 32 * 4];
    ReflectionPixelEffects.ApplyAlpha(disabled, 32, 32, new(100, 100, 0, 0, 4, type), wrap: false);
    Check(disabled.All(b => b == 0), "zero noise preserves transparency: " + type);
    ReflectionPixelEffects.ApplyAlpha(transparent, 32, 32, new(100, 0, 0, 75, 4, type), wrap: false);
    Check(transparent.Where((_, i) => i % 4 == 3).All(a => a == 0), "opacity zero hides noise too: " + type);
}

for (int type = 0; type < 5; type++)
{
    var field = new byte[128 * 128 * 4];
    ReflectionPixelEffects.ApplyAlpha(field, 128, 128, new(100, 100, 0, 100, 4, type), wrap: false);
    var visible = Enumerable.Range(0, 128 * 128).Where(i => field[i * 4 + 3] > 4).ToArray();
    Check(visible.Any(i => field[i * 4] < 110) && visible.Any(i => field[i * 4] > 145),
        "both dark and light grains: " + type);
    Check(visible.Select(i => field[i * 4]).Distinct().Count() >= (type == 4 ? 6 : 17),
        "varied luminance, not a solid color: " + type);
    Check(visible.All(i => field[i * 4] == field[i * 4 + 1] && field[i * 4] == field[i * 4 + 2]),
        "neutral RGB grain on transparent canvas: " + type);
    var dark = new byte[field.Length]; var light = new byte[field.Length];
    dark[3] = light[3] = 255;
    light[0] = light[1] = light[2] = 160;
    ReflectionPixelEffects.ApplyAlpha(dark, 128, 128, new(100, 100, 0, 100, 4, type), wrap: false);
    ReflectionPixelEffects.ApplyAlpha(light, 128, 128, new(100, 100, 0, 100, 4, type), wrap: false);
    Check(dark.Skip(4).SequenceEqual(light.Skip(4)), "background grain independent of source color: " + type);
}

for (int type = 0; type < 5; type++)
{
    byte[] RenderDensity(int density, int legacySize = 1)
    {
        var result = new byte[128 * 128 * 4];
        ReflectionPixelEffects.ApplyAlpha(result, 128, 128,
            new(NoisePercent: 100, NoiseSize: legacySize, NoiseType: type, NoiseDensity: density), wrap: false);
        return result;
    }
    var empty = RenderDensity(0); var sparse = RenderDensity(10); var dense = RenderDensity(90);
    int Coverage(byte[] pixels) => Enumerable.Range(0, 128 * 128).Count(i => pixels[i * 4 + 3] > 4);
    Check(empty.All(v => v == 0), "zero density adds no grain: " + type);
    Check(Coverage(dense) > Coverage(sparse), "density increases visible coverage: " + type);
    Check(dense.SequenceEqual(RenderDensity(90, 32)), "legacy size cannot enlarge grains: " + type);
    if (type == 2) Check(Coverage(dense) > 128 * 128 / 10, "dots have measurable coverage");
}
var bypass = (byte[])original.Clone();
ReflectionPixelEffects.ApplyAlpha(bypass, 2, 1,
    new(OpacityPercent: 0, Softness: 3, NoisePercent: 100, Enabled: false), wrap: true);
Check(bypass.SequenceEqual(original), "disabled processing preserves original pixels");
var contrastLevels = new List<double>();
for (int type = 0; type < 5; type++)
{
    var sample = new byte[96 * 96 * 4];
    ReflectionPixelEffects.ApplyAlpha(sample, 96, 96,
        new(NoisePercent: 50, NoiseType: type, NoiseDensity: 100), wrap: false);
    double sum = 0;
    for (int i = 0; i < sample.Length; i += 4)
    {
        double delta = (sample[i] - 127.5) * sample[i + 3] / 255.0;
        sum += delta * delta;
    }
    contrastLevels.Add(Math.Sqrt(sum / (96 * 96)));
    var faint = new byte[96 * 96 * 4];
    ReflectionPixelEffects.ApplyAlpha(faint, 96, 96,
        new(NoisePercent: 3, NoiseType: type, NoiseDensity: 100), wrap: false);
    Check(faint.Where((_, i) => i % 4 == 3).Max() <= 3, "low intensity remains faint: " + type);
}
Check(contrastLevels.Max() / contrastLevels.Min() < 1.15,
    "noise types have comparable neutral-background RMS contrast at full density");

var sand = new byte[256 * 256 * 4];
ReflectionPixelEffects.ApplyAlpha(sand, 256, 256,
    new(NoisePercent: 100, NoiseType: 4, NoiseDensity: 100), wrap: false);
var sandLevels = Enumerable.Range(0, 65536).Select(i => sand[i * 4]).Distinct().Order().ToArray();
Check(sandLevels.SequenceEqual(new byte[] { 0, 51, 102, 153, 204, 255 }), "sand reproduces six reference luminance levels");
int sharpNeighbours = Enumerable.Range(0, 65535).Count(i => Math.Abs(sand[i * 4] - sand[(i + 1) * 4]) >= 102);
Check(sharpNeighbours > 65536 / 10, "sand retains sharp unfiltered pixel contrast");
