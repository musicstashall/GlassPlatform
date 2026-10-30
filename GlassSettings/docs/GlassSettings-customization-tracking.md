# GlassSettings — учёт кастомизации параметров

Подсистема определяет, какие настройки считаются **изменёнными** относительно состояния «по умолчанию», формирует подстроки на **Обзоре** и участвует в **baseline снимка** для unsaved / revert.

См. также: [GlassSettings-notices.md](GlassSettings-notices.md) (dirty, save, revert), [GlassSettings-hosted-dialog.md](GlassSettings-hosted-dialog.md) (gate + штора), [GlassSettings-host-glass.md](GlassSettings-host-glass.md) (live DWM и рассылка).

---

## Принцип

| Состояние | Критерий |
|-----------|----------|
| **По умолчанию** | DWORD-значение **отсутствует** в реестре (ключ не создан) |
| **Изменено** | DWORD-значение **присутствует** в реестре |

Сравнение с fallback (`ReadDwordFb`) **не используется** для счётчика Обзора.

Выключение gate **удаляет** ключи из реестра — не записывает «нулевой» DWORD.

---

## Baseline снимка (SettingsSnapshot)

Отдельно от счётчика Обзора — **снимок при открытии окна** и **обновление при «Сохранить»** для отката при закрытии без сохранения. **Полная политика:** раздел [«Политика snapshot реестра»](#политика-snapshot-реестра) ниже.

| Момент | Код |
|--------|-----|
| Старт окна | `SettingsSession.Capture()` — все BWM/DWM партиции |
| «Сохранить» | `SettingsSession.ApplyCommitScopeAsync` → `SettingsSnapshot.CaptureScope(scope)` синхронно |
| После запуска Glass | `ReloadDwmSnapshotBaseline()` — только DWM-партиции |
| Отслеживаемые ключи scope | `CommitScopeCatalog.GetTrackedBwmParameters` + `SyncTrackedBwmParameters` / `UpsertParameterSnapshot` |
| Группа спойлера | `FlyoutParameterGroup` → upsert только параметров группы |
| Закрытие окна | `SettingsSession.Revert()` при `HasUnsavedChanges` |
| Откат подразделов | `CreateSubKey` при восстановлении; `PruneOrphanSubKeyTrees` — удаление поддеревьев, отсутствующих в снимке |

---

## Политика snapshot реестра

**Источник истины** для baseline/dirty/revert. Изменения — только через этот раздел и код (`RegistryPaths`, `SettingsSnapshot`, `SettingsSession`).

### Когда снимается baseline

| Момент | Код |
|--------|-----|
| Старт окна (первая активация) | `SettingsSession.Capture()` |
| «Сохранить» | `ApplyCommitScopeAsync` → `CaptureScope` |
| После запуска платформы Glass | `ReloadDwmSnapshotBaseline()` |
| Смена темы (Параметры Windows / плитка GS / host policy) | `ThemeDwmBaselineSync`: suppress RegWatch → settle fingerprint → `ReloadDwmSnapshotBaselineAsync` (`MainWindow` / `VisualStylesPage`); без жёлтого Save |
| Закрытие без сохранения | `RevertOnExit()` / `Revert()` |

### Партиции полного capture

| Область | Партиция / путь |
|---------|-----------------|
| DWM | Colorization, Glass, Theme |
| BWM General | values-only на `GlassPlatform\` |
| BWM Root | `Root\` (рекурсивно) |
| BWM Popup | `Menu\`, `DropDown\`, `ToolTip\` |
| BWM Taskbar | `Tray\` |

### Исключения из snapshot

| Правило | Ключи | Поведение |
|---------|-------|-----------|
| Прямые дети `BwmRoot` | AttachList, BlockList, Crashes, GlassSettings | Не capture, не dirty, не revert |
| Сегмент **DisabledList** в любом поддереве BWM | `Menu\DisabledList`, корневой `DisabledList`, … | То же |
| Placement UI | `GlassSettings` | Filter при capture/revert |

### Кто что создаёт в реестре

| Ключ | Кто пишет | Назначение |
|------|-----------|------------|
| AttachList | GlassSettings «Процессы» | Белый список инъекции |
| BlockList | GlassSettings «Процессы» + `ProcessBlockList` | Чёрный список — без GlassCompat |
| DisabledList | Legacy BWM | Отключение эффектов per-process / per-part |
| Menu, Root, Tray, … | Страницы кастомизации | Параметры оформления |
| Crashes, GlassSettings | Служебные / placement | Вне baseline |

---

## Live-правки DWM (рассылка)

Любая запись в `HKCU\Software\Microsoft\Windows\DWM` из UI, которая должна сразу отразиться в OpenGlass / DWM, — через `SettingsSession` с **`liveDwm: true`**. Маршрутизация по ключу: [`DwmLiveReloadKeys`](../../GlassSettings/Core/Platform/DwmLiveReloadKeys.cs); см. [host-glass — Рассылка](GlassSettings-host-glass.md#рассылка-термин-в-документации-glasssettings).

| Параметры | Сообщение |
|-----------|-----------|
| `CustomThemeReflection`, `CustomThemeMaterial`, `CustomThemeAtlas` | `WM_THEMECHANGED` (`0x031A`) — сразу |
| Цвет, прозрачность, blur, прочие DWORD | `WM_DWMCOLORIZATIONCOLORCHANGED` (`0x0320`) |
| `Root\NoBackdrop` → вкл (0) + SWCA | `WM_DWMCOLORIZATIONCOLORCHANGED` (через `RootBackdropLiveReload`) |
| `Root\NoBackdrop` → выкл (1) | без рассылки DWM; `RootBackdropReapply \| GlassCore` |

| Метод | Рассылка |
|-------|----------|
| `WriteDwmDword(..., liveDwm: true)` | debounce 100 ms → colorization (ColorPicker и т.п.) |
| `WriteDwmDword(..., liveDwm: true, dwmNotifyImmediate: true)` | сразу → `WM_DWMCOLORIZATIONCOLORCHANGED` |
| `WriteDwmDwordLivePreview` | реестр + немедленный notify без `MarkDirty` (live-слайдеры) |
| `WriteDwmString` / `DeleteDwmValue(..., liveDwm: true)` | сразу → theme или colorization по ключу |

Транспорт: `SendNotifyMessage` на окно класса **Dwm** (паритет OpenGlassGUI), см. [`ReloadRenderService`](../../GlassSettings/Core/Platform/ReloadRenderService.cs).

Примеры: плитки Reflection/Material → `WriteDwmString` → `WM_THEMECHANGED`; intensity/parallax/material opacity → `WriteDwmDwordLivePreview`; reflection opacity rows (OpenGlass) → Auto=delete, From theme=`0xFFFFFFFF`.

---

## Код

| Компонент | Путь | Назначение |
|-----------|------|------------|
| `CustomizationAreaId` | `GlassSettings/Core/Customization/CustomizationAreaId.cs` | Идентификаторы разделов Обзора |
| `RegistryParameterRef` | `GlassSettings/Core/Customization/RegistryParameterRef.cs` | Ссылка на один параметр (BWM / DWM) |
| `CustomizationCatalog` | `GlassSettings/Core/Customization/CustomizationCatalog.cs` | Каталог ключей; `RemoveFlyoutSection`, `GetFlyoutSectionTrackedParameters` |
| `CustomizationTracker` | `GlassSettings/Core/Customization/CustomizationTracker.cs` | Подсчёт, `Describe()` |
| `SettingsSnapshot` | `GlassSettings/Core/Snapshot/SettingsSnapshot.cs` | Capture / Revert партиций |
| `CommitScopeCatalog` | `GlassSettings/Core/Snapshot/CommitScopeCatalog.cs` | Партиции и tracked-параметры по scope |
| `BwmRegistry` | `GlassSettings/Core/Registry/BwmRegistry.cs` | `DeleteValue` (без CreateSubKey), `DeleteSubKeyTree` |
| `FlyoutAnimRegistry` | `GlassSettings/Core/Flyout/FlyoutAnimRegistry.cs` | `DeleteAll` → удаление поддерева `…\Animation` |
| `FlyoutThemeRegistry` | `GlassSettings/Core/Flyout/FlyoutThemeRegistry.cs` | Gradient / border |
| `BwmSectionGate` | `GlassSettings/Core/Settings/BwmSectionGate.cs` | `IsAnyPresent` / `DeleteAll` (значения) |
| `RegistrySectionGate` | `GlassSettings/Core/Settings/RegistrySectionGate.cs` | Gate DWM hub |

---

## Разделы Обзора

| `CustomizationAreaId` | Реестр | Подключ |
|-----------------------|--------|---------|
| `General` | BWM, корень | `FlyoutKindDetailPage` (General) |
| `Root` | BWM | `NoBackdrop`, `Root\Active`, `Root\Inactive` |
| `Popup` | BWM | `Menu`, `DropDown`, `ToolTip` |
| `Taskbar` | BWM | `Tray` |
| `DwmColorization` / `DwmGlass` / `DwmTheme` | DWM | hub/detail |

**DWM-партиции (UI ↔ snapshot):**

| Партиция | Ключи (кратко) |
|----------|----------------|
| **Colorization** | цвета (`ColorizationColor`/`Override`), opacity/balance, opaque blend, `BlurDeviation`, `GlassOverrideAccent`, `GlassType` |
| **Glass** (Стекло) | `BlurOptimization`, `RoundRectRadius`, `UseDirect3DRendering` |
| **Theme** (Стилизация) | caption, borders, текстуры/отражения (`CustomTheme*`, `ColorizationGlassReflection*`, `MaterialOpacity`), `CustomThemeAtlas` |

Изменения на странице «Текстуры» (`DwmReflectionPickerPage`) и «Атласы» относятся к **Theme**, не Glass.

**Точечные индикаторы на detail:** строки/spoiler с `UnsavedScope = SettingsCommitScope.ForDwmKeys(...)` — см. [`DwmThemeScopeCatalog`](../../GlassSettings/Core/Snapshot/DwmThemeScopeCatalog.cs). Hub и Обзор по-прежнему используют `ForArea(DwmTheme)`.

---

## Gate и удаление (обязательные паттерны)

### Hub Popup — выключение целого раздела (Menu, DropDown, ToolTip)

Удаляется **всё поддерево** `HKCU\...\GlassPlatform\<Section>` (включая `Animation` и прочие подключи):

```csharp
CustomizationCatalog.RemoveFlyoutSection(regSub); // BwmRegistry.DeleteSubKeyTree
SettingsSession.Current.MarkScopeDirty(SettingsCommitScope.ForBwmSubKey(regSub));
```

С UI: штора → `ConfirmThenAsync` → процедура → `Pop` → `RefreshSessionNoticeProgrammatic`. Эталон: `PopupHubPage.OnGateToggledAsync`.

**Не** использовать поштучное `DeleteValue` по каталогу параметров для hub gate — остаются пустые подключи.

### Спойлер анимации (detail / taskbar)

```csharp
FlyoutAnimRegistry.DeleteAll(regSub); // DeleteSubKeyTree Menu\Animation (и аналоги)
SettingsSession.Current.MarkDirty();
```

`BwmRegistry.DeleteValue` **не создаёт** отсутствующий подключ (только `OpenSubKey`).

### Спойлер gradient / border

```csharp
FlyoutThemeRegistry.DeleteGradient(regSub);
FlyoutThemeRegistry.DeleteBorder(regSub);
SettingsSession.Current.MarkDirty();
```

### Спойлер углы

```csharp
FlyoutCornerRegistry.DeleteAll(regSub);
SettingsSession.Current.MarkDirty();
```

### DWM hub gate

```csharp
RegistrySectionGate.DeleteAll(dwmKeys);
SettingsSession.Current.MarkDirty();
```

Эталон: `DwmSettingsHubPage`.

---

## Подстроки на Обзоре

Тексты — только из `GlassSettings/Strings/*/Resources.resw` (`Custom_*`). См. [GlassSettings-localization.md](GlassSettings-localization.md).

---

## Исключения DWM из snapshot

Capture HKCU `Software\Microsoft\Windows\DWM` — **полное перечисление value** минус [`DwmSnapshotExclusions.DwmNeverSnapshotValues`](../../GlassSettings/Core/Snapshot/DwmSnapshotExclusions.cs). Список синхронизировать с этим разделом.

| Ключ | Кто пишет | Почему исключён |
|------|-----------|-----------------|
| `AccentColor` | Windows (персонализация) | системный |
| `AlwaysHibernateThumbnails` | Windows | системный |
| `ColorizationGlassAttribute` | Windows DWM | не OpenGlass README |
| `ColorizationColor` | Windows (акцент / DWM) | исключён из baseline capture (политика), но участвует в UI-индикаторах/подсчётах через каталоги colorization |
| `ColorPrevalence` | Windows («акцент на заголовках») | системный |
| `Composition` | Windows | системный |
| `EnableAeroPeek` | Windows | системный |
| `EnableWindowColorization` | Windows | системный |

Включаемые ключи OpenGlass — по [OpenGlass README (registry reference)](https://github.com/ALTaleX531/OpenGlass?tab=readme-ov-file#registry-reference); новые ключи из релиза попадают в snapshot автоматически (full enum).

**Startup sync / baseline Root:** `SettingsSession.SyncRootBackdropBaseline()` после автополитики OpenGlass — только без `HasUnsavedChanges`. См. [GlassSettings-host-glass.md](GlassSettings-host-glass.md).

Правило расширения: новый **системный** ключ → `DwmNeverSnapshotValues` + строка в таблицу; ключ OpenGlass → **не** в исключения.

**Зеркало primary/Override** (OpenGlassGUI): [`DwmMirrorRegistry`](../../GlassSettings/Core/Dwm/DwmMirrorRegistry.cs) — чтение/запись effective key; `ColorizationColor` остаётся в snapshot-исключениях, но пара `ColorizationColor + ColorizationColorOverride` учитывается в hub/overview/scope-логике. UI-правка primary без Override — `PrimeDwmKeysFromRegistry` до записи.

---

## Расширение каталога

1. Добавить ключи в `CustomizationCatalog.GetParameters`.
2. Добавить в `CommitScopeCatalog.GetTrackedBwmParameters` при необходимости для snapshot scope.
3. При выключении gate — удалять те же ключи / поддеревья, что и в UI.
4. Проверить Обзор и revert (изменить → сохранить → удалить gate → закрыть окно без сохранения).

---

## Связанные документы

- [GlassSettings-detail-cards.md](GlassSettings-detail-cards.md) — gate-спойлеры
- [GlassSettings-host-glass.md](GlassSettings-host-glass.md) — live reload
- [GlassSettings-notices.md](GlassSettings-notices.md) — notice после gate
