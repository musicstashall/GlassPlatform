using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace GlassSettings.Core.Layout;

/// <summary>Две колонки карточек на Обзоре → одна при узком viewport.</summary>
public static class HomePageColumnsLayout
{
    public const double SingleColumnBreakpoint = 680;

    private sealed class Registration
    {
        public required ScrollViewer Scroll { get; init; }
        public required Grid ColumnsHost { get; init; }
        public required StackPanel LeftColumn { get; init; }
        public required StackPanel RightColumn { get; init; }
        public required ColumnDefinition GapColumn { get; init; }
        public required ColumnDefinition RightColumnDef { get; init; }
        public bool IsSingleColumn;
        public readonly List<UIElement> RightMoved = new();
    }

    private static readonly List<Registration> Registrations = new();

    public static void Attach(
        ScrollViewer scroll,
        Grid columnsHost,
        StackPanel leftColumn,
        StackPanel rightColumn,
        ColumnDefinition gapColumn,
        ColumnDefinition rightColumnDef)
    {
        var reg = new Registration
        {
            Scroll = scroll,
            ColumnsHost = columnsHost,
            LeftColumn = leftColumn,
            RightColumn = rightColumn,
            GapColumn = gapColumn,
            RightColumnDef = rightColumnDef
        };
        Registrations.Add(reg);

        void Refresh() => Apply(reg);

        void OnWideChanged(object? s, EventArgs e) => Apply(reg);

        scroll.SizeChanged += (_, _) => Refresh();
        columnsHost.SizeChanged += (_, _) => Refresh();
        scroll.Loaded += (_, _) =>
        {
            Refresh();
            SettingsContentColumnLayout.WideCenteredChanged += OnWideChanged;
        };
        scroll.Unloaded += (_, _) =>
        {
            SettingsContentColumnLayout.WideCenteredChanged -= OnWideChanged;
            Registrations.Remove(reg);
        };

        scroll.DispatcherQueue.TryEnqueue(DispatcherQueuePriority.Low, () => Apply(reg));
    }

    public static void NotifyViewportChanged()
    {
        for (var i = Registrations.Count - 1; i >= 0; i--)
        {
            var reg = Registrations[i];
            if (reg.Scroll.XamlRoot == null)
            {
                Registrations.RemoveAt(i);
                continue;
            }

            Apply(reg);
        }
    }

    private static void Apply(Registration reg)
    {
        var w = Math.Max(
            SettingsContentColumnLayout.GetViewportWidth(reg.Scroll),
            reg.ColumnsHost.ActualWidth);
        if (w <= 0)
            return;

        var single = w < SingleColumnBreakpoint;
        if (single == reg.IsSingleColumn && (single || reg.RightMoved.Count == 0))
        {
            if (single != reg.IsSingleColumn)
                SetColumnMode(reg, single);
            return;
        }

        if (single && !reg.IsSingleColumn)
        {
            reg.RightMoved.Clear();
            while (reg.RightColumn.Children.Count > 0)
            {
                if (reg.RightColumn.Children[0] is UIElement el)
                {
                    reg.RightColumn.Children.RemoveAt(0);
                    reg.LeftColumn.Children.Add(el);
                    reg.RightMoved.Add(el);
                }
            }
        }
        else if (!single && reg.IsSingleColumn)
        {
            foreach (var el in reg.RightMoved)
            {
                reg.LeftColumn.Children.Remove(el);
                reg.RightColumn.Children.Add(el);
            }
            reg.RightMoved.Clear();
        }

        SetColumnMode(reg, single);
        reg.IsSingleColumn = single;
    }

    private static void SetColumnMode(Registration reg, bool single)
    {
        if (single)
        {
            reg.RightColumnDef.Width = new GridLength(0);
            reg.GapColumn.Width = new GridLength(0);
            reg.RightColumn.Visibility = Visibility.Collapsed;
        }
        else
        {
            reg.RightColumnDef.Width = new GridLength(1, GridUnitType.Star);
            reg.GapColumn.Width = new GridLength(12);
            reg.RightColumn.Visibility = Visibility.Visible;
        }
    }
}
