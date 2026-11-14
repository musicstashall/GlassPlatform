# Мастер визуальной настройки ink (TextVisualTuning)

Концепция экранов и параметров: [`GlassSettings/GDI_AntiAliasing_Wizard_Concept_v3.md`](../../GlassSettings/GDI_AntiAliasing_Wizard_Concept_v3.md).  
Рендер превью: [`glasscore-text.md`](../../GlassCore/docs/glasscore-text.md) (hero / `PaintTextPreviewToBuffer`).

---

## Модули и код

| Компонент | Путь | Назначение |
|-----------|------|------------|
| Окно мастера | [`TextVisualTuningWindow.xaml.cs`](../../GlassSettings/TextWizard/TextVisualTuningWindow.xaml.cs) | Навигация, кэш, анимация страниц |
| Логика шагов | [`TextVisualTuningWizardEngine.cs`](../../GlassSettings/Core/Text/TextVisualTuningWizardEngine.cs) | Пресеты, `_working`, replay, Apply/Go |
| Кэш страниц | [`WizardPreviewCacheStore.cs`](../../GlassSettings/TextWizard/WizardPreviewCacheStore.cs) | `Ar[page]` → слот, invalidate, STA-render |
| Модель ArN | [`WizardPreviewPageSlot.cs`](../../GlassSettings/TextWizard/WizardPreviewPageSlot.cs) | `ArN[6][3]`, обход, метки |
| Сборка слота | [`WizardPreviewSlotBuilder.cs`](../../GlassSettings/TextWizard/WizardPreviewSlotBuilder.cs) | Snapshot engine → ArN |
| UI из кэша | [`WizardPreviewMapRenderer.cs`](../../GlassSettings/TextWizard/WizardPreviewMapRenderer.cs) | Карточки, scroll, chrome |
| Синхронизация выбора | [`WizardPreviewSelectionSync.cs`](../../GlassSettings/TextWizard/WizardPreviewSelectionSync.cs) | Engine ↔ markers ↔ UI |
| Bitmap (STA) | [`WizardPreviewBitmapFactory.cs`](../../GlassSettings/TextWizard/WizardPreviewBitmapFactory.cs), [`WizardPreviewRenderScheduler.cs`](../../GlassSettings/TextWizard/WizardPreviewRenderScheduler.cs) | GDI без UI-потока |
| Карточка | [`WizardInkPreviewCard.xaml.cs`](../../GlassSettings/Controls/WizardInkPreviewCard.xaml.cs) | Пакетная подстановка bitmap |

---

## Структура кэша ArN[6][3]

Каждая **страница** мастера хранится как корневой ярус `WizardPreviewTierNode` (до 6 строк).

| Col | Поле `WizardPreviewMapRow` | Содержимое |
|-----|---------------------------|------------|
| **0** | `Preset` | Лист: `WizardPreviewBlock` + `BitmapBuffer` + `WizardSelectionPath` |
| **1** | `Nested` | Дочерний ярус (та же структура ArN[6][3], рекурсивно) |
| **2** | `Marker` | `0` или `1` — выбранная строка **на этом ярусе** |

Корень страницы: `WizardPreviewPageSlot.Root`.

Глобальный кэш: `WizardPreviewCacheStore` — `List<WizardPreviewPageSlot?>` по индексу шага.

### Режимы UI (из структуры, без отдельных флагов)

| Режим | Условие | Пример |
|-------|---------|--------|
| **SingleTier** | Ни у одной строки нет `Nested` | Экран 1 (fork 2), экран 5 (blend 6) |
| **Tiered** | Есть хотя бы один `Nested` | Экраны 2–4 (гамма + контур / pseudo) |

`FlatGridColumns` на ярусе задаёт число колонок плоской сетки (2 или 3).

### Рекурсия ярусов

- Col1 может содержать полный дочерний `WizardPreviewTierNode`; тот же узел снова может иметь `Nested` в своих строках.
- Сейчас в продукте реализованы **два яруса** (верхний ряд + scroll нижнего). Трёхъярусные схемы расширяются добавлением `Nested` в builder и доработкой `WizardPreviewMapRenderer` без смены модели ArN.

---

## Метки выбора (col2)

**Источник истины для подсветки карточек** — метки в кэше страницы (`Marker == 1` ровно в одной строке яруса). Engine хранит **дубликат** выбора в массивах `_selectedBlockPerPage` / `_topColumnPerPage` / `_bottomColumnPerPage` / `_bottomRowActivePerPage` — они нужны для `ReplayWorkingProfileUpTo`, `ApplyStepChoice` и `GoNext`/`GoBack` до `PopulateFromMap`.

| Событие | Поведение |
|---------|-----------|
| **Сборка слота** (`WizardPreviewSlotBuilder`) | Метки на **корневом** ярусе из engine; во **вложенных** ярусах метки **не** ставятся |
| **Клик по карточке** | Engine → `OnSelectionMutated` → `ApplyEngineToSlot` на **текущей** странице кэша (in-place) |
| **Populate / Back (cache hit)** | `RestoreEngineFromSlot` ← метки; `ApplySlotMarkersToCards` → `IsCardSelected` |
| **Смена выбора на шаге N** | `InvalidateFrom(N+1)` + `ResetForwardSelections(N+1)` — **оба** обязательны |

API меток: `WizardPreviewSelectionMarkers` (`SetMarkedRow`, `FindMarkedRow`, `ClearNestedMarkers`).

---

## Механика выбора

### Два контура состояния

| Контур | Где | Для чего |
|--------|-----|----------|
| **Метки ArN[col2]** | `WizardPreviewPageSlot` в кэше | Подсветка карточек, восстановление при Back из bitmap-кэша |
| **Массивы engine** | `_selectedBlockPerPage`, `_topColumnPerPage`, … | Replay `_working`, фиксация при `GoNext`, дефолты при rebuild вперёд |

При **Populate** метки **перезаписывают** runtime engine (`RestoreEngineFromSlot`). При **клике** engine обновляет метки текущей страницы (`ApplyEngineToSlot`). Рассинхрон двух контуров — типичная причина «selected не тот» на следующей странице.

### Путь клика (`WizardSelectionPath`)

Каждый лист пресета несёт `WizardSelectionPath(Row, Column, PresetIndex, Role)`:

| `Role` | Экран | `Row` | `PresetIndex` | Обработчик |
|--------|-------|-------|---------------|------------|
| `FlatBlock` | SingleTier (fork, blend) | индекс карточки 0…N−1 | 0 | `SelectBlock` |
| `TopRow` | Tiered, верхний ряд | столбец гаммы 0…2 | 0 | `SelectTopColumn` |
| `BottomRow` | Tiered, дочерний ярус | **родительский** столбец верхнего ряда | индекс в nested 0…2 | `SelectBottomColumn` / toggle-off |

`ApplySelection(path, toggle)` — единая точка входа из `WizardPreviewMapRenderer`.

### SingleTier (плоская сетка)

- Одна метка на корневом ярусе: `Rows[i].Marker = 1`.
- Подсветка: карточка `i` selected ⟺ `FindMarkedRow(root) == i`.
- Дефолт при инициализации engine: страница 0 → block `0`; остальные → block `1` (средний столбец / blend).

### Tiered (верх + дочерний ярус)

**Верхний ряд (3 карточки):**

- Метка: `root.Rows[topCol].Marker = 1`; остальные строки root → `0`.
- Клик: `SelectTopColumn(topCol)` → **сбрасывает** нижний выбор (`ClearBottomSelectionInternal`: `bottomRowActive = false`).
- Метки **всех** nested-ярусов очищаются (`ClearNestedMarkers`) — контур «выкл», пока пользователь не кликнет нижний ряд.

**Дочерний ярус (9 карточек в одном горизонтальном списке):**

- В ArN: у каждой строки root свой `Nested` с 3 пресетами; в UI они **схлопнуты** в один ряд:  
  `[col0₀,col0₁,col0₂, col1₀,col1₁,col1₂, col2₀,col2₁,col2₂]`.
- Плоский индекс `i` → `parentRow = i / nestedPerRow`, `childCol = i % nestedPerRow` (`nestedPerRow` обычно 3).
- Подсветка нижней карточки **только** если одновременно:
  - `parentRow == FindMarkedRow(root)` (выбран тот же верхний столбец);
  - `childCol == FindMarkedRow(root.Rows[parentRow].Nested)` (есть метка в nested выбранного столбца).
- Если верх выбран, но nested без метки — **ни одна** нижняя карточка не selected (смысл: «контур выкл» при выборе только гаммы).

**Клик по нижней карточке:**

- `toggle: true` на bottom; `PressedCallback` (не `CardPressed`) — надёжнее внутри `ScrollViewer`.
- Если `path.Row != _topColumn` — **игнор** (клик по пресету «чужого» столбца в прокрученной ленте не меняет выбор).
- Повторный клик по **уже выбранной** нижней карточке (`toggle && bottomActive && bottomCol совпадает`) → `ClearBottomSelection` (снять контур, верхний столбец остаётся).

### Дефолты выбора (инициализация и сброс вперёд)

| Поле | Страница 0 | Страницы 1…4 |
|------|------------|--------------|
| `_selectedBlockPerPage` | `0` | `1` |
| `_topColumnPerPage` | — | `1` (средний столбец) |
| `_bottomColumnPerPage` | — | `0` |
| `_bottomRowActivePerPage` | — | `false` |

При отсутствии метки на paired-странице после restore: fallback `topRow = 1`.

### Цепочка при клике (обязательный порядок)

```
CardPressed / PressedCallback
  → Engine.ApplySelection(path, toggle)
  → Select* (early return если выбор не изменился)
  → OnSelectionMutated:
       ReplayWorkingProfileUpTo(step)     // _working немедленно
       ResetForwardSelections(step + 1)   // дефолты engine для страниц вперёд
       SelectionChanged(step)
  → Window.OnEngineSelectionChanged:
       ApplyEngineToSlot(cache[step])     // метки текущей страницы
       InvalidateFrom(step + 1)           // null в кэше для step+1…
       SyncSelectionChrome()
       ScrollBottomRowToTopColumn()
```

**Не вызывать** `ConfigurePage` / полный rebuild текущей страницы на клике — только in-place метки + invalidate вперёд.

---

## Сброс выбора

### Локальный (смена верхнего столбца)

`SelectTopColumn` → `ClearBottomSelectionInternal` + `ClearNestedMarkers` в кэше при следующем `ApplyEngineToSlot`. Профиль preview: гамма нового столбца, **без** контура (`ApplyPseudoOff` в `BuildGammaContourChoiceProfile`).

### Toggle-off нижнего ряда

Повторный клик по активной нижней карточке → `bottomRowActive = false`, nested-метки сброшены. Ветка мастера без pseudo на экранах 2–3.

### Сброс страниц вперёд (смена выбора на шаге N)

Два параллельных действия — **оба обязательны** (регрессия, если оставить только одно):

| Действие | Что делает | Без этого |
|----------|------------|-----------|
| `InvalidateFrom(N+1)` | Обнуляет `Ar[N+1]…` в кэше | Старые bitmap и метки на Back/вперёд |
| `ResetForwardSelections(N+1)` | Дефолты в `_topColumnPerPage` и др. | При rebuild слота engine отдаёт **старый** выбор → selected на шаге N+1 не сбрасывается |

После invalidate следующий визит шага N+1 → `SlotBuilder.Build` → `ApplyEngineToSlot` с уже сброшенными дефолтами engine.

### Back vs смена выбора

| Навигация | `_working` | Кэш / метки |
|-----------|------------|-------------|
| **Back** | Не replay всей цепочки | Cache hit → bitmap + метки как при первом визите |
| **Смена выбора на текущем шаге** | `ReplayWorkingProfileUpTo` | Текущая страница: метки in-place; вперёд: invalidate + reset |

`GoBack`/`GoNext` вызывают `RestorePageSelection(step)` из **массивов engine**; затем `PopulateFromMap` уточняет из **меток кэша** — при cache hit метки каноничны для UI.

---

## Зависимость карточек в списках (Tiered)

```
Верх:  [G0] [G1●] [G2]     ← root.Marker на строке 1
                │
Низ:   … G1₀  G1₁●  G1₂ …   ← только nested строки parentRow=1; marker на G1₁
       └─ в ленте 9 карточек, видимы ~3 (окно scroll)
```

| Состояние | Верх selected | Низ selected |
|-----------|---------------|--------------|
| Только гамма | `root.Rows[k].Marker=1` | нет (`childMarked = -1`) |
| Гамма + контур | тот же `k` | `root.Rows[k].Nested.Rows[j].Marker=1` |
| Смена верхнего столбца | новый `k` | сброс nested; прокрутка к секции `k` |

Индексация нижних блоков в engine (`BottomPresetIndex(topCol, j) = 3 + topCol×3 + j`) совпадает с порядком листьев в `CollectDescendantPresets`.

---

## Прокрутка дочернего яруса

### Компоновка

- 9 карточек в `StackPanel` (horizontal) внутри `ScrollViewer`.
- **Ширина viewport** ряда: `3 × TextLayerFootprintWidth + 2 × CardSpacing` = **1004** px; `WizardCardListHostWidth` = **1016**; `WizardCardListRowHorizontalSpill` = **6** px с каждой стороны (одинаково у верхнего и нижнего яруса). `WizardCardListAreaHeight` = **412** (включая `WizardCardRowInteractiveShadowSpillBottom` **16** под hover/selection). Зазор `WizardChromeSectionGap` (**12** px) у разделителей.
- **Окно:** `WindowHeight` = **712** px. Инфо-блок сверху фиксирован под **4** строки hint (`WizardInfoBlockHeight` = **148**). Футер: `FooterChromeHeight` = **2×12 + 32** = **56** px (без `pad.Bottom`); кнопки по центру полосы, без растягивания при смене страниц. Нижний разделитель сдвинут вверх на `FooterDividerLift` (**12** px) через margin без смещения кнопок.
- `HorizontalScrollMode = Disabled` — пользователь **не** скроллит жестом; смещение только программно (`ChangeView`). Иначе scroll перехватывает pointer и ломаются клики по нижним карточкам.

### Формула смещения

После `UpdateLayout()`:

```
topMarked = FindMarkedRow(root);  если < 0 → 1
firstPreset = topMarked * nestedPerRow
offsetX = firstPreset * (TextLayerFootprintWidth + CardSpacing)   // 340 px на шаг
ScrollViewer.ChangeView(offsetX, null, null, disableAnimation: false)
```

Пример: `topMarked = 2`, `nestedPerRow = 3` → `firstPreset = 6` → сдвиг на 6×340 px к третьей тройке (G2₀…G2₂).

### Когда вызывается

| Момент | Метод |
|--------|--------|
| После reveal страницы | `RevealPopulatedPage` → `ScheduleScrollBottomRowToTopColumn` |
| После клика / смены выбора | `OnEngineSelectionChanged` → `ScrollBottomRowToTopColumn` |
| ScrollViewer ещё не Loaded | `Schedule…` подписывается на `Loaded`, затем один раз `TryScroll` |

**Важно:** скроллить по **`topMarked` из меток кэша**, не по «текущему» engine без sync — иначе после Back лента показывает не ту тройку.

### Клики в scroll

- Нижний ряд: `PressedCallback` + дедуп в `RaiseCardPressed` (~15 Hz).
- Верхний ряд: `CardPressed` event.
- `HostInListView = true`, shadow/translation обнулены — карточка в плотной строке мастера.

---

## Известные регрессии (не повторять)

| Симптом | Причина | Правило |
|---------|---------|---------|
| Все превью одинаковые | `SampleText`/`IsDarkTheme` в initializer **до** `SuppressPaintInputRepaint` → `RepaintAsync` с default `Profile` перезаписывает `SetPreviewBitmap` | Suppress **до** paint-DP; после `await` в `RepaintAsync` проверять `_batchPreviewApplied` |
| Selected на шаге N+1 не сбрасывается после смены на N | Только `InvalidateFrom`, без `ResetForwardSelections` | При `OnSelectionMutated` всегда **оба** |
| Selected «залипает» при Back, хотя кликали другое | UI из engine, кэш с другими метками | `Populate` → `RestoreEngineFromSlot`; клик → `ApplyEngineToSlot` |
| Дефолтная подсветка нижнего ряда при первом входе | Метки в nested при `Build` | Метки только на **root** при сборке; nested — после клика |
| Бублик ~1 с на Back при cache hit | `RevealPopulatedPage` после `await animTask` + всегда `showLoadingRing: true` | Cache hit: ring off, populate+reveal до/параллельно анимации |
| Клики по нижним карточкам не доходят | `ListView` / scroll consumer / только `CardPressed` | `StackPanel` + `PressedCallback`; scroll programmatic only |
| Прокрутка не к той тройке | offset от `_engine.TopColumn` без sync меток | `FindMarkedRow(root)` из слота кэша |
| Клик по G0₁ при выбранном G1 меняет контур | Нет guard `path.Row != _topColumn` | Игнорировать bottom-клик чужого parent row |

---

## Жизненный цикл кэша

```
Первый визит страницы N
  → SlotBuilder.Build (EnsurePageConfigured + snapshot → ArN)
  → RenderAllCellsAsync (STA, все листья рекурсивно)
  → SetPage(N)

Back на страницу N (cache hit)
  → TryGetCompletePage → PopulateFromMap (без re-render)
  → showLoadingRing: false, Reveal до конца анимации

Смена выбора на N
  → ReplayWorkingProfileUpTo(N) — немедленный откат _working
  → InvalidateFrom(N+1)
  → метки текущей страницы обновляются in-place

WM_THEMECHANGED (окно мастера)
  → CaptureAllPageMarkers (col2) → ClearAll
  → для страниц 0…StepIndex: SlotBuilder.Build → ApplyTo(метки) → RenderAllCellsAsync (новый IsDarkTheme)
  → PopulateFromMap текущей страницы; prompt — AppThemeVisualResolver.ResolveThemeBrush

GoNext
  → ApplyStepChoice (фиксация в _working)
  → step++
```

**Не делать:** полный `ConfigurePage` + replay всех предыдущих шагов при **Back** — bitmap берётся из кэша; `_working` на Back **не** откатывается глобально, только при смене выбора на текущем шаге.

---

## Отрисовка превью

1. **STA-поток** — `WizardPreviewRenderScheduler` (один фоновый поток с `[STAThread]`).
2. **Буфер** — `TextPreviewNative.PaintProfileToBuffer` → straight-alpha BGRA **без** opaque-композита (как hero на «Текст и шрифты»); размер **284×136**, карточка **316×168** (`GlassSettingsTextLayerPreviewOuterWidth/Height`).
3. **UI** — `WizardInkPreviewCard`: прозрачный фон карточки, ink поверх акрила окна; `SuppressPaintInputRepaint` **до** `SampleText`/`Profile`; затем `SetPreviewBitmap`. Иначе отложенный `RepaintAsync` с default-профилем перезапишет bitmap.

Пакетный режим: `SuppressLoadedRepaint`, `SetPreviewBitmap` → `_batchPreviewApplied`; после `await` в `RepaintAsync` проверка `_batchPreviewApplied`.

---

## Навигация и анимация

- Cross-slide 500 ms; при **cache hit** — без `ProgressRing`, `Populate` + `Reveal` до `await animTask`.
- При **cache miss** — ring до готовности render; populate параллельно анимации через `ContinueWith` + `DispatcherQueue`.
- Прокрутка дочернего яруса — см. раздел **«Прокрутка дочернего яруса»** выше.

---

## Engine: ключевые контракты

| API | Назначение |
|-----|------------|
| `ReplayWorkingProfileUpTo(step)` | Сброс `_working` от baseline + replay выборов `0..step-1` + preview текущего |
| `ApplySelection(path, toggle)` | Клик карточки → SelectBlock / TopRow / BottomRow |
| `SelectionChanged` | Window: invalidate forward + sync markers + chrome |
| `RestoreFlatSelection` / `RestorePairedSelection` | Из меток кэша без replay |

Парные страницы: `IsPairedPage` — шаги 1, 2 и 3 при `branchHasPseudo`.

---

## Anti-patterns

| Нельзя | Почему |
|--------|--------|
| Repaint карточек через DP после batch-bitmap | Гонка → одинаковые превью (см. регрессии) |
| Только `InvalidateFrom` без `ResetForwardSelections` | Selected на следующих шагах залипает |
| Инвалидировать текущую страницу при клике | Потеря cache hit на Back |
| Ставить дефолтные метки во вложенных ярусах при build | Ложный selected на nested |
| `ConfigurePage` при GoBack | Лишний rebuild; кэш уже содержит bitmap |
| Ручной scroll нижнего ряда (`HorizontalScrollMode.Enabled`) | Клики и offset расходятся с метками |
| Opaque-композит bitmap (`CompositeStraightAlphaOverBackground`) | Маскирует акрил; ink нельзя оценивать на стекле |
| GDI+ | Политика проекта |

Полный список симптомов и фиксов: раздел **«Известные регрессии»** выше.

---

## Расширение (новый экран / 3-й ярус)

1. Добавить ветку в `ConfigurePage` / builder пресетов engine.
2. В `WizardPreviewSlotBuilder` — строки ArN: `Preset`, при необходимости `Nested` на col1.
3. При новом ярусе — расширить `WizardPreviewMapRenderer.BuildTiered` (или рекурсивный builder UI).
4. Метки: root при build; nested — только после выбора пользователя.
5. `InvalidateFrom` при смене выбора сохраняет инвариант «впереди устаревшие пресеты».

См. также: [GlassSettings-development.md](GlassSettings-development.md), [GlassSettings-hosted-dialog.md](GlassSettings-hosted-dialog.md) (modal child window).
