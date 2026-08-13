namespace GlassSettings.Core.Registry;

/// <summary>Режим инъекции AttachList — зеркало Shared::InjectionMode.</summary>
public enum AttachProcessMode : uint
{
    Forbidden = 0,
    PopupOnly = 1,
    Full = 2
}
