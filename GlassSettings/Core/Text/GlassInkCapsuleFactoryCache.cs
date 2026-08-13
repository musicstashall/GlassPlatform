using GlassSettings.Core.Registry;

namespace GlassSettings.Core.Text;

/// <summary>Кэш factory-профилей из Shared::InkCapsuleQuery::MakeFactoryCapsule.</summary>
internal static class GlassInkCapsuleFactoryCache
{
    private static GlassInkCapsuleModel? _factory;

    public static GlassInkCapsuleModel Factory
    {
        get
        {
            _factory ??= LoadFactory();
            return _factory;
        }
    }

    public static GlassInkProfileModel Single => Factory.Single;

    public static GlassInkProfileModel Dual => Factory.Dual;

    private static GlassInkCapsuleModel LoadFactory()
    {
        Span<byte> bytes = stackalloc byte[InkCapsuleInterop.CapsuleBinarySize];
        InkCapsuleInterop.ReadFactoryInkCapsule(bytes);
        return GlassInkCapsuleModel.FromBytes(bytes);
    }
}
