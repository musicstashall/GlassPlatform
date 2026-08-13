namespace GlassSettings.Controls;

/// <summary>
/// Общий контракт gate-спойлеров (кастомный и системный Expander).
/// </summary>
public interface ISettingsGateSection
{
    bool IsGateOn { get; set; }
}
