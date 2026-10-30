# GlassSettings — разработка (WinUI 3)

Окно настроек Glass на WinUI 3 + .NET 9 (unpackaged x64).

См. также: [GlassSettings-WinUI.md](GlassSettings-WinUI.md), [GlassSettings-host-glass.md](GlassSettings-host-glass.md) (**host-glass, live reload, hub ComboBox**), [GlassSettings-slider-commit.md](GlassSettings-slider-commit.md) (**слайдеры — deferred-commit 250 ms**), [GlassSettings-adaptive-layout.md](GlassSettings-adaptive-layout.md) (**адаптивная разметка — перенос слайдеров и карточек**), [GlassSettings-localization.md](GlassSettings-localization.md) (**строки UI — только из resw**), [GlassSettings-detail-cards.md](GlassSettings-detail-cards.md) (типы карточек, gate, hover), [GlassSettings-tooltips.md](GlassSettings-tooltips.md) (**acrylic ToolTip**), [GlassSettings-customization-tracking.md](GlassSettings-customization-tracking.md) (подсистема учёта изменений в реестре), [TextVisualTuningWizard.md](TextVisualTuningWizard.md) (**мастер ink — кэш ArN, метки, STA-preview**), [assistant/GlassSettings-WinUI-chat-prompt.md](../assistant/GlassSettings-WinUI-chat-prompt.md).

---

## Модули и код

| Область | Путь |
|---------|------|
| UI, страницы, контролы | `GlassSettings/` |
| Native | `GlassSettings/Native/` |
| Артефакты | `Build\x64\Debug\GlassSettings.exe` |

Сборка: MSBuild x64 Debug из корня solution — [contributing/build.md](../contributing/build.md).

---

## Навигация: breadcrumb и «Назад»

Три независимых механизма; **не смешивать** их логику.

| Механизм | Где | Назначение |
|----------|-----|------------|
| `_trail` / `BreadcrumbBar` | [`SettingsNavigationService`](../../GlassSettings/Navigation/SettingsNavigationService.cs) | **Логический** путь в адресной строке. После `NavigateHomeDrill` hub может быть в trail, но не в истории Frame (пропущенный шаг с Обзора). |
| `_globalBackStack` | тот же сервис + `RememberForBack` | Только **TitleBar «Назад»**: один pop = фактический предыдущий шаг (sidebar tag + snapshot trail). |
| `Frame.BackStack` | `NavFrame` | Только цепочка `Push` / `TryPop` внутри секции. |

**Клик по breadcrumb** — [`MainWindow.TrailBreadcrumb_OnItemClicked`](../../GlassSettings/MainWindow.xaml.cs) → `NavigateToIndex(index)`: обрезать `_trail`, `ClearBackStack()`, **прямой** `_frame.Navigate` на `_trail[index]`. Глобальный стек «Назад» **не** менять.

**Клик по пункту боковой панели** — `NavigationView.ItemInvoked` (не `SelectionChanged`): всегда `NavigateTo(tag, force: true)` → корень раздела, даже если пункт уже выделен и открыта дочерняя страница.

**Anti-pattern:** `Frame.GoBack()` в breadcrumb — после drill с Обзора в BackStack лежит `HomePage`, а в trail — hub; клик по hub уводит на Обзор вместо hub-страницы. **`SelectionChanged` для sidebar** — повторный клик по активному разделу не срабатывает.

Ключевые API: `NavigateToRoot`, `Push`, `NavigateHomeDrill`, `TryGoBackGlobal` / `RestoreSnapshot`, `NavigateToIndex`.

---

## Host-glass и live reload

Подробно: **[GlassSettings-host-glass.md](GlassSettings-host-glass.md)**.

Кратко:

- **Native** — `EnsureGlassRuntime()` при старте `MainWindow`; shutdown перед exit.
- **Root `\NoBackdrop`** — `RootBackdropLiveReload.NotifyNoBackdropChange(noBackdrop)`; DWM notify только при включении и SWCA (pipeline OG выкл), не `liveFlyout`.
- **OpenGlass ↔ Root policy** — [GlassSettings-host-glass.md](GlassSettings-host-glass.md#политика-root--openglass); **мульти-notice** — [GlassSettings-notices.md](GlassSettings-notices.md#слоты-notice-settingspagenoticestack).
- **Flyout / taskbar** — `FlyoutLiveReload.*` или `WriteBwmDword(..., liveFlyout: true)`.
- **Hub с ComboBox** — флаг `_loading` + отложенный `Loaded` (`DispatcherQueue.TryEnqueue`); эталон — [`GlassSettingsHubPage`](../../GlassSettings/Pages/Glass/GlassSettingsHubPage.xaml.cs).

---

## Версия продукта и авто-инкремент FileVersion

**Версия продукта** задаётся **только** в [`Directory.Build.props`](../../Directory.Build.props) — свойство `GlassProductVersion` (Major.Minor.Patch[.0]). Она **одинакова** во всех бинарниках одной сборки (`ProductVersion` / `AssemblyVersion` / `InformationalVersion`).

**Версия файла** (`FileVersion`, 4-е поле) автоинкрементируется при каждой сборке **конкретного** проекта:

- Счётчик — `BuildNumber.txt` в каталоге проекта (коммитится в репозиторий). Единый для Debug/Release и x64/x86 **этого** проекта.
- `FileVersion` = `<первые 3 поля GlassProductVersion>.<BUILD>` (например `2.0.0.47`).
- У разных модулей BUILD может отличаться (per-module); пересобирается только тот модуль, который попал в текущую сборку.

Механизм:

- **C++ DLL/EXE** — [`Directory.Build.targets`](../../Directory.Build.targets) генерирует `$(IntDir)GlassBuildVersion.h` и подключает [`Shared/GlassModuleVersion.rc`](../../Shared/GlassModuleVersion.rc).
- **C#** (`GlassSettings`) — тот же targets генерирует `GlassBuildVersion.g.cs` с `[assembly: AssemblyFileVersion(...)]`; `GenerateAssemblyFileVersionAttribute=false`.
- Карточка «Компоненты» на Обзоре читает **версию файла** и **версию продукта** через [`PlatformModuleVersions`](../../GlassSettings/Services/PlatformModuleVersions.cs).

Релиз: правка `GlassProductVersion` вручную + пересборка нужных модулей. При запущенной платформе перелинковка загруженных DLL может дать LNK1168 — остановка платформы только пользователем.

**Периодичность проверки обновлений** — единая для Glass и OpenGlass, хранится в `GlassSettingsRegistry.UpdateCheckInterval` (Daily/Weekly/Monthly/Never); контрол — на странице «Система». [`OpenGlassReleaseUpdateService`](../../GlassSettings/Services/OpenGlassReleaseUpdateService.cs) использует интервал как срок годности кэша (`Never` — без авто-запроса).

**Задачи планировщика** ([`PlatformSchedulerRow.cs`](../../GlassSettings/Controls/PlatformSchedulerRow.cs)): тумблер справа создаёт/удаляет задачу; кнопка управления видна всегда (заблокирована без задачи). Старт Glass с главной и со «Системы» не ждёт schtasks, mutex и baseline на UI: диалог подтверждения остаётся на диспетчере, остальное — в фоне. Пока идёт старт или стоп, подпись и тон берутся из снимка `PlatformPresenceService`, а не из прямого опроса планировщика. Кольцо прогресса на сплит-кнопке центрируется по основной секции, без шеврона (`SplitFlyoutPartWidth`).

---

## Интерфейс (UI)

### Типы карточек (обязательно перед правками detail/hub)

**[GlassSettings-detail-cards.md](GlassSettings-detail-cards.md)** — канонические виды:

1. `SettingsSystemGateExpander` — спойлер с gate (системный Expander + кастомный шеврон)
2. `SettingsDividerExpander` — прозрачный разделитель-спойлер без gate (секция «Дополнительно»)
3. `SettingsNavigateRow` — навигация (шеврон → другая страница)
4. `SettingsOptionRow` / `DwmOpacityGroupRow` — плоская строка без разворота

Эталон всех типов: страница **Colorization**. Информационные блоки — **`SettingsInfoBlock`** ([GlassSettings-detail-cards.md](GlassSettings-detail-cards.md#информационный-блок-settingsinfoblock)).

### `GlassSettingsLabeledToggleSwitch`

- `StateText` (MinWidth 40) + компактный `ToggleSwitch` **44×44** (`GlassSettingsCompactToggleSwitchStyle`).
- `Spacing` 6 между текстом и switch.
- В hub — через `SettingsNavigateRow.TrailingContent`; в спойлерах и opacity-строках — в `Trailing` / `GateSwitch`.

### Shell: Mica, прокрутка, тултипы

- **Фон окна** — `MicaBackdrop` + прозрачный `NavigationView` shell ([`GlassSettingsNavigationChrome.xaml`](../../GlassSettings/Themes/GlassSettingsNavigationChrome.xaml)).
- **Прокрутка** — [`SettingsPageShell`](../../GlassSettings/Controls/SettingsPageShell.xaml): composition opacity-mask ([`SettingsScrollEdgeFade.cs`](../../GlassSettings/Core/Layout/SettingsScrollEdgeFade.cs)); маска на viewport; зона fade ~28 px; верхний зазор контента — `SettingsPageShellContentMargin` (32 px, в scroll extent) — [`GlassSettingsScrollFade.xaml`](../../GlassSettings/Themes/GlassSettingsScrollFade.xaml).
- **Тултипы** — только [`GlassSettingsToolTipService.SetToolTip`](../../GlassSettings/Services/GlassSettingsToolTipService.cs); подробно: **[GlassSettings-tooltips.md](GlassSettings-tooltips.md)**.

### Hosted-диалоги и штора

**[GlassSettings-hosted-dialog.md](GlassSettings-hosted-dialog.md)** — единый паттерн модальных окон:

- штора — только [`WindowBusyOverlayService`](../../GlassSettings/Services/WindowBusyOverlayService.cs) (`Push` / `Pop` / `Run` / `RunAsync`);
- диалоги — [`HostedDialogService`](../../GlassSettings/Services/HostedDialogService.cs) + `HostedDialogHost` в `MainWindow`;
- layout, padding body, bounds при resize — разделы **Layout и отступы** / **Bounds при resize** в том же гайде;
- не использовать `ContentDialog` для модалок GlassSettings.

### Уведомления о несохранённых изменениях

**[GlassSettings-notices.md](GlassSettings-notices.md)** — единый сценарий notice-полосы: анимации, **зеркало viewport**, programmatic refresh после шторы, scope, baseline снимка, страница «Процессы». Политика severity: Success — информация, **Warning — несохранённые параметры и действия**, Error — сбои. Новые страницы — только по этому документу.

### Baseline реестра (unsaved / revert)

- Снимок: [`SettingsSnapshot`](../../GlassSettings/Core/Snapshot/SettingsSnapshot.cs), фасад [`SettingsSession`](../../GlassSettings/Services/SettingsSession.cs).
- «Сохранить» обновляет baseline **только затронутого scope** (`CaptureScope` + tracked-параметры).
- Закрытие окна без сохранения — `Revert()` (см. [GlassSettings-notices.md](GlassSettings-notices.md), [GlassSettings-customization-tracking.md](GlassSettings-customization-tracking.md)).

### Страница «Система» (исключение)

Спойлеры **«Система OpenGlass»** и **«Платформа Glass»**:

- **Нет** gate-toggle в шапке; тумблер установки — отдельный `ToggleSwitch` в **`TrailingContent`** (не переносится).
- **`PlatformSchedulerRow`** — логика; **`PlatformSchedulerStackableActions`** в `HeaderActions`, отдельный `ToggleSwitch` в `TrailingContent`. Паттерн: [GlassSettings-adaptive-layout.md § Паттерн 1b](GlassSettings-adaptive-layout.md#паттерн-1b-platformschedulerrow-на-странице-система).
- **Dual (GS x64):** две navigate-строки («Платформа Glass x86» / «x64»), у каждой свой split и свой install-gate.
### Доноры WinUI

- `D:\VS_PROJECT\WinUI-Gallery-2.9.3\WinUIGallery\Pages\SettingsPage.xaml` — ориентир UX (не копировать `SettingsCard` на detail).
- `App.xaml` — `CommunityToolkit.WinUI.Controls.SettingsControls/Themes/Generic.xaml` (merged; detail-карточки — свои контролы).

### Отложено

| Тема | Статус |
|------|--------|
| Cap/центрирование контента при ширине окна ≥840 (nav 320px) | не внедрено; откат из чата 2026-06-04 |
| Миграция `SettingsGateExpander` → `SettingsSystemGateExpander` (Flyout, Taskbar, SWCA) | в очереди |
---

## Workflow

Backup в `_backups/<дата>/` перед правками UI — [contributing/workflow.md](../contributing/workflow.md).

**Строки UI:** только `GlassSettings/Strings/*/Resources.resw` и `GlassSettingsResources` — см. **[GlassSettings-localization.md](GlassSettings-localization.md)**. Новый текст без ключей в обоих языках (ru-RU, en-US) не коммитить.

Платформу Glass не запускать/останавливать из ассистента. Проверку UI выполняет пользователь (перезапуск `GlassSettings.exe` из `Build\x64\Debug\`).

---

## Индекс

| Документ | Содержание |
|----------|------------|
| [GlassSettings-WinUI.md](GlassSettings-WinUI.md) | Краткий обзор проекта |
| [GlassSettings-host-glass.md](GlassSettings-host-glass.md) | **Host-glass**, WinEvent, live reload, hub Backdrop |
| [GlassSettings-adaptive-layout.md](GlassSettings-adaptive-layout.md) | **Адаптивная разметка** — перенос слайдеров/карточек, симметрия, без таймеров |
| [GlassSettings-localization.md](GlassSettings-localization.md) | **Строки UI — только resw**, ru-RU / en-US |
| [GlassSettings-detail-cards.md](GlassSettings-detail-cards.md) | **Типы карточек**, ресурсы, gate, анти-паттерны |
| [GlassSettings-slider-commit.md](GlassSettings-slider-commit.md) | **Слайдеры** — deferred-commit 250ms, live preview |
| [GlassSettings-hosted-dialog.md](GlassSettings-hosted-dialog.md) | **Hosted-диалоги**, штора, `ConfirmThenAsync`, Popup / Процессы |
| [GlassSettings-notices.md](GlassSettings-notices.md) | **Notice**, зеркало viewport, programmatic refresh, baseline |
| [GlassSettings-customization-tracking.md](GlassSettings-customization-tracking.md) | **Учёт кастомизации**, snapshot/revert, gate → `DeleteSubKeyTree` |
| [GlassSettings-visual-styles.md](GlassSettings-visual-styles.md) | **Визуальные стили**, ThemeManager, плитки `.theme` |
| [GlassSettings-tooltips.md](GlassSettings-tooltips.md) | **Acrylic ToolTip**, `GlassSettingsToolTipService` |
| [GlassSettings-WinUI-chat-prompt.md](../assistant/GlassSettings-WinUI-chat-prompt.md) | Промпт согласования |
