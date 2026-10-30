# UICompat — CoreWindow / Notification Center / Start / Search

Документ описывает `UICompat.dll`: XAML-патчинг **Notification Center**, **Quick Settings**, **toast** (SEH), **Пуск** и **Поиск** через IXDE на `Windows.UI.Xaml` (UWP). HWND: `Windows.UI.Core.CoreWindow` (SEH / SMEH / SearchHost) и `ControlCenterWindow` (ShellHost 24H2+).

Краткий контракт и реестр paint: [TaskbarBorders.md](TaskbarBorders.md) (секция CoreWindow).  
Модуль: [README.md](../README.md).  
Выгрузка GlassCompat: [GlassCompat-unload.md](GlassCompat-unload.md).

> **Не путать:** «NC» здесь — **Notification Center** (Action Center / Control Center). Flyout **NC-рамка** (non-client GDI) — отдельный гайд [FlyoutBorderPaintOrder.md](FlyoutBorderPaintOrder.md).

## Зачем отдельная DLL

| Модуль | HWND / surface | Runtime |
|--------|----------------|---------|
| TaskbarCompat | `Shell_*TrayWnd` island | explorer, WinUI IXDE |
| ExplorerCompat | Cabinet / popup islands | explorer |
| **UICompat** | `CoreWindow` / `ControlCenterWindow` | SEH / ShellHost / SMEH / SearchHost |

NC/Start/Search-surface **не** субклассируется. Патч — только Visual Tree (TAP + VTC), как Windhawk NC / Start Menu Styler.

## Процессы

| Exe | HWND | Surface | Remote inject (хост) |
|-----|------|---------|----------------------|
| `ShellExperienceHost.exe` | `Windows.UI.Core.CoreWindow` | NC / Calendar / Control / toast | **да** (PLM thaw) |
| `ShellHost.exe` | `ControlCenterWindow` | Quick Settings / Control (Win11 24H2+) | **да** (без PLM thaw) |
| `StartMenuExperienceHost.exe` | `Windows.UI.Core.CoreWindow` | Start | **да** (PLM thaw; TAP на UI-потоке) |
| `SearchHost.exe` / `SearchApp.exe` | `Windows.UI.Core.CoreWindow` | Search | **да** (PLM thaw; TAP на UI-потоке, defer PostMessage) |

Список remote inject: `IsCoreWindowHostExe` в `GlassCompat/src/CoreWindowHostInject.cpp`.  
PLM-freezable: `IsPlmFreezableCwHostExe` (SEH + SMEH + SearchHost + SearchApp).

Windhawk-parity: SEH → `CoreWindow`, ShellHost → `ControlCenterWindow`, Start/Search → `CoreWindow`; все — IXDE из `Windows.UI.Xaml.dll` (не FrameworkUdk / не Microsoft.UI.Xaml).

## Threading-контракт TAP (критично)

Два пути IXDE — **не смешивать**:

| Путь | Процессы | Где IXDE | Advise | VTC фильтр |
|------|----------|----------|--------|------------|
| **NC** | SEH / ShellHost | **UI-поток** окна (`RunFromWindowThread`, как ShellSurface) | inline retry ×8 | `GetCurrentThreadId() == g_shellSurfaceThreadId` |
| **ShellSurface** | SMEH / SearchHost / SearchApp | **UI-поток CoreWindow** (`UICompat_NotifyShellSurfaceWindow`) | **короткоживущий поток** | `GetCurrentThreadId() == g_shellSurfaceThreadId` |

### Почему прежняя регрессия Start/Search закрыта

Раньше Start/Search шли через NC-путь: `NotifyCoreWindow` с GlassGuiThread → IXDE на чужом STA → `OnVisualTreeChange` без фильтра tid → AV `0xC0000005` в `Windows.UI.Xaml.dll` + WerFault / PID-churn.

Исправление (модель Windhawk Start Menu Styler):

1. Marshal на поток CoreWindow: `WH_CALLWNDPROC` + `SendMessageTimeoutW` с приватным зарегистрированным сообщением (`RunFromWindowThread`).
2. **SearchHost**: синхронный init слишком ранний — defer через `PostMessage` + `WH_GETMESSAGE`.
3. CAS `g_shellSurfaceThreadId` → `InitializeXamlDiagnosticsEx` **на этом же потоке**; endpoint: `VisualDiagConnection1`, при `ERROR_NOT_FOUND` — перебор `VisualDiagConnection2..N`.
4. `AdviseVisualTreeChange` с отдельного потока (иначе hang в `Advising::RunOnUIThread`).
5. VTC: чужой tid → `S_OK` без `FromHandle`.

NC (SEH/ShellHost) переведён на тот же UI-thread IXDE: иначе `CatchUpExistingNcPanels` на STA видел `Window::Current()==nullptr` → патч QS только со 2-го показа.

### Pre-show NC (SEH) и ранний QS (ShellHost)

Цель: Apply **до** первого пользовательского SHOW (cloaked OK), первый кадр уже с кастомом.

| Поверхность | Триггер | Defer / retry |
|-------------|---------|----------------|
| **NC (SEH)** | HR16 `PostRemoteAttachExisting` → `AttachExisting` (HWND сортируются по `cy` desc) → UI-thread IXDE → CatchUp + Apply | `cy < 600` (toast/1×1) → Defer; затем поиск tall `CoreWindow` (`cy ≥ 600`) и retry. **Не** `Sleep(500)` на Advise (давал late custom после uncloak). |
| **QS (ShellHost)** | тот же HR16 / Attach; IXDE **только** с `ControlCenterWindow` | sibling `CoreWindow` и `ControlCenterWindow` с `cx/cy ≤ 0` → Defer; retry на sized `ControlCenterWindow` (LOCATIONCHANGE / SHOW / delayed enum). |

- EffectType **не** подменять (Acrylic `4` остаётся AcrylicBrush; не форсировать Blur).
- Host `CatchUpCoreWindowsForPids` под H47 (`if (false)`) — **не** включать вслепую (storm / host AV).
- SEH delayed CatchUp после Advise: короткий повтор **50 ms** (CATCHUP+REAPPLY), не 500.

### Конфликт XAML Diagnostics

Потребитель IXDE в процессе — **один**. Одновременный Windhawk Start Menu Styler / TranslucentTB / другой TAP в том же SMEH/SearchHost → конфликт (как у Taskbar Styler).

## Слои (сверху вниз)

```
GlassCompat host (rundll32)
  ├── ACL peer DLL (ALL APPLICATION PACKAGES)
  ├── CoreWindowHostInject::Refresh
  │     ├── SEH не CreateProcess (AppX cold-start → FailFast); inject при живом SEH
  │     ├── PLM thaw: IPackageDebugSettings EnableDebugging + Resume
  │     ├── CreateRemoteThread(LoadLibrary GlassCompat)
  │     ├── RemoteBootstrap → GlassGuiThread (+ UICompat eager)
  │     ├── process-wide SetWinEventHook(INCONTEXT, pid)
  │     └── Post AttachExisting / REMOTE_SHOW (cloaked OK)
  └── Stop: Unhook → HostShutdown → ForceUnloadResiduals
        ├── remote UICompat_Shutdown только если schedulable (thaw+probe)
        ├── PLM Resume
        ├── Signal unload
        └── DisableDebugging только если DLL уже снята

WndCompat
  ├── WindowClassifier: CoreWindow → ShellNotificationCore | ShellStartMenuCore
  ├── GlassAttachGate: surface только в host-процессе
  └── CoreWindowHost_Attach → NotifyCoreWindow | NotifyShellSurfaceWindow

UICompat
  ├── NC: NotifyCoreWindow → UI-thread IXDE (как ShellSurface) → TAP
  ├── ShellSurface: NotifyShellSurfaceWindow → IXDE на UI-потоке → TAP
  ├── UiVisualTreeWatcher → panel registries
  ├── NotificationCenterPanelRegistry
  ├── NcMenuFlyoutPanelRegistry (контекстное меню NC/QS + Пуск + Поиск)
  ├── JumpListPanelRegistry (Jump List кнопок трея в SEH)
  └── ShellSurfacePanelRegistry (Пуск + Поиск)
```

## Notification Center — что патчится

| Элемент | Патч |
|---------|------|
| `NotificationCenterGrid` | `Background`, `BorderBrush` / `BorderThickness`, `CornerRadius` |
| `CalendarCenterGrid` | то же |
| `ControlCenterRegion` | то же (Quick Settings в ShellHost) |
| `MediaTransportControlsRegion` | Backdrop + Border*/CornerRadius (видимый бэкдроп медиа-карточки) |
| `MediaTransportControlsRoot` | Background → Transparent (Windhawk); рамка/углы на Region, не на Root |
| L1 outer `Border` | Visibility Collapsed при borderless (если найден `L1Grid`; на части сборок отсутствует) |
| `MenuFlyoutPresenter` (popup host) | дочерний `Border`: Backdrop + Border* + CornerRadius; Presenter Background → Transparent |
| `Border#JumpListRestyledAcrylic` | Backdrop + Border* + CornerRadius; companion `JumpListAcrylic` → Transparent |

### Apply / Reapply (NC·QS) — не плодить Acrylic и не «залипать» на stock

| Правило | Зачем |
|---------|--------|
| `keepExistingBg`: повторный Register/Reload **не** создаёт новый `AcrylicBrush`, если effect/tint те же | иначе compositor Backdrop reconnect → поздний визуальный кастом |
| **Исключение:** `MediaTransportControlsRegion` — keep **выкл.** (shell часто перетирает BG) | иначе карточка остаётся stock при живом `bgPatched` |
| `PropertyChanged(Background)` на `NotificationCenterGrid` / `CalendarCenterGrid` / `ControlCenterRegion` / `MediaTransportControlsRegion`: чужой сброс → сброс keep + `PostReapplyIfActive` | shell overwrite (QS / календарь после темы) |
| Reload UICompat: только `UpdateSettings` / Reapply — **без** ClearValue-Recapture | Recapture на общем Reload ломает Binding/originals → залипший QS, Stop `noOrig`, Search Invisible |
| Theme (`UiThemeBrushRefresh`): `RefreshBrushesAfterThemeChange` — invalidate keep + новый brush; **не** live ClearValue | live кастом после visual style |
| Theme → Stop: NC `clearOnRestore=true` на BG/BorderBrush; ShellSurface VTS3 ClearProperty BG; Menu ClearValue BG | иначе Stop `SetValue` concrete brush **старой** темы (лог clearOnRestore=0 после theme) |

### Context menu / MenuFlyout (NC/QS/Start) + Search WebView menus

Контекстные меню центра уведомлений, Quick Settings и Пуска (напр. «Выключение») живут в HWND `Xaml_WindowedPopupClass` (заголовок часто `PopupHost`). Патч — **XAML** `CreateBackdropBrush` / `Tray\Xaml\EffectType` через `NcMenuFlyoutPanelRegistry` (донор: `MenuFlyoutPresenter > Border`).

Меню **Поиска** — не `MenuFlyoutPresenter`, а DOM в WebView2 (`.contextMenu` / `#menuContainer`). Системная acrylic/blur-кисть над WebView2 не сэмплит swapchain (MS #7796/#8267); tint без blur делает меню нечитаемыми. Glass: `SearchWebViewPanelRegistry` — CSS `border` / `border-radius` из `Tray\Xaml\*`; системный контур снимается (`box-shadow:none`, чужой `border` → наш или `none`); фон и frosted fill (`::before` background) не трогаем, у `::before` синхронизируем radius.

| Слой | Роль |
|------|------|
| `kHardList` | `Xaml_WindowedPopupClass` остаётся exempt (без Root/SWCA / DWM). |
| VTC (NC/QS/Start) | `MenuFlyoutPresenter` → `NcMenuFlyoutPanelRegistry`. |
| VTC (Search) | `HostedWebView*` / `WebView2` → `SearchWebViewPanelRegistry` (CSS). |
| Цели XAML | Дочерний `Border`: Background / Border* / CornerRadius; **не** `GetElementVisual` на Presenter. |
| Настройки | Общие `Tray\BorderDrawMode` + `Tray\Xaml\*`. |
| Out of scope | Карточка пользователя (`FlyoutPresenter` / `UserTileButton`); системный blur внутри WebView-меню. |
| Restore | Stop / `paintPathXaml==false`; WebView — clear CSS. |

Не путать с Tray MenuFlyout в explorer (`TaskbarCompat` / `TrayMenuFlyoutPanelRegistry`) и с toast (отключены).

**Исправления NcMenuFlyout (2026-08-20):**

1. **Убран гейт canPatchBg** — кисть применяется всегда при `paintPathXaml`, независимо от наличия оригинального Background через VTS3.
2. **Reuse слота при повторном handle** — RegisterPanel повторно использует существующий слот вместо создания нового, исправляет баг потери backdrop при втором показе меню "Выключение".
3. **RestoreBackgroundVts3 в Unregister** — при hide (Remove VTC event) восстанавливается Background через VTS3 (SMEH), не только через ClearValue.

### ToolTip (Start/Search)

Тултипы в окне Пуска и Поиска живут в HWND `Xaml_WindowedPopupClass` (WUX `ToolTip`). Патч через `ShellTooltipPanelRegistry` (донор: `TrayTooltipPanelRegistry` из TaskbarCompat).

| Слой | Роль |
|------|------|
| VTC | `Windows.UI.Xaml.Controls.ToolTip` → `ShellTooltipPanelRegistry`. |
| Цели | ToolTip: Background / Border* / CornerRadius / Shadow=null; innerBorder (первый Border в шаблоне) → Transparent (анти-двойной blur). |
| VTS3 | SMEH: CornerRadius через `IVisualTreeService3` (WinRT get → fail-fast); SearchHost — WinRT напрямую. |
| Настройки | `Tray\BorderDrawMode` + `Tray\Xaml\Tooltip\*` (fallback на `Tray\Xaml\*`). AppContainer: snapshot `Local\GlassPlatform.TrayXamlPolicy.v4` (v3→v4: +20 байт tooltip slot, 88→108). |
| Углы | `XamlPopupCornerRadiusDip` + высота трея; CornerRadius после `TipLayoutReady`. |
| Tid | Add на том же UI-потоке, что Start/Search ShellSurface. |

**Требует перезапуска GlassPlatform после сборки:** v4 mapping создаётся только при старте хоста; старый v3 mapping без tooltip slot отклоняется клиентом по несовпадению версии.

### Jump List (кнопки панели задач, SEH)

ПКМ по кнопкам приложений на трее — Jump List в `ShellExperienceHost.exe` (`CoreWindow`). Донор: Windhawk NC Styler / TranslucentShell (`Border#JumpListRestyledAcrylic`). Только chrome (backdrop / углы / тинт / бордюр); item/hover/`JumpViewUI.*` VisualState не патчатся. Не путать с Tray MenuFlyout в explorer.

| Слой | Роль |
|------|------|
| VTC | `Border#JumpListRestyledAcrylic` → `JumpListPanelRegistry`. |
| Цели | Background / Border* / CornerRadius / Shadow=null; companion `Border#JumpListAcrylic` → Transparent. |
| Tid | Add на чужом STA (относительно NC tid) — **без** adopt NC tid. |
| Настройки | Общие `Tray\BorderDrawMode` + `Tray\Xaml\*` (без новых ключей). |
| Углы | `XamlPopupCornerRadiusDip` + высота трея; CornerRadius после `SizeChanged`. |
| IXDE | Process-wide VTC после подъёма с NC-sized CoreWindow (`ShouldDeferSehToastSizedIxde` не меняем). |

### Toast (всплывающее уведомление приложения, SEH)

**Статус Glass: кастомизация toast отменена** (2026-08-01). VTC съедает toast-имена (`toast_disabled`) без registry/apply — чтобы не ломать NC/QS и Stop SEH. Паритет с донором TranslucentShell / WindhawkBlur на toast **не** поддерживается. `ShouldDeferSehToastSizedIxde` остаётся: IXDE только с NC-sized CoreWindow, не с toast/1×1.

### MediaTransport (QS) — Apply и Stop

**Apply (Windhawk-паритет):**

- `MediaTransportControlsRoot` — только Transparent Background (не Blur/Acrylic на Root).
- `MediaTransportControlsRegion` — кастомный Backdrop (`CreateBackdropBrush` / EffectType из `Tray\Xaml\*`).
- Не писать `BorderThickness{0}` поверх Unset/Binding у media (ломает layout после Stop).

**Restore Stop:**

- Снять chrome/backdrop; `InvalidateMeasure` / `InvalidateArrange` / `UpdateLayout` на Root и Region (и sibling).
- `clearOnRestore` → `ClearValue(Background)`. Если у **Region** после ClearValue кисти нет / Solid A=0 (дыра вместо theme acrylic) — fallback `AcrylicBrush` с tint из реестра (лучше дыры).
- Stop и live `paintPathXaml=false` — `RestoreSlotOnUiThread` (Win32 `ControlCenterWindow`), не только `CoreDispatcher.RunAsync` (закрытый QS не качает очередь). Live keepAlive: `clearRefs=false`. Перед restore на DWM — без `CaptureChrome`.

### ShellHost QS — IXDE только с ControlCenterWindow

В `ShellHost.exe` sibling `Windows.UI.Core.CoreWindow` (часто 1×1) **не** поднимает IXDE: иначе CAS tid крадётся при нулевом `ControlCenterWindow`, Advise идёт на пустое дерево, кастом QS — со 2-го SHOW.

IXDE стартует с **sized** `ControlCenterWindow` (`cx/cy > 1`): SHOW / `EVENT_OBJECT_LOCATIONCHANGE` / delayed retry после Defer на нулевом размере (см. Pre-show выше). AttachExisting при `cx/cy=0` сам по себе IXDE не поднимает.

### SEH AppContainer — host OOC forward

Глобальный INCONTEXT часто доставляет CREATE/SHOW/FOREGROUND/**UNCLOAKED** CoreWindow в процесс хоста. Хост вызывает `NotifyRemoteCoreWindowShow` → post GUI Attach/Reapply в SEH (не early-return без forward).

## Start / Search — что патчится

Watcher обрабатывает только `Windows.UI.Xaml.Controls.Border` (или короткий тип `Border`) с точным именем из allow-list. Регистрация идёт по событиям VTC `Add`; если TAP подключился после построения дерева, выполняется catch-up обход `Window.Current`.

| Элемент | Роль | Патч |
|---------|------|------|
| `Border#AcrylicBorder` | основной фон Пуск | Background + Border* + CornerRadius (на месте) |
| `Border#AcrylicOverlay` | штатный overlay Пуск | `Visibility=Collapsed` |
| `Border#AppBorder` | основной фон/рамка Поиск | Background + Border* + CornerRadius (на месте) |
| `Border#LayerBorder` | штатный слой Поиск | `Visibility=Collapsed` |
| `Border#DropShadow`, `dropshadow`, `RootGridDropShadow`, `StartDropShadow` | штатная тень | `Opacity=0` |

`Grid#BorderGrid`, `Grid#OuterBorderGrid` и `Border#AccentAppBorder` в текущий allow-list **не входят**. Их упоминание в старом плане было предварительной гипотезой; фактический дамп 24H2 показал рабочие Border-цели выше. Не добавлять Grid в этот путь без отдельного runtime proof: обращение к ним через текущую схему ранее давало AV/hang.

### Версионная совместимость имён

Имена XAML не являются публичным контрактом Windows. Текущая реализация намеренно fail-closed:

- известные имена и варианты регистра патчатся;
- неизвестное имя или другой тип элемента пропускается без изменения дерева;
- при переименовании `AcrylicBorder`/`AppBorder` в новой Windows кастом для соответствующей поверхности не применится, но UICompat не должен угадывать роль по произвольному соседнему Border;
- parent-path сейчас не проверяется: если новый XAML-макет добавит другой `Border` с тем же известным именем, потребуется проверить дамп и сузить matcher, а не полагаться только на совпадение имени;
- известные варианты имени тени уже перечислены отдельно.

Для поддержки нового build временно добавить узкий дамп `type + name + tid` в `OnVisualTreeChangeImpl` только для Start/Search, открыть Пуск и Поиск, зафиксировать реальные цели и их роли, затем:

1. добавить подтверждённые имена в `IsShellSurfaceTargetName`;
2. обновить `ShellSurfacePanelRegistry::ClassifyBorder`;
3. проверить apply и Stop/DWM restore;
4. удалить временный дамп до завершения задачи.

**Ограничение Поиска:** XAML chrome — `AppBorder` / `LayerBorder` / тени + in-tree `MenuFlyoutPresenter > Border`; основное содержимое — WebView (CSS вне UICompat).

Источник настроек: `Tray\BorderDrawMode == XAML` + `Tray\Xaml\Popup\*` (NC/QS/Start/Search) / `Tray\Xaml\Menu\*` (MenuFlyout/JumpList) / `Tray\Xaml\Tooltip\*` (Start/Search ToolTip); отсутствие value → fallback на `Tray\Xaml\*`. AppContainer: snapshot `Local\GlassPlatform.TrayXamlPolicy.v4` (v3→v4: +20 байт tooltip slot, 88→108 байт).

Reload: `kReloadRenderFlagTaskbarXaml` → `UICompat_ReloadSettingsAndReapply` (`UpdateSettings` / Reapply). Visual style: `+ kReloadRenderFlagUiThemeBrushRefresh` → `UICompat_RefreshBrushesAfterThemeChange` (без ClearValue). Спящий процесс не обслужит семафор — при следующем SHOW Attach → reapply читает свежий map (**eventual consistency**, отдельный wake на reload не добавляем).

Перед первым патчем `ShellSurfacePanelRegistry` получает индексы свойств из `IVisualTreeService3::GetPropertyValuesChain`, сохраняет исходные значения через `GetProperty` + удерживаемый `IInspectable`. `chromeCaptured` ставится **только** если захвачено обязательное свойство (для `AppBorder`/`AcrylicBorder` — Background); иначе capture повторяется, Apply фона не выполняется. Restore — WinRT с captured `IInspectable` (тот же канал, что Apply). Опасные WinRT-геттеры свойств и `ClearProperty` здесь не использовать.

## Attach / first frame

1. Хост видит SEH/SMEH/SearchHost (часто **Paused** = PLM suspend) или ShellHost.
2. PLM-freezable: `EnableDebugging` + `Resume` → Sleep-probe → remote LoadLibrary. ShellHost — без PLM thaw.
3. Bootstrap / DoInitialize: UICompat eager; **GlassCore skip**.
4. `WM_GLASS_COREWINDOW_ATTACH_EXISTING` / OOC SHOW → `CoreWindowHost_Attach` (cloaked OK). Attach на **SHOW/FOREGROUND** (Create для `ControlCenterWindow` слишком ранний).
5. NC (SEH/ShellHost): `UICompat_NotifyCoreWindow` → UI-thread IXDE (`RunFromWindowThread`, как ShellSurface) → TAP → VTC + catch-up на UI-потоке (`Window::Current` валиден).  
   Start/Search: `UICompat_NotifyShellSurfaceWindow` → UI-thread IXDE → TAP → VTC.

Скрывать surface (`SW_HIDE` / `DWMWA_CLOAK`) **запрещено**.

## PLM / unload

| Действие | API | Заметки |
|----------|-----|---------|
| Thaw | `IPackageDebugSettings::EnableDebugging` + `Resume` | SEH + SMEH + SearchHost/SearchApp |
| Rest после inject | держать EnableDebugging до stop | иначе PLM снова freeze |
| Rest после unload | `DisableDebugging` **только** если DLL снята | `Suspend` при stillLoaded замораживает FreeSelf |
| ShellHost | без PLM pause/thaw | residual: Unhook → WM_NULL wake |
| ForceUnload remote UICompat_Shutdown | thaw + `IsRemoteProcessSchedulable` (≤150 ms); не schedulable → **skip** remote (restore на path=1 StopEvent) | не раздувать бюджет 20s×N |

Windhawk injects at process create (CREATE_SUSPENDED + APC) — без PLM cycle. Glass — remote LoadLibrary в уже живущий процесс → нужен thaw.

## Shutdown UICompat

На CoreWindow-хостах:

1. `UICompat_Shutdown` (Unadvise panels, stop TAP).
2. `FreeLibrary(UICompat)` без долгого `CoFreeUnusedLibraries`.
3. При residual — `TryDropIxdeResidualModule`.
4. GlassCompat FreeSelf path=1 (GUI) **без** ожидания HostShutdown.

## Anti-patterns

- Root_Attach / SWCA на CoreWindow / ControlCenterWindow (держать exempt в `kHardList`).
- GlassCore в SEH/ShellHost/SMEH/SearchHost.
- `TerminateProcess` целевых процессов на stop.
- Remote LoadLibrary в PLM-frozen процессе без thaw.
- `Suspend` пакета до завершения FreeSelf.
- Hide/cloak surface ради «первого кадра».
- Ждать HostShutdown в GUI FreeSelf path=1 ([GlassCompat-unload.md](GlassCompat-unload.md)).
- FrameworkUdk / Microsoft.UI.Xaml для этих HWND — стек UWP WUX.
- Поднимать IXDE для Start/Search с чужого потока / без фильтра tid (регрессия H49).
- Субклассировать CoreWindow ради marshal (только `WH_CALLWNDPROC` / PostMessage).
- `Sleep(500)` в Advise / «подождать дерево» — late custom после uncloak; использовать tall HWND + короткий CatchUp (50 ms).
- Подмена EffectType Acrylic→Blur «ради первого кадра».
- ClearValue-Recapture на UICompat Reload (общий TaskbarXaml) — только TaskbarCompat; UICompat = UpdateSettings/Reapply.
- Вешать `UiThemeBrushRefresh` на GS commit / обычный XAML reload — theme force-rebrush только с host visual-style debounce.
- IXDE с 1×1 `CoreWindow` sibling в ShellHost или с `ControlCenterWindow` нулевого размера.
- `keepExistingBg` на `MediaTransportControlsRegion` — shell overwrite залипает; Region всегда переустанавливает brush.

## Связанный код

| Путь | Роль |
|------|------|
| `UICompat/xaml/NotificationCenterPanelRegistry.*` | NC apply/restore |
| `UICompat/xaml/NcMenuFlyoutPanelRegistry.*` | NC/QS/Start/Search MenuFlyout apply/restore |
| `UICompat/xaml/JumpListPanelRegistry.*` | Jump List chrome apply/restore |
| `UICompat/xaml/ShellSurfacePanelRegistry.*` | Start/Search apply/restore |
| `UICompat/xaml/ShellTooltipPanelRegistry.*` | Start/Search ToolTip apply/restore (VTS3 углы для SMEH) |
| `UICompat/xaml/SearchWebViewPanelRegistry.*` | Search WebView2 context menu CSS apply/restore |
| `UICompat/xaml/UiVisualTreeWatcher.*` | обнаружение Grid/Border |
| `UICompat/xaml/XamlDiagInit.*` | Notify NC / ShellSurface / IXDE |
| `WndCompat/WinUiBridge/CoreWindowHost.*` | Attach без subclass |
| `GlassCompat/src/CoreWindowHostInject.cpp` | remote inject / PLM / ForceUnload |
| `GlassShared/.../RootBackdropPolicy` | host-process helpers / `kHardList` |
| `GlassShared/include/GlassMessages.h` | `WM_GLASS_COREWINDOW_*` |
