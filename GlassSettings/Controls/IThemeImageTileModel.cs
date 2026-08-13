namespace GlassSettings.Controls;

/// <summary>Модель плитки коллекции изображений стилизации (контекстное меню удаления).</summary>
public interface IThemeImageTileModel
{
    event EventHandler? DeleteRequested;

    void RequestDelete();
}
