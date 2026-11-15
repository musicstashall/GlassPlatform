namespace GlassSettings.Services;

using Microsoft.Win32;
using System.Text.Json;
using System.Security.Cryptography;

public sealed record ReflectionRenderSettings(int ScalePercent = 100, int OpacityPercent = 100, int Softness = 0, int NoisePercent = 0, int NoiseSize = 1)
{
    public ReflectionRenderSettings Normalize() => new(
        Math.Clamp(ScalePercent, 25, 200), Math.Clamp(OpacityPercent, 0, 100), Math.Clamp(Softness, 0, 3),
        Math.Clamp(NoisePercent, 0, 100), Math.Clamp(NoiseSize, 1, 32));
}

/// <summary>One record for the single generated canvas; not per-source profiles or PNG metadata.</summary>
public static class ReflectionRenderState
{
    public const string RegistryPath = @"Software\StanislavKa\GlassPlatform\GlassSettings\Textured";
    public sealed record State(int Version, bool Ready, string Source, string Output, int Width, int Height,
        string SourceHash, string OutputHash, ReflectionRenderSettings Settings);

    public static State? Read()
    {
        try
        {
            using var root = OpenRoot();
            using var key = root.OpenSubKey(RegistryPath);
            var state = key?.GetValue("State") is string text ? JsonSerializer.Deserialize<State>(text) : null;
            if (state is not { Version: 1, Ready: true } || state.Settings is null || !File.Exists(state.Output))
                return null;
            // ProgramData is shared across users. Do not attribute another writer's PNG to this HKCU record.
            using var image = File.OpenRead(state.Output);
            return Convert.ToHexString(SHA256.HashData(image)) == state.OutputHash ? state : null;
        }
        catch { return null; }
    }

    public static ReflectionRenderSettings Settings => (Read()?.Settings ?? new()).Normalize();

    // Invalidate before changing the output, so a failed copy never leaves a valid stale recipe.
    public static void Invalidate() => Write(new(1, false, "", "", 0, 0, "", "", new()));

    public static void Save(string source, string output, int width, int height, string sourceHash,
        string outputHash, ReflectionRenderSettings settings) =>
        Write(new(1, true, source, output, width, height, sourceHash, outputHash, settings.Normalize()));

    private static RegistryKey OpenRoot() => RegistryKey.OpenBaseKey(RegistryHive.CurrentUser,
        Environment.Is64BitOperatingSystem ? RegistryView.Registry64 : RegistryView.Registry32);

    private static void Write(State state)
    {
        using var root = OpenRoot();
        using var key = root.CreateSubKey(RegistryPath, writable: true);
        // A single REG_SZ avoids torn reads of many independently written parameter values.
        key.SetValue("State", JsonSerializer.Serialize(state), RegistryValueKind.String);
    }
}
