using GlassSettings.Core.Text;

namespace GlassSettings.Core.Registry;

/// <summary>HKCU\...\BlackWindowManager\GlassInkAlphaCapsule (REG_BINARY, 100 byte).</summary>
public static class GlassInkAlphaCapsuleRegistry
{
    public const string ValueName = "GlassInkAlphaCapsule";

    public static bool IsPresent() => InkCapsuleInterop.IsPresent();

    public static bool TryLoad(out GlassInkCapsuleModel model)
    {
        Span<byte> bytes = stackalloc byte[InkCapsuleInterop.CapsuleBinarySize];
        var fromRegistry = InkCapsuleInterop.ReadUiInkCapsule(bytes);
        model = GlassInkCapsuleModel.FromBytes(bytes);
        return fromRegistry;
    }

    /// <summary>Live-preview drag: всегда REG_BINARY, без delete-on-factory.</summary>
    public static bool WriteForLivePreview(GlassInkCapsuleModel model)
    {
        model.Clamp();
        return GlassRegistry.WriteBinary(null, ValueName, model.ToBytes());
    }

    public static void WriteOrRemove(GlassInkCapsuleModel model)
    {
        model.Clamp();
        if (model.EqualsFactory())
        {
            GlassRegistry.DeleteValue(null, ValueName);
            return;
        }

        GlassRegistry.WriteBinary(null, ValueName, model.ToBytes());
    }

    public static void Delete() => GlassRegistry.DeleteValue(null, ValueName);
}
