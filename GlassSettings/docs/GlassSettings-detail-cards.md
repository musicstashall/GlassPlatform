# GlassSettings — типы detail-карточек

Канонические виды карточек на detail-страницах и hub-строках. Общая разметка заголовка gate-спойлеров — `SpoilerHeaderGrid` (сетка **3 col**: title | actions | trailing+шеврон).

См. также: [GlassSettings-development.md](GlassSettings-development.md), [GlassSettings-WinUI.md](GlassSettings-WinUI.md).

---

## Дерево выбора

```
Есть тело под заголовком (раскрываемый блок)?
  да → Нужен gate вкл/выкл значения в реестре?
          да → SettingsSystemGateExpander
          нет → Доп. секция-разделитель (Pivot, вложенные спойлеры)?
                  да → SettingsDividerExpander
                  нет → (редко — уточнить паттерн)
  нет → Есть переход на другую страницу?
          да → SettingsNavigateRow
          нет → Один контрол в строке?
                  да → SettingsOptionRow
                  нет → DwmOpacityGroupRow (режим + slider + gate)
```

---

## Тип 1 — спойлер с разворотом

| | |
|--|--|
| **Контрол** | `SettingsSystemGateExpander` |
| **Когда** | Секция с телом (color picker, `StackPanel` и т.д.) и gate вкл/выкл значения в реестре |
| **Разворот** | WinUI `Expander` (системная анимация тела) |
| **Шеврон** | Кастомный `FontIcon` в `SpoilerHeaderGrid`; системный `AnimatedIcon` скрыт |
| **Gate** | `ISettingsGateSection`, `GateToggled`, `IsGateOn` |
| **Файлы** | [`SettingsSystemGateExpander.xaml`](../../GlassSettings/Controls/SettingsSystemGateExpander.xaml), [`.xaml.cs`](../../GlassSettings/Controls/SettingsSystemGateExpander.xaml.cs) |
| **Эталон** | [`DwmColorizationDetailPage.xaml`](../../GlassSettings/Pages/Dwm/DwmColorizationDetailPage.xaml) — цветовые спойлеры |

### Поведение шеврона

- Глиф всегда `E76C` (вправо); поворот `RotateTransform`: 0° свёрнут, 90° развёрнут.
- Анимация 250 ms, `CubicEase EaseInOut`; угол сохранять **до** `Storyboard.Stop()` (иначе сворот 0°→0°).
- При gate **выкл.**: opacity шеврона 0.4, разворот заблокирован (`Expanding` откатывается).

### Обход ограничений WinUI Expander

| Проблема | Решение |
|----------|---------|
| `ExpanderHeaderPadding` в `Expander.Resources` не попадает в шаблон | `ApplyHeaderPadding()` на `ToggleButton` в `OnLoaded` |
| Системный шеврон мелкий / не кастомизируется | `HideSystemChevron()` → `ExpandCollapseChevronBorder.Visibility = Collapsed` |
| Hover шапки при gate выкл. | Подсветка только при `GateSwitch.IsOn` |

### Тело спойлера с `SettingsColorPickerPanel` (строгий паттерн)

Контрол: [`SettingsColorPickerPanel.xaml`](../../GlassSettings/Controls/SettingsColorPickerPanel.xaml), [`.xaml.cs`](../../GlassSettings/Controls/SettingsColorPickerPanel.xaml.cs).

**Когда:** развёрнутый спойлер с `GlassAlphaColorPicker` (градиент, цвет бордюра и т.п.).

**Внешняя таблица** (`LayoutRoot`): левая info-колонка (adaptive по ширине/высоте) | вертикальный разделитель | правая зона пикера (фиксированный rail).

**Левая колонка (info)** — три функциональные строки:

| # | Строка | Содержимое |
|---|--------|------------|
| 1 | Описание | Bullet + `Description` на всю ширину info-колонки, высота по тексту |
| 2 | Body-table | `FooterContent` — внутренняя таблица: **col0** подписи (ширина = max label), **col1** контролы (**120–200**), выравнивание слева; динамический правый зазор = `FooterHost.Padding.Right` (сначала shrink inset, затем col1 до 120) |
| 3 | Default action | Разделитель + кнопка «По умолчанию» (фиксированная высота блока); flex-зазор (`RowDefinition *`) между строкой 2 и 3 |

Дополнительно в XAML между строками 1 и 2 — горизонтальный разделитель при наличии описания или footer.

| # | Элемент | Условие |
|---|---------|---------|
| 1 | Bullet + описание (`Description`, `GlassSettingsSpoilerSecondaryTextStyle`) | всегда при наличии текста |
| 2 | Горизонтальный разделитель | если есть описание **или** `FooterContent`; **на всю ширину** info-колонки (`HorizontalAlignment="Stretch"`) |
| 3 | `FooterContent` | body-таблица `SettingsPickerBodyLayout` (подпись слева, контрол слева в col1): blur Win7, glow rows, чекбокс «Без бордюра» |
| 4 | Flex-зазор (`RowDefinition Height="*"`) | всегда |
| 5 | Нижний разделитель + кнопка «По умолчанию» (`ColorPicker_ResetDefault`) | gate вкл.; разделитель **на всю ширину** info-колонки; кнопка слева под ним; видна при gate вкл., `IsEnabled` — если сброс допустим |

**Правая колонка:** `GlassAlphaColorPicker` на `SubtleFillColorSecondaryBrush`, вертикальный разделитель на всю высоту.

**Адаптивность:** body-table — `ResolveInfoContentWidth()`; stack/unstack по слоту LayoutRoot→PickerZone и **cachedMin** (один раз при создании контента). Unstack: `slot >= cachedMin + 1`. `$env:PickerBodyLayoutTrace=1`. См. [GlassSettings-adaptive-layout.md § 2b](GlassSettings-adaptive-layout.md#2b-с-пикером--settingscolorpickerpanel-body-table).

**Эталоны:** [`FlyoutKindDetailPage`](../../GlassSettings/Pages/Glass/FlyoutKindDetailPage.xaml) (градиент + бордюр), [`RootWindowsPage`](../../GlassSettings/Pages/Glass/RootWindowsPage.xaml), [`TaskbarFlyoutPage`](../../GlassSettings/Pages/Glass/TaskbarFlyoutPage.xaml), [`DwmColorizationDetailPage`](../../GlassSettings/Pages/Dwm/DwmColorizationDetailPage.xaml).

### Тело спойлера без пикера (`SettingsParameterPanel`)

Контрол: [`SettingsParameterPanel.xaml`](../../GlassSettings/Controls/SettingsParameterPanel.xaml), [`.xaml.cs`](../../GlassSettings/Controls/SettingsParameterPanel.xaml.cs).

**Когда:** развёрнутый спойлер без color picker — параметры, combo, NumberBox (напр. «Вид окон», «Вид заголовков», Popup Animation).

**Разметка тела:** flat-grid **col0 | col1 | col2 | col3** (`CreateBodyTable`, `AddSingleRow`, `AddDualRow`); `UpdateParameterBodyLayout` + dynamic `BodyHost.Padding.Right`; dual-row wrap **per-row** при сужении. Описание и «По умолчанию» — без изменений.

**Семантика колонок:**

| Тип строки | Inline (достаточная ширина) | Narrow (wrap) |
|------------|----------------------------|---------------|
| `AddSingleRow` | col0 label, col1 control | без изменений (col0/col1) |
| `AddDualRow` | col0/col1 primary, col2/col3 secondary | secondary → подстрока col0/col1 |

**Конвейер layout (стабильный паттерн):**

1. Страница собирает таблицу в ctor: `CreateBodyTable` → `AddSingleRow` / `AddDualRow` → `panel.BodyContent = table`.
2. Expander: `WireExpandedLayoutRefresh(() => panel.RefreshLayout())` — deferred на следующий кадр.
3. `RefreshLayout`: `InvalidateParameterBodyLayoutCache` + сброс metrics + `UpdateParameterLayout`; при `layoutW == 0` — one-shot retry (max 3), без infinite enqueue.
4. `SettingsParameterBodyLayout`: `EnsureLogicalRows` (dual — по `DualRowPrimaryTag` или inline 4-col; singles по одной строке); per-row `InlineMinWidth` + wrap/unwrap; `HasVisualWrapMismatch` → resync; leaf ComboBox/NumberBox/ToggleSwitch не трогать programmatic width.

Подробнее и анти-паттерны — [GlassSettings-adaptive-layout.md §2a](GlassSettings-adaptive-layout.md#2a-без-пикера--settingsparameterpanel-4-col-body-table).

| # | Элемент |
|---|---------|
| 1 | Bullet + `Description` (`*BodyDesc`) |
| 2 | Верхний разделитель (автоматически при непустом `Description`) |
| 3 | `BodyContent` — parameter body-table |
| 4 | Нижний разделитель + «По умолчанию» — `SetDefaultActionState(gateOn, disable)` + `DefaultClick` |

**Эталоны:** [`DwmGlassDetailPage`](../../GlassSettings/Pages/Dwm/DwmGlassDetailPage.xaml) (`WindowParameterPanel`, `CaptionParameterPanel`), [`PopupHubPage`](../../GlassSettings/Pages/Glass/PopupHubPage.xaml) (`AnimationPanel`). Диагностика: `$env:ParameterBodyLayoutTrace=1`.

### `SettingsDividerExpander` — разделитель-спойлер (без gate)

Компактный WinUI `Expander` для секций «Дополнительно»: заголовок в одну строку с горизонтальным разделителем, тело — произвольный `ContentProperty`. **Без** gate-тумблера и **без** карточной рамки.

| | |
|--|--|
| **Контрол** | `SettingsDividerExpander` |
| **Когда** | Дополнительный блок настроек под основными карточками; разворот всей подсекции (Pivot, вложенные спойлеры), не отдельного ключа реестра |
| **Разворот** | Системный WinUI `Expander` (`ExpandDirection="Down"`) — нативная анимация сдвига контента |
| **Шеврон** | Кастомный `FontIcon` `E76C` **перед** текстом; системный шеврон скрыт (`SpoilerExpanderChrome.HideSystemChevron`) |
| **Gate** | Нет |
| **Файлы** | [`SettingsDividerExpander.xaml`](../../GlassSettings/Controls/SettingsDividerExpander.xaml), [`.xaml.cs`](../../GlassSettings/Controls/SettingsDividerExpander.xaml.cs) |
| **Chrome** | [`SpoilerExpanderChrome.ApplyDividerExpanderChrome`](../../GlassSettings/Controls/SpoilerExpanderChrome.cs) |
| **Эталон** | [`DwmColorizationDetailPage`](../../GlassSettings/Pages/Dwm/DwmColorizationDetailPage.xaml) — секция «Дополнительно» |

#### Разметка (XAML)

```xml
<ctrl:SettingsDividerExpander
    x:Name="AdvancedExpander"
    Title="{x:Bind ...}"
    Expanded="AdvancedExpander_OnExpanded">
    <StackPanel Spacing="{StaticResource SettingsCardSpacing}">
        <!-- Pivot, SettingsSystemGateExpander, SettingsOptionRow и т.д. -->
    </StackPanel>
</ctrl:SettingsDividerExpander>
```

- `Title` — текст слева от разделителя (`GlassSettingsSpoilerSecondaryTextStyle`).
- **Несохранённые изменения (свёрнут):** при dirty внутри тела и `IsExpanded=false` **фон шапки** Expander (`ToggleButton`) — полупрозрачный жёлтый (`SystemFillColorCautionBackground`, severity **Warning**, как у точек), pill-форма (`CornerRadius = height/2`), hover — плотнее тот же caution-фон; без dirty — hover `SubtleFillColorSecondaryBrush`. Текст и линия-разделитель не перекрашиваются. Scope — `UnsavedScope` или `HasUnsavedOverride`; обновление через `SettingsUnsavedIndicatorRefresh` на `DirtyChanged` (см. [GlassSettings-notices.md](GlassSettings-notices.md)).
- Дочерний элемент страницы попадает в `BodyContent` → `ContentPresenter` тела `Expander`.
- `IsExpanded` — прокси к `RootExpander.IsExpanded`.
- Событие `Expanded` — при любом изменении `IsExpanded` (после разворота обновить chrome вложенных спойлеров).
- **Host с `Visibility=Collapsed`:** шаблон inner `Expander` может не материализоваться на первом `Loaded` → системный шеврон справа, непрозрачная шапка. Контрол подписан на `RootExpander.Loaded`, делает deferred retry (`Normal`, до 5) и one-shot `LayoutUpdated`; при показе host вызывайте `RefreshDividerChrome()` (см. hub «Система» — `ApplySelectedHiveContent`).
- **Кэшированная навигация (Frame):** при возврате на hub/detail `Loaded` страницы может не повториться — вызывайте `RefreshDividerChrome()` синхронно и deferred через `DispatcherQueue` в `OnNavigatedTo` / `ApplySelectedHiveContent` (см. [`DwmSettingsHubPage`](../../GlassSettings/Pages/Dwm/DwmSettingsHubPage.xaml.cs) — `RefreshSystemDividerChromeDeferred`).

#### Заголовок

| Элемент | Поведение |
|---------|-----------|
| Высота | `MinHeight="0"`, padding шапки `2,0,0,0` — по высоте текста + шеврон 12px |
| Отступы секции | `Margin="0,20,0,20"` на `RootExpander` (симметрично сверху и снизу) |
| Отступ тела | `Margin="0,20,0,0"` на `BodyHost` — зазор между заголовком и контентом при развороте |
| Фон | Прозрачный в покое (`SystemControlTransparentBrush`); `SubtleFillColorSecondaryBrush` только при hover на `ToggleButton` шапки |
| Рамка / focus | `BorderThickness="0"`, `UseSystemFocusVisuals="false"` |
| Разделитель | `Border` 1px в колонке `*`, `IsHitTestVisible="False"`, вертикально по центру строки с текстом |

#### Шеврон

- Поворот `RotateTransform`: 0° свёрнут, 90° развёрнут; анимация 250 ms через `SpoilerExpanderChrome.AnimateChevron(FontIcon, ...)`.
- Синхронизация: `RegisterPropertyChangedCallback(Expander.IsExpandedProperty, ...)` в `OnLoaded`.

#### Отличие от `SettingsSystemGateExpander`

| | `SettingsSystemGateExpander` | `SettingsDividerExpander` |
|--|------------------------------|---------------------------|
| Вид | Карточка (фон, border, corner radius) | Прозрачная строка-разделитель |
| Gate | `GlassSettingsLabeledToggleSwitch` | Нет |
| MinHeight шапки | `GlassSettingsSystemExpanderHeaderMinHeight` (74) | По тексту |
| Типичное тело | Один пикер / панель параметров | Pivot + несколько вложенных спойлеров |

**Не** использовать `ExpanderBodyTransition` / кастомную покадровую анимацию — только системный `Expander`.

**Эталон Pivot внутри тела:** [`RootWindowsPage`](../../GlassSettings/Pages/Glass/RootWindowsPage.xaml), [`DwmThemeDetailPage`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml) — `PivotTabContentTransition.AnimateTabChangeAsync`.

**Анти-паттерны:**

1. `FooterContent` на спойлерах градиента или иных секций — чекбокс «Без бордюра» только у «Цвет бордюра».
2. Кнопку «По умолчанию» ставить под чекбокс вместо нижнего якоря — нарушает паттерн.
3. Info-карточку OpenGlass **внутри** тела спойлера бордюра — только **над** карточками страницы.
4. Кастомный `Border` + `PointerPressed` вместо `SettingsDividerExpander` — ломает нативную анимацию разворота.

### Gate выкл. (общее правило типов 1 и 3 с gate)

`SpoilerHeaderGrid.ApplyGateVisual(false)`:

- title, description, `HeaderActions`, шеврон — приглушены (0.55 / 0.4);
- **toggle всегда** `IsEnabled = true`, `TrailingHost.Opacity = 1`;
- hover карточки/шапки **нет**.

---

## Тип 2 — навигационная строка

| | |
|--|--|
| **Контрол** | `SettingsNavigateRow` |
| **Когда** | Переход на дочернюю страницу; шеврон **не** разворачивает секцию |
| **Шеврон** | Статичный `E76C`; клик по неинтерактивной зоне → `NavigateRequested` |
| **Hover** | `PointerOver` на всей карточке (`SubtleFillColorSecondaryBrush`) |
| **Файлы** | [`SettingsNavigateRow.xaml`](../../GlassSettings/Controls/SettingsNavigateRow.xaml), [`.xaml.cs`](../../GlassSettings/Controls/SettingsNavigateRow.xaml.cs) |
| **Эталоны** | Hub: [`DwmSettingsHubPage`](../../GlassSettings/Pages/Dwm/DwmSettingsHubPage.xaml.cs), [`GlassSettingsHubPage`](../../GlassSettings/Pages/Glass/GlassSettingsHubPage.xaml.cs) (ComboBox + `_loading` — см. [GlassSettings-host-glass.md](GlassSettings-host-glass.md)); detail: [`DwmThemeDetailPage`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml) (CaptionButtons, «Текстуры», «Атласы»), [`DwmGlassDetailPage`](../../GlassSettings/Pages/Dwm/DwmGlassDetailPage.xaml) (заглушка) |

### Hub vs detail

| Контекст | Trailing | HeaderActions |
|----------|----------|---------------|
| Hub (DWM, Popup, …) | `GlassSettingsLabeledToggleSwitch` в `TrailingContent` | `HubRowActionsGrid` (combo/slider) |
| Detail | часто только шеврон | `null` |

**Hub Popup** — выключение gate раздела (Menu и др.): hosted-диалог + `CustomizationCatalog.RemoveFlyoutSection` (удаление всего поддерева BWM), затем notice — см. [GlassSettings-hosted-dialog.md](GlassSettings-hosted-dialog.md), [GlassSettings-customization-tracking.md](GlassSettings-customization-tracking.md).

`CanNavigate = false` → opacity 0.55, шеврон 0.4, без navigate.

`SettingsNavigationHost` выставляет дочерним строкам `Width = SectionList.ActualWidth`.

### Исключение: страница «Система» (A11)

Спойлеры OpenGlass / Glass Platform: `PlatformSchedulerStackableActions` в `HeaderActions` (перенос в `ActionsBelowHost`), `ToggleSwitch` установки — отдельный `new` в `TrailingContent`. Dual Glass — две navigate-строки (x86 и x64), каждая со своим gate. См. [GlassSettings-adaptive-layout.md § Паттерн 1b](GlassSettings-adaptive-layout.md#паттерн-1b-platformschedulerrow-на-странице-система).

---

## Тип 3 — плоская группа контролов

Без разворота и без шеврона (`ShowChevron="False"`).

### 3a — `SettingsOptionRow`

| | |
|--|--|
| **Когда** | Один контрол в строке (например `ComboBox`) |
| **Gate** | нет |
| **Hover** | всегда |
| **ContentProperty** | `HeaderActions` (дочерний элемент = контрол справа) |
| **Эталон** | [`DwmThemeDetailPage`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml) — CaptionButtons; [`DwmColorizationDetailPage`](../../GlassSettings/Pages/Dwm/DwmColorizationDetailPage.xaml) — OpaqueBlendPriority |

### 3b — `SettingsProgressActionRow`

| | |
|--|--|
| **Когда** | Плоская action-строка без разворота с progress-fill шапкой (long-running download) |
| **Gate** | нет |
| **Hover** | только в idle (не во время download) |
| **Шапка** | `IconGlyph` слева; зелёный/красный fill по ширине %; текст процентов между title и кнопкой |
| **Подстрока** | относительный путь к `{OpenGlass.dll}\symbols` от `GlassSettings.exe` |
| **Preflight** | `SymbolDownloadPreflightService` (dbghelp/symsrv) + `NetworkConnectivityService`; блокеры — `OpenGlassSymbolDownloadDialogService` |
| **Кнопки** | Download ↔ Cancel в одном trailing-слоте |
| **Эталон** | [`DwmSettingsHubPage`](../../GlassSettings/Pages/Dwm/DwmSettingsHubPage.xaml) — «Скачать символы» |

### 3c — `DwmOpacityGroupRow`

| | |
|--|--|
| **Когда** | Режим (combo) + значение (slider) + gate |
| **Контролы** | `HubRowActionsGrid` в `HeaderActions`, toggle в `Trailing` |
| **Hover** | только при gate **вкл.** |
| **Эталон** | [`DwmColorizationDetailPage.xaml.cs`](../../GlassSettings/Pages/Dwm/DwmColorizationDetailPage.xaml.cs) — blur slider в `ActiveColorPanel.FooterContent` (Win7) |

---

## Общая разметка заголовка (`SpoilerHeaderGrid`)

```
Grid:  Auto (Icon)  |  * (Title)  |  Auto (HeaderActions)  |  Auto (TrailingHost [+ Chevron])
```

| Правило | Значение |
|---------|----------|
| Секционная иконка | `IconGlyph` (Segoe MDL2 `FontIcon`); константы — [`SettingsSectionGlyphs.cs`](../../GlassSettings/Controls/SettingsSectionGlyphs.cs); hub — автоподстановка по id в `AddNavigateSection` |
| Минимальная ширина подписи | `ColumnDefinition` col Title: `MinWidth` = `GlassSettingsSectionTitleMinWidth` (**150**); дублировать на `TitlePanel` — типы 1/2/3 |
| Подстрока (description) | `GlassSettingsSpoilerSecondaryTextStyle` (`CaptionTextBlockStyle` + `TextFillColorSecondaryBrush`); тот же стиль — подписи в шапке (toggle «Вкл.», `HubInlineLabel`, тело пикера); **без** `TextTrimming` — перенос `WrapWholeWords` |
| Заголовок спойлера | `GlassSettingsSpoilerTitleTextStyle` (`BodyStrongTextBlockStyle`) |
| Toggle и шеврон | в **одном** `StackPanel` col3, `Spacing` = `GlassSettingsToggleChevronSpacing` (8) |
| Switch в hub | только `TrailingContent` / `Trailing`, **не** в `HubRowActionsGrid` |
| `HubRowActionsGrid` | только combo + slider, без `*`, без switch |
| `HeaderActions` / actions под шапкой | **всегда справа**: inline — `SpoilerHeaderGrid` col2 (`HorizontalAlignment` / `HorizontalContentAlignment` = Right); при переносе — `ActionsBelowHost` с `HorizontalContentAlignment="Right"` |
| `MinHeight` | только на внешнем `Border` карточки, не на внутреннем `Grid` |
| Компактный switch | `GlassSettingsLabeledToggleSwitch`: `InnerSwitch` 44×44, `Spacing` 6 до `StateText` |

Файл: [`SpoilerHeaderGrid.xaml`](../../GlassSettings/Controls/SpoilerHeaderGrid.xaml).

### Адаптивность opacity-строк (`DwmOpacityGroupRow` + `HubRowActionsGrid`)

| Правило | Значение |
|---------|----------|
| Минимальная ширина колонки Title | `SpoilerHeaderGrid` col1: `ColumnDefinition MinWidth` = **150** (`GlassSettingsSectionTitleMinWidth`); **не** только `TitlePanel.MinWidth` — иначе `*` сжимает текст до нуля |
| Перенос actions | Когда ширина строки &lt; icon + title(**150**) + actions + toggle — `HubRowActionsGrid` **под** заголовком; порог с **гистерезисом** (не пересчитывать по `DesiredSize` после смены ориентации) |
| Узкий блок actions | combo и slider — **вертикально** (&lt; 240 px) |

Эталон: [`DwmReflectionPickerPage`](../../GlassSettings/Pages/Dwm/DwmReflectionPickerPage.xaml), [`DwmOpacityGroupRow.xaml.cs`](../../GlassSettings/Controls/DwmOpacityGroupRow.xaml.cs).

### Hero-превью (340×192)

| Ресурс | Значение | Назначение |
|--------|----------|------------|
| `GlassSettingsHeroPreviewOuterWidth` | 340 | Ширина рамки превью **с** бордюром |
| `GlassSettingsHeroPreviewOuterHeight` | 192 | Высота рамки превью **с** бордюром |
| `GlassSettingsHeroPreviewBorderThickness` | 3 (Thickness) | Толщина тёмной окантовки |
| `GlassSettingsHeroPreviewShadowMargin` | 0,0,8,8 | Отступ под тень |

Эталон: [`ThemeHeroPreview`](../../GlassSettings/Controls/ThemeHeroPreview.xaml). Колонка превью — **`Width=Auto`** (не `*`), колонка контролов — `*`. Сжимается только блок справа; превью не обрезается родителем.

Изображения **отражателя** (не тем): `Stretch=None`, `HorizontalAlignment=Left`, `VerticalAlignment=Top`, `ClipToBounds=True` — левый верхний угол без масштаба.

---

## Информационный блок (`SettingsInfoBlock`)

Единый паттерн для поясняющих блоков **над** основным контентом страницы (не карточка настроек, не notice-полоса).

| | |
|--|--|
| **Контрол** | `SettingsInfoBlock` |
| **Когда** | Статичное пояснение, предупреждение, контекст раздела; опционально гиперссылки в теле |
| **Файлы** | [`SettingsInfoBlock.xaml`](../../GlassSettings/Controls/SettingsInfoBlock.xaml), [`.xaml.cs`](../../GlassSettings/Controls/SettingsInfoBlock.xaml.cs) |
| **Эталоны** | [`DwmReflectionPickerPage`](../../GlassSettings/Pages/Dwm/DwmReflectionPickerPage.xaml) — title + message; [`SystemPage`](../../GlassSettings/Pages/SystemPage.xaml) — только rich-текст с ссылками |

### Визуал

| Параметр | Значение |
|----------|----------|
| Фон | `LayerFillColorAltBrush` |
| Обводка | `AccentFillColorDefaultBrush`, `BorderThickness` 1, `CornerRadius` 8 |
| Padding | 16 |
| Иконка | Segoe MDL2 `E946`, 27 px, `AccentFillColorDefaultBrush`, без заливки |
| Заголовок | `BodyStrongTextBlockStyle`; скрывается, если `Title` пустой |
| Текст | `CaptionTextBlockStyle`, `TextFillColorSecondaryBrush`, `WrapWholeWords` |

### API

- **`Title` / `Message`** — простой текст (XAML или code-behind).
- **`MessageTextBlock`** — для `Inlines` (гиперссылки): очистить `Inlines`, не задавать `Message` одновременно.
- **`IconGlyph`** — переопределение глифа (по умолчанию `E946`).

### Анти-паттерны

1. **Кастомный `Border` + `FontIcon`** на каждой странице — дублирует эталон; использовать `SettingsInfoBlock`.
2. **Notice-полоса** (`SettingsPageNoticeStack`) — для несохранённых изменений и scope; не смешивать с info block.
3. **Зелёная иконка успеха** (`SystemFillColorSuccessBrush`) — устаревший стиль; акцент только через `AccentFillColorDefaultBrush`.

---

## Визуальный chrome (detail-эталон — Colorization)

| Параметр | Значение |
|----------|----------|
| `CornerRadius` | 8 |
| `BorderBrush` | `CardStrokeColorDefaultBrush` |
| `Background` | `LayerFillColorDefaultBrush` |
| Hover | `SubtleFillColorSecondaryBrush` |
| Межкарточный зазор | `SettingsCardSpacing` (4) |

### Ресурсы (`App.xaml`)

| Ключ | Значение | Назначение |
|------|----------|------------|
| `GlassSettingsSystemExpanderHeaderMinHeight` | 74 | Спойлеры и detail-строки с title + description |
| `GlassSettingsSystemExpanderHeaderPadding` | 12,16,16,16 | Padding шапки Expander (code-behind) |
| `GlassSettingsSystemExpanderContentPadding` | 12,4,12,10 | Тело спойлера |
| `GlassSettingsOpacityRowPadding` | 12,16,16,16 | Плоские, navigate, option-строки |
| `GlassSettingsOpacityRowControlSpacing` | 32 | Зазор combo ↔ slider в opacity-строке |
| `GlassSettingsToggleChevronSpacing` | 8 | Зазор toggle ↔ шеврон |
| `GlassSettingsSectionIconSize` | 20 | Размер секционной иконки |
| `GlassSettingsSectionIconMargin` | 0,0,12,0 | Отступ иконки от заголовка |
| `GlassSettingsSectionTitleMinWidth` | **150** | Минимальная ширина col Title в `SpoilerHeaderGrid` (`ColumnDefinition MinWidth`) |
| `GlassSettingsPlatformSchedulerStackableWidth` | 220 | Подпись + split-button на «Система» (без toggle); см. [GlassSettings-adaptive-layout.md](GlassSettings-adaptive-layout.md#паттерн-1b-platformschedulerrow-на-странице-система) |
| `GlassSettingsHeroPreviewOuterWidth` | 340 | Hero-превью: ширина с бордюром |
| `GlassSettingsHeroPreviewOuterHeight` | 192 | Hero-превью: высота с бордюром |
| `GlassSettingsHeroPreviewBorderThickness` | 3 (Thickness) | Hero-превью: толщина окантовки |
| `GlassSettingsHeroPreviewShadowMargin` | 0,0,8,8 | Hero-превью: отступ под тень |
| `GlassSettingsSectionRowMinHeight` | 66 | Hub-строки без двухстрочного description |
| `GlassSettingsInfoBlockMargin` | 0,0,0,12 | Отступ под информационным блоком |

---

## Анти-паттерны

1. **`toolkit:SettingsCard`** на detail-страницах — другой radius, нет hover в нашем паттерне.
2. **`SettingsGateExpander`** — legacy (покадровая анимация тела); **не** использовать на новых страницах → миграция на `SettingsSystemGateExpander`.
3. **`Opacity` на всём заголовке** при gate off — затемняет toggle; использовать `ApplyGateVisual`.
4. **`Expander.Resources`** для `ExpanderChevronGlyphSize` / `ExpanderHeaderPadding` — не работает (`StaticResource` темы).
5. **Фикс. ширина колонки toggle** (96/116px), `ToggleColumn.Width = 0` — пропажа switch.
6. **`*` в `HubRowActionsGrid`** — toggle визуально в центре строки.
7. **`RelativePanel` / overlay** в заголовке hub — регрессии высоты.
8. **Разносить** toggle и chevron по соседним col с большими margin — лишний зазор у правого края.
9. **Дублировать `MinHeight`** на внутреннем `Grid` заголовка.

---

## Миграция (статус)

| Страница / область | Контролы |
|--------------------|----------|
| **Colorization** | эталон всех трёх типов |
| **Theme** | `SettingsOptionRow` + `SettingsNavigateRow` |
| **Glass (detail)** | `SettingsNavigateRow` |
| **Flyout / Taskbar / SWCA** | ещё `SettingsGateExpander` — **к миграции** |
| **GlassGeneralPage** | заглушка `SettingsCard` — заменить при переносе |

---

## Чеклист для ассистентов

1. Определить тип по дереву выбора; не вводить четвёртый вариант без согласования.
2. Заголовок — через `SpoilerHeaderGrid` (или обёртку из таблицы типов).
3. `IconGlyph` обязателен (`SettingsSectionGlyphs`); hub — id в `AddNavigateSection`.
4. Gate off — `ApplyGateVisual`, toggle не блокировать.
5. Padding и высота — из ресурсов `App.xaml`, не хардкод на странице.
6. Сверка с **Colorization** после правок layout.
