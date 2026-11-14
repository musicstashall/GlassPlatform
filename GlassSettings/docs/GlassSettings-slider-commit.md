# GlassSettings — commit для слайдеров

Паттерн записи параметров слайдера в реестр. Режимы: **Deferred** (250 ms) и **LiveImmediate** (паритет `wxEVT_SLIDER` OpenGlassGUI).

См. также: [GlassSettings-notices.md](GlassSettings-notices.md) (defer notice), [GlassSettings-host-glass.md](GlassSettings-host-glass.md) (DWM notify).

---

## Режимы

| Режим | Когда | Реестр + notify | MarkDirty |
|-------|-------|-----------------|-----------|
| `Deferred` (default) | BWM, legacy DWM detail | debounce 250 ms или flush на release | на каждый commit |
| `LiveImmediate` | DWM live-слайдеры (Textures) | **каждый** `ValueChanged` через `WriteDwmDwordLivePreview` / `WriteDwmDwordOrDelete(..., livePreview: true)`; опционально `liveDebounceMs: 100` | **в конце жеста** (`onGestureCommitted`) |

LiveImmediate разделяет **preview** (реестр + `NotifyDwmColorizationNow` на Dwm) и **housekeeping** (notice, Обзор) — иначе thumb «липнет» из‑за `MarkDirty` на каждый tick.

---

## Правило DWM live-слайдеров

Для слайдеров, которые должны обновлять DWM в реальном времени (как `wxEVT_SLIDER` в OpenGlassGUI):

1. **Подключение** — только `SettingsSliderCommit.Attach(..., CommitMode.LiveImmediate)`, **не** сырой `ValueChanged`.
2. **Preview на каждый tick** — `WriteDwmDwordLivePreview` или `WriteDwmDwordOrDelete(..., livePreview: true)` в `onCommit`; реестр + немедленный notify **без** `MarkDirty`.
3. **Housekeeping в конце жеста** — `MarkDirty` в `onGestureCommitted`; notice (`InvalidateDismissOnMutation`) вызывается автоматически из `SettingsSliderCommit` при `PointerReleased`.
4. **Delete-when-default (OpenGlass)** — через `WriteDwmDwordOrDelete`: intensity `0`, parallax `13`, material opacity `0` → удаление ключа вместо DWORD.
5. **После `Reload()`** — `SettingsSliderCommit.SyncCommittedValue(slider, slider.Value)`.

---

## Контракт Deferred

| Событие | Действие |
|---------|----------|
| `PointerPressed` | `EnterSliderGesture`; UI — сразу; реестр — нет |
| `ValueChanged` | `onPreview`; debounce **250 ms** (`ConfigureAwait(false)`); `onCommit` — **`Task.Run`** |
| `PointerReleased` | flush + `ExitSliderGesture` |

---

## Контракт LiveImmediate

| Событие | Действие |
|---------|----------|
| `PointerPressed` | `EnterSliderGesture` |
| `ValueChanged` | `onPreview` + `onCommit` через **`Task.Run`** (live preview, без MarkDirty) |
| `PointerReleased` | `onGestureCommitted` (MarkDirty) + notice |

---

## API

```csharp
SettingsSliderCommit.Attach(
    slider,
    canCommit: () => !_loading,
    onPreview: _ => UpdateLabel(),
    onCommit: value => SettingsSession.Current.WriteDwmDwordOrDelete(
        "Key", (uint)value, v => v == 0, livePreview: true),
    SettingsSliderCommit.CommitMode.LiveImmediate,
    onGestureCommitted: () => SettingsSession.Current.MarkDirty(),
    liveDebounceMs: SettingsSliderCommit.LivePreviewDebounceMs);
```

После `Reload()`:

```csharp
SettingsSliderCommit.SyncCommittedValue(slider, slider.Value);
```

---

## Эталон: страница Textures

Файл: [`DwmThemeDetailPage.xaml.cs`](../../GlassSettings/Pages/Dwm/DwmThemeDetailPage.xaml.cs).

| Контрол | Подключение |
|---------|-------------|
| `IntensitySlider`, `ParallaxSlider`, `MaterialOpacitySlider` | `ConfigureSliderRows()` → `AttachGateSliders()` — `LiveImmediate` + `WriteDwmDwordOrDelete` |
| Opacity rows | `DwmOpacityGroupRow` — `UseOpenGlassOpacitySemantics = true` |

---

## Эталон: страница Colorization

Файл: [`DwmColorizationDetailPage.xaml.cs`](../../GlassSettings/Pages/Dwm/DwmColorizationDetailPage.xaml.cs).

| Контрол | Ключи | Подключение |
|---------|-------|-------------|
| Active / Secondary picker α | `GlassOpacity*`, `Colorization*Balance` | Альфа пикера ↔ balance/opacity; RGB — `ToDwordPreserveAlpha` |
| Blur level | `ColorizationBlurBalance(Override)` | `ActiveColorPanel.FooterContent` — `LiveImmediate`, commit при gate active color |
| Base opacity ×4 | `ColorizationOpacity*` | `DwmOpacityGroupRow` (`UseOpenGlassModeWithoutGate`) в `BaseOpacityTabHost` |

Glass type — `Pivot` + `GlassTypeTabHost` (active color + secondary). Pivot: collapse → animate → commit `GlassType`.

После reload слайдера blur — `SettingsSliderCommit.SyncCommittedValue`.

---

## Ширина слайдеров

Единый диапазон для всех `Slider` в GlassSettings:

| Параметр | Значение |
|----------|----------|
| MinWidth | **120** px |
| MaxWidth | **200** px |
| Фабрика | [`SettingsSliderFactory`](../../GlassSettings/Controls/SettingsSliderFactory.cs) |
| Ресурсы | `GlassSettingsSliderMinWidth`, `GlassSettingsSliderMaxWidth`, `GlassSettingsHubSliderWidth` (200) в [`App.xaml`](../../GlassSettings/App.xaml) |

- **Шапка / inline** — `SettingsSliderFactory.Create()` или `CreateSliderValueSuffixPanel()` (Width = MaxWidth + suffix, слайдер stretch внутри 120–200 px).

---

## Anti-patterns

- `WriteDwmDword` в `ValueChanged` без deferral/LiveImmediate split — лаг thumb + notice.
- `MarkDirty` / `InvalidateDismissOnMutation` на каждый tick live-слайдера.
- Синхронный `GlassRegistry.WriteDword` + native reload в `ColorLivePreview` на UI-потоке — tooltip/picker отстаёт от thumb; использовать [`LiveReloadScheduler`](../../GlassSettings/Core/Platform/LiveReloadScheduler.cs).
- Забыть `SyncCommittedValue` после `Reload()`.
- Менять `DwmOpacityGroupRow` глобально без `UseOpenGlassOpacitySemantics`.
