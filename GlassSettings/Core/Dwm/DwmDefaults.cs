using GlassSettings.Core.Snapshot;
using Microsoft.Win32;

namespace GlassSettings.Core.Dwm;

public static class DwmDefaults
{
    public const uint OpacityAuto = 0xFFFFFFFEu;
    public const uint PriorityAuto = 0xFFFFFFFFu;

    /// <summary>Ключи OpenGlass: отсутствие в реестре ≡ DWORD по умолчанию (delete-on-default).</summary>
    private static readonly HashSet<string> DeleteOnDefaultKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "GlassOverrideAccent",
        "ColorizationOpaqueBlend",
        "BlurDeviation",
        "ColorizationOpaqueBlendPriority",
        "GlassOpacity"
    };

    public static bool IsOpacityAutoValue(uint v) =>
        v == OpacityAuto || v == PriorityAuto;

    public static bool UsesAbsentAsDefault(string valueName) =>
        DeleteOnDefaultKeys.Contains(valueName);

    public static uint DefaultWhenKeyAbsent(string valueName)
    {
        if (valueName.Contains("GlassOpacity", StringComparison.Ordinal))
            return 63;
        if (valueName.Contains("ColorizationOpacity", StringComparison.Ordinal) ||
            valueName.Contains("ColorizationGlassReflectionOpacity", StringComparison.Ordinal))
            return OpacityAuto;
        if (valueName.Contains("ColorizationColorCaption", StringComparison.Ordinal))
            return 0xFFFFFFFDu;
        if (valueName.Contains("ColorizationColor", StringComparison.Ordinal))
            return 0;
        return valueName switch
        {
            "GlassOverrideAccent" => 0,
            "ColorizationOpaqueBlend" => 0,
            "BlurDeviation" => 30,
            "ColorizationGlassReflectionParallaxIntensity" => 13,
            "ColorizationGlassReflectionPolicy" => PriorityAuto,
            "ColorizationOpaqueBlendPriority" => PriorityAuto,
            _ => 0
        };
    }

    /// <summary>
    /// Сравнение для dirty-tracking: absent ≡ default DWORD для delete-on-default ключей (паритет OpenGlassGUI).
    /// </summary>
    internal static bool IsSemanticallyEqual(
        string valueName,
        RegEntry? a,
        RegEntry? b,
        Func<RegEntry, RegEntry, bool> entryEquals)
    {
        if (a is null && b is null)
            return true;
        if (a is not null && b is not null)
            return entryEquals(a, b);
        if (!UsesAbsentAsDefault(valueName))
            return false;

        var present = a ?? b!;
        var dword = TryReadDword(present);
        return dword == DefaultWhenKeyAbsent(valueName);
    }

    private static uint? TryReadDword(RegEntry entry)
    {
        if (entry.Type != (int)RegistryValueKind.DWord || entry.Data.Length < 4)
            return null;

        return BitConverter.ToUInt32(entry.Data, 0);
    }

    public static int OpaqueBlendPriorityToComboSel(uint v) => v switch
    {
        0 => 0,
        1 => 1,
        _ => 2
    };

    public static uint OpaqueBlendPriorityFromComboSel(int sel) => sel switch
    {
        0 => 0,
        1 => 1,
        _ => PriorityAuto
    };
}
