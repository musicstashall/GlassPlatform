# GlassSettings — host GlassCore и live reload

Host-процесс `GlassSettings.exe` загружает **только GlassCore** локально — для превью текста (мастер ink), live reload в инжектированные процессы и вспомогательных native-операций. **WndCompat не используется:** WinUI 3 не создаёт Win32-окон, требующих attach/subclass.

Это **не** инжекция платформы в чужие процессы: `GlassSettings.exe` в [`ProcessBlockList`](../../GlassShared/src/ProcessBlockList.cpp) (GlassCompat не инжектирует сам себя).

См. также: [GlassSettings-WinUI.md](GlassSettings-WinUI.md), [GlassSettings-development.md](GlassSettings-development.md), [glasscore-text.md](../../GlassCore/docs/glasscore-text.md).

---

## Native runtime (`GlassSettings.Native.dll`)

| Экспорт | Назначение |
|---------|------------|
| `GlassSettings_LoadGlassCoreDll` | `LoadLibrary(GlassCore.dll)` + `GlassCore_HostUiEnsureEngine` |
| `GlassSettings_EnsureGlassCoreRuntime` | то же, что `LoadGlassCoreDll` (раньше — с WndCompat; удалено) |
| `GlassSettings_ReapplyOpenRootBackdropsIfEngineLoaded` | `GlassCore_ReapplyOpenRootBackdrops` если движок уже в процессе |
| `GlassSettings_PostLiveRootBackdropReapplyAllIfEngineLoaded` | live backdrop по всем `g_rootWindows` + `RootLiveBackdropAll` в платформе |
| `GlassSettings_PostLiveRootBackdropReapplyAppliedIfEngineLoaded` | live backdrop: `g_rootWindows` ∩ `DidGlassApplyRootBackdrop` (SetProp) |
| `GlassSettings_PostLiveRootColorizationToAppliedRootsIfEngineLoaded` | live composition: `WM_DWMCOLORIZATIONCHANGED` + `lParam` scope (Active/Inactive) |
| `GlassSettings_InvalidateThemedChromeOnTrackedRootsIfEngineLoaded` | DrawTheme alpha live: reload registry + soft-invalidate chrome |
| `GlassSettings_InvalidateTextGlowClientsOnTrackedRootsIfEngineLoaded` | InkGlow live: `GlassCore_InvalidateTextGlowClientsOnTrackedRoots` (Moderate/Global, defer через root subclass) |
| `GlassSettings_ReloadRenderTextInkFromRegistryIfEngineLoaded` | `GlassCore_ReloadRenderTextInkFromRegistry` — GlobalTextGlow / ink globals |
| `GlassSettings_IsGlassRuntimeLoaded` | `true`, если в процессе загружен GlassCore |
| `GlassSettings_ShutdownGlassRuntime` | `GlassCore_HostExitShutdown` / `GlassCore_Shutdown` → flush → `FreeLibrary` |
| `GlassSettings_PickExecutableFile` | `IFileOpenDialog` (Win32) без host-glass |

C#-обёртка: [`NativeHostInterop.cs`](../../GlassSettings/Services/NativeHostInterop.cs).

### Жизненный цикл

1. **Старт** — [`MainWindow.xaml.cs`](../../GlassSettings/MainWindow.xaml.cs): после готовности shell — `NativeHostInterop.EnsureGlassRuntime()` (только GlassCore).
2. **Shutdown** — hide-on-close → `PrepareForProcessExit` → `ShutdownGlassRuntime()` (если runtime загружен) или natural close.

Порядок native shutdown: drain + `WM_NULL` → `GlassCore_HostExitShutdown` (IAT + короткий join анимации) → `FreeLibrary(GlassCore)`.

При закрытии с несохранёнными изменениями (`MainWindow.RunAppCloseSequenceAsync`): на **UI-потоке** `CaptureRevertOnExitPlan()` → в фоне `RevertSnapshotOnly()` → снова на UI `ApplyRevertOnExitNotifications(plan)`:

| Что откатывалось | Действие |
|------------------|----------|
| **DWM** (любая партиция) | `NotifyThemeChangedWithFallback` / `NotifyDwmColorizationChangedWithFallback` — `FindWindow(Dwm)`; при `hwnd==0` → `HWND_BROADCAST` ([`DwmLiveReloadKeys`](../../GlassSettings/Core/Platform/DwmLiveReloadKeys.cs)) |
| **BWM / платформа Glass** (любая партиция) | Массовая рассылка `HWND_BROADCAST` + `WM_DWMCOLORIZATIONCOLORCHANGED` (`NotifyDwmColorizationChangedBroadcast`) |
| **Root** (`\NoBackdrop` и др.) | `NotifyRootNoBackdropChanged` → `PublishRootBackdropMode` (при смене mode → `SignalReloadRender` в хосте) + `NotifyDwmColorizationChanged` в injected-процессы |

### Manifest и Common Controls v6

[`GlassSettings/app.manifest`](../../GlassSettings/app.manifest): dependency `Microsoft.Windows.Common-Controls` v6. В [`GlassHost.cpp`](../../GlassSettings/Native/GlassHost.cpp) до `LoadLibrary(GlassCore)` вызывается `InitCommonControlsEx` — для SPI системных шрифтов и стабильного Win32 file picker.

---

## WinUI host HWND

WinUI 3 создаёт технические HWND (`DesktopChildSiteBridge` и т.п.), но GlassSettings **не** подключает WndCompat/ExplorerCompat в своём процессе. Политика attach — только в инжектированных GUI-процессах платформы.

---

## Live reload из UI

GlassSettings не вызывает GlassCompat напрямую; изменения реестра доставляются в платформу через именованные объекты ([`ReloadRenderService`](../../GlassSettings/Core/Platform/ReloadRenderService.cs)) и локальный GlassCore в host.

### Каналы (не смешивать)

| Сценарий | API | Флаги / действия |
|----------|-----|------------------|
| Flyout / popup / taskbar (BWM `Menu`, `DropDown`, `ToolTip`, `Taskbar`) | [`FlyoutLiveReload`](../../GlassSettings/Core/Flyout/FlyoutLiveReload.cs) | `NotifyDwmColorizationChanged` + `GlassSettingsCommit` (GlassCore + TaskbarSwca + TaskbarXaml) |
| **Root `\NoBackdrop`** | [`RootBackdropLiveReload`](../../GlassSettings/Core/Platform/RootBackdropLiveReload.cs) | `NotifyRootNoBackdropChanged` + `ResetReapplyFingerprintIfEngineLoaded` ( **`SignalReloadRender`** — только из `PublishRootBackdropMode` при смене mode) |
| **Root-окна** — CornerType, EffectType, gradient/border color | [`RootWindowsLiveReload`](../../GlassSettings/Core/Platform/RootWindowsLiveReload.cs) | `PostMessage` **`WM_DWMCOLORIZATIONCHANGED`** (`wParam=0`, **`lParam`** = Active/Inactive pivot) по **`g_rootWindows` ∩ `DidGlassApplyRootBackdrop`**; единая композиция в `OnRootDwmColorizationChanged` + `RDW_FRAME`; флаг `kReloadRenderFlagRootLiveColorizationApplied` |
| **Root-окна** — gradient/border **ColorPicker** (drag) | [`RootWindowsPage`](../../GlassSettings/Pages/Glass/RootWindowsPage.xaml.cs) | `UseLivePreview` + `ColorLivePreview` → **`LiveReloadScheduler.ScheduleRegistryWrite`** (debounce ~100 ms, фоновый поток) + `PostLiveRootColorization`; UI — только `UpdatePickerDefaultStates()`; commit на отпускании → `MarkDirty` |
| **Root-окна** — DrawTheme alpha | [`LiveReloadScheduler.ScheduleDrawThemeAlphaLive`](../../GlassSettings/Core/Platform/LiveReloadScheduler.cs) | debounced registry write + `InvalidateThemedChromeOnTrackedRoots` (фоновый поток) |
| **Root-окна** — scope Active/Inactive при live | [`ReloadRootLiveScope`](../../GlassShared/include/ReloadRenderFlags.h) mapping + [`WriteReloadRootLiveScope`](../../GlassShared/src/GlassPlatformTask.cpp) | GlassSettings пишет scope (`Infer`/`Active`/`Inactive`) **перед** `SignalReloadRender`; GlassCompat читает в `PostLiveRootColorizationToAppliedRoots` |
| **Root-окна** — режим UI «Без учёта состояния» / «По состоянию окна» | [`RootWindowsPage`](../../GlassSettings/Pages/Glass/RootWindowsPage.xaml.cs) | **In-memory** (как Popup pivot scope): переключатель меняет только `_regSub` / видимость pivot, **без** записи в реестр; `CornerType` — всегда `Root\`, вне pivot; engine fallthrough: `Root\Active|Inactive` → **`Root\`** — [`RootSwcaRegistryQuery`](../../GlassShared/src/RootSwcaRegistryQuery.cpp) |
| **Root-окна** — gate ON | [`RootWindowsPage`](../../GlassSettings/Pages/Glass/RootWindowsPage.xaml.cs) | Включение спойлера/строки — только UI; запись в реестр — при изменении параметра пользователем |
| **Text** — GlobalTextGlow (color / radius drag) | [`TextGlowLiveReload`](../../GlassSettings/Core/Platform/TextGlowLiveReload.cs) + [`LiveReloadScheduler.BeginTextGlowLivePreview`](../../GlassSettings/Core/Platform/LiveReloadScheduler.cs) | poll preview **100 ms** → `WriteAndReload` + `PostLiveRootComposition(compositionOnly, repaintOnly)` (`scope = Infer \| CompositionOnly \| RepaintOnly`, `0x30000`) + `RenderText`; в инжекте: `ReloadRenderTextInkFromRegistry` → `PostLiveRootColorizationToAppliedRoots` → `OnRootDwmColorizationChanged` **без** border/SWCA/`RDW_FRAME`; `SoftInvalidateTextGlowClientsForRoot` (Moderate/Global по spread; popup не инвалидируются) |
| DWM hub (BlurOptimization, прозрачность и т.д.) | `SettingsSession.WriteDwmDword(..., liveDwm: true, dwmNotifyImmediate: true)` | сразу → `WM_DWMCOLORIZATIONCOLORCHANGED` |
| DWM Reflection / Material / Atlas | `WriteDwmString` / `DeleteDwmValue` для `CustomThemeReflection`, `CustomThemeMaterial`, `CustomThemeAtlas` | **сразу** → `WM_THEMECHANGED` |
| Прочие DWM строки / удаление (цвет, gate) | `WriteDwmString` / `DeleteDwmValue(..., liveDwm: true)` | **сразу** → `WM_DWMCOLORIZATIONCOLORCHANGED` |

### Рассылка (термин в документации GlassSettings)

Маршрутизация: [`DwmLiveReloadKeys`](../../GlassSettings/Core/Platform/DwmLiveReloadKeys.cs) + `SettingsSession.NotifyDwmLiveReload`. DWM notify — **`SendNotifyMessage(FindWindow(Dwm), …)`** (паритет OpenGlassGUI `MainFrame::NotifySettingsChange`). Вызов с фонового потока (`Task.Run` в `LiveReloadScheduler`); не `SendMessage` на UI-потоке WinUI.

| Сообщение | Ключи / сценарии |
|-----------|------------------|
| **`WM_DWMCOLORIZATIONCOLORCHANGED`** (`0x0320`) | backdrop, цвет, прозрачность, blur; `Root\NoBackdrop=0` (SWCA-режим) |
| **`WM_THEMECHANGED`** (`0x031A`) | `CustomThemeReflection`, `CustomThemeMaterial`, `CustomThemeAtlas` |

| API | Когда |
|-----|--------|
| `ReloadRenderService.NotifyDwmColorizationChanged()` | runtime (backdrop SWCA, цвет, прозрачность) |
| `ReloadRenderService.NotifyThemeChanged()` | runtime (Reflection / Material / Atlas) |
| `LiveReloadScheduler.ScheduleDwmColorizationReload()` | debounce 100 ms (ColorPicker и прочие не-слайдеры) |
| `LiveReloadScheduler.ScheduleRegistryWrite` / `ScheduleRootCompositionReload` | debounced Root color/composition (gradient, border, EffectType) — **фоновый поток** |
| `LiveReloadScheduler.ScheduleFlyoutRegistryWrite` | debounced Taskbar color preview (registry + `SignalReloadRender`) |
| `LiveReloadScheduler.NotifyDwmColorizationNow()` | немедленно (слайдеры DWM, gate, строки) |
| `SettingsSession.WriteDwmDwordLivePreview` | реестр + немедленный notify без `MarkDirty` (live-слайдеры) |
| DWM live-слайдеры (`LiveImmediate`) | [`SettingsSliderCommit`](../../GlassSettings/Services/SettingsSliderCommit.cs) — см. [GlassSettings-slider-commit.md](GlassSettings-slider-commit.md) |
| `LiveReloadScheduler.NotifyThemeChangedNow()` | немедленно (плитки, gate Reflection/Material, Atlas) |

**Правило:** live-правка DWM из UI → `liveDwm: true` на `SettingsSession.WriteDwm*` / `DeleteDwmValue`. Без `liveDwm` — только `MarkDirty()`; рассылка при «Сохранить» / `ApplyCommitScopeAsync`.

**Anti-pattern:** синхронный `SendMessage` на UI-потоке WinUI — зависание при старте OpenGlass и смене Root backdrop. `SendNotifyMessage` на окно Dwm с фонового потока — допустимо.
| AttachList | [`AttachListLiveReload`](../../GlassSettings/Core/Platform/AttachListLiveReload.cs) | `AttachList` |
| Commit «Применить» | `SettingsSession.ApplyCommit` | `GlassSettingsCommit` |

#### AttachList → InjectionMode transition (injectee)

`SignalReloadRender(AttachList)` будит GUI-потоки всех injectee; каждый процесс читает **свой** mode из `AttachList` (`ReloadAttachListFromRegistry`). Если mode не изменился — только refresh кэшей, без strip/scan.

| Переход | Порядок |
|---------|---------|
| **Full → PopupOnly/Forbidden** (в т.ч. удаление exe → default PopupOnly) | `SetInjectionMode` (Full-scoped IAT off); затем `GlassCore_ReapplyOpenRootBackdrops` — тот же OFF-sweep, что NoBackdrop; gate = Full && NoBackdrop (`IsRootGlassVisualsAllowed` / Wnd `WinEventHook_GetMode`) |
| **не-Full → Full** (добавление exe всегда Full=2) | `SetInjectionMode(Full)`; затем initial-style `RunProcessWindowScan` + `ApplyBackdropPolicyToTrackedRoots` |

Не хранить InjectionMode-флаг в GlassShared static lib (отдельная копия на DLL). Runtime Forbidden: мягкий strip без unload path=3. `OnRootDetach` не требует Full.

**Не использовать** `liveFlyout: true` для `Root\NoBackdrop` — это flyout-канал; см. `RootBackdropLiveReload` в GlassSettings.

Запись в реестр без live-флага: [`SettingsSession.WriteBwmDword`](../../GlassSettings/Services/SettingsSession.cs) — только `MarkDirty()`; live вызывается отдельно там, где нужен preview.

### GlobalTextGlow live-reload (кратко)

1. **UI (poll 100 ms):** `WriteForLivePreview` → `ReloadRenderTextInkFromRegistry` в host → `PostLiveRootComposition(compositionOnly: true, repaintOnly: true)` → `SignalReloadRender(RenderText)`.
2. **Инжект (GUI-поток):** `ReloadRenderTextInkFromRegistry` → `PostLiveRootColorizationToAppliedRoots(scope)` → `WM_DWMCOLORIZATIONCHANGED` на **`g_rootWindows` ∩ `DidGlassApplyRootBackdrop`**.
3. **Root subclass:** `OnRootDwmColorizationChanged` при **`kGlassLiveRootScopeRepaintOnly` (`0x20000`)** — без `ApplyRootLiveComposition`, без `RedrawWindow(RDW_FRAME)`; `SoftInvalidateTextGlowClientsForRoot` (glow on: **все** visible non-Minimal; glow off: force sweep Moderate+Global).

### TextGlow spread (Распространение)

| mode | Регистрация в реестре | Glow (#126 / IAT paint-session) | Invalidate при reload |
|------|----------------------|----------------------------------|---------------------|
| **0 Minimal** | popup/menu/ImmersiveMenu, Tooltip, DropDown/ViewControl/ListviewPopup shell, ComboLBox; `SysListView32` под **`ListviewPopup`** или **`DropDown`** gaRoot; **`ToolbarWindow32`** под **`ViewControlClass`** | **только** hwnd из реестра (`IsTextGlowClientRegistered`; lookup через `PaintDispatcher::GetPaintSessionHostHwnd`) | Minimal — skip; glow off → force sweep Moderate+Global |
| **1 Moderate** | + **menubar owner**; **`SysHeader32`** (не под DropDown); **`ReBarWindow32`** (band labels через **uxtheme `DrawTextExW`**, Full); **`ToolbarWindow32`** на ReBar / standalone (Full); StatusBar | только зарегистрированные | non-Minimal visible + menubar NC при register |
| **2 Global** | + Static, Button, TreeView, root ListView/ListBox, DirectUI, … | только зарегистрированные | non-Minimal visible |

**Policy:** spread фильтрует **кто попадает в реестр** (`category <= spreadMode`); glow при отрисовке — **только** для hwnd в реестре. Menubar без отдельной TextGlow-персонификации (роль `MenuBarOwner` — только invalidate NC). `ReloadRenderTextInkFromRegistry`: при **понижении** spread — `SoftInvalidateRegisteredClientsDroppedBySpreadDecrease(prev, new)` **до** purge (standalone Toolbar и др. снимают glow); затем purge + rebind + `InvalidateTextGlowClientsOnTrackedRoots`.

Border / gradient live по-прежнему используют только `CompositionOnly` (`0x10000`) с border/SWCA и `RDW_FRAME`.

### Root border: «По умолчанию» (UI vs runtime)

| Слой | Источник |
|------|----------|
| **UI** (кнопка «По умолчанию», пикер) | [`FlyoutThemeRegistry`](../../GlassSettings/Core/Flyout/FlyoutThemeRegistry.cs) → [`PartRegistryQuery::ReadUiBorderArgb`](../../GlassShared/src/PartRegistryQuery.cpp) → [`BorderDisplayHelper::QueryForRootSubKey`](../../GlassShared/src/BorderDisplayHelper.cpp): при отсутствии ключа на `Root\Active|Inactive` — `EnableThemeColorization` / **DWM probe** (`DWMWA_COLOR_DEFAULT` на probe-окне) |
| **Runtime** (окно) | [`QueryRootBorderContext`](../../GlassCore/src/Subsystems/Settings.cpp) → [`ApplyBorderEffect`](../../GlassCore/src/Subsystems/Settings.cpp): явный override из `Root\Active|Inactive` (**без** fallthrough Active→Inactive для цвета); иначе `DWMWA_BORDER_COLOR = DWMWA_COLOR_DEFAULT`. При смене фокуса (Win11+) — [`ApplyRootBorderOnNcActivateChange`](../../GlassCore/src/Subsystems/RootEvents.cpp) из `FinalizeRootActivationVisualUpdate` (лог **`[Root] NC border`**).

### EffectType vs RootBackdropMode (не путать)

| Ось | Где в UI/реестре | Смысл |
|-----|------------------|--------|
| **EffectType** (0–8) | `Root\Active|Inactive\EffectType` | Какой *вид* стекла (1=default, 6=Acrylic, 7=Mica …) |
| **RootBackdropMode** (0–2) | только в памяти/snapshot хоста | *Как* применяется: 0=off, **1=DWM** (OpenGlass pipeline), **2=SWCA** (`SetWindowCompositionAttribute`) |

**Две оси для проводника (Win11):**

| EffectType | Бэкdrop | Анимация focus | CAPTION_COLOR при mode=2 |
|------------|---------|----------------|--------------------------|
| **1–5** | SWCA (`SetWindowCompositionAttribute`) | **нет** | block orig → полный `ApplyRootSwcaBackdrop` (лог `SWCA block CAPTION_COLOR reason=swca_et`) |
| **6–8** | DWM (blur + SBT / системный переход) | **да** | orig + `ApplyRootSwcaCaptionColorTransition` (без изменений) |

**Root border active/inactive:** `WM_NCACTIVATE` → `FinalizeRootActivationVisualUpdate` → **`ApplyRootBorderOnNcActivateChange`**. Не-explorer **ET 1–5 SWCA** — полный `ApplyRootSwcaBackdrop` (`reason=NCACTIVATE`); иначе только `ApplyBorderEffect` (лог **`[Root] NC border`**).

**RootBackdropMode:** Explorer IAT [`ExplorerFrameHooks`](../../GlassCore/src/Hooks/ExplorerFrameHooks.cpp): ENTER-лог с **`RootBackdropMode`**. **DWM** (`==1`) — path 1+2 (path 2 skip: ET=1, Mica, Acrylic). **SWCA** (`==2`) — block `SYSTEMBACKDROP_TYPE`; CAPTION_COLOR — см. таблицу ET выше. Live border: `lParam |= kGlassLiveRootScopeCompositionOnly` → `ApplyRootLiveCompositionForHwnd`.

**Проверка (LogViewer):**

| Сценарий | Ожидание |
|----------|----------|
| **Notepad, border только Active** | Alt+Tab → лог `[Root] NC border active=0` + `useDefault=1`; active → цветной border |
| **Explorer ET 1–5, mode=2** | **нет** `SWCA orig CAPTION_COLOR`; `SWCA block CAPTION_COLOR reason=swca_et`; SWCA не слетает |
| **Explorer ET 6–8, mode=2** | `SWCA orig CAPTION_COLOR` + `SWCA CAPTION_COLOR light`; анимация сохраняется |
| **Explorer, mode=1 (OG on)** | `path1/path2` или `path2 skip`; **нет** SWCA block на CAPTION_COLOR |

---

## Мониторинг платформы и OpenGlass (V10)

Наблюдатель хоста GlassCompat (`PlatformPresenceMonitor`, старт из [`EntryRundll.cpp`](../../GlassCompat/src/EntryRundll.cpp) при `/start`) пишет snapshot в общую память. GlassSettings этот mapping **не** использует как источник кнопок: процесс DWM не видит dual-arch поля Glass. Снимок для UI собирает STA-поток `PlatformPresence.Watcher` прямыми запросами (mutex, планировщик, SCM). Сигнал `GlassPlatform.PresenceChanged` и `NotifyLifecycleChanged` только будят этот поток.

### Именованные объекты

| Объект | Назначение |
|--------|------------|
| `Local\GlassPlatform.PresenceState` | file mapping со snapshot ([`PlatformPresenceState.h`](../../GlassShared/include/PlatformPresenceState.h)) |
| `GlassPlatform.PresenceChanged` | auto-reset event после записи snapshot |

C#: [`PlatformPresenceService`](../../GlassSettings/Services/PlatformPresenceService.cs) — watcher с периодом 500 мс, пока открыт GlassSettings. UI (главная и «Системы») читает `CurrentSnapshot`. Прямой обход дерева задач с UI-потока запрещён: холодный кэш отдаёт последнее известное состояние OpenGlass, чтобы кнопка OG не мигала серым на старте Glass.

### Три уровня OpenGlass (не путать)

| Уровень | Признак | Смысл |
|---------|---------|--------|
| **Orchestration** | задача «OpenGlass Host» **или** служба `OpenGlassHost` RUNNING | «Запускатель OG работает» → UI Running, кнопка Stop |
| **Service-path hint** | процесс **`OpenGlassHost.exe`** | OG идёт через службу, не только schtasks |
| **Pipeline** | **`FindWindowW(L"Dwm")`** | OG **уже в DWM**; reload backdrop через DWM работает |

В `dwm.exe` инжектируется **`OpenGlass.dll`** (не `OpenGlassHost.exe`). Pipeline ground truth — окно класса `Dwm`; модуль в dwm — страховка в фазе Starting.

### RootBackdropMode (автоматически в хосте)

В реестре **только** `Root\NoBackdrop` (1 = стекло выкл). Режим DWM/SWCA (**RootBackdropMode** в памяти/snapshot, 0=off, 1=DWM, 2=SWCA) считает хост:

```
NoBackdrop = 1     → 0 (эффекты backdrop выключены)
иначе pipeline OG  → 1 (DWM)
иначе              → 2 (SWCA)
```

Пишет в mapping для UI GlassSettings. В инжектированном процессе `ReadRootBackdropMode()` ([`RootPolicyQuery.cpp`](../../GlassShared/src/RootPolicyQuery.cpp)) на горячем пути отдаёт кэш. Первое чтение и `RefreshRootBackdropPolicyCache` / `SetRootBackdropMode` идут NoBackdrop → snapshot → OG-fallback. Смена `NoBackdrop` доходит до paint через refresh (`RootEvents`, `ModuleHandler`), не через реестр на каждом DC. При смене mode → `SignalReloadRender`.

Phase 2: push от SCM (`NotifyServiceStatusChange` для службы `OpenGlassHost`) — [`PlatformPresenceComPush.cpp`](../../GlassShared/src/PlatformPresenceComPush.cpp).

### Установка OpenGlass (service-first)

[`OpenGlassInstallService`](../../GlassSettings/Core/Platform/OpenGlassInstallService.cs):

1. **`OpenGlassHost.exe` найден** → установка **службы** `OpenGlassHost` (elevated), не задача.
2. **Exe нет**, есть **`OpenGlass.dll`** → legacy: `rundll32 OpenGlass.dll /install` → задача планировщика.

Start/Stop в UI: если backend = **service** → SCM; если **task** → schtasks / существующий native launch ([`PlatformSchedulerRow`](../../GlassSettings/Controls/PlatformSchedulerRow.cs)).

Native: `GlassSettings_InstallOpenGlassService`, `GlassSettings_QueryOpenGlassServiceState` ([`PlatformPresenceNative.cpp`](../../GlassSettings/Native/PlatformPresenceNative.cpp)).

---

## Hub «Настройки Glass» — «Сплошное стекло»

Файл: [`GlassSettingsHubPage.xaml.cs`](../../GlassSettings/Pages/Glass/GlassSettingsHubPage.xaml.cs).

**ToggleButton** «Сплошное стекло» (`RootFullGlassRow`) в `HeaderActions` строки «Root windows». Реестр: **`Root\NoBackdrop`** (инверсия):

| Кнопка | `NoBackdrop` | Смысл |
|--------|--------------|--------|
| **ON** (по умолчанию) | `0` | backdrop разрешён («сплошное стекло») |
| **OFF** | `1` | политика backdrop подавлена |

DWM vs SWCA **не выбирается в UI** — только хост по pipeline.

Страница Root-окна: спойлер **«Разрешить backdrop»** ([`RootWindowsPage`](../../GlassSettings/Pages/Glass/RootWindowsPage.xaml)) с тем же toggle в `HeaderActions`. `CanNavigate` на Hub: **ON** → можно зайти в Root-окна; **OFF** → навигация закрыта (NoBackdrop=1).

Паттерн загрузки: `_loading` + `Loaded` → sync из реестра без live reload; user change → `WriteBwmDword` + `RootBackdropLiveReload.NotifyNoBackdropChange` (`NotifyRootNoBackdropChanged` → `ResetReapplyFingerprint` → `NotifyDwmColorizationChanged`; **`SignalReloadRender`** — только из `PublishRootBackdropMode` при смене mode; **без** локального `ReapplyOpenRootBackdropsIfEngineLoaded` в GlassSettings — иначе sweep глобального OFF затронет превью UI хоста).

**Изоляция LogViewer:** `ProcessBlockList::IsCurrentProcessLogViewerHost()` — в процессе LogViewer `NoBackdrop` всегда 0, `RootBackdropMode` фиксирован (SWCA), `GlassCore_ReapplyOpenRootBackdrops` — no-op; старт/стоп платформы на локальные модули не влияет. **GlassSettings** (tool-host): `IsCurrentProcessGlassToolHost()` — без глобального NoBackdrop OFF для in-process GlassCore preview API.

**Anti-pattern:** platform sweep, `NotifyExplorerCompat`, `RescanAllMissingGlassHosts` или `GlassCore_ReapplyOpenRootBackdrops` без проверки tool-host — превью UI хоста настроек/log viewer потеряет glass при глобальном OFF.

LogViewer: `ApplyWindowBackdrop` в `WM_CREATE` главного окна.

Удалено: ComboBox `BackDropType`, `RootBackdropOpenGlassCoordinator` (автополитика DWM/SWCA теперь в хосте).

Legacy `Root\BackDropType` мигрируется один раз при старте хоста ([`RootRegistryMigration`](../../GlassShared/src/RootRegistryMigration.cpp)): `0` → `NoBackdrop=1`, ключ удаляется.

### Паритет NONE (`BackDropType=0` / `NoBackdrop=1`)

**Подробная пошаговая последовательность:** [RootBackdropDisableSequence.md](RootBackdropDisableSequence.md) — канон для live OFF, чеклист anti-regression и Explorer ЗОНА 3.

Live-выключение «сплошного стекла» **не** вызывает `Root_DetachAll` (только shutdown платформы). HWND остаются в in-memory реестре; **мягкий NONE** — subclass и IAT остаются, меняются backdrop/paint/hooks-cache.

Кратко при **OFF:** `GlassCore_ReapplyOpenRootBackdrops` → cache refresh, `ScanBackdropScopedModules(false)`, `ShutdownComctlScrollBarIat`, `ApplyBackdropPolicyToTrackedRoots` (per-root + per-control + Explorer host bar restore). При **ON:** обратный sweep + `ApplyRootGlassVisualsOnBackdropEnabled`.

ComboLBox и Tray — вне Root NONE. Paint-gate: `GlassIsBackdropAllowedForHwnd` ([`PaintDispatcher`](../../GlassCore/src/Subsystems/PaintDispatcher.cpp), [`ScrollBarThemeHooks`](../../GlassCore/src/Hooks/ScrollBarThemeHooks.cpp)).

---

## Политика Root ↔ OpenGlass

Автоматическое переключение DWM/SWCA по состоянию OpenGlass **больше не в GlassSettings** — только в `PlatformPresenceMonitor` + `RootPolicyQuery`.

GlassSettings реагирует на snapshot (`PlatformPresenceService.Changed`) для статусов UI, border-coordinator и Home/System.

| Событие | Действие UI |
|---------|-------------|
| Pipeline Active | RootBackdropMode=1 в mapping; GlassCore reapply через reload |
| Pipeline Idle, NoBackdrop=0 | mode=2 (SWCA) |
| NoBackdrop=1 | mode=0; Root pivot скрыт на Hub |

Запись `NoBackdrop` пользователем: `RootBackdropLiveReload.NotifyNoBackdropChange` + при необходимости `SettingsSession.SyncRootBackdropBaseline()`.

---

## Политика бордюров ↔ OpenGlass

Координатор: [`OpenGlassBorderPolicyCoordinator`](../../GlassSettings/Core/Platform/OpenGlassBorderPolicyCoordinator.cs).

**Детект OpenGlass (pipeline):** snapshot `openGlassPipelineActive` / `FindWindow(Dwm)` — через [`PlatformPresenceService`](../../GlassSettings/Services/PlatformPresenceService.cs). Orchestration (Running/Ready): задача или служба. В native: `Shared::OpenGlassDwmPolicy::IsOpenGlassNotificationWindowPresent()`.

**Info-карточка при OpenGlass** — только **Windows 11+** (build ≥ 22000): на Win10 карточка «Ограничено изменение цвета бордюров» не показывается. Баннер информирует пользователя; color picker и прочие контроли бордюра **не блокируются**.

| Состояние | UI (Win11+, OG RUNNING) | Runtime |
|-----------|-------------------------|---------|
| OpenGlass RUNNING | Info-карточка на Root/Popup/Taskbar; **только** на странице «Панель задач» заблокирован переключатель DWM/XAML; color picker доступен | Бордюры по реестру без OG-gate; OG может визуально доминировать поверх Glass |
| OpenGlass STOPPED | карточка скрыта; переключатель DWM/XAML разблокирован | штатный DWM border на Win11; `BorderDrawMode` не откатывается |

Строки: `Notice_OpenGlassBorderTitle`, `Notice_OpenGlassBorderBody`; на панели задач к телу добавляется `Notice_OpenGlassBorderTraySuffix` (два абзаца). Сборка текста: `OpenGlassBorderPolicyCoordinator.FormatBorderLimitNoticeMessage`.

Точки вызова: `MainWindow` startup (`ApplyStartupPolicy`), [`SystemPlatformRunActions`](../../GlassSettings/Core/Platform/SystemPlatformRunActions.cs) start/stop OG, подписки страниц Root/Popup/Taskbar на `Changed`. Подробнее: [TaskbarBorders.md](TaskbarBorders.md).

---

## IFileOpenDialog (выбор exe)

[`ExecutableFilePickerService.cs`](../../GlassSettings/Services/ExecutableFilePickerService.cs) → `NativeHostInterop.TryPickExecutableFile` → `GlassSettings_PickExecutableFile` в native. Win32-диалог (не WinUI `FileOpenPicker`) из‑за `requireAdministrator` в manifest.

---

## Проверка обновлений (OpenGlass / платформа)

Координатор: [`ProductUpdateCoordinator`](../../GlassSettings/Services/ProductUpdateCoordinator.cs). Точка входа: `MainWindow.OnFirstActivated` после `SettingsSession.Capture()` → `RunStartupAsync()`.

### Реестр (`HKCU\Software\StanislavKa\GlassPlatform\GlassSettings`)

| Значение | Назначение |
|----------|------------|
| `UpdateCheckInterval` | интервал сетевой проверки (дни) |
| `OpenGlassUpdateLastCheckUtc` | UTC последнего запроса GitHub API |
| `OpenGlassUpdatePendingVersion` | версия найденного релиза |
| `OpenGlassUpdatePendingUrl` | URL скачивания |
| `OpenGlassUpdateNoticeDismissedVersion` | версия pending, для которой пользователь закрыл notice (не показывать notice, пока pending совпадает) |

Устаревшие `OpenGlassUpdateCachedTag` / `CachedUrl` / `DismissedTag` мигрируются при старте; `DismissedTag` → `NoticeDismissedVersion` при совпадении с pending.

### Поток при старте

1. **Reconcile** — сравнить установленную версию OpenGlass с `PendingVersion`; если `installed >= pending`, очистить pending.
2. **Сеть** — если истёк интервал, [`OpenGlassReleaseUpdateService`](../../GlassSettings/Services/OpenGlassReleaseUpdateService.cs) запрашивает legacy-релиз; при новой версии пишет `LastCheckUtc` + pending version/url.
3. **UI** — `PendingUpdatesChanged`; notice на Обзоре/Системе; карточки читают pending через `TryReadPendingRelease()`.

Заготовка проверки платформы: [`GlassPlatformReleaseUpdateService`](../../GlassSettings/Services/GlassPlatformReleaseUpdateService.cs) (`TryCheckForUpdateAsync` → `null`).

UI notice и карточки: [GlassSettings-notices.md](GlassSettings-notices.md).

### Тема и backdrop без лишнего polling

| Источник | Действие |
|----------|----------|
| Смена темы/обоев рабочего стола | [`DesktopEnvironmentWatcher`](../../GlassSettings/Services/DesktopEnvironmentWatcher.cs) → `SyncWindowThemeFromDesktop()` (только `AppTheme` + title bar + nav chrome, **без** backdrop) |
| Запись в реестр GlassSettings (`AppTheme`, `WindowBackdrop`, …) | [`GlassSettingsPreferencesWatcher`](../../GlassSettings/Services/GlassSettingsPreferencesWatcher.cs) — reapply только при смене theme/backdrop/overlay (игнор pending-update и прочих ключей) |
| Title bar / pane chrome / backdrop | кэш последнего применённого состояния — без повторного `SystemBackdrop` и DWM title bar при том же режиме (снижает мерцание ~20 с от DWM/desktop events) |
| `PlatformSchedulerRow` / Home / System | подписка на `PlatformPresenceService.Changed`; **без** фонового poll в стабильном состоянии |

---

## Сборка и отладка

- MSBuild x64 Debug из корня solution → `Build\x64\Debug\GlassSettings.exe`, `GlassSettings.Native.dll`.
- Debug-лог native: `GLASS_ENABLE_LOGGING` в [`GlassSettings.Native.vcxproj`](../../GlassSettings/Native/GlassSettings.Native.vcxproj).
- Платформу Glass не запускать/останавливать из ассистента — [contributing/build.md](../contributing/build.md).
