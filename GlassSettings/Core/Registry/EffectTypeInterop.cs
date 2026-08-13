using System.Runtime.InteropServices;

namespace GlassSettings.Core.Registry;

/// <summary>ET 0вЂ“8: РµРґРёРЅС‹Р№ РёСЃС‚РѕС‡РЅРёРє вЂ” Shared::EffectTypeQuery (GlassShared).</summary>
internal static class EffectTypeInterop
{
    private const string Dll = "GlassSettings.Native.dll";

    public static uint DefaultFlyoutEffectType => Native_GetDefaultFlyoutEffectType();

    public static uint DefaultRootEffectType => Native_GetDefaultRootEffectType();

    public static uint ReadUiEffectType(string regSub) =>
        Native_ReadGlassUiEffectType(regSub ?? string.Empty);

    public static bool TryReadLocalEffectType(string regSub, out uint value)
    {
        value = 0;
        return Native_TryReadGlassLocalEffectType(regSub ?? string.Empty, out value);
    }

    [DllImport(Dll, EntryPoint = "GlassSettings_GetDefaultFlyoutEffectType")]
    private static extern uint Native_GetDefaultFlyoutEffectType();

    [DllImport(Dll, EntryPoint = "GlassSettings_GetDefaultRootEffectType")]
    private static extern uint Native_GetDefaultRootEffectType();

    [DllImport(Dll, EntryPoint = "GlassSettings_ReadGlassUiEffectType", CharSet = CharSet.Unicode)]
    private static extern uint Native_ReadGlassUiEffectType(string GlassSubKey);

    [DllImport(Dll, EntryPoint = "GlassSettings_TryReadGlassLocalEffectType", CharSet = CharSet.Unicode)]
    private static extern bool Native_TryReadGlassLocalEffectType(string GlassSubKey, out uint outValue);
}
