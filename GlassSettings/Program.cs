// Вход только для Debug: в Release используется генерируемый WinUI Main (свой bootstrap WASDK),
// поэтому замеры старта (StartupPhaseTrace) и порядок инициализации сверять на обеих конфигурациях.
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
