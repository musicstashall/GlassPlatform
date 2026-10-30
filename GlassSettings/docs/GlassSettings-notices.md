# GlassSettings — уведомления о несохранённых изменениях

Единый сценарий notice-полосы (`SettingsUnsavedNoticeStrip`) на **Обзоре**, **hub-** и **detail-** страницах. Новые экраны и правки — только в рамках этого шаблона.

См. также: [GlassSettings-development.md](GlassSettings-development.md), [GlassSettings-hosted-dialog.md](GlassSettings-hosted-dialog.md), [GlassSettings-customization-tracking.md](GlassSettings-customization-tracking.md) (gate → удаление ключей, baseline снимка).

---

## Компоненты

| Компонент | Путь | Назначение |
|-----------|------|------------|
| Полоса notice | [`SettingsUnsavedNoticeStrip`](../../GlassSettings/Controls/SettingsUnsavedNoticeStrip.xaml) | UI: фон, иконка, текст, «Сохранить», крестик |
| Стек notice | [`SettingsPageNoticeStack`](../../GlassSettings/Controls/SettingsPageNoticeStack.xaml) | Session + ExternalSync + Action в `SettingsPageShell` |
| Notice на detail/hub | [`SettingsSessionNoticeBar`](../../GlassSettings/Controls/SettingsSessionNoticeBar.xaml) | Слот Session внутри стека |
| Зеркало viewport | [`NoticeViewportMirrorService`](../../GlassSettings/Services/NoticeViewportMirrorService.cs) | Дублирование закреплённых полос стека у верхней кромки; единственный канал для информационных Success |
| **Центр уведомлений** | [`SettingsNoticeCoordinator`](../../GlassSettings/Services/SettingsNoticeCoordinator.cs) | Единая политика слотов (≤3), бэклог, проекция на полосы/зеркало; host: Обзор, shell, Процессы |
| Политика показа | [`NoticePresentationPolicy`](../../GlassSettings/Services/NoticePresentationPolicy.cs) | Green = MirrorOnly (Success без Save/action); Yellow/Action/Error = PinnedLayout |
| Анимация host зеркала | [`NoticeHostSlideAnimator`](../../GlassSettings/Services/NoticeHostSlideAnimator.cs) | Slide/fade на `WindowTopNoticeColumn` |
| Host зеркала | [`MainWindow`](../../GlassSettings/MainWindow.xaml) — `WindowTopNoticeColumn` | Acrylic, бордюр, `ThemeShadow` (Z=12) |
| Обзор | [`HomePage`](../../GlassSettings/Pages/HomePage.xaml) — `SessionNoticeStrip` | Глобальный commit + footer карточки устройства |
| Состояние Обзора | [`HomeSessionNoticeState`](../../GlassSettings/Services/HomeSessionNoticeState.cs) | dismiss, startup green, save success |
| Состояние страницы | [`PageSessionNoticeState`](../../GlassSettings/Services/PageSessionNoticeState.cs) | dismiss, `RequestAnimateNextOpen` |
| Baseline реестра | [`SettingsSnapshot`](../../GlassSettings/Core/Snapshot/SettingsSnapshot.cs), [`SettingsSession`](../../GlassSettings/Services/SettingsSession.cs) | dirty / save / revert |
| Мониторинг DWM | [`RegistrySnapshotWatchService`](../../GlassSettings/Services/RegistrySnapshotWatchService.cs) | RegNotify HKCU\\DWM, debounce 450 ms + **settle 1000 ms**, fingerprint + **semantic** dirty; внешние изменения → baseline sync + зелёное MirrorOnly notice на detail |
| Scope commit | [`SettingsCommitScope`](../../GlassSettings/Core/Snapshot/SettingsCommitScope.cs), [`CommitScopeRegistry`](../../GlassSettings/Services/CommitScopeRegistry.cs) | Область «Сохранить» на странице |

---

## SettingsNoticeCoordinator (единый центр)

Все страницы запрашивают показ/скрытие через [`SettingsNoticeCoordinator`](../../GlassSettings/Services/SettingsNoticeCoordinator.cs). Зеркало и анимации — по-прежнему в `NoticeViewportMirrorService`; coordinator ведёт **экземпляры** notice per-page и применяет политику слотов.

| Host | Адаптер |
|------|---------|
| «Обзор» | `HomeNoticePageHost` — три legacy-полосы в `HomeNoticeHost` |
| hub/detail/Система | `ShellNoticePageHost` — `SettingsPageNoticeStack` |
| «Процессы» | `LegacyNoticePageHost` — `ActionNoticeStrip` |

**Лимит:** не более **3** видимых экземпляров на странице. Цвет слота: Green (Success без кнопок), Yellow (session Warning), Action (Warning + action), Error.

| Правило | Поведение |
|---------|-----------|
| **1a** | При `visibleCount < 3` однотипные могут занимать несколько слотов; на «Обзоре» на полосе — последний экземпляр цвета |
| **1b** | При 3 занятых и (все одного цвета **или** цвет на ≥2 слота): скрыть **ранний**同色 → слот новому **другому** цвету |
| **2** | Три разных цвета + четвёртый: новый в `Hidden`, без auto-promote |
| **3** | Promote из бэклога — **только** крестик (`OnUserDismissed`); policy-hide (save, retract) — **без** promote |

**Зелёное (Green):** везде **только зеркало** (`MirrorOnly`, полоса `Collapsed`) — hub/detail/Процессы/Обзор/ExternalSync; без viewport-check; одноразовое (не restore при навигации). Первая активность (scroll / wheel / click после grace 600 ms) или крестик на зеркале — retract green. **Обзор:** тот же dismiss → левый footer. См. [информационные Success](#информационные-success-уведомления).

**«Обзор»:** зелёное — только MirrorOnly; жёлтое + update OpenGlass — pinned в `HomeNoticeHost`. Update **не** вытесняет yellow. Якорь pinned-зеркала — `HomeNoticeViewportAnchor` (ниже полос).

---

## Внешний вид и severity

| Severity | Цвет | Когда |
|----------|------|--------|
| **Success** | зелёный (`SystemFillColorSuccess*`) | Информационные уведомления **без** обязательного действия (нет «Сохранить», нет action-кнопки) — см. [информационные Success](#информационные-success-уведомления) |
| **Warning** | жёлтый (`SystemFillColorCaution*`) | **Требует действия:** несохранённые параметры (session notice с «Сохранить»), доступна новая версия OpenGlass, прочие предупреждения |
| **Error** | красный (`SystemFillColorCritical*`) | Ошибки операций, сбои policy, attach/process errors |

- **Фон всей полосы** (не рамка): см. таблицу выше — `SettingsUnsavedNoticeStrip.ApplySeverityChrome()`.
- **Session notice «несохранённые параметры»** — всегда **Warning**, не Error (detail/hub через `SettingsSessionNoticeBar`, Обзор через `HomePage.ShowSessionNotice`).
- **Кнопка «Сохранить»** — стиль `GlassSettingsNoticeSaveButtonStyle`, **сразу после текста** с интервалом **20 px**.
- **Кнопка действия** (опционально) — тот же слот и стиль, что «Сохранить»: `ShowActionButton`, `ActionButtonText`, событие `ActionRequested`. Текст по умолчанию для загрузки релиза — `Notice_Download` («Скачать»). Зеркало проксирует клик через `NotifyActionRequested()`.
- **Крестик** — `SubtleButtonStyle`, tooltip `Dialog_Close`; в зеркале (`FloatingOverlayChrome`) — hover `ControlFillColorSecondaryBrush`.
- Hosted-диалоги — отдельно ([hosted-dialog](GlassSettings-hosted-dialog.md)).

---

## Анимации (`SettingsUnsavedNoticeStrip`)

| Действие | Длительность | Примечание |
|----------|--------------|------------|
| Появление (fade + slide сверху) | **1000 ms** | Transform на корневом `UserControl`, смещение ~−56 px |
| Исчезновение (fade) | **500 ms** | Только явное закрытие (`CloseAnimatedAsync`) |
| Смена сообщения | без паузы | Старое — `CloseImmediate`, новое — `OpenAnimatedAsync` |
| Зеркало (host) show / activity hide / green X | **1000 ms** | `NoticeHostSlideAnimator` на `WindowTopNoticeColumn`; внутренняя полоса — `OpenImmediate` |
| Крестик pinned-зеркала | **0** (immediate) | `HideMirrorImmediate` + dismiss epoch; штатная полоса не трогать |

---

## Слоты notice (SettingsPageNoticeStack)

| Слот | Содержимое | Replace |
|------|------------|---------|
| **Session** | unsaved / save success | как раньше: success заменяет unsaved |
| **ExternalSync** | зелёное MirrorOnly: внешняя синхронизация DWM раздела | показ только в момент sync-commit; **не** restore при входе; `ClearPending` на навигации |
| Policy | OpenGlass ↔ Root (unsaved flow) | удалено — только session notice |
| **Action** | success/error операций; **доступна новая версия OpenGlass** | один action-notice; кнопка «Скачать» через `ShowActionNoticeAsync(..., actionButtonText)` |

Разные слоты **стекируются** вертикально над контентом страницы.

### Semantic dirty (DWM)

Единый критерий snapshot↔реестр для точек строк, page notice (`HasUnsavedInScope`), registry watch и external-sync: [`IsSemanticallyEqual`](../../GlassSettings/Core/Dwm/DwmDefaults.cs) (delete-on-default: absent ≡ default DWORD). Fingerprint change без semantic diff — только обновление fingerprint, без sync/notice.

### External-sync commit (registry watch)

Детект быстрый; **commit** (baseline sync, `MarkScopesSynced`, зелёное notice, reload detail) — только после паузы:

| Этап | Интервал | Поведение |
|------|----------|-----------|
| Debounce | 450 ms | сжатие RegNotify-шторма на HKCU/HKLM `DWM` |
| Settle | **1000 ms** | **один** таймер на весь DWM; каждое новое semantic-dirty **перезапускает** его |
| Commit | tick settle | read-only semantic probe → один `ApplyExternalRegistrySyncFromWatch()` (batch всех затронутых scope) |

Во время settle (`IsExternalSyncCommitPending`) session notice (жёлтое «Сохранить») **не** открывается — probe read-only, без `StampDirtyScopesFromSnapshot`.

**ExternalSync green (MirrorOnly, one-shot):** pending в [`PageExternalSyncNoticeState`](../../GlassSettings/Services/PageExternalSyncNoticeState.cs) только до показа/dismiss на текущей странице; при входе на страницу — `ClearPending()` + green entries снимаются в `RestoreShellAfterNavigationAsync` (**без** `TryShowExternalSyncNoticeAsync` на navigate). Повторный показ — только новый sync-commit (`DirtyChanged` → `TryShowExternalSyncNoticeAsync`).

Исключения (settle **отменяется**, sync не ставится в очередь): suppress на наши `WriteDwm*` / `DeleteDwm*` (600 ms), suppress после save/revert/sync (800 ms–2 s), активный жест слайдера (`SettingsInteractionDeferral.IsSliderGestureActive`), semantic clean после probe, **user-origin DWM dirty** (`HasUserOriginDwmDirty` — `_scopeDirtyFlags` ∩ DWM scopes после `MarkDirty` / `MarkScopeDirty`; semantic dirty от наших live-write **не** external-sync), **page operation pipeline** (фазы Animating / RegistryMutation / Cosmetic / NoticeSync — см. [GlassSettings-page-pipeline.md](GlassSettings-page-pipeline.md)).

### Page operation pipeline (serial-конвейер)

> **Канонический гайд для ассистентов:** [GlassSettings-page-pipeline.md](GlassSettings-page-pipeline.md) — назначение, **обязательный порядок фаз**, API, defer-флаги, deferred preview, эталон GlassType pivot, anti-patterns, adopters.

[`SettingsPageOperationGate`](../../GlassSettings/Services/SettingsPageOperationGate.cs) на [`SettingsPageShell`](../../GlassSettings/Controls/SettingsPageShell.xaml.cs): **одна очередь** на страницу, фазы **не пересекаются**.

| Фаза | Содержимое | Notice | Registry watch |
|------|------------|--------|----------------|
| **Animating** | pivot transition, collapse expander | defer | defer evaluate/commit |
| **RegistryMutation** | `WriteDwm*` / `DeleteDwm*`, `Reload*`, UI settle ×2, sync fingerprint | defer | defer |
| **Cosmetic** | preview, indicators (read-only) | defer | defer |
| **NoticeSync** | один `FlushSessionNotice` / `RefreshProgrammatic` | **sync** | defer |
| **Idle** | обычная работа | live | live |

Правила notice внутри pipeline:

- `MarkDirty` / `DirtyChanged` во время defer-фаз **обновляют session**, но notice **не** мигает — один flush в **NoticeSync**.
- При новой мутации (`dirty=true`) — `ClearSaveSuccessPending()`; открытый Success-notice **апгрейдится** в Warning (`SyncOpenNoticeForDirtyAsync`), а не quiet sync.
- При ручном возврате к снимку (`!dirty`) — `ClearSaveSuccessPending()` + закрытие notice (без повторного success).

Остальное (порядок фаз, preview defer, GlassType pivot, anti-patterns) — в [GlassSettings-page-pipeline.md](GlassSettings-page-pipeline.md).

---

## Информационные Success-уведомления

Критерий: `SettingsNoticeSeverity.Success` **и** нет кнопки «Сохранить» **и** нет action-кнопки (`NoticePresentationPolicy.IsInformationalMirrorOnly`). `ResolvePresentMode` → Green всегда `MirrorOnly` (включая «Обзор» и ExternalSync).

| Правило | Поведение |
|---------|-----------|
| Штатная полоса | **Нет** — `Visibility.Collapsed`, без `OpenImmediate` / `OpenAnimatedAsync` |
| Показ | Только плавающее зеркало у верхней кромки — slide-in host (`NoticeHostSlideAnimator`, **1000 ms**), без viewport-check |
| Автоскрытие | После **grace 600 ms**: wheel, click или прокрутка `|ΔVerticalOffset| > 4 px` → retract green (`NotifyDismissFromEphemeral`) |
| Крестик на зеркале | Всегда (`AllowDismiss`); dismiss → `CompleteInformationalDismissFromMirrorAsync` (SlideUpOut 1000 ms) → ephemeral retract / Home footer |
| Informational epoch | Пока `IsInformationalMirrorActive`: `SyncMirrorToViewportAsync` skip; session `OnDirtyChanged(!dirty)` не закрывает notice |
| Session strip слоты | На одном strip **взаимоисключение** Green ↔ Yellow / Action / Error |
| Ephemeral dismiss | `RetractInformationalSuccessAsync`; **не** restore при навигации |
| Навигация | `OnPageNavigated()` скрывает зеркало; green entries / ExternalSync pending сбрасываются |

**Примеры:** «Параметры сохранены»; ExternalSync DWM; стартовое green на «Обзоре»; action-success без кнопки на «Процессы».

### Обзор — green MirrorOnly + pinned yellow/update

| Полоса | Режим | Назначение |
|--------|-------|------------|
| `SessionNoticeStrip` | **MirrorOnly** (зелёное) | кастомизация — одноразовое зеркало |
| `UnsavedNoticeStrip` | Pinned (`DisableInformationalMirror`) | несохранённые + «Сохранить» |
| `UpdateNoticeStrip` | Pinned | новая версия OpenGlass |

Координатор: [`RefreshHomeAsync`](../../GlassSettings/Services/SettingsNoticeCoordinator.cs). Якорь pinned-зеркала: **`HomeNoticeViewportAnchor`** (1px под полосами; не весь `HomeNoticeHost`).

**Стартовое зелёное (единственное → footer):** после fade-in контента (`ShellContentReadyGate` на Completed, не в первом кадре) → `ArmStartupGreenFooterDeposit` → present зеркала со штатным slide-in → `MarkStartupGreenPresentedIfArmed`. На **первом** показе Обзора левый footer скрыт до закрытия green (activity/крестик). **Любая навигация с Обзора** (`ConsumeGreenForNavigation`) — снять green и **всегда** открыть левый footer (даже если green ещё pending/не armed); при возврате на Обзор футер с кастомизацией виден. Green **не** restore при возврате. Splash WarmAttach Processes↔Home — suppress `OnNavigatedFrom` (`_suppressHomeNavigatedFrom`), чтобы не осадить footer до первого показа green. **Не** снимать informational startup-green при open pinned yellow/update.

**Pinned yellow/update:** зеркало по `HomeNoticeViewportAnchor`; первая активность **или крестик на зеркале** гасит **только зеркало** текущего lifecycle ID (штатная полоса жива); новая мутация → новый ID → зеркало снова по viewport-policy. Полосы живут до крестика **на штатной полосе**. При уходе с Обзора — `CloseImmediate` (без анимации); при возврате — снова в **штатных** местах (`Refresh` / `OpenImmediate`).

**Сброс жёлтого на новую мутацию:** `InvalidateDismissOnMutation`.

**Startup green → footer (особый scroll-compensate):** green MirrorOnly не меняет layout страницы; при dismiss осаждается левый footer (`RevealSummaryFooterAnimatedAsync`, ~65 px). Если карточка устройства / зона футера **вне viewport** (`VerticalOffset > ε` и probe не Visible) — мгновенный reveal без MaxHeight-анимации + `TryRevealHomeFooterOffScreenPreservingScroll` (`ChangeView` на +ΔExtent, suppress `RepositionThemeTransition`). В кадре — прежняя анимация высоты футера (~350 ms), без scroll-compensate. Остальные open/close штатных полос — обычная off-screen компенсация.

**Footer карточки устройства — две независимые части (left/right):**

| Часть | Содержимое | Reveal-флаг | Когда раскрывается |
|-------|------------|-------------|--------------------|
| Левая | кастомизация («Изменённые параметры: N») | `HomeSessionNoticeState.FooterLeftRevealed` | после **скрытия** стартового green (activity/крестик) **или** навигация с Обзора при armed/awaiting; первая строка — анимация высоты футера (~350 ms) on-screen / instant+compensate off-screen |
| Правая | несохранённые («Несохранённые изменения») + «Сохранить» | `HomeSessionNoticeState.FooterRightRevealed` | крестик **штатного** жёлтого (не зеркала, не навигация) |

- Отступ снизу строки футера: **0** (зазор до следующей карточки — `RootStack.Spacing` 12); сверху под разделителем — 12 px.
- On-screen reveal: suppress `RepositionThemeTransition`; старт `MaxHeight` с 12 (замена снятого MainPanel bottom pad) → target, без двойного скачка.
- Пустой `HomeNoticeHost`: `Visibility=Collapsed` (иначе layout давал ~24px до колонок); якорь зеркала — 1px overlay на `ColumnsHost`. При yellow/update host Visible.
- `SummaryFooterRevealedChanged` → `MainWindow.RefreshDisplayedHomeFooter` на текущей `HomePage`.
- Пустая левая колонка — `ZeroLength`; ширина &lt; 480 px — две строки.

### Сводка: показ и скрытие (informational vs pinned)

| Тип | Показ | Скрытие по активности | Крестик зеркала | Крестик штатной полосы |
|-----|-------|----------------------|-----------------|------------------------|
| Informational Success (green) | Только зеркало, slide-in **1000 ms** | Retract green (+ Home → left footer); навигация — immediate hide + footer | SlideUpOut → ephemeral retract / footer | N/A (`Collapsed`) |
| Pinned Warning / Action / Error | Layout + зеркало при off-screen | Hide mirror текущего epoch ID; полоса жива | Только зеркало (`HideMirrorImmediate` + dismiss epoch); полоса жива | Animated close; навигация — `CloseImmediate` |

`ReleaseSource` на session strip — только при `IsVisuallyOpen` (ephemeral dismiss informational не снимает mirror-binding).

---

## Зеркало notice (viewport mirror)

Единый сценарий для страниц с `SettingsPageNoticeStack`, а также legacy-полос (`HomePage`, `ProcessesPage`).

| Правило | Поведение |
|---------|-----------|
| Инвариант (pinned) | `mirrorVisible ⇔` открытые pinned-полосы **и** якорь вне viewport **и** штатная не в кадре **и** epoch не dismissed **и** `VerticalOffset > ε` |
| Когда показывать (pinned) | `MustShow` только если якорь `NotVisible`, полосы измерены, scroll не у верха, штатное notice не видно, epoch активен |
| Когда скрывать (pinned) | `MustHide`: `scrollTopNoMirror`, якорь `Visible`, strip в viewport, нет полос, `dismissedEpoch`; **activity** → dismiss текущего epoch (полосы не закрывать) |
| **Возврат на страницу** | Зеркало **не** поднимать: страница сверху, якорь ниже полос, штатное видно. Restore полос + `Reattach*Async` — только quiet sync |
| Gate активации | До измерения якоря/полос — `indeterminate`, новое зеркало **не** активируется |
| Зелёный Success (все страницы) | Только `PresentInformationalMirrorAsync`; без viewport-check; activity/крестик → retract; **не** restore |
| Стек в зеркале | `WindowTopNoticeMirrorStack` дублирует **все** открытые полосы страницы сверху вниз (мульти-notice) |
| При навигации | `OnPageNavigated()` — immediate hide + сброс epoch ID. Обзор: `OnNavigatedFrom` → green→footer. Возврат: yellow/red в layout; green нет |
| Показ зеркала | Slide-in host (`NoticeHostSlideAnimator`) — **один раз** при появлении по viewport-policy |
| Повторное изменение настроек | Тихий sync `SyncMirrorForStackQuietAsync` (`animateShow:false`, `animateHide:false`) + `ScheduleSettleSync` (4×70 ms) |
| On-screen open (`!useMirror`) | `OpenAnimatedAsync` для штатной полосы; зеркало скрыто policy |
| Прокрутка при показанном зеркале | Идемпотентный sync; прокрутка — `SyncMirrorForStackAsync` с анимацией show/hide |
| Settle / reattach | Тихий sync без анимации (`RunSettleSyncAsync`, `ReattachPageAsync` / `ReattachLegacyAsync`) |
| Прокрутка вниз | Якорь ушёл из вида → зеркало **появляется** (slide-in) — единственный полезный кейс |
| Прокрутка вверх | Якорь снова в viewport / scroll≈0 → зеркало **скрывается** |
| Activity-dismiss зеркала | гасит текущий **lifecycle ID** (`_dismissedPinnedMirrorEpochId`); новая мутация → `NotifyNewPinnedNoticeEpoch` → новый ID → зеркало снова по viewport-policy |
| Крестик на зеркале (pinned) | `HideMirrorImmediate(manualDismissForPage: true)` — host hide **без** slide-out + dismiss epoch; **штатная полоса не закрывается** |
| Крестик на зеркале (green) | `CompleteInformationalDismissFromMirrorAsync` — SlideUpOut **1000 ms** → ephemeral retract / Home left footer |
| Дедуп при scroll≈0 | `scrollTopNoMirror` + `scrollTopGuard` + `anchorVisible` / `stripVisibleInViewport` |
| Дедуп содержимого | `BuildSpecs` — одинаковые полосы не дублируются |
| Новое уведомление off-screen | Штатное `OpenImmediate` + scroll-compensate при `VerticalOffset > ε` (не ждать `useMirror`/measure); зеркало по policy / settle |
| Новое уведомление on-screen | Штатное `OpenAnimatedAsync`; зеркало скрыто |
| Стабилизация скролла (off-screen) | Скрытие/показ штатной при `VerticalOffset > ε`: `ClosePinned*` / open-compensate → `ChangeView(offset±ΔExtent)` + suppress `RepositionThemeTransition`; synthetic scroll **не** activity-dismiss. **Обзор startup green→footer:** если карточка вне кадра — instant footer reveal + compensate (`TryRevealHomeFooterOffScreenPreservingScroll`); в кадре — анимация высоты без ChangeView |
| Крестик на штатной полосе | `HideMirrorImmediate` + `ClosePinnedSourcePreservingScrollAsync` + `DismissRequested` / `OnUserDismissed` |
| Штатное при зеркале | **Никогда не скрывается** — зеркало дубль для off-screen якоря |
| Save success mirror | После retract + commit: `RunAfterUiSettledAsync` ×2 + `InformationalLayoutSettleMs` (400 ms), затем present informational; informational epoch до user dismiss |
| Смена страницы | `OnPageNavigated()` + restore полос + quiet `ReattachPageAsync` |

**Подключение (shell-страницы):** `SettingsSessionNoticeBar` → `PresentStackItemAsync(stack, strip, PageScrollViewer, …)`; якорь — `ViewportAnchor` под стеком. **Legacy:** `HomePage`, `ProcessesPage` — `PresentAsync` + свой якорь под полосами; после `OnNavigatedTo` и внешней policy-mutation (OpenGlass) — `ReattachLegacyAsync(scroll, anchor)`. Все полосы одной страницы — **общий** якорь и **один** `ScrollViewer` shell.

**Мульти-notice:** pinned legacy-полосы в зеркале сортируются по порядку в `HomeNoticeHost` (yellow → update). Green на Обзоре — отдельный informational epoch (не в pinned-стеке).

**Update-notice (OpenGlass):** координатор [`ProductUpdateCoordinator`](../../GlassSettings/Services/ProductUpdateCoordinator.cs). Severity — **Warning** (жёлтый caution). Notice скрывается крестиком до **новой** pending-версии: в реестре `OpenGlassUpdateNoticeDismissedVersion` (сравнивается с `PendingVersion`). Карточка «Подсистемы» и спойлер OpenGlass на **Система** (третья подстрока): точка + ссылка «новая версия …», пока есть `PendingUrl` — **всегда**, независимо от dismiss notice. Показ notice на shell-страницах — после `NavFrame.Navigated` (defer `Loaded`), иначе зеркало сбрасывается `OnPageNavigated`. Кнопка «Скачать» / ссылка → `Launcher.OpenUriAsync(pendingUrl)`.

**Оформление зеркала:** `ApplyMirrorNoticeChromeBackground`, `SurfaceStrokeColorDefaultBrush`, `ThemeShadow` (Z=12).

---

## Когда анимировать, когда нет

### С анимацией появления

- Первый показ после **закрытия** notice — `RequestAnimateNextOpen`.
- **Сохранение**: `CloseImmediate` → commit → success: плавающее green-зеркало до первой активности / крестика.
- **Изменение параметра** на detail/hub: после `PointerReleased` / `KeyUp` на shell + debounce **120 ms**.
- **После шторы / диалога** (программный показ): `RequestAnimateNextOpen` + `RefreshSessionNoticeProgrammatic()` (закрывает возможный notice без анимации, открывает заново с slide).

### Без анимации (`OpenImmediate`)

- **Вход на страницу** (`RefreshForNavigation`) — восстановить **не-зелёные** notice, если scope dirty и **не** dismiss крестиком; без анимации. **Все green** (save-success, ExternalSync, Home) — **не** восстанавливать (`ResetForNavigation` / `ClearPending` / `ConsumeGreenForNavigation`).
- Notice **уже открыт** — только обновить текст/кнопку.
- Штатное notice при показе зеркала: открывается через `OpenImmediate`, т.к. якорь сейчас вне viewport (пользователь его не видит из-за прокрутки, **а не** из-за скрытия). Само штатное notice остаётся видимым и при возврате в кадр. Δ высоты layout при off-screen open/close гасится компенсацией скролла (см. таблицу зеркала).

### Программный показ (`RefreshProgrammatic`)

После hosted-диалога, удаления раздела на hub, операций под шторой — когда нет `PointerReleased` на shell:

```csharp
PageSessionNoticeState.RequestAnimateNextOpen();
PageShell?.RefreshSessionNoticeProgrammatic();
```

`SettingsSessionNoticeBar.RefreshProgrammatic()` сбрасывает defer шторы, закрывает notice (`CloseNoticeImmediate`), выставляет `_dirtyPending` / `_pointerReleased`, вызывает `Refresh`.

`OnOverlayBecameIdle` только сбрасывает `_deferNoticeUntilOverlayIdle` — **не** открывает notice (избегает показа без анимации перед `RefreshProgrammatic`).

### ComboBox и notice

[`SettingsInteractionDeferral.WhenComboCommit`](../../GlassSettings/Services/SettingsInteractionDeferral.cs) после commit вызывает `SettingsPageShell.NotifyInteractionCompleted()` — иначе notice после смены Effect на hub не откроется.

### Слайдеры и notice

Паттерн deferred-commit: [GlassSettings-slider-commit.md](GlassSettings-slider-commit.md).

| Правило | Поведение |
|---------|-----------|
| Во время drag | `IsSliderGestureActive` → notice **не** открывается; только `_dirtyPending` |
| После commit / отпускания | `ExitSliderGesture` → `NotifyInteractionCompleted` → debounce **120 ms** → одно notice |
| Dismiss крестиком | `InvalidateDismissOnMutation()` после нового commit — notice снова показывается |

### Dismiss и повторный показ

Dismiss крестиком **штатной** полосы жёлтого/action сохраняется между входами на страницу (hub/detail). Крестик **зеркала** pinned — только dismiss текущего mirror-epoch (полоса и dismiss-флаг страницы не меняются); после новой мутации (`NotifyNewPinnedNoticeEpoch`) зеркало снова по viewport-policy. При возврате **не-зелёные** полосы — всегда в **штатных** местах (`RefreshForNavigation` / `RestoreShellAfterNavigationAsync` / Home `Refresh`); **зеркало** при входе не поднимается (`scrollTopNoMirror`). Зеркало снова только после прокрутки off-screen. **Green** — одноразовые: не restore; на Обзоре при уходе с armed/awaiting — сразу в левый footer. ExternalSync pending сбрасывается `ClearPending` при входе.

`PageSessionNoticeState.InvalidateDismissOnMutation()` / `HomeSessionNoticeState.InvalidateDismissOnMutation()` — сброс dismiss при новой мутации параметров, пока scope остаётся dirty.

### Закрытие окна

При закрытии с несохранёнными параметрами `SettingsSession.BeginShutdown()` выставляется до `RevertOnExit`: `DirtyChanged` не рассылается, shell/Home notice и зеркало не обновляются и не анимируются — только откат снимка. Завершение: `AppWindow.Hide()` → `Environment.Exit(0)` (см. `MainWindow.RunAppCloseSequenceAsync`).

### Обзор (`HomePage`)

- Green — MirrorOnly; yellow/update — pinned (`DisableInformationalMirror` на yellow/update).
- Footer left — после dismiss green (activity/крестик); right — после крестика yellow. Подробнее: [Обзор — green MirrorOnly + pinned yellow/update](#обзор--green-mirroronly--pinned-yellowupdate).
- Якорь pinned-зеркала — `HomeNoticeViewportAnchor`.
- `RefreshSessionNotice` не перетирает notice во время `_saveInProgress`.

---

## Scope страниц (notice + «Сохранить»)

| Страница | `CommitScope` |
|----------|----------------|
| Обзор | commit всего (`ApplyCommitAsync`) |
| Настройки Glass (hub) | без session notice (точки на строках; Save — на detail / Обзоре) |
| Popup hub | `ForArea(Popup)` |
| Панель задач | `TaskbarScopeCatalog.BarPageScope` — Tray + `Tray\Xaml`, без `Tray\Xaml\*` |
| Смежные окна | `TaskbarScopeCatalog.AdjacentPageScope` — только `Tray\Xaml\*` |
| DWM hub | `DwmPlatform` |
| DWM detail | `ForArea(Dwm*)` |
| Процессы | без session notice (списки пишутся сразу); действия — `ActionNoticeStrip` |

Точки unsaved на спойлерах — **гранулярные** scope (`ForFlyoutParameterGroup`).

Успех после «Сохранить» привязан к scope страницы (`PageSessionNoticeState.SaveSuccessScope`) и флагу `TryConsumePendingSuccessDisplay()` (только сразу после commit). **Ручной возврат** параметров к снимку (`!dirty` без commit) — notice **закрывается**, success **не** показывается (`ClearSaveSuccessPending`).

---

## Baseline снимка и dirty

| Событие | Действие |
|---------|----------|
| Первый показ окна | `SettingsSession.Capture()` — полный снимок BWM/DWM партиций |
| Изменение в реестре | `MarkDirty()` / `MarkScopeDirty()` → сравнение live vs snapshot |
| **Сохранить** | `ApplyCommitScopeAsync(scope)` → `SettingsSnapshot.CaptureScope(scope)` **синхронно**, затем live-reload в фоне |
| `CaptureScope` | Партиции scope + `SyncTrackedBwmParameters` (все отслеживаемые ключи из `CommitScopeCatalog.GetTrackedBwmParameters`) |
| Группа flyout | `FlyoutParameterGroup` → `UpsertParameterSnapshot` по параметрам группы |
| Закрытие окна | `HasUnsavedChanges` → `SettingsSession.Revert()` |
| `Revert` | Восстановление значений; `CreateSubKey` для отсутствующих подразделов; `PruneOrphanSubKeyTrees` — удаление поддеревьев, отсутствующих в снимке |

AttachList / BlockList (корень BWM), GlassSettings (placement), Crashes — вне снимка; сегмент **DisabledList** — вне снимка **везде**. Канон: [GlassSettings-customization-tracking.md — Политика snapshot](GlassSettings-customization-tracking.md#политика-snapshot-реестра).

---

## Индикаторные точки (unsaved / update)

Цвет точки слева от иконки — та же **severity**, что у notice-полосы ([`SettingsIndicatorChrome`](../../GlassSettings/Services/SettingsIndicatorChrome.cs), [`UnsavedIndicatorDot`](../../GlassSettings/Controls/UnsavedIndicatorDot.xaml)):

| Severity | Цвет | Когда |
|----------|------|--------|
| **Success** | зелёный | информационные состояния (зарезервировано) |
| **Warning** | жёлтый | несохранённые параметры, доступно обновление |
| **Error** | красный | ошибки (зарезервировано) |

| Элемент | Severity по умолчанию |
|---------|----------------------|
| `UnsavedDot` на hub/detail/Обзоре | **Warning** |
| `SettingsDividerExpander` «Дополнительно» (свёрнут) | **Warning** (полупрозрачный жёлтый фон шапки; без точки) |
| `UpdateDot` (новая версия OpenGlass) | **Warning** |
| `StatusIndicatorDot` на Обзоре | цвет статуса платформы (отдельно от unsaved) |

Цепочка DWM: Обзор (строки Colorization / Стекло / Стилизация) → пункт меню «Настройки DWM» (`DwmPlatform`) → hub-строки → detail-строки с `UnsavedScope`. Glass: аналогично через `SettingsUnsavedIndicatorCoordinator` (`glass` → `GlassPlatform`, `home` → `All`). Обновление: `SettingsUnsavedIndicatorCoordinator.RefreshAll` на `DirtyChanged`.

### Divider «Дополнительно» (`SettingsDividerExpander`)

Секция свёрнута → пользователь не видит точки на вложенных pivot/spoiler. Вместо точки на шапке divider:

| Состояние | Поведение |
|-----------|-----------|
| Свёрнут + dirty в scope секции | Полупрозрачный жёлтый фон шапки (`SystemFillColorCautionBackground`), pill-форма (`CornerRadius = height/2`), hover — плотнее тот же caution-фон |
| Развёрнут | Прозрачный фон шапки; точки на дочерних контролах (pivot tabs, gate-expander) |

Обновление: `SettingsUnsavedIndicatorRefresh.Refresh` на `DirtyChanged` (через `SettingsUnsavedIndicatorCoordinator.RefreshAll`). Для divider вызывается `RefreshUnsavedIndicator()` (учитывает `HasUnsavedOverride`, если `UnsavedScope` не задан).

| Страница | Scope |
|----------|--------|
| Popup → «Дополнительно» (Menu) | `HasUnsavedOverride`: любой `MenuItemKind` dirty |
| DWM Colorization (Композиция) | `DwmColorizationScopeCatalog.AdvancedScope(hive)` |
| DWM Theme (Стилизация) | `DwmThemeScopeCatalog.AdvancedScope(hive)` |

Контрол: [`SettingsDividerExpander`](../../GlassSettings/Controls/SettingsDividerExpander.xaml.cs) — `UnsavedScope`, опционально `HasUnsavedOverride`, `RefreshUnsavedIndicator()`.

---

## Action-кнопка в полосе

API на [`SettingsUnsavedNoticeStrip`](../../GlassSettings/Controls/SettingsUnsavedNoticeStrip.xaml.cs):

| Свойство / событие | Назначение |
|--------------------|------------|
| `ShowActionButton` | показать кнопку вместо «Сохранить» |
| `ActionButtonText` | подпись (например `Notice_Download`) |
| `ActionRequested` | клик по кнопке |

`OpenImmediate` / `OpenAnimatedAsync` принимают `showAction` и `actionText`, чтобы зеркало (`OpenSourceStripAsync`) не сбрасывало action при off-screen показе.

На shell-страницах: `SettingsPageNoticeStack.ShowActionNoticeAsync(severity, message, actionButtonText)`. Публичный доступ к полосе: `ActionNotice`.

---

## Страница «Процессы» — action notice

Списки Attach/Block пишутся в реестр сразу; session notice с «Сохранить» нет.

| Сценарий | Порядок |
|----------|---------|
| Добавление из списка процессов | Штора → диалог (`retainOverlayAfterSelection`) → под шторой: запись, обновление списка, прокрутка к строке → `Pop` → **одно** notice с анимацией |
| Добавление из файла | Picker → штора → то же ядро → `Pop` → notice |
| Дубликат процесса | Сброс фильтра, прокрутка к строке, подсветка, красный notice |

API: `CompleteAttachProcessSelectionAsync`, `NoticeViewportMirrorService.PresentAsync` на `ActionNoticeStrip`.

Прокрутка: `ScrollViewer.ChangeView` + `TransformToVisual` (не только `BringIntoView` на repeater).

---

## Hub Popup — отключение раздела (gate)

Штора → hosted-диалог (`ConfirmThenAsync`, `manageOverlay: false`) → под шторой: `CustomizationCatalog.RemoveFlyoutSection` (удаление **всего** поддерева `Menu` / …) → `Pop` → `RefreshSessionNoticeProgrammatic`.

Эталон: [`PopupHubPage.OnGateToggledAsync`](../../GlassSettings/Pages/Glass/PopupHubPage.xaml.cs), [`FlyoutSectionRemoveDialogService`](../../GlassSettings/Services/FlyoutSectionRemoveDialogService.cs).

---

## Диагностика (Debug)

Env-gated трасса в Debug Output Visual Studio. Включение (PowerShell перед запуском GlassSettings):

```powershell
$env:NoticeMirrorTrace = "1"   # зеркало / scroll / policy
$env:NoticeCoordTrace = "1"    # SettingsNoticeCoordinator
$env:NoticeSessionTrace = "1"  # session bar / dismiss / TryOpen
```

| Префикс | Класс | События |
|---------|-------|---------|
| `[NoticeMirror]` | [`NoticeMirrorTrace`](../../GlassSettings/Services/NoticeMirrorTrace.cs) | `EvaluatePinnedMirrorPolicy`, `SyncMirrorToViewportAsync`, `STICKY: indeterminate`, scroll sync skip, strip state |
| `[NoticeCoord]` | тот же | `Upsert`, `ProjectHost`, `Present`, `OnUserDismissed`, `RetractInformationalSuccess` |
| `[NoticeSession]` | тот же | `OnDirtyChanged`, `TryOpen`, `ShouldHide` / `_dismissed`, `OnInteractionCompleted`, `ShowNoticeAsync: skip dismissed`, `OperationGate` phase |

Типовые проверки:

- **Mirror X keep strip:** крестик pinned-зеркала → `HideMirrorImmediate` + `dismissedEpoch`; штатная `IsVisuallyOpen` остаётся true; нет `OnUserDismissed` / `ShouldHide` от зеркала.
- **Dismiss reopen:** после крестика **штатного** жёлтого — `ShouldHide=true`; клик по спойлеру → `OnInteractionCompleted: skip dismissed`.
- **Sticky mirror:** scroll≈0 + штатная полоса видна → `MustHide` / `scrollTopGuard` / `stripVisibleInViewport`; нет бесконечного `indeterminate defer`. Для диагностики залипшего жёлтого зеркала **обязательно** `$env:NoticeMirrorTrace=1` — без него в логе будут только `[RegWatch]`, не `[NoticeMirror]`.
- **Save→Green→Yellow:** после Save и новой мутации scroll должен управлять зеркалом; не должно быть `skip — informational epoch` при открытой жёлтой session-полосе.
- **ComboBox без смены:** нет `ShowNoticeAsync: skip dismissed` → reopen; нет semantic dirty от combo.

---

## Anti-patterns

- `ContentDialog` для модалок — только [`HostedDialogService`](GlassSettings-hosted-dialog.md).
- Notice на каждый `DirtyChanged` во время перетаскивания слайдера — defer до `PointerReleased` + debounce.
- `Refresh()` после шторы без `RefreshProgrammatic` — notice не появится или без анимации.
- `CommitScope` DP callback → `RefreshForNavigation` (ломает pointer-state); только `Refresh()`.
- Дублировать текст notice в footer: левая часть раскрывается **после** закрытия зелёного, правая — **после** крестика **штатного** жёлтого; пока notice открыт, соответствующая часть скрыта. Warning-notice **не** скрывает уже раскрытые части.
- Анимация при каждом `OnNavigatedTo`.
- **Скрывать штатную полосу (`Opacity`/`Visibility`/`IsHitTestVisible`) при активном зеркале.** Дедуп наложения «зеркало ↔ штатное» при scroll≈0 делается **скрытием зеркала**, а не штатной полосы.
- **Закрывать штатную полосу крестиком pinned-зеркала** (`NotifyDismissFromEphemeral` / `CloseImmediate` / `OnUserDismissed` на source) — крестик зеркала только `HideMirrorImmediate` + epoch; полоса жива до крестика на штатной.
- **Скрывать штатную полосу при зеркале ради «не дёргать» скролл** — вместо этого компенсировать `VerticalOffset` (`ScrollCompensate`); полоса остаётся в layout.
- **Анимировать MaxHeight футера Обзора при off-screen green→footer без ChangeView** — двойной дёрганье; только `TryRevealHomeFooterOffScreenPreservingScroll`.
- Закреплять информационные Success (green) в layout страницы — всегда только MirrorOnly; footer «Обзора» раскрывается через ephemeral dismiss, не через pinned green.

---

## Эталоны

| Сценарий | Файл |
|----------|------|
| Detail notice + pointer deferral | `SettingsPageShell.xaml.cs`, `SettingsSessionNoticeBar.xaml.cs` |
| Зеркало viewport | `NoticeViewportMirrorService.cs`, `MainWindow.xaml.cs` |
| Обзор + footer | `HomePage.xaml.cs` |
| Процессы: штора + notice | `ProcessesPage.xaml.cs`, `RunningProcessPickerDialogService.cs` |
| Popup hub: gate + диалог | `PopupHubPage.xaml.cs`, `FlyoutSectionRemoveDialogService.cs` |
| ComboBox + notice | `SettingsInteractionDeferral.cs` |
| Baseline / revert | `SettingsSnapshot.cs`, `SettingsSession.cs` |
