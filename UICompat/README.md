# UICompat

**Связанный код:** `UICompat/`  
**Гайд по пути:** [docs/UICompat-CoreWindow.md](docs/UICompat-CoreWindow.md)  
**Настройки paint:** [TaskbarCompat/docs/TaskbarBorders.md](../TaskbarCompat/docs/TaskbarBorders.md) (секция CoreWindow / `Tray\Xaml\*`)

## Назначение

`UICompat.dll` — XAML-патчинг NC/QS/JumpList/Start/Search поверхностей (не HWND-subclass, не TaskbarCompat, не ExplorerCompat):

| HWND | Процессы | Surface |
|------|----------|---------|
| `Windows.UI.Core.CoreWindow` | SEH / SMEH / SearchHost / SearchApp | NC / Jump List / Start / Search |
| `ControlCenterWindow` | `ShellHost.exe` | Quick Settings (24H2+) |

| Surface | Процессы | Статус |
|---------|----------|--------|
| Notification Center / Action / Control | `ShellExperienceHost.exe`, `ShellHost.exe` | **активен** (IXDE на UI-потоке; pre-show tall NC / sized QS — [UICompat-CoreWindow.md](docs/UICompat-CoreWindow.md#pre-show-nc-seh-и-ранний-qs-shellhost)) |
| NC/QS context menu (`Xaml_WindowedPopupClass` / PopupHost) | SEH / ShellHost | **активен** (`MenuFlyoutPresenter` → `NcMenuFlyoutPanelRegistry`) |
| Start context menu (`Xaml_WindowedPopupClass` / PopupHost) | `StartMenuExperienceHost.exe` | **активен** (`MenuFlyoutPresenter` → `NcMenuFlyoutPanelRegistry`; user card / `FlyoutPresenter` — out of scope) |
| Start tooltip (`Xaml_WindowedPopupClass`) | `StartMenuExperienceHost.exe` | **активен** (`ToolTip` → `ShellTooltipPanelRegistry`; VTS3 углы для SMEH) |
| Search context menu (in-tree CoreWindow) | `SearchHost.exe` / `SearchApp.exe` | **активен** (`MenuFlyoutPresenter` → `NcMenuFlyoutPanelRegistry`; Blur→Acrylic) |
| Jump List (кнопки трея) | `ShellExperienceHost.exe` | **активен** (`Border#JumpListRestyledAcrylic` → `JumpListPanelRegistry`) |
| Start Menu | `StartMenuExperienceHost.exe` | **активен** (IXDE на UI-потоке CoreWindow) |
| Search | `SearchHost.exe` / `SearchApp.exe` | **активен** (IXDE на UI-потоке, defer PostMessage) |

Модель как у Windhawk NC / Start Menu Styler: одна DLL, WUX IXDE (`Windows.UI.Xaml.dll`).

## ShellSurface Start/Search

`UiVisualTreeWatcher` принимает только `Border` с точным именем из allow-list:

| Процесс | Имена | Действие |
|---------|-------|----------|
| Start | `AcrylicBorder` | Background + Border* + CornerRadius |
| Start | `AcrylicOverlay` | Visibility Collapsed |
| Search | `AppBorder` | Background + Border* + CornerRadius |
| Search | `LayerBorder` | Visibility Collapsed |
| Start/Search | `DropShadow`, `dropshadow`, `RootGridDropShadow`, `StartDropShadow` | Opacity 0 |

Имена версионно-зависимы. Неизвестные имена/типы пропускаются (fail-closed), поэтому новый Windows build может потребовать временного дампа `type + name + tid` и добавления подтверждённых алиасов. `BorderGrid`, `OuterBorderGrid`, `AccentAppBorder` сейчас не являются целями.

Исходные значения Background/Border*/CornerRadius/Visibility/Opacity сохраняются через `IVisualTreeService3` handles и возвращаются на Stop или при переходе в DWM-режим. Capture считается успешным только при обязательном свойстве (для `AppBorder` — Background); иначе Apply фона не выполняется. Для ShellSurface не использовать WinRT-геттеры этих свойств и `ClearProperty`.

## Структура проекта

| Папка / файл | Роль |
|--------------|------|
| `UICompat.cpp`, `dllmain.cpp`, `UICompat.def` | Экспорты lifecycle / Notify |
| `config/UiRegistry.*` | Чтение `Tray\` / `Tray\Xaml\` (+ AppContainer map) |
| `xaml/XamlDiagInit.*` | IXDE NC (STA) / ShellSurface (UI-thread) |
| `xaml/UiTapSite.*` | `IXamlDiagnostics` TAP site |
| `xaml/UiVisualTreeWatcher.*` | VTC → panel registries |
| `xaml/NotificationCenterPanelRegistry.*` | NC Grids: Background + Border*/CornerRadius + L1Chrome |
| `xaml/NcMenuFlyoutPanelRegistry.*` | NC/QS + Start + Search MenuFlyoutPresenter > Border; SMEH corners via VTS3; Search Blur→Acrylic |
| `xaml/JumpListPanelRegistry.*` | SEH Jump List: Border#JumpListRestyledAcrylic (+ companion clear) |
| `xaml/ShellSurfacePanelRegistry.*` | Start/Search Borders: backdrop + corners + border + overlay/shadow |
| `xaml/ShellTooltipPanelRegistry.*` | Start/Search ToolTip: backdrop + corners (VTS3 для SMEH) + border + innerBorder transparent |
| `xaml/SearchWebViewPanelRegistry.*` | Search WebView2 context menu: CSS .contextMenu |
| `xaml/brushes/*`, `xaml/effects/*` | Backdrop / blur (как TaskbarCompat) |
| `xaml/XamlClassFactory.*`, `DllGetClassObject.cpp` | COM TAP factory |

В Solution Explorer фильтры зеркалят диск: `config`, `xaml`, `xaml\brushes`, `xaml\effects` (`UICompat.vcxproj.filters`).

## Экспорты (`UICompat.def`)

| Экспорт | Кто зовёт |
|---------|-----------|
| `UICompat_Initialize` / `Shutdown` / `IsShutdownComplete` | GlassCompat Ensure / ExecuteShutdown |
| `UICompat_NotifyCoreWindow` | `WndCompat` `CoreWindowHost_Attach` (NC; Start/Search редирект) |
| `UICompat_NotifyShellSurfaceWindow` | Start/Search — IXDE на UI-потоке CoreWindow |
| `UICompat_HandleWinEvent` | опциональный WinEvent-мост (если подключён) |
| `UICompat_ReloadSettingsAndReapply` | live reload настроек |
| `UICompat_ReleaseIxdeSurfaceInterest` | Detach / stop surface |
| `DllGetClassObject` / `DllCanUnloadNow` | IXDE TAP (PRIVATE) |

## На что обращать внимание

- В CoreWindow-хостах **не** грузить GlassCore (IAT/Immersive ломают NC).
- `CoreWindow` в `kHardList` — SWCA/Root **exempt**, не «запрет инжекта».
- Attach только через `CoreWindowHost` + `GlassAttachGate` в `IsCoreWindowHostProcess()`.
- AppContainer: глобальный INCONTEXT часто **не** грузит DLL — хост делает remote `LoadLibrary` + bootstrap (`CoreWindowHostInject`).
- SEH/SMEH/SearchHost часто в PLM «Paused»: перед CRT inject / unload — `IPackageDebugSettings` EnableDebugging+Resume; **не** Suspend до FreeSelf.
- Start/Search: IXDE **только** на UI-потоке CoreWindow + фильтр tid в VTC.
- NC pre-show: tall `CoreWindow` (`cy≥600`); QS: sized `ControlCenterWindow` — не IXDE с toast/1×1 sibling.
- Reload: `UpdateSettings`/Reapply **без** ClearValue-Recapture (в отличие от TaskbarCompat).
- Media QS: backdrop на `MediaTransportControlsRegion`; Root Transparent; Stop ClearValue + acrylic fallback при theme hole.
- При новом XAML-макете сначала временный дамп дерева, затем точечное расширение allow-list; дамп в финальном коде не оставлять.
- Shutdown: Taskbar-style (без долгого `CoFreeUnusedLibraries` на CW); path=1 FreeSelf без ожидания HostShutdown.
