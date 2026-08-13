using System.Runtime.InteropServices;

namespace GlassSettings.Core.Registry;

/// <summary>Part UI/Effective reads вЂ” Shared::PartRegistryQuery (GlassShared).</summary>
internal static class PartRegistryInterop
{
    private const string Dll = "GlassSettings.Native.dll";

    public static bool TryReadLocalDword(string regSub, string valueName, out uint value)
    {
        value = 0;
        return Native_TryReadGlassLocalDword(regSub ?? string.Empty, valueName, out value);
    }

    public static uint ReadEffectiveDword(string regSub, string valueName, uint defaultValue) =>
        Native_ReadGlassEffectiveDword(regSub ?? string.Empty, valueName, defaultValue);

    public static uint ReadUiGradientArgb(string regSub, bool darkMode) =>
        Native_ReadGlassUiGradientArgb(regSub ?? string.Empty, darkMode);

    public static uint ReadUiBorderArgb(string regSub, bool darkMode) =>
        Native_ReadGlassUiBorderArgb(regSub ?? string.Empty, darkMode);

    public static uint ReadUiCornerType(string regSub) =>
        Native_ReadGlassUiCornerType(regSub ?? string.Empty);

    public static bool ReadUiNoBorderColor(string regSub) =>
        Native_ReadGlassUiNoBorderColor(regSub ?? string.Empty);

    [DllImport(Dll, EntryPoint = "GlassSettings_TryReadGlassLocalDword", CharSet = CharSet.Unicode)]
    private static extern bool Native_TryReadGlassLocalDword(string GlassSubKey, string valueName, out uint outValue);

    [DllImport(Dll, EntryPoint = "GlassSettings_ReadGlassEffectiveDword", CharSet = CharSet.Unicode)]
    private static extern uint Native_ReadGlassEffectiveDword(string GlassSubKey, string valueName, uint defaultValue);

    [DllImport(Dll, EntryPoint = "GlassSettings_ReadGlassUiGradientArgb", CharSet = CharSet.Unicode)]
    private static extern uint Native_ReadGlassUiGradientArgb(string GlassSubKey, bool darkMode);

    [DllImport(Dll, EntryPoint = "GlassSettings_ReadGlassUiBorderArgb", CharSet = CharSet.Unicode)]
    private static extern uint Native_ReadGlassUiBorderArgb(string GlassSubKey, bool darkMode);

    [DllImport(Dll, EntryPoint = "GlassSettings_ReadGlassUiCornerType", CharSet = CharSet.Unicode)]
    private static extern uint Native_ReadGlassUiCornerType(string GlassSubKey);

    [DllImport(Dll, EntryPoint = "GlassSettings_ReadGlassUiNoBorderColor", CharSet = CharSet.Unicode)]
    private static extern bool Native_ReadGlassUiNoBorderColor(string GlassSubKey);
}
