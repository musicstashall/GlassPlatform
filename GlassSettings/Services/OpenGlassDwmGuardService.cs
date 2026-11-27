using GlassSettings.Core.Platform;
using GlassSettings.Strings;
using Microsoft.UI.Dispatching;

namespace GlassSettings.Services;

/// <summary>
/// «Сторож» падений DWM после старта OpenGlass: вооружается ДО старта OG,
/// после карантина показывает жёлтое уведомление в Home/System с действием
/// «Включить автозапуск и запустить» (снимает карантин, включает автозапуск, запускает OG).
/// Карантин снимается автоматически при строго более новой версии OpenGlass
/// (IsVersionStrictlyGreater в маркере) или вручную по кнопке в уведомлении.
/// </summary>
public static class OpenGlassDwmGuardService
{
    public const uint ArmedBySettings = 2; // OpenGlassDwmGuard::kArmedBySettings

    /// <summary>Уведомление карантина нужно показать (маркер активен, не скрыто на эту сессию).</summary>
    public static event EventHandler? Changed;

    private static readonly object Sync = new();
    private static DispatcherQueueTimer? _pollTimer;
    private static bool _wasWatching;
    private static bool _noticeDismissed;

    /// <summary>Вызывать ДО команды старта OpenGlass. Side effect: если сторож уже ведёт
    /// Setup (named mutex), наблюдение не дублируется — вернёт true.</summary>
    public static bool ArmForSettings()
    {
        OpenGlassVersionService.TryGetInstalledVersion(out var version, out _);
        var armed = NativeHostInterop.OpenGlassDwmGuardArm(ArmedBySettings, version ?? string.Empty);
        // Другой процесс (Setup) мог уже владеть именованным мьютексом — тогда мы не watcher'ы.
        if (armed && NativeHostInterop.OpenGlassDwmGuardIsWatching())
            NoteWatching();
        return armed;
    }

    public static void Disarm() => NativeHostInterop.OpenGlassDwmGuardDisarm();

    public static bool IsWatching() => NativeHostInterop.OpenGlassDwmGuardIsWatching();

    /// <summary>Есть ли что показать в уведомлении. Побочный эффект: карантин снимается,
    /// если установленная версия OG строго новее записанной в маркере.</summary>
    public static bool ShouldShowNotice()
    {
        if (_noticeDismissed)
            return false;
        // ShouldSuppressOpenGlassStart выполняет и авторез «новее → снять».
        OpenGlassVersionService.TryGetInstalledVersion(out var installed, out _);
        return NativeHostInterop.OpenGlassDwmGuardShouldSuppressOpenGlassStart(installed ?? string.Empty);
    }

    /// <summary>Пользователь закрыл уведомление (карантин остаётся активен до версии/кнопки).</summary>
    public static void DismissNotice()
    {
        lock (Sync) { _noticeDismissed = true; }
        Changed?.Invoke(null, EventArgs.Empty);
    }

    /// <summary>Пользователь явно включил автозапуск OpenGlass в другом месте UI
    /// (меню строки): это согласие — снимаем карантин.</summary>
    public static void NotifyUserEnabledAutostart()
    {
        NativeHostInterop.OpenGlassDwmGuardClearQuarantine();
        lock (Sync) { _noticeDismissed = false; }
        Changed?.Invoke(null, EventArgs.Empty);
    }

    /// <summary>«Включить автозапуск и запустить»: снять карантин, включить автозапуск,
    /// запустить OpenGlass (запуск ручной — гард снова вооружается внутри StartOpenGlassAsync).</summary>
    public static async Task<bool> ReinstateAndLaunchAsync()
    {
        NativeHostInterop.OpenGlassDwmGuardClearQuarantine();
        await Task.Run(() => OpenGlassHostService.SetOrchestrationAutostartEnabled(true)).ConfigureAwait(true);
        lock (Sync) { _noticeDismissed = false; }
        var started = await SystemPlatformRunActions.StartOpenGlassAsync().ConfigureAwait(true);
        Changed?.Invoke(null, EventArgs.Empty);
        return started;
    }

    public static string BuildNoticeMessage()
    {
        var body = GlassSettingsResources.Get("OpenGlass_GuardNotice_Body");
        if (NativeHostInterop.OpenGlassDwmGuardGetQuarantineInfo(out var crashes, out _)
            && crashes > 0)
        {
            body = $"{body} ({crashes}× dwm.exe)";
        }
        return body;
    }

    private static void NoteWatching()
    {
        lock (Sync)
        {
            _wasWatching = true;
            _noticeDismissed = false; // новая попытка запуска — уведомление снова актуально
        }
        EnsurePollTimer();
    }

    private static void EnsurePollTimer()
    {
        if (_pollTimer is not null)
            return;

        // DispatcherQueueTimer живёт на UI-потоке; тикает только пока IsWatching().
        var queue = DispatcherQueue.GetForCurrentThread();
        if (queue is null)
            return;

        _pollTimer = queue.CreateTimer();
        _pollTimer.Interval = TimeSpan.FromMilliseconds(500);
        _pollTimer.IsRepeating = true;
        _pollTimer.Tick += (_, _) => OnPollTick();
        _pollTimer.Start();
    }

    private static void OnPollTick()
    {
        if (IsWatching())
            return;

        lock (Sync)
        {
            if (!_wasWatching)
                return;
            _wasWatching = false;
        }

        _pollTimer?.Stop();
        _pollTimer = null;
        // Сторож мог выполнить карантин — presence и уведомления должны это увидеть.
        PlatformPresenceService.Instance.NotifyLifecycleChanged();
        Changed?.Invoke(null, EventArgs.Empty);
    }
}
