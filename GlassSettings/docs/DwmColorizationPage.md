# GlassSettings — страница Colorization (паритет OpenGlassGUI «Glass colors»)

Страница [`DwmColorizationDetailPage`](../../GlassSettings/Pages/Dwm/DwmColorizationDetailPage.xaml) воспроизводит вкладку **Glass colors** из OpenGlassGUI. Источник истины — `MainFrame.Tabs.cpp::CreateGlassColorsTab` и `MainFrame.State.cpp::UpdateUIVisibility`, семантика ключей — [OpenGlass README](https://github.com/ALTaleX531/OpenGlass).

**Page operation pipeline:** эталонная реализация serial-конвейера — [GlassSettings-page-pipeline.md](GlassSettings-page-pipeline.md) (GlassType pivot, composition toggles, master OFF).

## Структура (сверху вниз)

| Секция | Контрол | Ключ(и) |
|--------|---------|---------|
| Верхний блок «Композиция» | hero-layout: миниатюра слева (вне карточки) + карточка контролов справа | live-миниатюра слоёв + `GlassOverrideAccent`, `ColorizationOpaqueBlend` |
| Glass type | `Pivot` (Vista / Win7) + `GlassTypeTabHost` | `GlassType` |
| Цвет стекла (активный) | `ActiveColorExpander` в TabHost; `FooterContent`: **Blur radius** (Vista+Win7) → **Blur balance** (только Win7) | `ColorizationColor(Override)` + Vista: `GlassOpacity` / Win7: `ColorizationColorBalance(Override)`, `ColorizationBlurBalance(Override)` + `BlurDeviation` (gate OFF delete; «По умолчанию» — write 30 только если ключ уже есть) |
| Вторичный цвет | `SecondaryColorExpander` в TabHost | Vista: `ColorizationColorInactive` + `GlassOpacityInactive`; Win7: `ColorizationAfterglow(Override)` + `ColorizationAfterglowBalance(Override)` |
| Advanced (разделитель-спойлер) | `SettingsDividerExpander` | `ColorizationBase*`, приоритет базового цвета (`ColorizationOpaqueBlendPriority`), `ColorizationOpacity*` |

Верхний control-stack (справа от превью) фиксирован в порядке:
1) `Override accent`, 2) `Непрозрачно`.

**Blur radius** (`BlurDeviation`) — в footer спойлера active color; **не** часть master-карточки. Подчиняется gate/«По умолчанию» спойлера (enable: gate ON + `!D3D`).

Адаптив верхнего блока:

- сжимается колонка контролов карточки в диапазоне **120–200 px** (метрики `GlassSettingsSliderMetrics`);
- после достижения минимума (`120`) карточка контролов переносится на следующую строку под превью;
- при расширении возвращается рядом с превью по симметричному порогу.

При смене вкладки Glass type — через [`RunSequenceAsync`](GlassSettings-page-pipeline.md#эталон-glasstype-pivot-dwmcolorizationdetailpage): Animating (pivot + preview crossfade) → RegistryMutation (`CommitGlassTypeFromPivot`, reload) → Cosmetic → NoticeSync. Programmatic sync pivot при `Reload()` (`SyncGlassTypePivotFromRegistry`) **не** коммитит `GlassType`: `_loading` + `_lastGlassTypePivotIndex` до `SelectedIndex`.

## Правило: «По умолчанию» vs gate

- **Gate OFF** — **удаляет** `*Override` active/afterglow (Win7) / Vista `GlassOpacity`+Override и **`BlurDeviation`** (если есть). Платформа и GS синхронизируют popup Menu gradient в **`ColorizationColor`** (Primary), пока `ColorizationColorOverride` absent. После записи — reload OG.
- **Gate ON** — **создаёт** Override (`Apply*Defaults`); дальше только update существующих Override. **`BlurDeviation` при ON не создаёт** (только пользовательский слайдер).
- **«По умолчанию»** на спойлерах с пикером — **записывает** OG-дефолты (`Apply*Defaults`, чёрный цвет + balance); gate остаётся ON. См. `DwmColorRegistry.Apply*Defaults`. Для **`BlurDeviation`**: писать default **30** только если ключ уже есть; absent не создавать (`ApplyBlurDeviationDefaultIfPresent`).

### Три семантики (не смешивать)

| Ситуация | Реестр | Пикер / превью | Запись |
|----------|--------|----------------|--------|
| **Gate OFF** | нет `*Override`; `ColorizationColor` = popup gradient | factual absent в пикере (RGB `0`) / OG effective из Primary | Primary sync (host/GS), не Override |
| **Gate ON** | `*Override` present | из реестра / live picker | по действию пользователя |
| **Absent keys** (legacy) | ключей нет | OG factual defaults | нет (до Primary sync) |

### Смена темы / accent

**Gate:** [`ShouldApplyDwmCompositionPolicy`](../../GlassShared/src/OpenGlassDwmPolicy.cpp) — pipeline/модуль OpenGlass **или** зарегистрированы служба/задача OG. Иначе **никаких** записей/delete в `HKCU\...\DWM` (в т.ч. inactive reset).

**Запись в реестр** (delete Override → Primary sync → update Override / afterglow←accent):

- **Внешняя смена темы** (Параметры Windows и т.п.) — только **GlassCompat host** ([`EntryRundll.cpp`](../../GlassCompat/src/EntryRundll.cpp): `WM_THEMECHANGED`, `ImmersiveColorSet`, `WM_DWMCOLORIZATIONCOLORCHANGED` → debounce 300 ms → `ApplyDwmPolicyOnVisualStyleChange`, жёсткий Phase1).
- **Плитка GS** — native policy только если host **не** Running; иначе host = writer.

**GlassSettings** при открытом окне — **reader**: [`DesktopEnvironmentWatcher`](../../GlassSettings/Services/DesktopEnvironmentWatcher.cs) (`ThemeChanged` / `AccentChanged`) → [`ThemeDwmBaselineSync`](../../GlassSettings/Services/ThemeDwmBaselineSync.cs) (suppress + fingerprint settle + baseline) + [`DwmColorizationDetailPage`](../../GlassSettings/Pages/Dwm/DwmColorizationDetailPage.xaml.cs) `Reload()` пикеров. Не поднимать жёлтое «Сохранить» на промежуточный drift.

Порядок фаз policy (строго): **1)** theme-branch / visual-style → удалить `ColorizationColorOverride` (+ Vista inactive); Menu-ключ по `COLOR_WINDOW` (`IsRegistryDarkModeFromColorWindow`); **2)** Primary sync `ColorizationColor` ← Menu gradient; **3)** update существующих Override / afterglow. Один `NotifyDwmColorizationChanged` после всех фаз.

### Vista: gate OFF

| Состояние реестра | Gate | Пикер | Tint миниатюры |
|-------------------|:----:|-------|----------------|
| Нет `ColorizationColorOverride` | OFF | OG factual / Primary | compositor |
| `ColorizationColorOverride` present | ON | из реестра | live picker |

### Win7: gate OFF vs gate ON (UI и превью)

| Состояние реестра | Gate | Пикер / превью |
|-------------------|:----:|----------------|
| Нет active `*Override` | OFF | Factual absent: RGB `0`, balance `0` |
| `*Override` present | ON | Из реестра / live picker |

**Gate state:** `Is*GateOn` = наличие соответствующих Override-ключей (`Win7GlassColorPolicy` / `VistaGlassColorPolicy`).

При **AccentChanged** / **ThemeChanged**: Primary sync + theme-branch drop `ColorizationColorOverride`; Win7 `ColorizationAfterglowOverride` (если есть) → system accent.

## Правило: видимость по `GlassType`

`GlassType`: `0x0` = Vista Aero (default), `0x1` = Windows 7 Aero.

| Элемент | Vista (0) | Win7 (1) |
|--------|:--------:|:--------:|
| Active color title | «… и параметры размытия» | «… и параметры размытия» |
| Secondary color | Inactive (gate + picker) | Afterglow (gate + picker) |
| Альфа пикера active | `GlassOpacity` | `ColorizationColorBalance` |
| Альфа пикера secondary | `GlassOpacityInactive` | `ColorizationAfterglowBalance` |
| Blur radius (footer) | `BlurDeviation` (radius↔deviation), enable = gate ON + `!D3D` | то же |
| Blur balance (footer) | скрыт | после радиуса; `Dwm_BlurLevelTitle`; enable = gate ON |

### Два параметра blur (не путать)

| UI | Реестр | Режим | Назначение |
|----|--------|-------|------------|
| **Blur radius** (footer Active color, первый) | `BlurDeviation` | Vista + Win7 | Сила **гауссова** размытия фона |
| **Blur balance** (footer Active color, второй) | `ColorizationBlurBalance(Override)` | **только Win7** | Вклад уже размытого backdrop в Aero shader; **не** радиус гаусса |

Tooltip слайдера blur balance: `Dwm_BlurBalanceDescription`.

## Win7 Aero shader (превью и реальный композит)

Формула OpenGlass (`AeroColorizationEffect.hlsl`, pre-scale в `GlassRenderer.cpp`):

```
luminance = dot(gaussian, gray)          // до blurBalance
result = blurBalance * gaussian(wallpaper)
result = luminance * afterglow + result
result = color + result
```

- `blurBalance`, `colorBalance`, `afterglowBalance` — 0..100 → /100.
- Гаусс всегда на полную силу по `BlurDeviation`; blur balance только масштабирует вклад размытого слоя.
- При `blurBalance = 0` и `afterglowBalance = 0` — `IsGlassFullyOpaque`: backdrop blur не рисуется; область стекла остаётся opaque (solid tint поверх нулевого blur).

Blur-слой **каскадного превью** ([`Win7AeroPreviewCompositor`](../../GlassSettings/Core/Dwm/Win7AeroPreviewCompositor.cs)): **opaque** BGRA (`alpha = 255`); при `blurBalance = 0` — чёрный прямоугольник (вклад backdrop = 0), не просвет обоев; при росте balance — linear `gaussian × balance`. **Glow** — `BuildAfterglowStage`: **`(1 − luminance(gaussian)) × afterglow`**, premultiplied BGRA (OG literal — `luminance × afterglow`; в превью — perceptual parity: **темнее фон → ярче glow**, белый фон → прозрачно); при `afterglowBalance = 0` — прозрачный слой. **Tint** — flat brush поверх glow (`ColorizationColorBalance` маскирует свечение, без spatial map).

## `Blur radius` (донорская конверсия)

Паритет OpenGlassGUI (`MainFrame.Core.cpp`):

- `radius -> deviation`: `deviation = clamp((radius * 10 + 2) / 3, 0..100)`
- `deviation -> radius`: `radius = clamp((deviation * 3 + 5) / 10, 0..30)`
- значение `deviation = 30` считается дефолтом и удаляется из реестра (`DeleteDwmValue*`), а не записывается.

### Ограничение `UseDirect3DRendering=1`

- верхний `Blur radius` блокируется (`IsEnabled = false`);
- в UI принудительно показывается радиус `3` (как в доноре).

## Протокол RGB / альфа пикера

Два независимых канала (паритет OpenGlass `colorToDwordIgnoreAlpha`):

- **Запись цвета** — только RGB/BGR через `DwmMirrorRegistry.ToDwordPreserveAlpha`; байт альфы в DWORD цвета не перезаписывается из пикера.
- **Альфа-слайдер пикера** — отдельный ключ opacity/balance (0–100), линейный масштаб `percent = round(alpha × 100 / 255)`.
- **Загрузка** — RGB из цветового DWORD; альфа пикера из balance/opacity (не из alpha-byte DWORD).

При gate ON — запись только RGB; balance/opacity пишется при изменении пользователем. При gate OFF — Vista: **запись SWCA** preset (`ColorizationColor` + `GlassOpacity`); Win7: SWCA BGR + balance **35**, blur **50**, afterglow **20**.

**Read vs write defaults:** UI и каскадное превью при **отсутствии** ключей читают OG factual (`Win7GlassColorPolicy.ReadFactual*`, `VistaGlassColorPolicy.ReadOgFactual*`) — без подстановки write-defaults GlassSettings (black, blur 50 и т.д.). Запись preset — только gate OFF и «По умолчанию» (`ApplyActiveGateOff*`, `DwmColorRegistry.ApplyActiveColorDefaults`).

## Advanced: base color / base opacity

- **Base color:** Pivot (Transparent / Maximized / Opaque) → `BaseColorTabHost` → expander + picker, mode в header; при «Авто»/«Из темы» expander визуально disabled (`IsExpansionEnabled=false`).
- **Base opacity:** Pivot (Обычное / Развёрнутое окно) → `BaseOpacityTabHost` → два [`DwmOpacityGroupRow`](../../GlassSettings/Controls/DwmOpacityGroupRow.xaml) (`UseOpenGlassModeWithoutGate`).

## Матрица «По умолчанию»

| Секция | Действие |
|--------|----------|
| Active color | Gate OFF: clear Overrides (+ Vista `GlassOpacity`) + delete `BlurDeviation` if present; «По умолчанию» (gate ON): OG black + balances + `BlurDeviation`=30 **только если ключ уже был** |
| Secondary (Vista) | Gate ON: armed без записи (как afterglow Win7); gate OFF: `DeleteDwmValue` inactive + `GlassOpacityInactive`; «По умолчанию»: OG defaults |
| Secondary (Win7) | Gate OFF: accent + afterglow balance **20**; «По умолчанию»: OG black + afterglow balance |
| Blur radius | Gate OFF → delete; «По умолчанию» → write 30 if present; master OFF карточки **не** трогает |
| Base color (вкладка) | `DeleteDwmValue` текущего `ColorizationBase*` |

## Правило: слайдеры и пикеры (live-preview)

Паритет [`DwmThemeDetailPage`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs) (Интенсивность / Параллакс) и пикеров на этой странице:

- `SettingsSliderCommit.Attach(..., CommitMode.LiveImmediate)`; на странице «Композиция» для **Blur radius** и **Blur balance** — `liveDebounceMs: 100` (`SettingsSliderCommit.LivePreviewDebounceMs`).
- Debounced live: после 100 ms на **UI-потоке** перечитывается `Slider.Value` и `canCommit`, затем `Task.Run` → `Write*LivePreview` / `WriteBalancePairLivePreview` + `NotifyDwmColorizationNow` (без `MarkDirty`).
- Пикеры: `UseLivePreview` + `ColorLivePreview` — gate/значение на UI, реестр в `Task.Run`; `RefreshCompositionPreview(LiveSlider)` сразу (тинт/glow без blur); `MarkDirty` — в конце жеста (`ColorChanged` / `Commit*Gesture`).
- Blur radius: `CommitBlurRadiusGesture` + `LostFocus` — финальный flush и `MarkDirty` (в т.ч. ввод с клавиатуры без pointer-gesture); `CancelExternalPreviewRefresh` перед commit, иначе deferred `RefreshCompositionPreviewFromRegistry` откатывает миниатюру.
- Blur balance: `CommitBlurBalanceGesture` + `LostFocus` — финальный flush перед `MarkDirty` (как blur radius), иначе `RefreshCompositionPreviewFromRegistry` откатывает миниатюру к старому реестру.
- См. [GlassSettings-slider-commit.md](GlassSettings-slider-commit.md).

## Каскадное live-превью (верхний блок)

- Контрол: два [`DwmCompositionCascadePreview`](../../GlassSettings/Controls/DwmCompositionCascadePreview.xaml) в `CompositionPreviewHost` на странице + [`UiElementCrossfade`](../../GlassSettings/Controls/UiElementCrossfade.cs) (**500 ms** = `PivotTabContentTransition.TabChangeTotalDuration`, только при смене вкладки `GlassType` pivot Vista↔Win7; crossfade **параллелен** slide/fade контента pivot в фазе Animating; перед crossfade — `RefreshCompositionPreviewStatesFromRegistry`; слайдеры/gate/reload — без crossfade, только заливки).
- **Vista** (3 слоя): `Wallpaper` → `Blur` → `Tint` — tint через [`VistaGlassPreviewCompositor`](../../GlassSettings/Core/Dwm/VistaGlassPreviewCompositor.cs) (`BuildActiveTintArgb` / `BuildInactiveTintArgb`). Без inactive: один tint = blur − **Inset (24 px)** справа/снизу (тот же шаг каскада, что обои→blur; не inset×3).
- **Vista inactive split** (gate ON / ключ в реестре): общие обои + цельный blur; два независимых tint-rect (`CornerRadius=6`, `ThemeShadow`); ширина каждого = `tintWidth/2 + 20 px`; высота = `blurHeight − 48 px` (`Inset × 2`); active сверху blur; inactive: `Y = +24 px` от верха blur, правая кромка = `blurWidth − 24 px`; подписи снизу — «Тинт активного» / «Тинт неактивного». **Триггер split:** live (`BuildVistaPreviewState`) — `IsInactiveGateOn()` (armed или ключ); load/pivot (`RefreshCompositionPreviewStatesFromRegistry`) — только наличие ключа `ColorizationColorInactive`. **Gate inactive ON/OFF** — [`ApplyInactiveSplitLayout`](../../GlassSettings/Controls/DwmCompositionCascadePreview.xaml.cs): только split-host + tint (snap, без 150 ms fade); обои/blur pipeline не пересобираются.
- **Win7** (4 слоя): `Wallpaper` → `Blur` → `Glow` → `Tint` (afterglow под tint, как в Aero shader).
- Blur-слой: sharp-кэш обоев (async, `kernelRadius=0`) + sync `ApplyBlurLayerSync` на tick `ApplyState` (кроме скрытого каскада при drag слайдера — см. ниже). **Смена обоев** (identity): фоновый pipeline sharp→gaussian→blur/glow bitmap в `Task.Run`, старые слои остаются видимыми до готовности; в **одном** [`ApplyPipelineSnapshot`](../../GlassSettings/Controls/DwmCompositionCascadePreview.xaml.cs) — crossfade blur → glow → `DisplayWallpaper` **500 ms** ([`WallpaperPreviewCrossfade.DefaultCrossfadeDurationMs`](../../GlassSettings/Controls/WallpaperPreviewCrossfade.cs) + [`UiElementCrossfade`](../../GlassSettings/Controls/UiElementCrossfade.cs)); первый показ — без анимации; слайдеры blur radius/balance — только front brush, без crossfade. Подробнее: [GlassSettings-wallpaper-preview-crossfade.md](GlassSettings-wallpaper-preview-crossfade.md).
- **Deferred blur (скрытый каскад):** при drag **Blur radius** / **Blur balance** видимый каскад пересчитывает blur на лету; скрытый — только tint/glow/wallpaper (`skipBlurRecompute`, crossfade обоев без blur/glow); полный blur — на отпускании thumb (`Commit*Gesture` / `LostFocus`, scope `Full`).
- **Win7 compositor** (`Win7AeroPreviewCompositor`):
  - **Blur** — `gaussian × (balance / 100)` linear, opaque BGRA; gaussian из sharp-кэша по обои+radius; balance = 0 → чёрный opaque (backdrop = 0);
  - **Glow** — `(1 − dot(gaussian, gray)) × afterglow` (afterglow RGB уже × balance); luminance из **полного** gaussian до blurBalance; premultiplied BGRA; balance = 0 → **прозрачный**; `ImageBrush`, не flat picker;
  - **Tint** — flat `SolidColorBrush` = active color RGB × alpha из `ColorizationColorBalance` (поверх glow; additive step в OG, в UI — alpha-over).
- Страница передаёт raw BGR + balance percents (как `GlassRenderer` pre-scale), не ARGB пикера.
- Live-синхронизация: `GlassType`, active/afterglow BGR + balances, `ColorizationOpaqueBlend`, `GlassOverrideAccent`, `BlurDeviation`, `ColorizationBlurBalance`.
- Подписи слоёв не наследуют opacity/tint визуального содержимого.
- Ветка HKLM/HKCU conflict-notice — intentional warning.

### Внешние обновления превью

- Подписка построена единым паттерном `DwmThemeDetailPage`: `Hook/Unhook` в `Loaded/Unloaded`, `DesktopEnvironmentWatcher` + debounce timers.
- `WallpaperChanged` → `RefreshWallpaperLayer` на каскаде (identity обоев, не только path); blur/glow + обои — sync snapshot + crossfade **500 ms**, без полного reload и без flash пустого blur.
- `ThemeChanged` делает отложенный `Reload()` страницы.
- Внешние изменения DWM (через snapshot-watch → `SettingsSession.DirtyChanged`) обновляют превью от реестра без ручного poll.

## Индикаторы несохранённых изменений

- Спойлеры — scope по активной вкладке Glass type (`ActiveColorWin7Scope` / `ActiveColorVistaScope` в `DwmColorizationScopeCatalog`), включая **`BlurDeviation`**. Ключи верхней карточки (`GlassOverrideAccent`, `ColorizationOpaqueBlend`) — отдельные точки на строках (`CompositionCardScope`).
- Карточка «Композиция»: строки accent / opaque — точка по своему ключу; master-тумблер OFF удаляет **только** эти два ключа. **`BlurDeviation` master не трогает** (радиус ушёл в спойлер). **Master ON:** `armed` **или** хотя бы один ключ карточки в реестре (`_compositionMasterArmed` / `IsCompositionCardActive()`); armed без записи — как gate ON у спойлеров.
- Dirty-tracking для delete-on-default ключей: **absent ≡ default DWORD** (напр. `GlassOverrideAccent=0` в snapshot и absent после OFF считаются равными; паритет OpenGlassGUI). Тумблеры composition (`WriteCompositionBoolToggle`) после OFF вызывают `RefreshSessionNoticeProgrammatic()` — session notice гаснет при чистом scope; `ReloadCompositionMaster` не re-enter master-handler (`_loading`).
- Вкладка Glass type — точка **только** при несохранённом ключе `GlassType`, на вкладке с **текущим** значением (Vista = ключ отсутствует/0, Win7 = `1`). После Apply/Revert — как у спойлеров.

## Чек-лист ручной проверки

- Верхний блок: миниатюра располагается вне карточки (как в `DwmThemeDetailPage`), карточка контролов (accent / opaque) стоит справа и переносится вниз только после сжатия колонки контролов до min.
- Blur radius в footer спойлера: master OFF **не** трогает; gate OFF **удаляет**; «По умолчанию» пишет 30 **только если ключ уже был**; enable при gate ON + `!D3D`.
- В превью Win7: `Wallpaper`, `Blur`, `Glow`, `Tint` (glow под tint); Vista — без `Glow`.
- Подписи слоёв остаются читаемыми и не меняют прозрачность вместе со слоем.
- Win7 blur balance = 0 + чёрный active color → тёмное/чёрное стекло в миниатюре (паритет shader @ 0).
- Win7 blur balance 10–50%: blur-слой светлее linear (preview gamma), tooltip показывает реальный %.
- Непрерывный drag **Blur radius** и **Blur balance** (Win7): blur-слой **видимого** каскада следует за thumb; скрытый догоняет после отпускания.
- Vista inactive split: gate ON (armed) → сразу два tint в live-превью; после pivot/reload — split только при ключе `ColorizationColorInactive` в реестре; inactive rect ниже active на **24 px**; live-picker inactive — при gate ON.
- Win7 glow: darkWeight-map afterglow (темнее фон обоев → ярче); tint поверх glow маскирует через color balance.
- `Непрозрачно` больше не размещается в header `ActiveColorExpander`.
- `Blur radius` live-пишет `BlurDeviation`; при `UseDirect3DRendering=1` слайдер заблокирован и показывает `3`.
- `Override accent` live-пишет/удаляет `GlassOverrideAccent`.
- При изменениях цвета/opacity/secondary/blur balance слои миниатюры обновляются без перезагрузки страницы.
- Кейс `ColorizationColor` (без `Override`) отражается в hub-override бейдже и в подсчёте кастомизации раздела «Композиция».
