# Бордюры и пути отрисовки панели задач (GlassProject)

**Загрузка TaskbarCompat:** только **Win11** (`build ≥ 22000`), по аттачу `{WindowRole::Tray, ControlKind::ShellTray}` (`TaskbarHost_Attach` → `GlassCompat_EnsureTaskbarCompatLoaded`). Не на каждый `explorer.exe` в `DoInitialize`. Нет `TaskbarCompat.dll` в каталоге — soft skip. Win10 / чужие Explorer без `Shell_*TrayWnd` DLL не грузят.

## CoreWindow / NC / Start / Search (`UICompat`)

`Windows.UI.Core.CoreWindow` и `ControlCenterWindow` (ShellHost QS) **не** патчатся TaskbarCompat. Модуль `UICompat.dll` — единая NC XAML-библиотека (eager в `DoInitialize` + Attach).

**Решение (Windhawk + Glass):** Start + Search — **одна** DLL (как [Start Menu Styler](https://github.com/m417z/my-windhawk-mods/blob/main/mods/windows-11-start-menu-styler.wh.cpp): `@include` SMEH + SearchHost). В Glass это **UICompat** (рядом с NC), не новый модуль и не TaskbarCompat. Отдельная DLL оправдана только при несовместимом lifecycle Start vs NC.

**Полный гайд (inject, PLM, NC VTC, unload):** [UICompat-CoreWindow.md](UICompat-CoreWindow.md) · модуль [UICompat/README.md](../../UICompat/README.md).

### Контракт пути (не Root)

| Слой | Роль |
|------|------|
| `kHardList` / `CoreWindow` + `ControlCenterWindow` | **SWCA/Root exempt** — не «запрет инжекта». Убирать из списка нельзя: откроется `Root_Attach` / SWCA. |
| Classify carve-out (до `IsHardBlockedClass`) | Win11 + SEH → `CoreWindow` → `Special`/`ShellNotificationCore`; ShellHost → `ControlCenterWindow` → то же; SMEH/SearchHost/SearchApp → `Special`/`ShellStartMenuCore`; иначе `Ignored`. **Никогда Root.** |
| `GlassAttachGate` | NC-surface root разрешён **только** в host-процессе (`CoreWindow` → `IsCoreWindowHostProcess`, `ControlCenterWindow` → `IsShellNotificationHostProcess`). |
| Attach | `CoreWindowHost_Attach` (без HWND subclass) → Ensure UICompat → NC: `UICompat_NotifyCoreWindow` (UI-thread IXDE) / Start·Search: `UICompat_NotifyShellSurfaceWindow` → IXDE + VTC. SHOW/FOREGROUND (не Create для ControlCenterWindow). |
| AppContainer bootstrap | Глобальный `SetWinEventHook(INCONTEXT)` **не** грузит DLL в SEH/SMEH/SearchHost. Хост: ACL peer DLL → PLM thaw → remote `LoadLibrary` + `RemoteBootstrap` + process-wide INCONTEXT. |
| OOC SHOW | Host OOC → `RemoteCoreWindowShow` → Post `WM_GLASS_COREWINDOW_REMOTE_SHOW` на GlassGuiThread. |
| GlassCore | В cw-hosts **не** грузится. |
| Настройки paint | `Tray\BorderDrawMode == XAML` + `Tray\Xaml\*` (панель) / `Tray\Xaml\{Popup\|Menu}` (смежные). AppContainer: snapshot `Local\GlassPlatform.TrayXamlPolicy.v3` (хост публикует; UICompat читает первым). |
| Выгрузка | path=1 FreeSelf (+ remote LoadLibrary pin). На stop: remote `GlassCompat_RequestShutdown`, **не** raw `FreeLibrary`. Remote `UICompat_Shutdown` — только если процесс schedulable после thaw. |

| Процесс | HWND | Surface | Цели VTC |
|---------|------|---------|----------|
| `ShellExperienceHost.exe` | `Windows.UI.Core.CoreWindow` | NC / Calendar | `NotificationCenterGrid`, `CalendarCenterGrid`, `ControlCenterRegion` → `Background` + `Border*`/`CornerRadius`. Pre-show: tall CoreWindow (`cy≥600`) → IXDE до SHOW; toast sibling → Defer + tall retry ([UICompat-CoreWindow.md](UICompat-CoreWindow.md#pre-show-nc-seh-и-ранний-qs-shellhost)). |
| `ShellExperienceHost.exe` | `Windows.UI.Core.CoreWindow` | Jump List (кнопки трея) | `Border#JumpListRestyledAcrylic` → `JumpListPanelRegistry` |
| `ShellExperienceHost.exe` / `ShellHost.exe` | `Xaml_WindowedPopupClass` (PopupHost) | NC/QS context menu | `MenuFlyoutPresenter` → дочерний `Border` (`NcMenuFlyoutPanelRegistry`) |
| `StartMenuExperienceHost.exe` | `Xaml_WindowedPopupClass` (PopupHost) | Start context menu (напр. Выключение) | `MenuFlyoutPresenter` → дочерний `Border` (`NcMenuFlyoutPanelRegistry`; corners VTS3) |
| `ShellHost.exe` | `ControlCenterWindow` | Quick Settings | `ControlCenterRegion` (+ media: `MediaTransportControlsRegion` Backdrop, Root → Transparent). IXDE только с sized `ControlCenterWindow` (не 1×1 CoreWindow sibling); при `cx/cy=0` — Defer + sized retry. Shell BG overwrite → PropertyChanged invalidate + Reapply. |
| `StartMenuExperienceHost.exe` | `CoreWindow` | Start | `AcrylicBorder` → фон/рамка/углы; `AcrylicOverlay` → Collapsed; `DropShadow`/алиасы → Opacity 0 |
| `SearchHost.exe`, `SearchApp.exe` | `CoreWindow` | Search | `AppBorder` → фон/рамка/углы; `LayerBorder` → Collapsed; `dropshadow`/алиасы → Opacity 0 (контент — WebView) |
| `SearchHost.exe`, `SearchApp.exe` | `CoreWindow` (in-tree) | Search context menu | `MenuFlyoutPresenter` → дочерний `Border` (`NcMenuFlyoutPanelRegistry`) |

Start/Search IXDE работает на UI-потоке CoreWindow с фильтром tid в VTC (см. [UICompat-CoreWindow.md](UICompat-CoreWindow.md)). `searchhost.exe` **не** в ProcessBlockList. Цели ShellSurface — точный allow-list `Border`-имён; неизвестные имена на новом build безопасно пропускаются и требуют нового временного дампа дерева.

### NC/QS / Start / Search MenuFlyout (`UICompat`)

Контекстные меню центра уведомлений, Quick Settings, Пуска (`Xaml_WindowedPopupClass` / PopupHost) и Поиска (in-tree в Search `CoreWindow`, WUX `MenuFlyoutPresenter`) — **UICompat**, не TaskbarCompat. Та же IXDE-сессия; `Xaml_WindowedPopupClass` в `kHardList` (exempt Root/SWCA). Цели и углы — как у Tray MenuFlyout (`MenuFlyoutPresenter > Border`, `XamlPopupCornerRadiusDip`), registry `NcMenuFlyoutPanelRegistry`; XAML-бэкдроп = `Tray\Xaml\Menu\*` с fallback на `Tray\Xaml\*`. В SMEH `CornerRadius` на Border — только VTS3 (WinRT get/set fail-fast). Карточка пользователя / `FlyoutPresenter` / in-tree `UserTileButton` / WebView CSS `.contextMenu` — **не** патчатся. Toast по-прежнему не патчатся.

### Theme / visual style reload (при работающей платформе)

**TaskbarCompat** (bar + Overflow/Preview/Menu/Tooltip): `InvalidateSystemBrushSamples` (mem + delete `CachedSystemStrokeArgb`) → **RecaptureAfterThemeChange** (ClearValue → новый system sample → Apply). Если Fill сразу после ClearValue null/transparent Solid — отложенный re-sample (иначе Stop `RestoreFill` → дыра). Overflow: после Recapture `EnsureSizeHook`, пока `chromeCaptured` (CR) не готов.

**Stop после theme (критично для Overflow / Tooltip / Menu):** `ClearValue(Background)` снимает наш local; system Binding часто **не** восстанавливается → дыра. Fallback — `CreateNeutralAcrylicBackdropBrush()`: Acrylic с `TintColor`/`FallbackColor` = `{ThemeResource …}` через `XamlReader` (следует теме после unload). **Запрещено** как Stop-restore после theme: `SetValue` concrete brush / hardcoded tint «текущей» темы (липнет при смене стиля без платформы); `SetValue` кисти из `Application.Resources` как local (не обновляет элемент при theme). Neutral **только** после theme-epoch (`m_themePreferClearRestore` / `m_themePreferNeutralRestore`), не до первой смены темы.

**UICompat** (NC/QS/Start/Search): общий Reload (`TaskbarXaml` / GS commit) — только `UpdateSettings` / Reapply (**без** ClearValue-Recapture). Visual style host debounce дополнительно ставит `kReloadRenderFlagUiThemeBrushRefresh` → `UICompat_RefreshBrushesAfterThemeChange`: force new backdrop (сброс `keepExistingBg` / `backgroundPatched`) **без** ClearValue и без null originals. Чужой overwrite в bg-watcher (вкл. `CalendarCenterGrid`) пересэмплирует `originalBackground` для Stop.

### EffectType Blur на Start / Search / HoverFlyout

Донор ([Start Menu Styler](https://github.com/ramensoftware/windows-11-start-menu-styling-guide) / WindhawkBlur) на Пуск/Поиск поддерживает отдельный blur-brush, не только `AcrylicBrush`. В Glass: `EffectType=3` → `XamlBlurBrush` (паритет WindhawkBlur); при fail `GetElementVisual` — fallback на Acrylic (`4`). Не форсировать `3→4` до попытки Blur. Jump List при `element=nullptr` по-прежнему не может создать Blur (остаётся Acrylic).

### Jump List (`UICompat`)

Контекстные меню кнопок приложений на панели задач (Jump List) — в `ShellExperienceHost.exe` / `CoreWindow`, **UICompat** (`JumpListPanelRegistry`), не TaskbarCompat. Донор: Windhawk NC Styler / TranslucentShell. Цель: `Border#JumpListRestyledAcrylic` (Background / Border* / CornerRadius); companion `Border#JumpListAcrylic` → Transparent. Настройки и углы — `Tray\Xaml\Menu\*` (fallback `Tray\Xaml\*`) / `XamlPopupCornerRadiusDip`. Не путать с Tray MenuFlyout в explorer.

## Смежные окна: InputSwitcher (`TaskbarCompat`)

`Shell_InputSwitchTopLevelWindow` (выбор раскладки) — **не** UICompat и **не** subclass/SWCA. Патч через ту же WUX IXDE/TAP-сессию TaskbarCompat, что и трей (паритет Windhawk Taskbar Styler: одна DLL, один потребитель Diagnostics).

| Слой | Роль |
|------|------|
| `kHardList` | `Shell_InputSwitch*` остаются exempt (без Root/SWCA). |
| Загрузка | Отдельного LoadLibrary нет: TaskbarCompat уже в explorer после Tray-аттача; process-wide VTC видит дерево InputSwitcher. |
| VTC | Тип `WindowsInternal.ComposableShell.Experiences.TextInput.Common.InputSwitcher` → `InputSwitchPanelRegistry`. |
| Цели | `ContentControl > ContentPresenter > Grid`: Background / Border* / CornerRadius / Shadow; вложенный `Grid`/`OverlayPanel` → Transparent (без двойного Blur). |
| Настройки | `Tray\BorderDrawMode` + `Tray\Xaml\Popup\*` (fallback `Tray\Xaml\*`). |
| Тень (live) | Как Overflow: system ThemeShadow **не** гасим при патче. |
| Restore | Stop → `RestoreAllFills`; DWM-режим (`paintPathXaml=false`) → restore без перезапуска explorer. |

**Потоковая модель (жёстко):** apply синхронно на VTC-потоке элемента; ретрай по последующим `Add` в поддереве (без `Loaded`/`DispatcherTimer`); отложенные тела (reload) только через `weak_ptr` + `shutdown`-флаг; restore — heap-payload (`shared_ptr` + событие), ожидание ≤250 мс без мьютекса.

## Смежные окна: Tray overflow (`TaskbarCompat`)

`TopLevelWindowForOverflowXamlIsland` (переполнение области уведомлений) — **не** UICompat и **не** subclass/SWCA. Патч через ту же WUX IXDE/TAP-сессию TaskbarCompat (паритет Windhawk Taskbar Styler: одна DLL, один потребитель Diagnostics). Класс HWND в стилизации не участвует — остров живёт в UI-потоке трея.

| Слой | Роль |
|------|------|
| `kHardList` | `TopLevelWindowForOverflowXamlIsland` остаётся exempt (без Root/SWCA). |
| Загрузка | Отдельного LoadLibrary нет: TaskbarCompat уже в explorer после Tray-аттача; process-wide VTC видит дерево overflow. |
| VTC | Тип `SystemTray.NotificationAreaOverflow` и/или `Border#OverflowFlyoutBackgroundBorder` → `TrayOverflowPanelRegistry`. |
| Цели | `Border#OverflowFlyoutBackgroundBorder`: Background / Border* / CornerRadius / Shadow; `Grid#OverflowRootGrid` → Transparent (без двойного Blur). |
| Настройки | `Tray\BorderDrawMode` + `Tray\Xaml\Popup\*` (fallback `Tray\Xaml\*`). |
| Тень (live) | System `ThemeShadow` **не** гасим (`Shadow:=nullptr` нет) — иначе flyout без тени при кастомных углах. |
| Chrome capture | После theme ClearValue sample BorderBrush/Shadow/CR часто 0 — **не** затирать ранее захваченные originals (`keepPrev` в `CaptureChrome`). |
| Restore | Stop → `RestoreAllFills`; DWM-режим (`paintPathXaml=false`) → restore без перезапуска explorer. |

**Слои Background (не путать):**

| Элемент | Live Apply | Stop после theme |
|---------|------------|------------------|
| `Border#OverflowFlyoutBackgroundBorder` | Glass backdrop | `ClearValue` + Neutral ThemeResource Acrylic (epoch); cold Stop — `ClearValue` + `SetValue(original)` при наличии |
| `Grid#OverflowRootGrid` | Transparent Solid (анти-двойной Blur) | **только** `ClearValue` — **без** Neutral |

Neutral на `OverflowRootGrid` → backdrop на весь XAML-остров (включая зону теней / «невидимую» часть HWND) **поверх** flyout → визуальный двойной фон. Не чинить Overflow правками `TaskbarBackgroundRegistry` (бар стабилен отдельно).

**Chrome Stop:** `BorderBrush` / `Thickness` / `CornerRadius` / `Shadow` — `SetValue(kept originals)`. `ClearValue` на Border* после theme **не** ребиндит system chrome (теряются углы/бордюр при живой тени).

**Потоковая модель:** как у InputSwitcher (sync VTC apply, событийный retry, `weak_ptr` + `shutdown` на reload, restore ≤250 мс). Схему выгрузки не менять.

## Смежные окна: HoverFlyout (`TaskbarCompat`)

Превью / flyout над кнопками приложений на панели (`XamlExplorerHostIslandWindow` host) — **не** UICompat / ExplorerCompat и **не** subclass/SWCA. Патч через ту же WUX IXDE/TAP-сессию TaskbarCompat (паритет Windhawk Taskbar Styler). Класс HWND в стилизации не участвует — только XAML chrome; DWM на host не трогаем.

| Слой | Роль |
|------|------|
| `kHardList` | `XamlExplorerHostIslandWindow` остаётся exempt (без Root/SWCA). |
| Загрузка | Отдельного LoadLibrary нет: TaskbarCompat уже в explorer после Tray-аттача; process-wide VTC видит дерево HoverFlyout. |
| VTC | `Taskbar.TaskbarBackground#HoverFlyoutBackgroundControl` / `Grid#HoverFlyoutGrid` → якорь; `Border#HoverFlyoutBackground`; `Rectangle#BackgroundFill` / `#BackgroundStroke` под HoverFlyout (маршрутизация **до** tray `TaskbarFrame`, иначе ложный pending). |
| Цели | Fill → backdrop; Stroke → Collapsed (верхняя кромка). Chrome — см. политику углов ниже. |
| Настройки | `Tray\BorderDrawMode` + `Tray\Xaml\Preview\*` (fallback `Tray\Xaml\*`). Углы и тинт настраиваются в GS (вкладка Preview). Tint — независимо от Effect Disable/System. Blur — см. секцию EffectType Blur выше. |
| Углы / тень | **`CornerType≠0`:** `Shadow:=` (донор: ThemeShadow ≠ кастомный R); свой периметр `Fill.Stroke` + `RadiusX=R`; Border Thickness=0. **`CornerType=0`:** system ThemeShadow не гасим; system grid/CR не клипаем; тинт — `Border.BorderBrush/Thickness` (system CR); если Border* недоступен — `Fill.Stroke` без смены RadiusX. R = `XamlCornerRadiusDip` от trayH; без ActualHeight. |
| Restore | **Один** путь: `RestorePanelVisuals` / `RestoreChrome`. Stop → `RestoreAllFills`; live XAML→DWM (`paintPathXaml=false`) → тот же `RestorePanelVisuals` (не `ClearValue`-stub). |

**Потоковая модель:** sync VTC apply; углы/tint — `Dispatcher.RunAsync` + `RetryUnresolved` (без SizeChanged / без чтения ActualHeight на HoverFlyout); `weak_ptr` + `shutdown` на reload; restore ≤250 мс.

## Смежные окна: Taskbar ToolTip (`TaskbarCompat`)

Тултипы панели задач (`Xaml_WindowedPopupClass` host, WUX `ToolTip`) — **не** UICompat / ExplorerCompat и **не** subclass/SWCA. Патч через ту же WUX IXDE/TAP-сессию TaskbarCompat.

| Слой | Роль |
|------|------|
| `kHardList` | `Xaml_WindowedPopupClass` остаётся exempt (без Root/SWCA). |
| Загрузка | Отдельного LoadLibrary нет: TaskbarCompat уже в explorer после Tray-аттача; process-wide VTC видит дерево ToolTip. |
| VTC | Тип `Windows.UI.Xaml.Controls.ToolTip` (или `*.ToolTip`) → `TrayTooltipPanelRegistry`. |
| Цели | `ToolTip`: Background / Border* / CornerRadius / Shadow; первый вложенный `Border` → Transparent (без двойного Blur). |
| Настройки | `Tray\BorderDrawMode` + `Tray\Xaml\Tooltip\*` (fallback `Tray\Xaml\*`). |
| Углы | `XamlPopupCornerRadiusDip`: R всегда от высоты трея; MaxRound при `popupH < 2×R_tray` → Increased, иначе R_tray. Только XAML CornerRadius — **без** DWM на host HWND. |
| Restore | Stop → `RestoreAllFills`; DWM-режим (`paintPathXaml=false`) → restore без перезапуска explorer. |

**Theme → Stop / Unregister (Background):**

- После `RecaptureAfterThemeChange` — флаг `m_themePreferNeutralRestore`: Stop restore = `ClearValue(Background)` + Neutral ThemeResource Acrylic (как Overflow Border / Tray Menu).
- **Не** `SetValue(originalBackground)` на ToolTip Background: Unregister/Stop до или без epoch штампует кисть **старой** темы; экземпляр часто переживает unload → после смены стиля без платформы тултип остаётся тёмным/светлым «как при захвате».
- Cold path (epoch off): только `ClearValue(Background)` — без `SetValue(original)`. Chrome (Border*/Shadow/CR) по-прежнему `SetValue(originals)`.

**Потоковая модель:** sync VTC + `SizeChanged` для углов после layout; `weak_ptr` + `shutdown` на reload; restore ≤250 мс.

## Смежные окна: Taskbar MenuFlyout (`TaskbarCompat`)

Контекстные меню трея и кнопки Пуск (`Xaml_WindowedPopupClass` host, WUX `MenuFlyoutPresenter`) — тот же IXDE/TAP, что и трей.

| Слой | Роль |
|------|------|
| `kHardList` | `Xaml_WindowedPopupClass` остаётся exempt (без Root/SWCA). |
| Загрузка | TaskbarCompat после Tray-аттача; process-wide VTC. |
| VTC | Тип `Windows.UI.Xaml.Controls.MenuFlyoutPresenter` → `TrayMenuFlyoutPanelRegistry`. |
| Цели | Дочерний `Border`: Background / Border* / CornerRadius / Shadow; Presenter Background → Transparent + тот же CornerRadius. **Не** `GetElementVisual` на Presenter. |
| Настройки | `Tray\BorderDrawMode` + `Tray\Xaml\Menu\*` (fallback `Tray\Xaml\*`). |
| Углы | Как у ToolTip; CornerRadius после `SizeChanged` (layout > 0). Только XAML — **без** DWM на host HWND (ломает shadow insets UWP). |
| Restore | **Один** путь: `RestorePanelVisuals` (Border + Presenter CR/BG). Stop и live XAML→DWM — без дублирующих stub-веток; async читает live `paintPathXaml`. |

**Потоковая модель:** как у ToolTip (SizeChanged для первого показа).

### Контракт Restore (Stop ≡ XAML→DWM)

| Событие | Lifecycle | Вызов |
|---------|-----------|--------|
| **Stop** | `shutdown=true`, clear map, Unadvise | `RestoreAllFills` → per-registry `RestoreAll` → тот же `RestorePanelVisuals` |
| **XAML→DWM** | TAP жив | `ReloadSettingsAndReapply` → `!paintPathXaml` → **тот же** `RestorePanelVisuals` / `RestoreFill` |

Запрещено: отдельный `ClearValue`-stub для HoverFlyout; stale `settingsCopy` в async без re-read пути. Бар: `RestoreFill` при пустом original → `ClearValue(Fill)` (system XAML-backdrop после Stop в DWM-режиме).

## Два пути отрисовки (`Tray\BorderDrawMode`)

| Mode | UI (Win11) | Фон | Бордюр | Углы |
|------|------------|-----|--------|------|
| **0 = DWM/SWCA** | сегмент «DWM/SWCA» | SWCA на `Shell_*TrayWnd` (`Tray\EffectType` 0–8) | `DWMWA_BORDER_COLOR` | `DWMWA_WINDOW_CORNER_PREFERENCE` |
| **1 = XAML** | сегмент «XAML» | кисти на `BackgroundFill` / зонах (`Tray\Xaml\EffectType`) | `BackgroundStroke` Fill | XAML `RadiusX/Y` или `CornerRadius` плиток |

На **Win10** сегмент-бара нет — только DWM/SWCA (TaskbarCompat не грузится).

### Общие ключи (`Tray\`)

| Ключ | Смысл |
|------|--------|
| `BorderDrawMode` | 0 DWM/SWCA / 1 XAML |
| `LightMode_GradientColor` / `DarkMode_GradientColor` | Тинт DWM/SWCA |
| `CornerType` | DWM path: 0–3 (Default / Rect / Round / SmallRound) |
| `LightMode_BorderColor` / `DarkMode_BorderColor`, `NoBorderColor`, legacy `BorderColor` | Цвет бордюра DWM |

### Только XAML (`Tray\Xaml\`) — панель

| Ключ | Смысл |
|------|--------|
| `EffectType` | 0 Disable, 1 Transparent, 2 Solid, 3 Blur, 4 Acrylic, 5 System (оригинальный бэкдроп) |
| `CornerType` | 0–5: Default / Rect / Round / SmallRound / **Increased** (R = NC system) / **MaxRound** (R = h/2) |
| `SeparateZones` | 0/1 — раздельные плитки (left/widgets, center=Пуск+apps, tray) |
| `LightMode_GradientColor` / `DarkMode_GradientColor` | Тинт XAML; **отсутствие ключа** = системный бэкдроп |
| `LightMode_BorderColor` / `DarkMode_BorderColor`, `NoBorderColor` | Бордюр XAML; **отсутствие** = системный stroke (после capture). Tint применяется **независимо** от `CornerType`. Pre-capture / GS fallback: ChartView (`Tray` / `Tray\Xaml` / `Tray\Xaml\*`). |
| `CachedSystemStrokeArgb` | Sample stroke до патча (пишет TaskbarCompat `NoteSystemStrokeArgb`). GS «По умолчанию» для `Tray\Xaml*` пишет этот ARGB (иначе ChartView). При смене visual style / `InvalidateSystemBrushSamples` ключ **удаляется** из HKCU (иначе stale theme → wrong default / Stop restore). |

`System` (и legacy Mica/MicaAlt 5/6): кисть острова без подмены.

### Смежные окна (`Tray\Xaml\{Popup|Preview|Menu|Tooltip}\`)

Те же value-имена, что у панели. **Нет value** → читать `Tray\Xaml\*`. GS: страница «Смежные окна» (Pivot) с Taskbar (только XAML path).

| Surface | Подключ | Кто применяет |
|---------|---------|---------------|
| **Popup** | `Tray\Xaml\Popup` | UICompat: NC / QS / Start / Search chrome; TaskbarCompat: Overflow / InputSwitcher |
| **Preview** | `Tray\Xaml\Preview` | TaskbarCompat: HoverFlyout (углы + тинт бордюра) |
| **Menu** | `Tray\Xaml\Menu` | UICompat MenuFlyout/JumpList/SearchWebView + TaskbarCompat Tray MenuFlyout |
| **Tooltip** | `Tray\Xaml\Tooltip` | TaskbarCompat: тултипы панели |

AppContainer map: `Local\GlassPlatform.TrayXamlPolicy.v3` (slots bar + Popup + Menu).

## XAML border vs stroke (режим XAML)

Tint `BorderBrush` / thickness на меню, тултипах, Overflow, InputSwitch, JumpList, HoverFlyout — **не** зависит от `wantCorners` / `CornerType` и от Effect Disable/System. Углы (CR) — при ненулевом CornerType (включая Preview / HoverFlyout).

| CornerType | Геометрия бордюра (панель) |
|------------|----------------------------|
| **0 Default** | Системный `BackgroundStroke` (обычно верхняя кромка); кастомный цвет — Fill/кисть stroke |
| **1–3** (Rectangle / Round / SmallRound) | Perimeter на `RootGrid` (`BorderThickness=1` + `BorderBrush`); `BackgroundStroke` Collapsed |

| NoBorderColor | DWM `BORDER_COLOR` | XAML |
|---------------|--------------------|------|
| 1 | NONE | без патча stroke / без perimeter |
| 0 | NONE (`colorUseNone`) | см. таблицу CornerType выше |

При **DWM/SWCA** пути XAML stroke скрыт (Fill=0); бордюр рисует DWM.

## Порядок apply на `Shell_*TrayWnd`

`ApplyRootSwcaBackdrop` → на потоке трея:

| Шаг | Действие |
|-----|----------|
| 0 | `TaskbarHooks::SyncHooksForTrayPaintPath` — IAT SWCA в `Taskbar.dll` **всегда**: DWM защищает Glass accent; XAML force-clear |
| 1 | Backdrop: при XAML — SWCA Transparent + `gradientColor=0`; иначе `Tray\EffectType` |
| 2 | `ApplyBorderEffect`: при XAML — DWM border NONE + `DONOTROUND`; при DWM — CornerType + BorderColor |
| 3 | `TaskbarCompat_ReloadSettingsAndReapply` |
| 4 | Redraw trays (при XAML без `RDW_FRAME`, override `DebugXamlRedrawWithFrame=1`) |

### Переключение пути (live)

| Переход | Действие |
|---------|----------|
| **SWCA → XAML** | IAT остаётся; force SWCA clear (Transparent+grad=0); DWM углы/бордюр off; XAML reload |
| **XAML → SWCA** | IAT: защита Glass accent; EffectType + DWM углы/бордюр; XAML reload (fill transparent) |
| **Старт платформы** | `OnTaskbarModuleLoaded`: IAT SWCA на Taskbar.dll |

## Раздельные зоны (`Tray\Xaml\SeparateZones=1`)

UI: кнопка «Раздельные зоны» в шапке спойлера «Цвет бордюров» — только Win11 + режим XAML; **disabled**, если gate углов выключен (`CornerType` отсутствует).

Runtime:

1. Гасить цельный слой: `TaskbarBackground` Opacity=0, `BackgroundFill` Collapsed, `RootGrid.Background`=null.
2. Bounds: left=Widgets, center=union(Start + точный `TaskListButton`, clip), tray=`SystemTrayFrameGrid`; +padX по бокам (не у краёв панели).
3. Left/center/tray — overlay-`Border` на `RootGrid` (не патчить `SystemTrayFrameGrid` — ломает содержимое).
4. Soft-fail при &lt;2 зон → single-tile.

## CornerType

| Index | UI | DWM (`Tray\`) | XAML (`Tray\Xaml\`) |
|-------|-----|---------------|---------------------|
| 0 | Default | `DWMWCP_DEFAULT` | Radius ≈ 8 |
| 1 | Rectangle | `DONOTROUND` | 0 |
| 2 | Round | `ROUND` | 8 |
| 3 | SmallRound | `ROUNDSMALL` | 4 |
| 4 | Increased | — (только XAML) | **R = 16** (между Large и MaxRound) |
| 5 | MaxRound | — (только XAML) | **R = высота панели / 2** (полукруг по бокам) |

**Миграция:** schema `Tray\Xaml\CornerTypeSchema`. Layout 0/1 (без Increased): `CornerType=4` → 5. Schema ≥2: `4` = Increased.

**Ключа нет** (gate off) ≠ Default. SeparateZones при отсутствии ключа углов выключены. На **XAML** пути DWM corner HWND всегда `DONOTROUND` (скругление на RootGrid/зонах). Legacy: если `Tray\Xaml\CornerType` нет — читается `Tray\CornerType`.

## Политика OpenGlass

**Runtime:** по реестру. **UI:** info-баннер; сегмент DWM/SWCA|XAML **не** блокируется политикой OG.

### Debug registry (Tray)

| Ключ | Значение | Назначение |
|------|----------|------------|
| `DebugXamlRedrawWithFrame` | 1 | Вернуть `RDW_FRAME` при XAML redraw |
| `DebugXamlStrokeMode` | 0/1/2 | A/B геометрии stroke |
| `DebugXamlStrokeInset` | px | Override inset |

## Live-reload (GlassSettings → GlassCompat)

| Действие UI | ReloadRender flags |
|-------------|-------------------|
| EffectType (DWM), CornerType (DWM), градиент (DWM) | `TaskbarSwca` |
| BorderDrawMode, SeparateZones, XAML EffectType/tint/corners | `TaskbarBoth` |
| BorderColor preview/commit (XAML) | `TaskbarXaml` |
| BorderColor (DWM) | `TaskbarSwca` |

## LightMode_* / DarkMode_* в `Tray\` / `Tray\Xaml*`

Активная ветка для панели и смежных (TaskbarCompat / UICompat / `TrayXamlPolicy` map) —
`ThemeHelper::IsTrayXamlDarkMode()`: uxtheme `#132` (`ShouldAppsUseDarkMode`), fallback
`Personalize\AppsUseLightTheme==0`. Не `#139` (в host/AppContainer часто false при dark) и
не `COLOR_WINDOW` (тот остаётся для Root/Menu BWM-ключей).
