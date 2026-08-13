using GlassSettings.Controls;

namespace GlassSettings.Services;

internal sealed class TopNoticeMirrorStripSpec
{
    public TopNoticeMirrorStripSpec(
        SettingsNoticeSeverity severity,
        string message,
        bool showSave,
        bool saveEnabled,
        SettingsUnsavedNoticeStrip? sourceStrip = null,
        bool showAction = false,
        string? actionText = null)
    {
        Severity = severity;
        Message = message;
        ShowSave = showSave;
        SaveEnabled = saveEnabled;
        SourceStrip = sourceStrip;
        ShowAction = showAction;
        ActionText = actionText ?? string.Empty;
    }

    public SettingsNoticeSeverity Severity { get; }

    public string Message { get; }

    public bool ShowSave { get; }

    public bool SaveEnabled { get; }

    public bool ShowAction { get; }

    public string ActionText { get; }

    public SettingsUnsavedNoticeStrip? SourceStrip { get; }
}
