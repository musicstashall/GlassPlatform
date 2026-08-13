#if DEBUG
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using WinRT;

namespace GlassSettings;

public static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        ComWrappersSupport.InitializeComWrappers();

        Application.Start(p =>
        {
            _ = p;
            var context = new DispatcherQueueSynchronizationContext(DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            new App();
        });
    }
}
#endif
