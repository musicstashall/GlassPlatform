using GlassSettings.Core.Text;
using GlassSettings.Services;
using Microsoft.UI.Xaml;

namespace GlassSettings;

public partial class App : Application
{
    public static MainWindow? ShellWindow { get; private set; }

    public App()
    {
        InitializeComponent();
        UnhandledException += (_, e) =>
        {
            System.Diagnostics.Debug.WriteLine($"GlassSettings UnhandledException: {e.Exception}");
            if (e.Exception.StackTrace is not null)
                System.Diagnostics.Debug.WriteLine(e.Exception.StackTrace);
            if (e.Exception.InnerException is not null)
                System.Diagnostics.Debug.WriteLine($"  Inner: {e.Exception.InnerException}");
        };
        TaskScheduler.UnobservedTaskException += (_, e) => e.SetObserved();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        if (!SingleInstance.TryAcquire())
        {
            Exit();
            return;
        }

        SettingsBackupAssociation.EnsureIfMissing();
        if (SettingsBackupOpenRequest.FromCommandLine() is { } backupPath)
            SettingsBackupOpenRequest.Set(backupPath);

        // UI-поток: с него нельзя обходить дерево задач планировщика.
        NativeHostInterop.NoteUiThread();

        AppThemeService.ApplyApplicationThemeAtStartup();
        UiLanguageService.ApplyAtStartup();
        SystemFontsSpiCache.LoadAtStartup();

        // «Проверять настройки при старте» — на фоновом потоке, запуск не задерживает.
        if (GlassSettings.Core.Registry.GlassSettingsRegistry.ReadValidateSettingsOnStartup())
            _ = System.Threading.Tasks.Task.Run(() => NativeHostInterop.ValidateRegistryState(applyFixes: true));

        // Штатный старт: ctor готовит тему/backdrop/chrome/placement, затем shell первого кадра,
        // показ — один Activate() (без cloak, парковки за экраном и «догоняющего» backdrop).
        // Порядок shell после ShellWindow: HomePage/NoticeViewportMirrorService читают App.ShellWindow.
        // См. docs/GlassSettings-startup-window-plan.md.
        var window = new MainWindow();
        ShellWindow = window;
        window.PrepareShellForStartup();
        window.ActivateAtStartup();
    }
}
