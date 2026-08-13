using System.Runtime.InteropServices;

namespace GlassSettings.Services;

/// <summary>Клик/двойной клик в TreeView file picker по системному интервалу (user32).</summary>
internal static class ExecutableFilePickerTreeClickHelper
{
    private static readonly object ClickStateLock = new();

    private static string? _lastClickKey;
    private static uint _lastClickPointerId;
    private static long _lastClickTick;
    private static bool _lastWasExpanded;
    private static string? _consumeReleaseKey;

    [DllImport("user32.dll")]
    private static extern uint GetDoubleClickTime();

    /// <summary>Стабильный ключ узла (не зависит от пересоздания объекта после навигации).</summary>
    public static string GetItemClickKey(ExecutableFilePickerTreeItem item) =>
        !string.IsNullOrWhiteSpace(item.FullPath)
            ? item.FullPath
            : $"{(int)item.Kind}:{item.Name}";

    /// <summary>
    /// Регистрирует PointerPressed на фоновом потоке; при double-click вызывает <paramref name="onDoubleClick"/>
    /// с состоянием IsExpanded на первом клике пары.
    /// </summary>
    public static void RegisterPointerPressed(
        ExecutableFilePickerTreeItem item,
        uint pointerId,
        long pressedAtTick,
        bool wasExpandedAtPress,
        Action<bool> onDoubleClick)
    {
        var work = new ClickWorkItem(
            GetItemClickKey(item),
            pointerId,
            pressedAtTick,
            wasExpandedAtPress,
            onDoubleClick);
        ThreadPool.UnsafeQueueUserWorkItem(ProcessClickWorkItem, work);
    }

    /// <summary>Поглощает PointerReleased второго клика пары double-click.</summary>
    public static bool TryConsumePointerReleased(ExecutableFilePickerTreeItem item)
    {
        var clickKey = GetItemClickKey(item);
        lock (ClickStateLock)
        {
            if (!string.Equals(_consumeReleaseKey, clickKey, StringComparison.Ordinal))
                return false;

            _consumeReleaseKey = null;
            return true;
        }
    }

    private static void ProcessClickWorkItem(object? state)
    {
        if (state is not ClickWorkItem work)
            return;

        EvaluatePointerPressedOnBackground(
            work.ClickKey,
            work.PointerId,
            work.PressedAtTick,
            work.WasExpandedAtPress,
            work.OnDoubleClick);
    }

    private sealed class ClickWorkItem(
        string clickKey,
        uint pointerId,
        long pressedAtTick,
        bool wasExpandedAtPress,
        Action<bool> onDoubleClick)
    {
        public string ClickKey { get; } = clickKey;
        public uint PointerId { get; } = pointerId;
        public long PressedAtTick { get; } = pressedAtTick;
        public bool WasExpandedAtPress { get; } = wasExpandedAtPress;
        public Action<bool> OnDoubleClick { get; } = onDoubleClick;
    }

    private static void EvaluatePointerPressedOnBackground(
        string clickKey,
        uint pointerId,
        long pressedAtTick,
        bool wasExpandedAtPress,
        Action<bool> onDoubleClick)
    {
        var interval = GetDoubleClickTime();
        bool isDouble;
        var wasExpandedAtFirstClick = false;

        lock (ClickStateLock)
        {
            var elapsed = _lastClickKey is not null ? pressedAtTick - _lastClickTick : -1;
            isDouble = _lastClickKey == clickKey &&
                       _lastClickPointerId == pointerId &&
                       _lastClickKey is not null &&
                       elapsed >= 0 &&
                       elapsed <= interval;

            if (!isDouble)
            {
                _lastClickKey = clickKey;
                _lastClickPointerId = pointerId;
                _lastClickTick = pressedAtTick;
                _lastWasExpanded = wasExpandedAtPress;
            }
            else
            {
                wasExpandedAtFirstClick = _lastWasExpanded;
                _lastClickKey = null;
                _lastClickPointerId = 0;
                _lastClickTick = 0;
                _consumeReleaseKey = clickKey;
            }
        }

        if (isDouble)
            onDoubleClick(wasExpandedAtFirstClick);
    }
}
