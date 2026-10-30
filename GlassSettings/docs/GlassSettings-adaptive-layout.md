# GlassSettings — адаптивная разметка

Паттерны адаптивного переноса элементов при изменении ширины контента. Применяется к спойлерам со слайдерами, hero-блокам с карточками, grid-панелям.

См. также: [GlassSettings-detail-cards.md](GlassSettings-detail-cards.md), [GlassSettings-slider-commit.md](GlassSettings-slider-commit.md), [GlassSettings-development.md](GlassSettings-development.md).

---

## Принципы

### 1. Прямой вызов `UpdateLayout()` при `SizeChanged`

**Правильно** (эталон: [`VisualStylesPage.xaml.cs:33`](../../GlassSettings/Pages/VisualStylesPage.xaml.cs)):

```csharp
HeroLayoutHost.SizeChanged += (_, _) => UpdatePageLayout();
```

**Неправильно** — debounce-таймер создаёт задержку и рассинхронизацию:

```csharp
// ❌ НЕ ИСПОЛЬЗОВАТЬ
HeroLayoutHost.SizeChanged += ScheduleUpdateWithTimer();
```

**Почему:** WinUI 3 уже имеет внутренний debounce для `SizeChanged`. Дополнительный таймер (8-32ms) создаёт визуальную задержку — элементы успевают сжаться до нуля перед переносом.

### 2. Симметричные пороги переключения

**Правильно** — один порог для обоих направлений:

```csharp
var sideBySide = _heroSideBySide
    ? width >= minRequiredWidth      // перенос вниз
    : width >= minRequiredWidth;     // возврат вверх — тот же порог
```

**Неправильно** — асимметричные пороги с hysteresis:

```csharp
// ❌ НЕ ИСПОЛЬЗОВАТЬ без необходимости
var sideBySide = _heroSideBySide
    ? width >= minRequiredWidth
    : width >= minRequiredWidth + HysteresisOffset;  // возврат позже переноса
```

**Когда hysteresis допустим:** Только если элементы внутри контейнера имеют переменную ширину и нужно избежать «дребезга» (rapid toggling) при hover/focus. Для hero-блоков с фиксированными размерами hysteresis не нужен.

### 3. Расчёт порога от доступного места, а не от запомненной ширины

**Правильно** (эталон: [`DwmThemeDetailPage.xaml.cs:796-815`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs)):

```csharp
var minHeroBeside = PreviewWidth + Gap + CardMinWidth;  // константы
var sideBySide = width >= minHeroBeside;
```

**Неправильно** — запоминание ширины контейнера:

```csharp
// ❌ НЕ ИСПОЛЬЗОВАТЬ
if (width < minHeroBeside)
{
    _switchWidth = width;  // запомнили момент переноса
}
// возврат при width >= _switchWidth + 1
// Проблема: _switchWidth может быть меньше minHeroBeside
```

**Почему:** Карточка может перенестись при width=664px (меньше `minHeroBeside=712px` из-за скачка ширины), но для нормального размещения side-by-side нужно минимум 712px. Возврат при 665px приведёт к тому, что карточка вернётся раньше, чем появится достаточно места.

---

## Паттерн 1: Шапки спойлеров (единый layout)

### Схема колонок `SpoilerHeaderGrid`

**Icon (Auto) | Title (`*`, min 150) | ControlsTable (Auto) | TrailingRail (Auto)**

| Col | Содержимое | При сужении |
|-----|------------|-------------|
| 0 | Icon + dots | статично |
| 1 | Title / Description | сжимается первой до 150 px |
| 2 | `SpoilerHeaderControlsTable` (0–2 ячейки) | сжимается второй; при `available < inlineMin` — **вся таблица** на 2-ю строку, выравнивание справа |
| 3 | `TrailingRailPanel`: `StaticHost` (gate / статичный контрол) + chevron | **никогда не переносится**; gate↔chevron spacing = `GlassSettingsToggleChevronSpacing` (8 px) |

**Зазоры trailing:** ControlsTable → TrailingRail **8 px** (`TrailingRailPanel` `Margin` left); Gate → Chevron **8 px** (spacing внутри панели). Панель **Collapsed**, если нет StaticHost и `ShowChevron=False` (без лишнего 8 px у option-row).

**Правило gate-слота:** если Gate отсутствует (`ShowGateSwitch=False`) — `HeaderActions` помещается в `StaticHost`, таблица контролов содержит 0–1 ячейку (напр. slider). Пустая таблица → ширина 0, не участвует в пороге.

### Ячейка контрола

[`SpoilerHeaderControlCell`](../../GlassSettings/Controls/SpoilerHeaderControlCell.xaml) — вложенная таблица **label (Auto) | control**:

- ComboBox — **фиксированная** measured width
- **ToggleSwitch** — при помещении в ячейку зажимается до **44 px** (`Width/MinWidth/MaxWidth`): шаблон WinUI резервирует ~154 px под On/Off-контент, без зажима ячейка получает ~110 px пустоты справа от «пилюли»
- **CheckBox** — подпись в `Content`, в ячейке `Label=""` (не `HubInlineLabel`); `SetCellFromLegacy` с HubInlineLabel+CheckBox сворачивает автоматически
- `Slider` / `CreateSliderValueSuffixPanel` — clamp **120–200** через [`GlassSettingsSliderMetrics`](../../GlassSettings/Controls/GlassSettingsSliderMetrics.cs)

[`SpoilerHeaderControlsTable`](../../GlassSettings/Controls/SpoilerHeaderControlsTable.xaml) — до двух ячеек, spacing = `GlassSettingsRowControlSpacing`.

```csharp
row.HeaderControlsTable.SetCell(0, "Эффект", effectCombo);
row.HeaderControlsTable.SetCell(1, "Углы", cornerCombo);
// legacy HubInlineLabel:
row.HeaderControlsTable.SetCellFromLegacy(0, hubInlineLabel);
```

### Единый движок

| Компонент | Файл |
|-----------|------|
| Layout engine | [`SpoilerHeaderLayoutEngine.cs`](../../GlassSettings/Controls/SpoilerHeaderLayoutEngine.cs) |
| Host (SizeChanged) | [`SpoilerHeaderLayoutHost.cs`](../../GlassSettings/Controls/SpoilerHeaderLayoutHost.cs) |
| Row bridge | [`SpoilerHeaderRowLayoutBridge.cs`](../../GlassSettings/Controls/SpoilerHeaderRowLayoutBridge.cs) |
| Metrics | [`SpoilerHeaderLayoutMetrics.cs`](../../GlassSettings/Controls/SpoilerHeaderLayoutMetrics.cs) |

**Порядок сжатия inline (две фазы):**

1. **Controls table** max→min (slider 200→120), пока **title на естественной ширине** — слайдер сжимается первым.
2. **Title** max→150 px, controls table зафиксирована на min — заголовок сжимается последним.
3. **Stack** при `available < inlineMin`.

При расширении — зеркально: сначала растёт title до естественной ширины, затем controls (slider 120→200). Для шапок без слайдера (combo/toggle фиксированные) `controlsMin == controlsMax` — флексит только title.

**Зазор title→controls:** **10 px** (`GlassSettingsTitleControlsGap` / `TitlePanel` `Margin` right). Учитывается в `MeasureInlineMinWidth` и `controlsBudget`.

**Inline clamp:** после `ApplyInlineWidths(budget)` таблице задаётся **`MaxWidth = budget`** (без фиксированного `Width`), ячейки выравниваются слева — fixed combo не обрезаются, таблица не растягивается шире контента.

**Порог stack/unstack:** `inlineMin = fixedRails + title(150) + TitleControlsGap + controlsTable.MeasureMinWidth()` — **без ручных estimates**. Unstack: `available >= inlineMin + 1 px` (симметрично ±1).

Row-контролы подключают host в `Loaded`:

```csharp
_layoutHost.Attach(RootBorder, HeaderChrome, ActionsBelowHost,
    SpoilerHeaderRowLayoutBridge.CreateOptions(HeaderChrome, hasGateRail: true),
    () => GateSwitch.IsOn);
```

### Контролы с adaptive layout

| Контрол | Примечание |
|---------|------------|
| `SettingsSystemGateExpander` | Gate → StaticHost; `ShowGateSwitch=False` → actions в StaticHost |
| `SettingsFlatGateRow` | `RefreshHeaderLayout()` после прямой правки таблицы |
| `SettingsNavigateRow` | stackable → ControlsTable; toggle → StaticHost |
| `SettingsOptionRow` | combo/toggle в ControlsTable; `hasGateRail: false`; без trailing |
| `DwmOpacityGroupRow` | combo → StaticHost (split); slider → cell 0 |

### Минимальная ширина текста спойлера

Колонка Title: **`GlassSettingsSectionTitleMinWidth` = 150 px**.

### Анти-паттерны (шапка)

- ❌ `HeaderActionsInlineWidthEstimate` — удалено; ширина измеряется автоматически
- ❌ `HubRowActionsGrid` — obsolete; две ячейки в `SpoilerHeaderControlsTable`
- ❌ `SliderBelowHeaderOnly` / `SpoilerHeaderAdaptiveLayout` — заменены структурой StaticHost + ControlsTable
- ❌ Debounce-таймер на `SizeChanged` шапки
- ❌ Гистерезис 48 px для unstack шапки (только ±1 px)

---

## Паттерн 1 (legacy reference): старые слайдеры — удалено

<details>
<summary>Архив: SpoilerHeaderAdaptiveLayout (до 2026-06-30)</summary>

Схема col Slider | col Actions заменена на ControlsTable. Split-layout заменён размещением combo в StaticHost.

</details>

### Контролы (legacy table — см. паттерн 1 выше)

| Тип спойлера | Механизм переноса | Файлы |
|--------------|-------------------|-------|
| `SettingsSystemGateExpander` | `SpoilerHeaderAdaptiveLayout.UpdateLayout` | [`SettingsSystemGateExpander.xaml.cs`](../../GlassSettings/Controls/SettingsSystemGateExpander.xaml.cs) |
| `SettingsFlatGateRow` | то же | [`SettingsFlatGateRow.xaml.cs`](../../GlassSettings/Controls/SettingsFlatGateRow.xaml.cs) |
| `DwmOpacityGroupRow` | то же (+ `SliderBelowHeaderOnly` для split) | [`DwmOpacityGroupRow.xaml.cs`](../../GlassSettings/Controls/DwmOpacityGroupRow.xaml.cs) |
| `SettingsNavigateRow` | то же (`TrailingExtra` вместо gate rail) | [`SettingsNavigateRow.xaml.cs`](../../GlassSettings/Controls/SettingsNavigateRow.xaml.cs) |

### Структура XAML

```xml
<StackPanel Spacing="8">
    <ctrl:SpoilerHeaderGrid x:Name="HeaderChrome" ShowChevron="False">
        <ctrl:GlassSettingsLabeledToggleSwitch x:Name="GateSwitch" />
    </ctrl:SpoilerHeaderGrid>
    
    <ContentControl
        x:Name="ActionsBelowHost"
        HorizontalAlignment="Stretch"
        HorizontalContentAlignment="Right"
        Visibility="Collapsed" />
</StackPanel>
```

### Логика переноса (code-behind)

Единая точка — [`SpoilerHeaderAdaptiveLayout`](../../GlassSettings/Controls/SpoilerHeaderAdaptiveLayout.cs). Контролы **не** дублируют `ApplyHeaderChromeLayout` / `ApplyActionsAlignment`.

```csharp
private void UpdateActionsLayout()
{
    var options = new SpoilerHeaderAdaptiveOptions
    {
        ActionsWidthEstimate = HeaderActionsInlineWidthEstimate > 0
            ? HeaderActionsInlineWidthEstimate
            : DefaultActionsInlineWidthEstimate,
        HasSlider = _headerSliderContent is not null,
        HasTrailingGate = true  // false + TrailingExtra для SettingsNavigateRow
    };

    var hosts = new SpoilerHeaderAdaptiveHosts
    {
        Chrome = HeaderChrome,
        ActionsBelowHost = ActionsBelowHost,
        HeaderSliderContent = _headerSliderContent,
        HeaderActionsContent = _headerActionsContent,
        StackedBelowPanel = _stackedBelowPanel
    };

    SpoilerHeaderAdaptiveLayout.UpdateLayout(
        RootBorder.ActualWidth,
        ref _actionsStacked,
        ref _actionsLayoutInitialized,
        options,
        hosts);
    _stackedBelowPanel = hosts.StackedBelowPanel;

    SyncActionsBelowGateChrome();
}
```

**Обязательные правила:**

1. **Ширина слайдера пересчитывается на каждом `SizeChanged`**, даже если stacked-состояние не менялось (inline 120–200 → stack max available → unstack восстанавливает max).
2. **Гистерезис 48 px** (`SpoilerHeaderAdaptiveOptions.UnstackHysteresis`) — только для возврата в inline, не для переноса.
3. Суффикс-панели (`SettingsSliderFactory.CreateSliderValueSuffixPanel`) — **без** фиксированного outer `Width`; ширина через `ApplyAdaptiveSliderPanelWidth`.

### Подбор `ActionsInlineWidthEstimate`

| Контрол | Значение | Обоснование |
|---------|----------|-------------|
| `SettingsSystemGateExpander` | 260 | `HubInlineLabel` (label + slider до 200px + gap 8px) ≈ 260px + запас для ComboBox |
| `SettingsSystemGateExpander` (несколько контролов) | **420+** | `StackPanel` + `HubInlineLabel` + CheckBox — см. «Параметры композиции» и [`TaskbarFlyoutPage`](../../GlassSettings/Pages/Glass/TaskbarFlyoutPage.xaml.cs) BorderExpander (260) |
| `SettingsFlatGateRow` | 180 | Один слайдер с label; **248** (`MaxWidth 200` + suffix) для слайдера в шапке; **340** для «Углы и тени» |
| `DwmOpacityGroupRow` | 300 | ComboBox + слайдер + value-suffix |

**Правило:** Замерить `DesiredSize.Width` элемента `_headerActionsContent` при `Measure(Size.Infinity, Infinity)` и добавить 20-40px запаса.

### Гистерезис для слайдеров

`SpoilerHeaderAdaptiveOptions.UnstackHysteresis` (по умолчанию **48**) — только для возврата в inline, не для переноса.

### Переопределение ширины actions (`SettingsFlatGateRow`)

Свойство **`HeaderActionsInlineWidthEstimate`** (0 = дефолт 180). Задавать, когда фактическая ширина `HeaderActions` заметно больше одного слайдера — иначе перенос на новую строку происходит слишком поздно.

| Страница / строка | Значение | Состав actions |
|-------------------|----------|----------------|
| «Углы и тени» (Popup) | **340** | `HubInlineLabel` «Форма» + ComboBox + `HubInlineLabel` «Тени» + CheckBox |
| «Параметры композиции» (Colorization) | **140** | CheckBox «Непрозрачно» в header (`ShowGateSwitch=False`) |
| «Цвет бордюра» (Taskbar) | **260** | `StackPanel`: подпись + ToggleButton |
| Hub «Стекло» (DWM) | **480** | `HubRowActionsGrid`: Combo + Slider (mode + deviation) |

Эталон multi-control expander: [`TaskbarFlyoutPage.xaml.cs`](../../GlassSettings/Pages/Glass/TaskbarFlyoutPage.xaml.cs) — `StackPanel` в `HeaderActions`, не `HubRowActionsGrid`, если контролы не combo+slider пара.

Эталон: [`FlyoutKindDetailPage.xaml.cs`](../../GlassSettings/Pages/Glass/FlyoutKindDetailPage.xaml.cs) — `CornerRow.HeaderActionsInlineWidthEstimate = 340`.

Подписи слева от контрола — **`HubInlineLabel`** или `SetCell(label, control)` для ComboBox/slider; **CheckBox** — bare control с `Content`, без левой колонки ячейки.

### Выравнивание actions (обязательно)

| Режим | Где | Выравнивание |
|-------|-----|--------------|
| Inline (в шапке) | `SpoilerHeaderGrid.HeaderActionsHost` | Корень `HeaderActions`: `HorizontalAlignment="Right"`; **не** прижимать к колонке title |
| Перенос (новая строка) | `ActionsBelowHost` | `HorizontalContentAlignment="Right"`; дочерние контролы — `HorizontalAlignment.Right` (кроме suffix-grid: inner `Slider` остаётся `Stretch`) |

Контролы: `SettingsSystemGateExpander`, `SettingsFlatGateRow`, `DwmOpacityGroupRow`, `SettingsNavigateRow` — через `SpoilerHeaderAdaptiveLayout.ApplyActionsAlignment`.

**Анти-паттерны:**

- `HorizontalAlignment.Left` для inline-actions.
- Две star-колонки Title+Slider в `SpoilerHeaderGrid` (слайдер должен быть `Auto`).
- Фиксированный outer `Width` на suffix-panel (ширина — `ApplyAdaptiveSliderPanelWidth`).
- `ColumnDefinition.MaxWidth = 200` на колонке Slider при suffix-панели до 248 px.
- Early return в `UpdateActionsLayout` без пересчёта ширины слайдера при resize в stacked-режиме.
- Рекурсивный `HorizontalAlignment.Right` на children suffix-grid (`IsSliderValueSuffixPanel`) — ломает star-колонку трека.

---

## Паттерн 1b: `PlatformSchedulerRow` на странице «Система»

Как на **Popup-страницах** ([`PopupHubPage.xaml.cs`](../../GlassSettings/Pages/Glass/PopupHubPage.xaml.cs)): контролы создаются **отдельно** (`new`), **не** извлекаются из дочерних элементов чужого `UserControl` — иначе WinUI падает при `ContentPresenter.Content = …` (`ArgumentException`).

| Элемент | Создание | Размещение | Перенос |
|---------|----------|------------|---------|
| `PlatformSchedulerStackableActions` | `new` (отдельный `UserControl`) | `HeaderActions` → при нехватке ширины `ActionsBelowHost` | **Да**, **справа** |
| `GlassSettingsLabeledToggleSwitch` установки | `new GlassSettingsLabeledToggleSwitch()` | `TrailingContent` (col3 + chevron) | **Нет** |
| Отступ split ↔ toggle | `GlassSettingsSplitButtonSpoilerMargin` (0,0,8,0) | — | — |
| Ширина split | `GlassSettingsSplitButtonWidth` (**140** = статус + `SplitFlyoutPartWidth` 36) | — | — |
| Estimate inline | `GlassSettingsPlatformSchedulerStackableWidth` (**230**) | — | — |

**Dual Glass (OS x64 + Settings x64):** две navigate-строки по паттерну 1b — «Платформа Glass x86» и «Платформа Glass x64»; у каждой свой `PlatformSchedulerStackableActions` в `HeaderActions` и свой install-gate в `TrailingContent` (install/remove только своей arch). OpenGlass — одна пара как раньше.

### Порядок подключения (обязательный)

1. **`AddNavigateSection`** — navigate-строка уже в визуальном дереве (`SectionList.Children`).
2. **`AttachToNavigateRow(navRow)`**:
   - `HeaderActions = stackable`, `TrailingContent = installGate`;
   - Dual: два независимых вызова (x86 и x64) — без общего gate;
   - `HeaderActionsInlineWidthEstimate` = `GlassSettingsPlatformSchedulerStackableWidth` (230).
3. Логика остаётся в **`PlatformSchedulerRow`** (класс без XAML, не `UserControl`).

```csharp
_ogRow = new PlatformSchedulerRow { Kind = PlatformSchedulerKind.OpenGlass };
_ogNav = NavHost.AddNavigateSection(/* … */);
_ogRow.AttachToNavigateRow(_ogNav);
```

`SettingsNavigateRow` присваивает `TrailingContent` в `HeaderChrome` только после **`Loaded`** navigate-строки.

### Layout после навигации

При переходе на страницу «Система» ширина `RootBorder` может быть **0** на первом кадре — [`SpoilerHeaderLayoutHost`](../../GlassSettings/Controls/SpoilerHeaderLayoutHost.cs) делает **one-shot retry** (max 3) при `available <= 0`; страница дополнительно вызывает `RefreshHeaderLayout()` на navigate-rows в deferred pass после `Loaded`:

```csharp
_ogNav?.RefreshHeaderLayout();
_glassNav?.RefreshHeaderLayout();
_glassNavX86?.RefreshHeaderLayout();
_glassNavX64?.RefreshHeaderLayout();
```

Для navigate-row с `TrailingContent` (toggle): `CreateOptions(..., hasGateRail: trailing is not null)` — резерв `TrailingToggleRail` до полного measure toggle.

### Порог inline

Как в паттерне 1: padding + icon + **title(150)** + `HeaderActions` (~220) + **toggle rail** (~52) + chevron. Гистерезис: `ActionsUnstackHysteresis` = 48.

| Ресурс | Значение | Назначение |
|--------|----------|------------|
| `GlassSettingsSectionTitleMinWidth` | **150** | Минимальная ширина колонки Title |
| `GlassSettingsPlatformSchedulerStackableWidth` | 220 | Подпись + split-button (без toggle) |

Файлы: [`PlatformSchedulerRow.cs`](../../GlassSettings/Controls/PlatformSchedulerRow.cs), [`PlatformSchedulerStackableActions.xaml`](../../GlassSettings/Controls/PlatformSchedulerStackableActions.xaml), [`SettingsNavigateRow.xaml.cs`](../../GlassSettings/Controls/SettingsNavigateRow.xaml.cs), [`SystemPage.xaml.cs`](../../GlassSettings/Pages/SystemPage.xaml.cs).

**Анти-паттерны:**

- Класть весь `PlatformSchedulerRow` в `TrailingContent` — toggle уедет при переносе.
- Экспортировать `StackPanel` / `ToggleSwitch` **изнутри** одного `UserControl` в `HeaderActions` + `TrailingContent` — краш при навигации.
- `CreateOptions(..., hasGateRail: false)` при наличии `TrailingContent` — split-button обрезается на первом кадре (trailing ≈ chevron-only).
- Полагаться только на `SizeChanged` без retry при `available <= 0` — misaligned/clipped split до ручного resize.

---

## Паттерн 2: тело expander (не шапка)

**Шапка** спойлера (`SpoilerHeaderGrid`, `SpoilerHeaderLayoutEngine`) — см. [Паттерн 1](#паттерн-1-шапка-спойлера-spoilerheadergrid); **не смешивать** с правилами тела.

Два независимых паттерна **тела разворота**:

### 2a. Без пикера — `SettingsParameterPanel` (4-col body-table)

| Правило | Значение |
|---------|----------|
| Контейнер | `BodyHost` → [`SettingsParameterBodyLayout.CreateBodyTable`](../../GlassSettings/Controls/SettingsParameterBodyLayout.cs) |
| Колонки | **col0** текст \| **col1** контрол \| **col2** текст \| **col3** контрол; dual-row через `AddDualRow`; single через `AddSingleRow` |
| Budget | `ResolveBodyContentWidth()` = `LayoutRoot.ActualWidth` − **padding 44** (28+16) |
| Dynamic inset | `BodyHost.Padding.Right` после `UpdateParameterBodyLayout`; **не** применять inset при `metrics.TableWidth <= 0` |
| **Порядок фаз сжатия** | **A** — уменьшается **правый inset** (`BodyHost.Padding.Right`), таблица на max content width; **B** — inset = min, **сжимается ширина таблицы** до `minRowContent`; **C** — при `layoutW < row.InlineMinWidth` dual-row wrap (col2/col3 → подстрока), unwrap при `layoutW >= InlineMinWidth + 1` |
| Сжатие | Реализация фаз A→B→C: `ComputeTableWidthAndInset` + `ClampTableWidthToBudget`; slider-tracks **120–200**; wrap **только dual-row** (`AddDualRow`) |
| `cachedMin` | `InlineMinWidth` на каждой dual-row (inline 4-col); `CachedMinInlineWidth` = max; кеш при первом layout в **inline**-позициях; сброс — `InvalidateParameterBodyLayoutCache` / `RefreshLayout()` |
| Single-row | `AddSingleRow` — **всегда col0/col1**; не участвует в wrap; **не** объединять с соседней single-row в pseudo-dual |
| Dual-row | `AddDualRow`: primary col0/col1, secondary col2/col3 (inline); primary-контрол помечается `DualRowPrimaryTag` для `EnsureLogicalRows` |
| Leaf-контролы | **ComboBox / NumberBox / ToggleSwitch** — style `MinWidth`/`Width`, **без** `ClearValue` и без programmatic `MaxWidth`; programmatic width — **только Slider** |
| `table.Width` | Compact width от engine; **никогда** `Width = 0` при детях в grid — иначе `ClearValue(Width/MaxWidth)` |
| Layout при expand | `WireExpandedLayoutRefresh` → `RefreshLayout()` (invalidate + layout); one-shot retry **max 3** кадра при `layoutW == 0`; **запрет** бесконечного `TryEnqueue` при `layoutW <= 0` |
| Gate внутри divider | `SettingsDividerExpander` → `SettingsSystemGateExpander` → `SettingsParameterPanel`: refresh на **обоих** expand — `AdvancedExpander_OnExpanded` (chrome gate) + `WireExpandedLayoutRefresh` на gate → `RefreshLayout()` |
| `layoutW <= 0` | Early return (collapsed expander) — как picker-panel; relayout на `SizeChanged` / expand |
| Ресурсы | `GlassSettingsParameterBodyInnerPairGap` (12), `GlassSettingsPickerBodyLabelControlGap`, `GlassSettingsPickerBodyTableRightInset`; `ReadResource` через `TryGetValue` |
| Описание / default | Без изменений — row0 description, row3 default action |
| Диагностика | `$env:ParameterBodyLayoutTrace=1` → `[ParameterBodyLayout]` |

Файлы: [`SettingsParameterPanel.xaml`](../../GlassSettings/Controls/SettingsParameterPanel.xaml), [`.xaml.cs`](../../GlassSettings/Controls/SettingsParameterPanel.xaml.cs), [`SettingsParameterBodyLayout.cs`](../../GlassSettings/Controls/SettingsParameterBodyLayout.cs), [`SettingsSystemGateExpanderLayout.cs`](../../GlassSettings/Controls/SettingsSystemGateExpanderLayout.cs) (`WireExpandedLayoutRefresh`).

Эталоны: [`DwmGlassDetailPage`](../../GlassSettings/Pages/Dwm/DwmGlassDetailPage.xaml) (Вид окон / Вид заголовков), [`PopupHubPage`](../../GlassSettings/Pages/Glass/PopupHubPage.xaml) `AnimationPanel`, [`DwmSettingsHubPage`](../../GlassSettings/Pages/Dwm/DwmSettingsHubPage.xaml) `HooksPanel` (gate внутри divider).

**Анти-паттерны (parameter-body):**

- `Application.Current.Resources[key]` без `TryGetValue` — missing key бросает COMException.
- `ScheduleDeferredLayoutPass` / re-enqueue при каждом `layoutW == 0` — зависание UI (панель в collapsed expander).
- `ApplyControlColumnWidth` + `ClearValue(MinWidth)` на ComboBox — контрол 0 px до apply template.
- `MaxWidth` NumberBox ниже style-width (150) при resize — контрол «пропадает».
- `tableWidth > layoutWidth` без clamp — col2/col3 за clip expander'а.
- `ApplyBodyHostInset(layoutWidth)` при `maxRowContent == 0` — padding съедает всю ширину body.
- Склеивать две подряд `AddSingleRow` в один dual-row (вручную или эвристикой `EnsureLogicalRows`) — secondary окажется в col2/col3 чужой строки (регрессия: toggle «Классическая рамка», «Режим свечения»).
- Global wrap на всю таблицу для single-row — single всегда col0/col1; wrap только у строк с `HasSecondary`.

### 2b. С пикером — `SettingsColorPickerPanel` (body-table)

Полное описание левой колонки — [GlassSettings-detail-cards.md § Тело с SettingsColorPickerPanel](GlassSettings-detail-cards.md#тело-спойлера-с-settingscolorpickerpanel-строгий-паттерн).

**Внешняя таблица** `LayoutRoot`: info-колонка (adaptive) | separator | picker (fixed rail).

**Внутренняя body-table** (`FooterContent`): col0 — подписи (ширина = max label через `SyncLabelColumnWidths`), col1 — контролы **120–200**; динамический зазор = `FooterHost.Padding.Right` (фазы: inset → col1).

| Правило | Значение |
|---------|----------|
| Constraint ширины панели | **`GetLayoutHostWidth()`** = `LayoutRoot.ActualWidth` (fallback `ActualWidth`); **запрет** `InfoColumn.ActualWidth` для budget |
| Budget info-контента (inline) | `ResolveInfoContentWidth` = host − picker-rail − separator − **info padding (28+16=44)**; **`BodyTableRightInset` не вычитается** — только computed |
| Body-таблица `FooterContent` | `UpdateBodyTableLayout(table, layoutWidth)` → compact `table.Width = label + track + trailing`; inset через **`ApplyFooterHostInset(metrics.RightInset)`** после Update |
| Трек col1 | **120–200** через `ComputeBodyTableTrackAndInset`: сначала shrink inset, затем col1 |
| Порог inline/stack | Слот **LayoutRoot → PickerZone**; `cachedMin` = padding + `EstimateMinimumInfoContentWidth` — **один раз** при создании контента панели; `cachedPickerRail` — measure picker-колонки |
| Переход inline ↔ stacked | Stack: `slot < cachedMin`; unstack: `slot >= cachedMin + 1 px` (stacked: predicted `host − cachedRail − sep`); **не** `EstimateInlineMinWidth` / `PickerInlineRail` |
| Диагностика | `$env:PickerBodyLayoutTrace=1` → Debug Output `[PickerBodyLayout]` (см. [`SettingsPickerBodyLayoutTrace.cs`](../../GlassSettings/Controls/SettingsPickerBodyLayoutTrace.cs)) |
| Отличие от composition hero | Hero без picker/inset-host; **не** копировать `ApplyCompositionTableWidth` на picker-body |
| `FooterContent` | body-таблица: blur, glow rows, чекбокс (auto-wrap через `NormalizeFooterContent`) |
| Скрытый footer | `SetFooterContentVisible(false)` — не участвует в пороге (Vista blur) |
| Кнопка «По умолчанию» | Нижний якорь левой колонки (`DefaultActionPanel`), не под чекбоксом |
| Разделители info-колонки | Верхний и нижний — **на всю ширину** колонки; `DefaultActionPanel` = `HorizontalAlignment="Stretch"`, кнопка — `Left` |

Файлы: [`SettingsColorPickerPanel.xaml`](../../GlassSettings/Controls/SettingsColorPickerPanel.xaml), [`.xaml.cs`](../../GlassSettings/Controls/SettingsColorPickerPanel.xaml.cs), [`SettingsPickerBodyLayout.cs`](../../GlassSettings/Controls/SettingsPickerBodyLayout.cs).

---

## Паттерн 3: Hero-блоки с карточками

### Эталоны

| Страница | Hero-блок | Элементы |
|----------|-----------|----------|
| `DwmThemeDetailPage` | Превью + карточка «Политика рендеринга» | [`DwmThemeDetailPage.xaml:11-263`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml) |
| `VisualStylesPage` | Превью + карточки Quick Actions | [`VisualStylesPage.xaml:14-80`](../../GlassSettings/Pages/VisualStylesPage.xaml) |

### Структура XAML

```xml
<Grid x:Name="HeroLayoutHost" ColumnSpacing="24" RowSpacing="16">
    <Grid.ColumnDefinitions>
        <ColumnDefinition x:Name="PreviewColumn" Width="Auto" />
        <ColumnDefinition x:Name="ActionsColumn" Width="2*" MinWidth="200" />
    </Grid.ColumnDefinitions>
    <Grid.RowDefinitions>
        <RowDefinition Height="Auto" />
        <RowDefinition Height="Auto" />
    </Grid.RowDefinitions>
    
    <!-- Превью: 350x196 px (footprint) -->
    <Border x:Name="PreviewCard" Grid.Row="0" Grid.Column="0" />
    
    <!-- Карточка / действия -->
    <Border x:Name="ActionsCard" Grid.Row="0" Grid.Column="1" />
</Grid>
```

### Логика переноса

```csharp
private bool _heroSideBySide = true;

// В конструкторе
HeroLayoutHost.SizeChanged += (_, _) => UpdateHeroLayout();

private void UpdateHeroLayout()
{
    var width = HeroLayoutHost.ActualWidth;
    if (width <= 0) return;

    var minHeroBeside = HeroPreviewLayoutMetrics.FootprintWidth  // 350
                      + HeroColumnGap                             // 24
                      + ActionsCardMinWidth;                      // 340
    // = 714 px

    // Симметричная логика без hysteresis
    var sideBySide = width >= minHeroBeside;

    if (sideBySide != _heroSideBySide)
    {
        _heroSideBySide = sideBySide;

        if (sideBySide)
        {
            // Side-by-side: превью слева, карточка справа
            PreviewColumn.Width = GridLength.Auto;
            ActionsColumn.Width = new GridLength(2, GridUnitType.Star);

            Grid.SetColumn(PreviewCard, 0);
            Grid.SetRow(PreviewCard, 0);
            Grid.SetColumnSpan(PreviewCard, 1);

            Grid.SetColumn(ActionsCard, 1);
            Grid.SetRow(ActionsCard, 0);
            Grid.SetColumnSpan(ActionsCard, 1);
        }
        else
        {
            // Stacked: превью сверху, карточка снизу
            PreviewColumn.Width = GridLength.Auto;
            ActionsColumn.Width = new GridLength(1, GridUnitType.Star);

            Grid.SetColumn(PreviewCard, 0);
            Grid.SetRow(PreviewCard, 0);
            Grid.SetColumnSpan(PreviewCard, 2);

            Grid.SetColumn(ActionsCard, 0);
            Grid.SetRow(ActionsCard, 1);
            Grid.SetColumnSpan(ActionsCard, 2);
        }
    }

    // Дополнительная логика: высота карточки = высота превью (side-by-side)
    if (sideBySide)
    {
        ActionsCard.MinHeight = HeroPreviewLayoutMetrics.OuterHeight;  // 196
        ActionsCard.MaxHeight = HeroPreviewLayoutMetrics.OuterHeight;
    }
    else
    {
        ActionsCard.ClearValue(FrameworkElement.MaxHeightProperty);
    }
}
```

### Расчёт порога

```csharp
// Константы из HeroPreviewLayoutMetrics
public static class HeroPreviewLayoutMetrics
{
    public const double FootprintWidth = 350;   // ширина превью (с shadow margin)
    public const double OuterHeight = 196;      // высота превью
}

private const double HeroColumnGap = 24;              // ColumnSpacing
private const double ActionsCardMinWidth = 340;       // минимум для карточки

// Порог переключения
var minHeroBeside = 350 + 24 + 340 = 714 px;
```

**Правило:** Порог = `PreviewFootprint + Gap + CardMinWidth`. Значение `CardMinWidth` определяется минимальной шириной, при которой карточка не ломает вёрстку (кнопки не переносятся, текст не обрезается).

### Debug-логирование (опционально)

```csharp
System.Diagnostics.Debug.WriteLine(
    $"[HeroLayout] Width={width:F1}, MinBeside={minHeroBeside:F1}, " +
    $"CurrentState={(_heroSideBySide ? "SideBySide" : "Stacked")}");

if (sideBySide != _heroSideBySide)
{
    var transition = sideBySide ? "ВОЗВРАТ В SIDE-BY-SIDE" : "ПЕРЕХОД В STACKED";
    System.Diagnostics.Debug.WriteLine($"[HeroLayout] >>> {transition}: ширина {width:F1} px");
}
```

Логи помогают диагностировать проблемы симметрии. После отладки можно удалить или оставить под `#if DEBUG`.

---

## Паттерн 3: Вложенный контент в карточке «Политика рендеринга»

Внутри карточки «Политика рендеринга» есть адаптивная разметка чекбоксов (inline ↔ stacked). См. [`DwmThemeDetailPage.xaml.cs:843-959`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs).

### Порог переключения

```csharp
private const double PolicyCardStackedThreshold = 450;
private const double PolicyLayoutHysteresis = 16;

private PolicyLayoutMode ResolvePolicyLayoutMode(double cardWidth)
{
    if (_policyLayoutMode == PolicyLayoutMode.Stacked)
    {
        return cardWidth >= PolicyCardStackedThreshold + PolicyLayoutHysteresis
            ? PolicyLayoutMode.Inline
            : PolicyLayoutMode.Stacked;
    }

    return cardWidth < PolicyCardStackedThreshold
        ? PolicyLayoutMode.Stacked
        : PolicyLayoutMode.Inline;
}
```

**Hysteresis здесь оправдан:** Ширина карточки меняется при переключении side-by-side ↔ stacked для hero-блока. Без hysteresis чекбоксы внутри карточки могут «дребезжать» (переключаться туда-сюда) при пороговых значениях.

---

## Паттерн 4: Grid-плитки с динамическими колонками

Для `ItemsView` с `UniformGridLayout` (плитки `.theme`, отражения, материалы) адаптация происходит автоматически через `MaximumRowsOrColumns`. Ручной расчёт не требуется.

**Эталон:** [`DwmThemeDetailPage.xaml.cs:982-1022`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs) — метод `UpdateTilesLayout` для расчёта видимости empty-state.

---

## Паттерн 5: Hosted-диалоги

Body с content-aware layout реализует [`IHostedDialogAdaptiveBody`](../../GlassSettings/Services/HostedDialogTypes.cs). Host вызывает `OnHostedDialogBoundsChanged(EffectiveWidth, EffectiveHeight)` из `MainWindow.NotifyHostedDialogBodyBounds` после `ApplyHostedDialogBounds` (без debounce-таймера на body; debounce только на `DialogBoundsHost`).

Опционально body переопределяет `GetHostedDialogMinWidth()` — host читает перед `HostedDialogBoundsHelper.Compute`. При смене режима body вызывает `HostedDialogHostContext.RequestBoundsRefresh()`.

Дополнительно body может подписаться на собственный `SizeChanged` (как `SpoilerHeaderLayoutHost`).

**Эталон file picker:** [`ExecutableFilePickerPanel`](../../GlassSettings/Controls/Processes/ExecutableFilePickerPanel.xaml.cs) + [`ExecutableFilePickerLayoutMetrics`](../../GlassSettings/Services/ExecutableFilePickerLayoutMetrics.cs) — compact при `width <= 583`, inline при `width >= 585`, гистерезис ±1 px; min shell **534** (compact) / **584** (inline).

**Зона bounds:** `HostedDialogHost` — `RootGrid` `RowSpan="2"`, после overlay, `Margin.Top = AppTitleBar.ActualHeight` — см. [GlassSettings-hosted-dialog.md § Вертикальная зона bounds](GlassSettings-hosted-dialog.md#вертикальная-зона-bounds).

См. также [GlassSettings-hosted-dialog.md](GlassSettings-hosted-dialog.md).

---

## Чек-лист для новых адаптивных элементов

### Спойлер со слайдерами

- [ ] `ActionsBelowHost` с `HorizontalContentAlignment="Right"` в XAML
- [ ] `UpdateActionsLayout()` вызывает `SpoilerHeaderAdaptiveLayout.UpdateLayout`
- [ ] Подписка на `SizeChanged` через прямой вызов (без таймера)
- [ ] Подобран `HeaderActionsInlineWidthEstimate` по замеру `DesiredSize`
- [ ] `SpoilerHeaderGrid`: Title `*`, Slider `Auto` 120–200
- [ ] Гистерезис 48 px только для unstack
- [ ] Проверено выравнивание справа при переносе на новую строку

### Hero-блок с карточками

- [ ] `Grid` с 2 колонками + 2 строками, `ColumnSpacing=24`, `RowSpacing=16`
- [ ] Подписка на `SizeChanged` через прямой вызов (без таймера)
- [ ] Рассчитан порог `minHeroBeside = PreviewWidth + Gap + CardMinWidth`
- [ ] Симметричная логика `width >= minHeroBeside` для обоих направлений
- [ ] Переключение `Grid.SetColumn/Row/ColumnSpan` для элементов
- [ ] Опционально: блокировка высоты карточки в side-by-side режиме
- [ ] Debug-логи для диагностики (опционально, можно удалить после проверки)

---

## Анти-паттерны

### ❌ Debounce-таймер для адаптации

```csharp
// НЕ ИСПОЛЬЗОВАТЬ
private DispatcherQueueTimer? _layoutTimer;

HeroLayoutHost.SizeChanged += (_, _) => ScheduleLayoutUpdate();

private void ScheduleLayoutUpdate()
{
    _layoutTimer ??= CreateLayoutTimer();
    _layoutTimer.Stop();
    _layoutTimer.Start();
}
```

**Почему плохо:** Создаёт задержку 8-32ms, элементы успевают сжаться до нуля перед переносом.

### ❌ Асимметричные пороги без необходимости

```csharp
// НЕ ИСПОЛЬЗОВАТЬ без обоснования
var sideBySide = _heroSideBySide
    ? width >= 714
    : width >= 738;  // +24 hysteresis без причины
```

**Когда допустимо:** Только если внутренние элементы меняют ширину контейнера (hover/focus), и без hysteresis возникает «дребезг».

### ❌ Запоминание ширины контейнера для порога

```csharp
// НЕ ИСПОЛЬЗОВАТЬ
if (width < minBeside)
{
    _switchWidth = width;  // сохранили момент переноса
}

var sideBySide = width >= _switchWidth + 1;  // возврат по запомненной ширине
```

**Почему плохо:** `_switchWidth` может быть меньше `minBeside` из-за скачка ширины при быстром resize. Возврат произойдёт раньше, чем появится достаточно места для side-by-side.

### ❌ Использование `Frame.GoBack()` в breadcrumb

Не относится к адаптивной разметке, но часто возникает при правках hero-блоков. См. [GlassSettings-development.md](GlassSettings-development.md#навигация-breadcrumb-и-назад).

---

## История изменений

| Дата | Автор | Изменения |
|------|-------|-----------|
| 2026-07-13 | Ассистент | Порядок фаз инвертирован: controls (slider) сжимается первым, title последним; ToggleSwitch в ячейке шапки зажимается до 44 px |
| 2026-06-30 | Ассистент | SettingsOptionRow + layout-host; двухфазное сжатие title→controls; TrailingRailPanel collapsed when empty |
| 2026-06-30 | Ассистент | TrailingRailPanel (Gate+chevron); MaxWidth без Width; Left alignment combo-ячеек; trailing gaps 8 px |
| 2026-06-30 | Ассистент | `TitleControlsGap` 10 px; inline `MaxWidth` таблицы = budget; правка обрезки контролов до stack |
| 2026-06-30 | Ассистент | Единый `SpoilerHeaderLayoutEngine` + `SpoilerHeaderControlsTable`; measure-based inlineMin; unstack ±1 px; удалены `SpoilerHeaderAdaptiveLayout`, `HeaderActionsInlineWidthEstimate`, `HubRowActionsGrid` |
| 2026-06-26 | Ассистент | Picker-body: симметричный порог inline/stacked ±1 px; rail пикера — константа `PickerInlineRail`; удалён гистерезис 48 для пикера |
| 2026-06-26 | Ассистент | Colorization suffix-panel: panel 168–248, RefreshHeaderLayout при expand Advanced |
| 2026-06-26 | Ассистент | Единый `SpoilerHeaderAdaptiveLayout`; Slider col Auto; пересчёт ширины на каждом SizeChanged |
| 2026-06-26 | Ассистент | Picker-body: `SettingsPickerBodyLayout`, порог по body-таблице text\|control, controls left |
| 2026-06-26 | Ассистент | `GlassSettingsSliderMetrics` (120–200): единые min/max трека для шапки и body; stack после сжатия col1 |
| 2026-06-13 | Ассистент | Создание документа на основе рефакторинга DwmThemeDetailPage, SettingsSystemGateExpander, SettingsFlatGateRow |
