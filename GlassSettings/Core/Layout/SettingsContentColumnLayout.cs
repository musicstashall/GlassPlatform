using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Layout;

/// <summary>
/// В Wide-режиме колонка ограничена 1064 и центрируется только если viewport достаточен;
/// ScrollViewer остаётся на всю ширину — полоса прокрутки у правого края окна.
/// </summary>
public static class SettingsContentColumnLayout
{
    private sealed class ColumnRegistration(FrameworkElement column, ScrollViewer scroll)
    {
        public FrameworkElement Column { get; } = column;
        public ScrollViewer Scroll { get; } = scroll;
    }

    private static readonly List<ColumnRegistration> Registrations = new();
    private static readonly List<FrameworkElement> HostColumns = new();
    private static ScrollViewer? _pageContentScroll;

    public static bool IsWideCentered { get; private set; }

    public static event EventHandler? WideCenteredChanged;

    public static double GetViewportWidth(ScrollViewer scrollViewer)
    {
        var w = scrollViewer.ActualWidth;
        if (w <= 0)
            return 0;
        var pad = scrollViewer.Padding;
        return Math.Max(0, w - pad.Left - pad.Right);
    }

    public static void SetWideCentered(bool value)
    {
        if (IsWideCentered == value)
            return;
        IsWideCentered = value;
        WideCenteredChanged?.Invoke(null, EventArgs.Empty);
        NotifyViewportChanged();
    }

    public static ScrollViewer? GetPageContentScroll() => _pageContentScroll;

    public static void SetPageContentScroll(ScrollViewer? scrollViewer)
    {
        if (ReferenceEquals(_pageContentScroll, scrollViewer))
            return;
        _pageContentScroll = scrollViewer;
        NotifyViewportChanged();
    }

    public static void AttachHostColumn(FrameworkElement column)
    {
        if (HostColumns.Contains(column))
            return;

        HostColumns.Add(column);

        void Refresh()
        {
            if (_pageContentScroll?.XamlRoot != null)
                ApplyToColumn(column, GetViewportWidth(_pageContentScroll));
        }

        void OnWideChanged(object? s, EventArgs e) => Refresh();

        column.Loaded += (_, _) =>
        {
            Refresh();
            WideCenteredChanged += OnWideChanged;
        };
        column.Unloaded += (_, _) =>
        {
            WideCenteredChanged -= OnWideChanged;
            HostColumns.Remove(column);
        };
    }

    public static void NotifyViewportChanged()
    {
        for (var i = Registrations.Count - 1; i >= 0; i--)
        {
            var reg = Registrations[i];
            if (reg.Column.XamlRoot == null)
            {
                Registrations.RemoveAt(i);
                continue;
            }

            ApplyToColumn(reg.Column, GetViewportWidth(reg.Scroll));
        }

        if (_pageContentScroll?.XamlRoot == null)
            return;

        var hostViewport = GetViewportWidth(_pageContentScroll);
        for (var i = HostColumns.Count - 1; i >= 0; i--)
        {
            var column = HostColumns[i];
            if (column.XamlRoot == null)
            {
                HostColumns.RemoveAt(i);
                continue;
            }

            ApplyToColumn(column, hostViewport);
        }
    }

    public static void ApplyToColumn(FrameworkElement column, double viewportWidth)
    {
        if (!IsWideCentered)
        {
            column.ClearValue(FrameworkElement.WidthProperty);
            column.ClearValue(FrameworkElement.MinWidthProperty);
            column.ClearValue(FrameworkElement.HorizontalAlignmentProperty);
            column.ClearValue(FrameworkElement.MaxWidthProperty);
            return;
        }

        var max = SettingsWindowLayout.ContentMaxWidth;
        column.MaxWidth = max;

        if (viewportWidth >= max)
        {
            column.Width = max;
            column.MinWidth = max;
            column.HorizontalAlignment = HorizontalAlignment.Center;
            return;
        }

        column.ClearValue(FrameworkElement.WidthProperty);
        column.ClearValue(FrameworkElement.MinWidthProperty);
        column.HorizontalAlignment = HorizontalAlignment.Stretch;
    }

    public static void AttachColumn(FrameworkElement column, ScrollViewer scrollViewer)
    {
        var reg = new ColumnRegistration(column, scrollViewer);
        Registrations.Add(reg);

        void Refresh() => ApplyToColumn(column, GetViewportWidth(scrollViewer));

        void OnWideChanged(object? s, EventArgs e) => Refresh();

        scrollViewer.SizeChanged += (_, _) =>
        {
            Refresh();
            NotifyViewportChanged();
        };
        column.Loaded += (_, _) =>
        {
            Refresh();
            WideCenteredChanged += OnWideChanged;
        };
        column.Unloaded += (_, _) =>
        {
            WideCenteredChanged -= OnWideChanged;
            Registrations.Remove(reg);
        };
    }
}
