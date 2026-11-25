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

        if (SettingsBackupOpenRequest.FromCommandLine() is { } backupPath)
            SettingsBackupOpenRequest.Set(backupPath);

        // UI-поток: с него нельзя обходить дерево задач планировщика.
        NativeHostInterop.NoteUiThread();

        AppThemeService.ApplyApplicationThemeAtStartup();
        UiLanguageService.ApplyAtStartup();

        var window = new MainWindow();
        ShellWindow = window;
        // Показываем только заставку. Подготовка shell начнётся после её первого кадра.
        window.ActivateAtStartup();
    }
}
