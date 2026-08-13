using System.Runtime.InteropServices;

namespace GlassSettings.Services;

public static partial class NativeHostInterop
{
    /// <summary>
    /// uxtheme #65 + опционально OpenGlass DWM policy. flags: 0 без шрифтов, 65 со шрифтами.
    /// applyVisualStyle=false — без #65. applyDwmPolicy=false — без DWM policy (host уже writer).
    /// </summary>
    public static int ApplyVisualStyleAndDwmPolicy(
        string? msstylesPath,
        string? colorStyle,
        string? size,
        int flags,
        bool applyVisualStyle,
        bool applyDwmPolicy = true) =>
        Native_ApplyVisualStyleAndDwmPolicy(
            msstylesPath,
            colorStyle,
            size,
            flags,
            applyVisualStyle,
            applyDwmPolicy);

    public static void ApplyDwmPolicyOnVisualStyleChange() =>
        Native_ApplyDwmPolicyOnVisualStyleChange();

    [DllImport(Dll, EntryPoint = "GlassSettings_ApplyVisualStyleAndDwmPolicy", CharSet = CharSet.Unicode)]
    private static extern int Native_ApplyVisualStyleAndDwmPolicy(
        string? msstylesPath,
        string? colorStyle,
        string? size,
        int flags,
        [MarshalAs(UnmanagedType.Bool)] bool applyVisualStyle,
        [MarshalAs(UnmanagedType.Bool)] bool applyDwmPolicy);

    [DllImport(Dll, EntryPoint = "GlassSettings_ApplyDwmPolicyOnVisualStyleChange")]
    private static extern void Native_ApplyDwmPolicyOnVisualStyleChange();
}
