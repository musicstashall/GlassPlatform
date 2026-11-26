namespace GlassSettings.Services;

/// <summary>Какое действие выполнить для применённого значения отражения.</summary>
internal enum ReflectionProfileAction
{
    /// <summary>Чужой выбор: значение не трогаем.</summary>
    None,

    /// <summary>Применить готовое полотно текущего профиля.</summary>
    Adopt,

    /// <summary>Нашего полотна для профиля нет: снять устаревшее значение.</summary>
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
    /// <param name="managed">HKCU Textured\Managed == 1.</param>
    /// <param name="activeEmpty">CustomThemeReflection отсутствует или пуст.</param>
    /// <param name="activeWasOurs">значение указывает на опубликованное полотно
    /// (ProgramData\OpenGlass\Presets\Textured): доказывает прежнее владение, даже
    /// если флаг Managed сброшен или платформа ни разу не запускалась.</param>
    /// <param name="readyProfile">для текущего профиля есть проверенное полотно.</param>
    public static ReflectionProfileDecision Decide(
        bool managed, bool activeEmpty, bool activeWasOurs, bool readyProfile)
    {
        if (activeEmpty)
            return new(managed && readyProfile ? ReflectionProfileAction.Adopt : ReflectionProfileAction.None);
        if (readyProfile && (managed || activeWasOurs))
            return new(ReflectionProfileAction.Adopt);
        // Усыновлять нечего: значение доказуемо указывает на наше опубликованное
        // полотно, значит это полотно другой топологии — снимаем его, чтобы не
        // показывать картинку прежней конфигурации. Одного флага Managed мало:
        // удалять можно только путь, который опубликовали мы.
        if (!readyProfile && activeWasOurs)
            return new(ReflectionProfileAction.Clear);
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
    public static ReflectionProfileDecision DecideForRegistry(string? active, bool readyProfile) =>
        Decide(ReflectionRenderState.IsManaged(), string.IsNullOrWhiteSpace(active),
            IsPublishedCanvas(active), readyProfile);
}
