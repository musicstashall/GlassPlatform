namespace GlassSettings.Services;

public enum ProcessMenuAction
{
    SetFull,
    SetPopup,
    SetForbidden,
    DeleteAndBlock,
    Delete,
    RemoveFromBlockList
}

public sealed class ProcessActionRequestedEventArgs : EventArgs
{
    public ProcessActionRequestedEventArgs(ProcessEntryModel entry, ProcessMenuAction action)
    {
        Entry = entry;
        Action = action;
    }

    public ProcessEntryModel Entry { get; }

    public ProcessMenuAction Action { get; }
}
