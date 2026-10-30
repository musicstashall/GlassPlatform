# GlassSettings — hosted-диалоги и штора

Единый паттерн модальных окон GlassSettings: полнооконная **штора** + **панель по центру** (не `ContentDialog`).

См. также: [GlassSettings-development.md](GlassSettings-development.md), [GlassSettings-notices.md](GlassSettings-notices.md), [GlassSettings-WinUI.md](GlassSettings-WinUI.md).

---

## Штора (`WindowBusyOverlay`)

| API | Назначение |
|-----|------------|
| [`WindowBusyOverlayService.Push`](../../GlassSettings/Services/WindowBusyOverlayService.cs) | Показать штору; для диалогов — `showProgress: false` |
| [`WindowBusyOverlayService.Pop`](../../GlassSettings/Services/WindowBusyOverlayService.cs) | Скрыть штору (ref-count в `MainWindow`) |
| [`WindowBusyOverlayService.Run`](../../GlassSettings/Services/WindowBusyOverlayService.cs) / `RunAsync` | Штора на время операции (системный picker, загрузка) |
| [`WindowBusyOverlayService.ForceResetIfActive`](../../GlassSettings/Services/WindowBusyOverlayService.cs) | Сброс «залипшей» шторы при смене страницы (`NavFrame_OnNavigated`) |
| [`WindowBusyOverlayService.ApplyFill`](../../GlassSettings/Services/WindowBusyOverlayService.cs) | Заливка шторы (acrylic chrome / dim) — только из `MainWindow` |

**Правило:** не вызывать `MainWindow.PushWindowBusyOverlay` / `PopWindowBusyOverlay` напрямую из страниц — только через `WindowBusyOverlayService`.

Режим заливки — реестр `GlassSettings\WindowBusyOverlayMode`.

---

## Hosted-диалог

| Компонент | Путь |
|-----------|------|
| Сервис | [`HostedDialogService.cs`](../../GlassSettings/Services/HostedDialogService.cs) |
| Подтверждения | [`HostedConfirmationDialogService.cs`](../../GlassSettings/Services/HostedConfirmationDialogService.cs) |
| Анимация проявления | [`HostedDialogRevealAnimator.cs`](../../GlassSettings/Services/HostedDialogRevealAnimator.cs) (250 мс) |
| Типы | [`HostedDialogTypes.cs`](../../GlassSettings/Services/HostedDialogTypes.cs) |
| Layout helper | `HostedDialogLayout` в [`HostedDialogTypes.cs`](../../GlassSettings/Services/HostedDialogTypes.cs) |
| Bounds helper | [`HostedDialogBoundsHelper.cs`](../../GlassSettings/Services/HostedDialogBoundsHelper.cs) |
| Хост в окне | `MainWindow`: `HostedDialogHost` на `RootGrid` `Grid.RowSpan="2"`, **после** `WindowBusyOverlay`; `Margin.Top = AppTitleBar.ActualHeight` |
| Заголовок + крестик | [`HostedDialogHeader`](../../GlassSettings/Controls/HostedDialogHeader.xaml) |
| WinUI file picker | [`ExecutableFilePickerDialogService.cs`](../../GlassSettings/Services/ExecutableFilePickerDialogService.cs) + [`ExecutableFilePickerPanel`](../../GlassSettings/Controls/Processes/ExecutableFilePickerPanel.xaml) — exe / image; **Folder-Picker** (`PickFolderAsync`, `foldersOnly`: только папки, без combo фильтров) |
| WinUI process picker | [`RunningProcessPickerDialogService.cs`](../../GlassSettings/Services/RunningProcessPickerDialogService.cs) + [`RunningProcessPickerPanel`](../../GlassSettings/Controls/Processes/RunningProcessPickerPanel.xaml) |

### Поведение

- Штора (`WindowBusyOverlayService.Push(false)`) + acrylic-панель по центру — если `HostedDialogSpec.ManageOverlay = true` (по умолчанию).
- **Проявление панели:** opacity 0→1 за **250 мс** (`HostedDialogRevealAnimator` в `MainWindow.RunHostedDialogAsync`) — единое правило для всех hosted-диалогов.
- **Крестик** — закрытие без footer «Закрыть».
- **Клик по шторе** (вне панели) — `Dismissed`; только после показа hosted-панели.
- Footer: secondary (отмена) + primary; ширина **88 px**; разделитель над footer-баром.
- Тема панели — `AppThemeVisualResolver` по реестру `AppTheme`.

### `ConfirmThenAsync` — диалог + процедура под шторой

При **Primary**: панель скрывается **синхронно**, штора остаётся с progress, выполняется `procedure`, затем `Pop` в `ShowHostedDialogThenRunAsync`.

```csharp
var ok = await HostedDialogService.ConfirmThenAsync(spec, procedure, manageOverlay: true);
```

Параметр **`manageOverlay`**: `false` — если штору поднимает вызывающий код (см. Popup hub, picker процессов).

**Notice после процедуры** — на стороне страницы, **после** `Pop` внешней шторы:

```csharp
PageSessionNoticeState.RequestAnimateNextOpen();
PageShell?.RefreshSessionNoticeProgrammatic();
```

См. [GlassSettings-notices.md](GlassSettings-notices.md) — программный показ и defer во время шторы.

---

## Layout и отступы

### Вертикальная зона bounds

`HostedDialogHost` на корневом `RootGrid`, **`Grid.RowSpan="2"`**, **после** `WindowBusyOverlay` в XAML — панель поверх шторы. Верхняя граница зоны размещения: `HostedDialogHost.Margin.Top = AppTitleBar.ActualHeight` (`UpdateHostedDialogVerticalInset` в `MainWindow`; пересчёт на `AppTitleBar.SizeChanged` и при открытии диалога). Центрирование `DialogShadowRect` — между нижней кромкой TitleBar и низом окна.

`WindowBusyOverlay` — `RootGrid`, `Grid.RowSpan="2"` — штора на весь клиент, включая TitleBar; z-order **ниже** hosted-панели.

Fallback в `ApplyHostedDialogBounds`: при `DialogBoundsHost.ActualHeight <= 0` высота доступной зоны = `AppWindow.Size.Height - AppTitleBar.ActualHeight`.

| Слой | Правило |
|------|---------|
| `DialogBoundsHost` | `Margin="20"` в XAML — отступ панели от краёв **контентной** зоны (не под TitleBar) |
| `HostedDialogSurface` | При открытии: **горизонтальный padding = 0**; верхний (и нижний без footer) — из `ContentDialogPadding` |
| Chrome | `Grid`: header (auto) → body scroll (`*` при `MaxHeight`) → footer (auto, вне scroll) |
| `HostedDialogContent` | `HorizontalContentAlignment` / `VerticalContentAlignment` = `Stretch` |
| Header / footer | Горизонтальный inset 24px (`ContentDialogPadding`) — в `RunHostedDialogAsync` |
| Body (обычный диалог) | `HostedDialogLayout.WrapBodyWithContentPadding(body)` — Grid с H-margin + stretch |
| Body (edge-to-edge) | Без обёртки; `HostedDialogLayout.ApplyBodyStretch(body)` в host |
| `BodyHandlesScroll` | `true` — внешний body-scroll отключён; TreeView/ListView скроллят сами (file picker) |
| Footer band | `footerBar.Height = 44 + dialogPad.Bottom`; внутренняя полоса **32px** (`FooterRow`/`footerBand`) по центру полной зоны; **без** `dialogPad.Bottom` в `footerBar.Margin`; combo col0, кнопки col1 справа |
| Child dialog | `HostedDialogHostContext.ShowChildDialogAsync` — snapshot/restore родителя; **не** `HostedDialogService.ShowAsync` из body |
| `IHostedDialogAdaptiveBody` | `OnHostedDialogBoundsChanged` + опционально `GetHostedDialogMinWidth()` |
| File picker compact | inline min **584**, compact min **534**; list min **300**; гистерезис **±1px**; смена min через `RequestBoundsRefresh` |
| Clipping | `HostedDialogSurface.Clip` (`RectangleGeometry` по фактическому размеру surface) |

**Обычный диалог** — body оборачивается в call site (или в `HostedConfirmationDialogService.ToDialogSpec`):

```csharp
Body = HostedDialogLayout.WrapBodyWithContentPadding(panel),
```

**Edge-to-edge** — body передаётся напрямую в `HostedDialogSpec`; host вызывает `HostedDialogLayout.ApplyBodyStretch(body)`. File picker: `BodyHandlesScroll = true`, панель [`ExecutableFilePickerPanel`](../../GlassSettings/Controls/Processes/ExecutableFilePickerPanel.xaml) с внутренним Grid (nav / command / content).

---

## Bounds при resize

- [`HostedDialogBoundsHelper`](../../GlassSettings/Services/HostedDialogBoundsHelper.cs) — `EffectiveWidth` / `EffectiveHeight` от `DialogBoundsHost.ActualSize` (margin 20 уже учтён контейнером).
- `DialogShadowRect.Width` = `EffectiveWidth` (shell заполняет доступную ширину до `MaxWidth`); при `MaxHeight` — `Height` = `EffectiveHeight`.
- Пересчёт на `DialogBoundsHost.SizeChanged` с debounce (~1 кадр через `DispatcherQueue`), **не** на каждый `AppWindow.Changed`.
- `MinWidth` из `GetHostedDialogMinWidth()` (если body реализует) или `HostedDialogSpec.MinWidth`; обновляется при каждом `ApplyHostedDialogBounds` (в т.ч. live resize и `RequestBoundsRefresh`).
- Body при `MaxHeight`: row `*` в chrome-grid; overflow только в scroll body, footer закреплён внизу shell.
- Адаптивный body: реализовать [`IHostedDialogAdaptiveBody`](../../GlassSettings/Services/HostedDialogTypes.cs) — host вызывает после каждого пересчёта bounds (и после `RevealAsync`).

### Вложенный child-диалог (file picker → предупреждение)

Если открытый hosted-диалог должен показать модальное подтверждение **поверх** себя (без закрытия родителя):

- вызывать `HostedDialogHostContext.ShowChildDialogAsync(spec)` из `configure`/`AttachDialogContext`;
- **не** вызывать `HostedDialogService.ShowAsync` / `HostedConfirmationDialogService.ConfirmAsync` — это перезаписывает `_hostedDialogTcs` и ломает штору.

`MainWindow.ShowChildHostedDialogAsync`: snapshot chrome родителя → child chrome → await child → restore snapshot. Overlay depth не меняется (`ManageOverlay = false` у child).

Эталон: invalid extension в [`ExecutableFilePickerPanel.TryConfirmSelectionAsync`](../../GlassSettings/Controls/Processes/ExecutableFilePickerPanel.xaml.cs).

---

## Эталонные сценарии

### Подтверждения (общий паттерн)

[`HostedConfirmationDialogService`](../../GlassSettings/Services/HostedConfirmationDialogService.cs) — `CreateBody`, `CreateCancelConfirmFooter`, `ConfirmAsync` / `ConfirmThenAsync`.

Специализированные обёртки:

| Сценарий | Сервис |
|----------|--------|
| Сохранить перед запуском платформы | `SaveBeforePlatformDialogService` |
| Root NONE (OpenGlass) | `OpenGlassRootNoneDialogService` |
| Удаление раздела Popup | `FlyoutSectionRemoveDialogService` |
| Выход без сохранения | `UnsavedChangesExitDialogService` |
| Экспорт атласа темы | `ThemeAtlasExportDialogService` |

### Настройки приложения (без footer)

[`AppSettingsDialogService.cs`](../../GlassSettings/Services/AppSettingsDialogService.cs) — `HostedDialogService.ShowAsync`.

### Popup hub — отключение раздела (Menu / DropDown / ToolTip)

Внешняя штора → диалог → процедура → внешний `Pop` → notice.

```csharp
WindowBusyOverlayService.Push(showProgress: true);
try
{
    confirmed = await FlyoutSectionRemoveDialogService.ConfirmThenAsync(
        title,
        () => ClearFlyoutSectionAsync(kind),
        manageOverlay: false);
}
finally
{
    WindowBusyOverlayService.Pop();
}
if (confirmed)
{
    PageSessionNoticeState.RequestAnimateNextOpen();
    PageShell?.RefreshSessionNoticeProgrammatic();
}
```

Процедура: `CustomizationCatalog.RemoveFlyoutSection(regSub)` — удаление **всего** поддерева BWM (`DeleteSubKeyTree`), не только значений.

Эталон: [`PopupHubPage.xaml.cs`](../../GlassSettings/Pages/Glass/PopupHubPage.xaml.cs).

### Процессы — picker запущенных процессов

1. `Push(showProgress: true)` — сразу при вызове.
2. Сбор списка + hosted-диалог с `ManageOverlay = false`.
3. Body — [`RunningProcessPickerPanel`](../../GlassSettings/Controls/Processes/RunningProcessPickerPanel.xaml) в `WrapBodyWithContentPadding` (горизонтальный inset 24px); `MaxHeight = 560`, `BodyHandlesScroll = true`.
4. При выборе — штора **остаётся** (`retainOverlayAfterSelection: true`).
5. Страница: запись в реестр, обновление списка, прокрутка к строке.
6. `Pop` → notice с анимацией.

```csharp
var exe = await RunningProcessPickerDialogService.PickAttachListAsync(
    dispatcherQueue, retainOverlayAfterSelection: true);
await CompleteAttachProcessSelectionAsync(exe, overlayAlreadyUp: true);
```

Эталон: [`RunningProcessPickerDialogService.cs`](../../GlassSettings/Services/RunningProcessPickerDialogService.cs), [`RunningProcessPickerPanel`](../../GlassSettings/Controls/Processes/RunningProcessPickerPanel.xaml), [`ProcessesPage.xaml.cs`](../../GlassSettings/Pages/ProcessesPage.xaml.cs).

### Процессы — выбор exe-файла (hosted WinUI)

[`ExecutableFilePickerDialogService.cs`](../../GlassSettings/Services/ExecutableFilePickerDialogService.cs) — hosted-диалог с **edge-to-edge body** (без `WrapBodyWithContentPadding`). Панель: [`ExecutableFilePickerPanel`](../../GlassSettings/Controls/Processes/ExecutableFilePickerPanel.xaml).

### Системный picker exe (Win32, legacy)

[`ExecutableFilePickerService.cs`](../../GlassSettings/Services/ExecutableFilePickerService.cs) — `WindowBusyOverlayService.Run(..., showProgress: true)`. Win32-диалог из native host; см. [GlassSettings-host-glass.md](GlassSettings-host-glass.md).

---

## Anti-patterns

- `ContentDialog` для модалок GlassSettings.
- Двойная штора: `Push` вокруг `ShowAsync` при `ManageOverlay = true`.
- `RefreshSessionNotice()` вместо `RefreshSessionNoticeProgrammatic()` после `ConfirmThenAsync` — notice не появится или без анимации.
- Отдельные overlay-хосты на страницах.
- Отрицательный `Margin` на body-панели «чтобы компенсировать» surface padding — для edge-to-edge body не оборачивать в padded Grid.
- Header и footer внутри одного `ScrollViewer` с body — footer уезжает в scroll; использовать Grid-chrome в `RunHostedDialogAsync`.
- File picker в `StackPanel` без `BodyHandlesScroll` — прокручивается весь диалог вместо TreeView/ListView.
- Process picker со `StackPanel` + `ListView.MaxHeight` без `HostedDialogSpec.MaxHeight` — футер обрезается при resize окна.
- `HostedDialogHost` внутри `WindowBlurSourceHost` — `WindowBusyOverlay` на `RootGrid` перекрывает панель; хост — sibling **после** overlay на `RootGrid`.
- `HostedDialogHost` на `RootGrid.Row="1"` без inset TitleBar — зона bounds = вся высота клиента; верх диалога уезжает под заголовок при resize.
- `HostedDialogHost` с `Grid.RowSpan="2"` **без** `Margin.Top` по `AppTitleBar.ActualHeight` — та же проблема с TitleBar.
- Process picker / обычный диалог без `WrapBodyWithContentPadding` — body прилипает к краям surface (исключение: file picker).
- `footerBar.Height` с `+ dialogPad.Bottom` при том же inset в `Margin` — двойной нижний отступ.
- Синхронный пересчёт `MinWidth`/`MaxWidth` на каждый `AppWindow.Changed` — только debounced bounds от `DialogBoundsHost`.
- Дублирование margin 20px в helper и XAML — margin только на `DialogBoundsHost`.

---

## Эталоны

| Сценарий | Файл |
|----------|------|
| Настройки приложения | `AppSettingsDialogService` |
| Удаление раздела Popup | `FlyoutSectionRemoveDialogService`, `PopupHubPage` |
| Выбор процесса | `RunningProcessPickerDialogService`, `RunningProcessPickerPanel`, `ProcessesPage` |
| Выбор exe-файла (hosted) | `ExecutableFilePickerDialogService`, `ExecutableFilePickerPanel` |
| Выбор exe-файла (Win32) | `ExecutableFilePickerService` |
