namespace GlassSettings.Services;

/// <summary>Detail-страница: перечитать UI из реестра после external-sync baseline.</summary>
public interface IExternalRegistrySyncReloadable
{
    void ReloadFromExternalRegistrySync();
}
