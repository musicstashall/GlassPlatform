namespace GlassSettings.Services;

/// <summary>Какое действие выполнить для применённого значения отражения.</summary>
internal enum ReflectionProfileAction
{
    /// <summary>Применённое значение уже совпадает с полотном текущего профиля.</summary>
    None,

    /// <summary>Применить готовое полотно текущего профиля.</summary>
    Adopt,

    /// <summary>Рецепт есть, полотна нет: построить полотно и применить его.</summary>
    Regenerate,

    /// <summary>Для профиля ничего нет: снять значение, OpenGlass использует своё отражение.</summary>
    Clear
}

internal readonly record struct ReflectionProfileDecision(ReflectionProfileAction Action);

/// <summary>
/// Единые правила для GlassSettings и optional Native-хелпера платформы.
/// Зеркало <c>Shared::ReflectionProfiles::Decide</c> из
/// <c>GlassShared/include/ReflectionProfileApi.h</c>: обе стороны обязаны прийти к
/// одному выводу для одинакового состояния реестра, поэтому правила живут в одном
/// месте ABI, а не переписываются в каждом модуле по-своему.
/// </summary>
internal static class ReflectionProfilePolicy
{
    /// <param name="activeEmpty">CustomThemeReflection отсутствует или пуст.</param>
    /// <param name="activeWasOurs">значение указывает на опубликованное полотно
    /// (ProgramData\OpenGlass\Presets\Textured); любой другой путь — чужое
    /// изображение, его нельзя ни перезаписывать, ни удалять.</param>
    /// <param name="activeMatchesProfile">применено уже полотно, записанное для
    /// текущего профиля: синхронизировать нечего.</param>
    /// <param name="readyProfile">для текущего профиля есть проверенное полотно.</param>
    /// <param name="recipeProfile">для текущего профиля есть рецепт, который можно
    /// отрендерить заново.</param>
    public static ReflectionProfileDecision Decide(
        bool activeEmpty, bool activeWasOurs, bool activeMatchesProfile,
        bool readyProfile, bool recipeProfile)
    {
        // Целевое состояние OG — проверенное полотно текущего профиля. Оно пишется
        // всегда, кроме случая, когда уже применено: неважно, что стоит сейчас —
        // полотно другого профиля или изображение, которого GS не знает.
        if (readyProfile)
            return new(activeMatchesProfile ? ReflectionProfileAction.None : ReflectionProfileAction.Adopt);
        // Полотна для профиля нет. Наше применённое полотно оставлять нельзя:
        // строим заново по рецепту, а если рецепта нет — снимаем значение, чтобы OG
        // вернулся к своему отражению по умолчанию.
        if (!activeEmpty && activeWasOurs)
            return new(recipeProfile ? ReflectionProfileAction.Regenerate : ReflectionProfileAction.Clear);
        // Ничего не применено или чужое изображение, и применить нечего — не трогаем.
        return new(ReflectionProfileAction.None);
    }

    /// <summary>Путь внутри каталога опубликованных полотен и без выхода из него.</summary>
    public static bool IsPublishedCanvas(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return false;
        try
        {
            var directory = Path.GetFullPath(Core.Platform.PlatformPaths.ReflectionAppliedDirectory);
            var full = Path.GetFullPath(path);
            if (!string.Equals(Path.GetDirectoryName(full), directory, StringComparison.OrdinalIgnoreCase)) return false;
            var name = Path.GetFileName(full);
            return name.Length > 0 && !name.Contains("..", StringComparison.Ordinal);
        }
        catch { return false; }
    }

    /// <summary>Входные данные политики для текущего состояния реестра.</summary>
    public static ReflectionProfileDecision DecideForRegistry(
        string? active, string? readyOutput, bool recipeProfile)
    {
        var matches = false;
        if (!string.IsNullOrWhiteSpace(active) && !string.IsNullOrWhiteSpace(readyOutput))
        {
            try
            {
                matches = string.Equals(Path.GetFullPath(active), Path.GetFullPath(readyOutput),
                    StringComparison.OrdinalIgnoreCase);
            }
            catch { matches = string.Equals(active, readyOutput, StringComparison.OrdinalIgnoreCase); }
        }
        return Decide(string.IsNullOrWhiteSpace(active), IsPublishedCanvas(active), matches,
            !string.IsNullOrWhiteSpace(readyOutput), recipeProfile);
    }
}
