namespace GlassSettings.Services;

/// <summary>
/// Состояние уведомлений на «Обзоре» — переживает пересоздание HomePage.
///  • СТАРТОВОЕ зелёное (кастомизация) — MirrorOnly; activity/крестик/навигация с Обзора → левый footer;
///  • ЖЁЛТОЕ — pinned до крестика → правый footer; при возврате — снова в layout.
/// </summary>
internal static class HomeSessionNoticeState
{
    public static bool StartupNoticePending { get; set; } = true;

    /// <summary>Отложенный показ стартового notice уже поставлен в очередь (переживает пересоздание HomePage).</summary>
    public static bool StartupNoticeDispatchScheduled { get; set; }

    /// <summary>Стартовое green запланировано (ещё не обязательно показано на экране).</summary>
    private static bool _startupGreenArmed;

    /// <summary>
    /// Стартовое green реально показано (informational mirror) и ждёт осаждения в левый footer.
    /// </summary>
    public static bool StartupGreenAwaitingFooterDeposit { get; private set; }

    /// <summary>Зелёное закрыто / потреблено — больше не показывать.</summary>
    public static bool GreenDismissed { get; private set; }

    /// <summary>Жёлтое закрыто руками в текущей сессии несохранённых — не показывать до новой мутации/сохранения.</summary>
    public static bool YellowDismissed { get; private set; }

    /// <summary>Жёлтое уведомление о перезагрузке закрыто — показать кнопку в карточке, не повторять notice.</summary>
    public static bool RebootNoticeDismissed { get; private set; }

    /// <summary>Левая часть футера (кастомизация) раскрыта — после осаждения стартового green.</summary>
    public static bool FooterLeftRevealed { get; private set; }

    /// <summary>Правая часть футера (несохранённые) раскрыта — после закрытия жёлтого notice.</summary>
    public static bool FooterRightRevealed { get; private set; }

    /// <summary>Любая часть футера раскрыта (для совместимости с прежней логикой видимости).</summary>
    public static bool SummaryFooterRevealed => FooterLeftRevealed || FooterRightRevealed;

    public static event EventHandler? SummaryFooterRevealedChanged;

    private static bool _animateNextOpen;

    // --- Стартовое зелёное (кастомизация → footer) ---

    /// <summary>Показывать ли зелёное уведомление на странице.</summary>
    public static bool ShouldShowGreen => !GreenDismissed;

    /// <summary>Сохранять informational startup-green при открытии pinned yellow/update.</summary>
    public static bool ShouldPreserveStartupGreenInformational =>
        (_startupGreenArmed || StartupGreenAwaitingFooterDeposit) && !GreenDismissed;

    /// <summary>Запланировать стартовое green→footer (до фактического present зеркала).</summary>
    public static void ArmStartupGreenFooterDeposit()
    {
        if (GreenDismissed || FooterLeftRevealed)
            return;

        _startupGreenArmed = true;
    }

    /// <summary>
    /// Подтвердить present стартового green (вызывать в начале PresentInformationalMirror,
    /// до slide-in/grace — иначе activity-dismiss не осадит footer).
    /// </summary>
    public static void MarkStartupGreenPresentedIfArmed()
    {
        if (!_startupGreenArmed || GreenDismissed || FooterLeftRevealed)
            return;

        _startupGreenArmed = false;
        StartupGreenAwaitingFooterDeposit = true;
    }

    /// <summary>Начало закрытия green — больше не показывать зеркало/полосу.</summary>
    public static void StartDismissGreen()
    {
        GreenDismissed = true;
    }

    /// <summary>
    /// Завершение закрытия green: в footer только после реального показа (AwaitingFooterDeposit).
    /// </summary>
    public static void CompleteDismissGreen()
    {
        DepositStartupGreenToFooterIfPending();
    }

    /// <summary>
    /// Уход с «Обзора»: снять green и всегда открыть левый footer (кастомизация).
    /// Независимо от того, успело ли стартовое green показаться (pending/armed/awaiting).
    /// WarmAttach под splash не вызывает этот путь (<c>_suppressHomeNavigatedFrom</c>).
    /// </summary>
    public static void ConsumeGreenForNavigation()
    {
        // Первый уход с Обзора завершает hide-until-green: футер при возврате всегда (если customCount>0).
        if (!FooterLeftRevealed)
            RevealFooterLeft();

        GreenDismissed = true;
        _startupGreenArmed = false;
        StartupGreenAwaitingFooterDeposit = false;
        StartupNoticePending = false;
        StartupNoticeDispatchScheduled = false;
    }

    /// <summary>Idempotent: осадить стартовое green в левый footer, если оно было показано и ещё не осаждено.</summary>
    public static void DepositStartupGreenToFooterIfPending()
    {
        if (!StartupGreenAwaitingFooterDeposit)
            return;

        StartupGreenAwaitingFooterDeposit = false;
        _startupGreenArmed = false;
        RevealFooterLeft();
    }

    // --- Жёлтое (несохранённые) ---

    /// <summary>Показывать ли жёлтое уведомление (есть несохранённые и оно не закрыто руками).</summary>
    public static bool ShouldShowYellow(bool hasUnsavedChanges) => hasUnsavedChanges && !YellowDismissed;

    /// <summary>Начало ручного закрытия жёлтого по крестику — только сброс флага dismissed.</summary>
    public static void StartDismissYellow()
    {
        YellowDismissed = true;
    }

    /// <summary>Завершение закрытия жёлтого — раскрыть правую строку футера.</summary>
    public static void CompleteDismissYellow(bool hasUnsavedChanges)
    {
        // При ручном закрытии крестиком раскрываем футер всегда, даже если несохранённых сейчас нет
        // (может быть сценарий: были изменения, закрыли notice, потом сохранили программно).
        RevealFooterRight();
    }

    public static void OnUnsavedChanged(bool hasUnsavedChanges)
    {
        if (hasUnsavedChanges)
            return;

        // Сохранили/откатили — жёлтое неактуально: сброс на следующую сессию изменений + скрыть правый футер.
        YellowDismissed = false;
        if (FooterRightRevealed)
        {
            FooterRightRevealed = false;
            SummaryFooterRevealedChanged?.Invoke(null, EventArgs.Empty);
        }
    }

    /// <summary>Новая мутация при несохранённых — вернуть жёлтое, даже если было закрыто руками.</summary>
    public static void InvalidateDismissOnMutation()
    {
        var changed = false;

        if (YellowDismissed)
        {
            YellowDismissed = false;
            changed = true;
        }

        if (FooterRightRevealed)
        {
            FooterRightRevealed = false;
            changed = true;
        }

        // Новая мутация = новый lifecycle ID зеркала на Обзоре.
        NoticeViewportMirrorService.NotifyNewPinnedNoticeEpoch();

        if (changed)
            SummaryFooterRevealedChanged?.Invoke(null, EventArgs.Empty);
    }

    /// <summary>Пользователь закрыл жёлтое уведомление о перезагрузке.</summary>
    public static void DismissRebootNotice() => RebootNoticeDismissed = true;

    /// <summary>Требование перезагрузки отменено — сбросить dismiss для следующего цикла.</summary>
    public static void ResetRebootNoticeIfInactive(bool requiresRestart)
    {
        if (!requiresRestart)
            RebootNoticeDismissed = false;
    }

    // --- Анимация открытия ---

    public static void SuppressAnimateForNavigation() => _animateNextOpen = false;

    public static void RequestAnimateNextOpen() => _animateNextOpen = true;

    public static bool ConsumeAnimateNextOpen()
    {
        if (!_animateNextOpen)
            return false;

        _animateNextOpen = false;
        return true;
    }

    // --- Футер ---

    /// <summary>
    /// После завершения lifecycle стартового green (dismiss/consume) левый footer обязан быть открыт.
    /// Восстанавливает состояние, если Consume вызвали до arm или со старой логикой.
    /// </summary>
    public static void EnsureFooterLeftAfterStartupLifecycle()
    {
        if (!GreenDismissed || FooterLeftRevealed)
            return;

        RevealFooterLeft();
    }

    /// <summary>Раскрыть левую часть футера (после осаждения стартового green).</summary>
    public static void RevealFooterLeft()
    {
        if (FooterLeftRevealed)
            return;

        FooterLeftRevealed = true;
        SummaryFooterRevealedChanged?.Invoke(null, EventArgs.Empty);
    }

    /// <summary>Раскрыть правую часть футера (после закрытия жёлтого notice).</summary>
    public static void RevealFooterRight()
    {
        if (FooterRightRevealed)
            return;

        FooterRightRevealed = true;
        SummaryFooterRevealedChanged?.Invoke(null, EventArgs.Empty);
    }
}
