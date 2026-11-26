namespace GlassSettings.Services;

using Microsoft.Win32;
using System.Text.Json;
using System.Security.Cryptography;

// NoiseSize is retained only to read older registry recipes; rendering ignores it.
public sealed record ReflectionRenderSettings(int ScalePercent = 100, int OpacityPercent = 100, int Softness = 0, int NoisePercent = 0, int NoiseSize = 1, int NoiseType = -1, int NoiseDensity = 50, bool Enabled = true, uint ColorRgb = 0)
{
    public ReflectionRenderSettings Normalize() => new(
        Math.Clamp(ScalePercent, 25, 200), Math.Clamp(OpacityPercent, 0, 100), Math.Clamp(Softness, 0, 20),
        Math.Clamp(NoisePercent, 0, 100), 1, Math.Clamp(NoiseType, -1, 4), Math.Clamp(NoiseDensity, 0, 100), Enabled, ColorRgb & 0x00FFFFFFu);
}

/// <summary>Independent recipes and verified publications per native display configuration.</summary>
public static class ReflectionRenderState
{
    public const string RegistryPath = @"Software\StanislavKa\GlassPlatform\GlassSettings\Textured";
    public sealed record State(int Version, bool Ready, string Source, string Output, int Width, int Height,
        string SourceHash, string OutputHash, ReflectionRenderSettings Settings, string Profile = "");

    public static State? Read()
    {
        try
        {
            var snapshot = ReflectionDisplayProfile.Capture();
            string profile = snapshot.Key;
            using var root = OpenRoot();
            using var key = root.OpenSubKey(RegistryPath + @"\Profiles\" + profile);
            var state = key?.GetValue("State") is string text ? JsonSerializer.Deserialize<State>(text) : null;
            if (state is not { Version: 2, Ready: true } || state.Settings is null || state.Profile != profile
                || state.Width != snapshot.Width || state.Height != snapshot.Height
                || key?.GetValue("Ready") is not int ready || ready != 1 || !File.Exists(state.Output))
                return null;
            // ProgramData is shared across users. Do not attribute another writer's PNG to this HKCU record.
            using var image = File.OpenRead(state.Output);
            return Convert.ToHexString(SHA256.HashData(image)) == state.OutputHash ? state : null;
        }
        catch { return null; }
    }

    // Resolve only explicit saved records, never infer recipes from a PNG name.
    // Used after topology changed, when Read() already refers to the new profile.
    public static State? ReadForOutput(string? output)
    {
        if (string.IsNullOrWhiteSpace(output)) return null;
        try
        {
            using var root = OpenRoot();
            using var profiles = root.OpenSubKey(RegistryPath + @"\Profiles");
            if (profiles is null) return null;
            foreach (string profile in profiles.GetSubKeyNames())
            {
                using var key = profiles.OpenSubKey(profile);
                if (key?.GetValue("Ready") is not int ready || ready != 1) continue;
                var state = key.GetValue("State") is string text ? JsonSerializer.Deserialize<State>(text) : null;
                if (state is not { Version: 2, Ready: true } || state.Profile != profile || state.Settings is null
                    || !string.Equals(state.Output, output, StringComparison.OrdinalIgnoreCase) || !File.Exists(state.Source)) continue;
                using var image = File.OpenRead(state.Output);
                if (Convert.ToHexString(SHA256.HashData(image)) == state.OutputHash) return state;
            }
        }
        catch { }
        return null;
    }

    /// <summary>
    /// Ready canvas of the current display topology. Unlike <see cref="Read"/> this
    /// does not re-check the source file, so the applied image can still be identified
    /// after its source was deleted.
    /// </summary>
    public static State? ReadReadyProfile()
    {
        try
        {
            var snapshot = ReflectionDisplayProfile.Capture();
            string profile = snapshot.Key;
            using var root = OpenRoot();
            using var key = root.OpenSubKey(RegistryPath + @"\Profiles\" + profile);
            var state = key?.GetValue("State") is string text ? JsonSerializer.Deserialize<State>(text) : null;
            if (state is not { Version: 2, Ready: true } || state.Settings is null || state.Profile != profile
                || state.Width != snapshot.Width || state.Height != snapshot.Height
                || key?.GetValue("Ready") is not int ready || ready != 1) return null;
            return state;
        }
        catch { return null; }
    }

    public static ReflectionRenderSettings Settings => (Read()?.Settings ?? new()).Normalize();

    public static void Invalidate(string? profile = null) => Write(new(2, false, "", "", 0, 0, "", "", new(),
        profile ?? ReflectionDisplayProfile.Capture().Key));

    public static void Save(string source, string output, int width, int height, string sourceHash,
        string outputHash, ReflectionRenderSettings settings, string? profile = null) =>
        Write(new(2, true, source, output, width, height, sourceHash, outputHash, settings.Normalize(),
            profile ?? ReflectionDisplayProfile.Capture().Key));

    public static bool IsManaged()
    {
        try
        {
            using var root = OpenRoot();
            using var key = root.OpenSubKey(RegistryPath);
            return key?.GetValue("Managed") is int managed && managed == 1;
        }
        catch { return false; }
    }

    public static bool IsManagedOutput(string? output)
    {
        try
        {
            using var root = OpenRoot();
            using var key = root.OpenSubKey(RegistryPath);
            return key?.GetValue("Managed") is int managed && managed == 1
                && string.Equals(key.GetValue("ManagedOutput") as string ?? "", output ?? "", StringComparison.OrdinalIgnoreCase);
        }
        catch { return false; }
    }

    public static void MarkManaged(string output)
    {
        var state = Read();
        if (state is null || !string.Equals(state.Output, output, StringComparison.OrdinalIgnoreCase)) return;
        using var root = OpenRoot();
        using var key = root.CreateSubKey(RegistryPath, writable: true);
        key.SetValue("ManagedOutput", output, RegistryValueKind.String);
        key.SetValue("Managed", 1, RegistryValueKind.DWord);
        _ = PostMessage(new IntPtr(0xFFFF), RegisterWindowMessage("GlassPlatform.ReflectionProfileChanged.V1"), IntPtr.Zero, IntPtr.Zero);
    }

    /// <summary>
    /// Adopts the ready canvas published for the current topology: writes it as the
    /// applied value and re-asserts ownership. Only our own published PNG is used.
    /// Returns true when the applied value actually changed.
    /// </summary>
    public static bool AdoptReadyProfile(string profile)
    {
        var state = ReadReadyProfile();
        if (state is null || state.Profile != profile) return false;
        // Published canvases live in ProgramData; a record pointing elsewhere is foreign.
        if (!ReflectionProfilePolicy.IsPublishedCanvas(state.Output) || !File.Exists(state.Output)) return false;
        try
        {
            using var image = File.OpenRead(state.Output);
            if (Convert.ToHexString(SHA256.HashData(image)) != state.OutputHash) return false;
        }
        catch { return false; }
        try
        {
            using var dwm = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\DWM", writable: true);
            if (dwm is null) return false;
            bool changed = !string.Equals(dwm.GetValue("CustomThemeReflection") as string ?? "", state.Output,
                StringComparison.OrdinalIgnoreCase);
            if (changed) dwm.SetValue("CustomThemeReflection", state.Output, RegistryValueKind.String);
            // Ownership after the value: Read() must resolve the profile it now points at.
            using var root = OpenRoot();
            using var key = root.CreateSubKey(RegistryPath, writable: true);
            key.SetValue("ManagedOutput", state.Output, RegistryValueKind.String);
            key.SetValue("Managed", 1, RegistryValueKind.DWord);
            return changed;
        }
        catch { return false; }
    }

    /// <summary>
    /// This profile has no canvas but an ours-looking value is applied (e.g. a canvas
    /// of a previous topology). Drop it so OpenGlass falls back to its own reflection
    /// instead of showing a picture that belongs to another display configuration.
    /// The published PNG stays on disk; the recipe record is untouched.
    /// </summary>
    public static bool ClearStaleProfileCanvas(string? active)
    {
        // Only a published canvas may be dropped; an external selection stays.
        if (string.IsNullOrWhiteSpace(active) || !ReflectionProfilePolicy.IsPublishedCanvas(active)) return false;
        try
        {
            using var dwm = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\DWM", writable: true);
            if (dwm is null) return false;
            if (!string.Equals(dwm.GetValue("CustomThemeReflection") as string ?? "", active,
                    StringComparison.OrdinalIgnoreCase)) return false;
            dwm.DeleteValue("CustomThemeReflection", throwOnMissingValue: false);
            using var root = OpenRoot();
            using var key = root.OpenSubKey(RegistryPath, writable: true);
            key?.SetValue("ManagedOutput", "", RegistryValueKind.String);
            key?.SetValue("Managed", 0, RegistryValueKind.DWord);
            return true;
        }
        catch { return false; }
    }

    public static void ReleaseManagement()
    {
        using var root = OpenRoot();
        using var key = root.OpenSubKey(RegistryPath, writable: true);
        key?.SetValue("Managed", 0, RegistryValueKind.DWord);
    }

    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    private static extern uint RegisterWindowMessage(string message);
    [System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "PostMessageW")]
    [return: System.Runtime.InteropServices.MarshalAs(System.Runtime.InteropServices.UnmanagedType.Bool)]
    private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    private static RegistryKey OpenRoot() => RegistryKey.OpenBaseKey(RegistryHive.CurrentUser,
        Environment.Is64BitOperatingSystem ? RegistryView.Registry64 : RegistryView.Registry32);

    private static void Write(State state)
    {
        using var root = OpenRoot();
        using var key = root.CreateSubKey(RegistryPath + @"\Profiles\" + state.Profile, writable: true);
        key.SetValue("Ready", 0, RegistryValueKind.DWord);
        // A single REG_SZ avoids torn reads of many independently written parameter values.
        key.SetValue("State", JsonSerializer.Serialize(state), RegistryValueKind.String);
        key.SetValue("Output", state.Output, RegistryValueKind.String);
        key.SetValue("OutputHash", state.OutputHash, RegistryValueKind.String);
        key.SetValue("Ready", state.Ready ? 1 : 0, RegistryValueKind.DWord);
    }
}
