using GlassSettings.Core.Text;

namespace GlassSettings.Core.Registry;

/// <summary>HKCU\...\GlassPlatform\LightMode_|DarkMode_GlobalTextGlow (REG_BINARY, 16 byte).</summary>
public static class GlobalTextGlowRegistry
{
    public static bool IsPresent()
    {
        foreach (var name in ThemeParameterNames.GlobalTextGlowNames)
        {
            if (ReadBinary(name) is not null)
                return true;
        }

        return false;
    }

    public static bool TryLoadForTheme(bool darkMode, out GlobalTextGlowModel model)
    {
        var bytes = ReadBinary(ThemeValueName(darkMode));
        if (bytes is null)
        {
            model = GlobalTextGlowModel.Factory;
            return false;
        }

        model = GlobalTextGlowModel.FromBytes(bytes);
        return true;
    }

    public static bool TryLoad(out GlobalTextGlowModel model) =>
        TryLoadForTheme(ThemeRegistry.IsDarkMode(), out model);

    /// <summary>Live-preview drag: всегда REG_BINARY, без delete-on-factory.</summary>
    public static void WriteForLivePreview(bool darkMode, GlobalTextGlowModel model)
    {
        model.Clamp();
        GlassRegistry.WriteBinary(null, ThemeValueName(darkMode), model.ToBytes());
    }

    public static void WriteOrRemoveForTheme(bool darkMode, GlobalTextGlowModel model)
    {
        model.Clamp();
        var valueName = ThemeValueName(darkMode);
        if (model.EqualsFactory())
        {
            GlassRegistry.DeleteValue(null, valueName);
            return;
        }

        GlassRegistry.WriteBinary(null, valueName, model.ToBytes());
    }

    public static void DeleteAll()
    {
        foreach (var name in ThemeParameterNames.GlobalTextGlowNames)
            GlassRegistry.DeleteValue(null, name);
    }

    public static bool ShouldDisableDefaultColorButton(bool darkMode)
    {
        if (!TryLoadForTheme(darkMode, out var model))
            return true;

        return TextGlowColorHelper.IsColorDefault(model);
    }

    private static string ThemeValueName(bool darkMode) =>
        darkMode ? "DarkMode_GlobalTextGlow" : "LightMode_GlobalTextGlow";

    private static byte[]? ReadBinary(string valueName)
    {
        var hkcu = GlassRegistry.ReadBinary(null, valueName);
        if (hkcu is not null)
            return hkcu;

        using var hklm = global::Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
            RegistryPaths.GlassRoot, false);
        if (hklm?.GetValue(valueName) is byte[] bytes)
            return bytes;

        return null;
    }
}
