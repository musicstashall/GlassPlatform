using System.Collections.Concurrent;

namespace GlassSettings.Services;

/// <summary>GDI+/System.Drawing требуют STA; один фоновый поток + таймаут для зависших Shell-вызовов.</summary>
internal static class StaThreadRunner
{
    private const int DefaultTimeoutMs = 3000;

    private sealed record WorkItem(Func<object?> Action, TaskCompletionSource<object?> Completion);

    private static readonly BlockingCollection<WorkItem> Queue = new();
    private static readonly Thread Worker;

    static StaThreadRunner()
    {
        Worker = new Thread(WorkerLoop)
        {
            IsBackground = true,
            Name = "GlassSettings.StaThreadRunner"
        };
        Worker.SetApartmentState(ApartmentState.STA);
        Worker.Start();
    }

    public static T? Run<T>(Func<T> func)
    {
        if (Thread.CurrentThread.GetApartmentState() == ApartmentState.STA)
            return func();

        var tcs = new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously);
        Queue.Add(new WorkItem(() => func(), tcs));
        var result = tcs.Task.GetAwaiter().GetResult();
        return result is T typed ? typed : default;
    }

    public static Task<T?> RunAsync<T>(Func<T> func)
    {
        // Всегда в фоновый STA-worker: поток WinUI тоже STA, иначе ApplyModeChoice
        // выполняется синхронно на UI и блокирует fade на сотни мс.
        var tcs = new TaskCompletionSource<object?>(TaskCreationOptions.RunContinuationsAsynchronously);
        Queue.Add(new WorkItem(() => func(), tcs));
        return tcs.Task.ContinueWith(
            static t =>
            {
                if (t.IsFaulted)
                {
                    t.Exception?.Handle(static _ => true);
                    return default;
                }

                return t.Result is T typed ? typed : default;
            },
            CancellationToken.None,
            TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    public static T? RunWithTimeout<T>(Func<T> func, int timeoutMs = DefaultTimeoutMs)
    {
        if (Thread.CurrentThread.GetApartmentState() == ApartmentState.STA)
            return func();

        T? result = default;
        Exception? error = null;
        var thread = new Thread(() =>
        {
            try
            {
                result = func();
            }
            catch (Exception ex)
            {
                error = ex;
            }
        })
        {
            IsBackground = true,
            Name = "GlassSettings.StaCall"
        };
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        if (!thread.Join(timeoutMs))
            return default;

        if (error is not null)
            return default;

        return result;
    }

    public static Task<T?> RunAsyncWithTimeout<T>(Func<T> func, int timeoutMs = DefaultTimeoutMs) =>
        Task.Run(() => RunWithTimeout(func, timeoutMs));

    private static void WorkerLoop()
    {
        foreach (var item in Queue.GetConsumingEnumerable())
        {
            try
            {
                item.Completion.TrySetResult(item.Action());
            }
            catch (Exception ex)
            {
                item.Completion.TrySetException(ex);
            }
        }
    }
}
