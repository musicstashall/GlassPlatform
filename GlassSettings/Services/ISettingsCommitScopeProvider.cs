using GlassSettings.Core.Snapshot;

namespace GlassSettings.Services;

/// <summary>Страница с фиксированным commit-scope уровня всей страницы (не pivot/scope bar).</summary>
public interface ISettingsCommitScopeProvider
{
    SettingsCommitScope GetCommitScope();
}
