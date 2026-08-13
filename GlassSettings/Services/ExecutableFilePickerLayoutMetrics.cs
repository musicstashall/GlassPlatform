namespace GlassSettings.Services;

/// <summary>Метрики compact/inline layout файлового picker (hosted-диалог).</summary>
public static class ExecutableFilePickerLayoutMetrics
{
    public const double TreePaneWidth = 260;

    public const double TreeListGutter = 12;

    public const double TreeListDividerWidth = 1;

    /// <summary>Вертикальный отступ разделителя дерева/списка сверху и снизу.</summary>
    public const double TreeListDividerInset = 12;

    public const double ListMinWidth = 300;

    public const double ContentRightInset = 24;

    public const double Hysteresis = 1;

    /// <summary>Ширина shell: дерево + разделитель + правый inset + минимум списка.</summary>
    public const double CompactBreakpoint = TreePaneWidth
        + TreeListGutter
        + TreeListDividerWidth
        + TreeListGutter
        + ContentRightInset
        + ListMinWidth;

    public const double InlineDialogMinWidth = CompactBreakpoint;

    /// <summary>Минимум shell в compact (дерево скрыто) — ещё ~50 px уже inline.</summary>
    public const double CompactDialogMinWidth = CompactBreakpoint - 50;

    public const double DialogMinWidth = InlineDialogMinWidth;

    /// <summary>Оценка высоты строки списка (иконка + подпись).</summary>
    public const double ListModeRowEstimatedHeight = 52;

    /// <summary>Оценка высоты строки таблицы (детальный вид).</summary>
    public const double DetailsModeRowEstimatedHeight = 40;

    /// <summary>Иконка дерева (как WinUI Gallery, 20px).</summary>
    public const double TreeIconSize = 20;

    /// <summary>Высота TreeViewItem (иконка 20px + вертикальный зазор).</summary>
    public const double TreeRowFluentMinHeight = 32;

    /// <summary>FontSize подписи дерева и списка file picker.</summary>
    public const double FilePickerContentFontSize = 12;

    /// <summary>Для диагностики: ожидаемая высота строки.</summary>
    public const double TreeRowTotalHeight = TreeRowFluentMinHeight;

    public const double TreeRowContentHeight = TreeIconSize;

    /// <summary>Дополнительный зазор: последний пункт чуть выше нижней кромки viewport.</summary>
    public const double ListScrollBottomLift = 10;

    /// <summary>Нижний overscroll дерева: одна строка + lift (как у списка).</summary>
    public const double TreeScrollBottomLift = 10;

    /// <summary>Доп. extent снизу (не уменьшает viewport) — чуть прокрутить ниже последнего пункта.</summary>
    public static double GetScrollExtentBottomLift() => ListScrollBottomLift;

    /// <summary>Padding ScrollViewer: 0 — видимая область = высота контрола.</summary>
    public static double GetContentScrollBottomInset(bool detailsView) => 0;

    public static double GetListScrollBottomInset(bool detailsView) =>
        GetContentScrollBottomInset(detailsView);

    public static double GetTreeScrollBottomInset() => GetContentScrollBottomInset(false);

    public const double TreeItemEstimatedHeight = TreeRowTotalHeight;

    /// <summary>Оценка высоты header hosted-диалога (title + margin).</summary>
    public const double HostedDialogHeaderEstimate = 40;

    /// <summary>Margin сверху ScrollViewer body в hosted chrome.</summary>
    public const double HostedDialogScrollTopMargin = 8;

    /// <summary>Оценка footer hosted-диалога (divider + кнопки + padding).</summary>
    public const double HostedDialogFooterEstimate = 80;

    /// <summary>Навигационная полоса file picker.</summary>
    public const double PanelNavigationChrome = 56;

    /// <summary>Командная полоса file picker.</summary>
    public const double PanelCommandChrome = 48;

    /// <summary>Минимальная высота зоны дерева/списка.</summary>
    public const double TreePaneMinHeight = 240;

    /// <summary>Запас на внешний clip hosted-диалога (≈1 строка дерева).</summary>
    public const double HostedDialogClipRowSafety = TreeRowTotalHeight;

    /// <summary>Бюджет высоты body внутри hosted ScrollViewer.</summary>
    public static double ComputeHostedBodyBudget(double dialogHeight) =>
        Math.Max(0, dialogHeight - HostedDialogHeaderEstimate - HostedDialogScrollTopMargin - HostedDialogFooterEstimate);

    /// <summary>Бюджет высоты panel body с учётом clip одной строки.</summary>
    public static double ComputeHostedPanelBudget(double dialogHeight)
    {
        var bodyBudget = ComputeHostedBodyBudget(dialogHeight);
        return bodyBudget > HostedDialogClipRowSafety
            ? bodyBudget - HostedDialogClipRowSafety
            : bodyBudget;
    }

    /// <summary>Бюджет высоты строки дерева+списка (row 2 panel).</summary>
    public static double ComputeTreePaneBudget(double dialogHeight)
    {
        var panelBudget = ComputeHostedPanelBudget(dialogHeight);
        var treeBudget = panelBudget - PanelNavigationChrome - PanelCommandChrome;
        return Math.Max(TreePaneMinHeight - HostedDialogClipRowSafety, treeBudget);
    }
}
