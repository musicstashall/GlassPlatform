using GlassSettings.Core.Registry;
using GlassSettings.Services;
using GlassSettings.Strings;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Controls.Processes;

/// <summary>Общее меню «…» для плиток процессов.</summary>
internal static class ProcessTileMenu
{
    public static MenuFlyout CreateFlyout(ProcessEntryModel entry, EventHandler<ProcessActionRequestedEventArgs>? handler) =>
        entry.Section == ProcessSection.BlockList
            ? CreateBlockListFlyout(entry, handler)
            : CreateAttachFlyout(entry, handler);

    private static MenuFlyout CreateAttachFlyout(ProcessEntryModel entry, EventHandler<ProcessActionRequestedEventArgs>? handler)
    {
        var flyout = new MenuFlyout();
        flyout.Items.Add(CreateModeItem(entry, AttachProcessMode.Full, ProcessMenuAction.SetFull, handler));
        flyout.Items.Add(CreateModeItem(entry, AttachProcessMode.PopupOnly, ProcessMenuAction.SetPopup, handler));
        flyout.Items.Add(CreateModeItem(entry, AttachProcessMode.Forbidden, ProcessMenuAction.SetForbidden, handler));
        flyout.Items.Add(new MenuFlyoutSeparator());

        var deleteAndBlockItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Attach_MenuDeleteAndBlock")
        };
        deleteAndBlockItem.Click += (_, _) =>
            handler?.Invoke(entry, new ProcessActionRequestedEventArgs(entry, ProcessMenuAction.DeleteAndBlock));
        flyout.Items.Add(deleteAndBlockItem);

        var deleteItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("Attach_MenuDelete")
        };
        deleteItem.Click += (_, _) => handler?.Invoke(entry, new ProcessActionRequestedEventArgs(entry, ProcessMenuAction.Delete));
        flyout.Items.Add(deleteItem);

        return flyout;
    }

    private static MenuFlyout CreateBlockListFlyout(ProcessEntryModel entry, EventHandler<ProcessActionRequestedEventArgs>? handler)
    {
        var flyout = new MenuFlyout();

        var deleteItem = new MenuFlyoutItem
        {
            Text = GlassSettingsResources.Get("BlockList_MenuRemove"),
            IsEnabled = !entry.IsBuiltInBlockList
        };
        deleteItem.Click += (_, _) =>
            handler?.Invoke(entry, new ProcessActionRequestedEventArgs(entry, ProcessMenuAction.RemoveFromBlockList));
        flyout.Items.Add(deleteItem);

        return flyout;
    }

    private static MenuFlyoutItem CreateModeItem(
        ProcessEntryModel entry,
        AttachProcessMode mode,
        ProcessMenuAction action,
        EventHandler<ProcessActionRequestedEventArgs>? handler)
    {
        var text = mode switch
        {
            AttachProcessMode.Full => GlassSettingsResources.Get("Attach_ModeFull"),
            AttachProcessMode.PopupOnly => GlassSettingsResources.Get("Attach_ModePopup"),
            _ => GlassSettingsResources.Get("Attach_MenuDisable")
        };

        var item = new MenuFlyoutItem
        {
            Text = text,
            IsEnabled = entry.Mode != mode
        };

        if (entry.Mode == mode)
            item.Icon = new FontIcon { Glyph = "\uE73E" };

        item.Click += (_, _) => handler?.Invoke(entry, new ProcessActionRequestedEventArgs(entry, action));
        return item;
    }
}
