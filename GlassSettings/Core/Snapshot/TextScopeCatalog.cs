using GlassSettings.Core.Registry;
using GlassSettings.Core.Text;

namespace GlassSettings.Core.Snapshot;

/// <summary>Точечные commit-scope для страницы «Текст и шрифты».</summary>
public static class TextScopeCatalog
{
    public static SettingsCommitScope GlowScope =>
        SettingsCommitScope.ForGlassValues(ThemeParameterNames.GlobalTextGlowNames);

    public static SettingsCommitScope InkCapsuleScope =>
        SettingsCommitScope.ForGlassValues(GlassInkAlphaCapsuleRegistry.ValueName);

    public static SettingsCommitScope SystemFontsScope =>
        SettingsCommitScope.ForWindowMetricsKeys(SystemFontsCatalog.RegistryValueNames);

    public static SettingsCommitScope SystemFontScope(int index) =>
        index is >= 0 and < SystemFontsCatalog.SlotCount
            ? SettingsCommitScope.ForWindowMetricsKeys(SystemFontsCatalog.RegistryValueNames[index])
            : SystemFontsScope;

    /// <summary>Dirty ink capsule или GlobalTextGlow (live-preview писал в реестр).</summary>
    public static bool HasTextRenderRegistryDirty(SettingsSnapshot snapshot) =>
        snapshot.HasUnsavedInScope(InkCapsuleScope) || snapshot.HasUnsavedInScope(GlowScope);

    public static bool HasSystemFontsRegistryDirty(SettingsSnapshot snapshot) =>
        snapshot.HasUnsavedInScope(SystemFontsScope);
}
