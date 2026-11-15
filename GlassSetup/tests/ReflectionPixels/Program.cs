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
