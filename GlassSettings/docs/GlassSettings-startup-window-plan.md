# GlassSettings — старт окна без костылей: живой акрил в первом кадре (план)

**Статус:** план на согласование. Код не изменён — правки начнутся только после «ок» по этапам (см. [contributing/workflow.md](../../contributing/workflow.md)).
**Цель:** окно создаётся и показывается штатно (`new MainWindow()` → один `Activate()`), без park/cloak/hidden-from-switchers/solid-bootstrap/двух fade, и **первый же кадр содержит живой backdrop** (Mica/Acrylic) — так, как это сделано в [WinUI Gallery](https://github.com/microsoft/WinUI-Gallery).
Журнал предыдущих попыток и отвергнутые ветки: [GlassSettings-startup-white-flash.md](GlassSettings-startup-white-flash.md). Связанное: [GlassSettings-WinUI.md](GlassSettings-WinUI.md), [GlassSettings-development.md](GlassSettings-development.md).

---

## 1. Что сейчас в коде (branch-база `01a4736`)

| # | Шаг | Где | Что делает |
|---|-----|-----|-----------|
| 1 | Тема приложения | `App.OnLaunched` (`AppThemeService.ApplyApplicationThemeAtStartup`, App.xaml.cs:40) | `Application.RequestedTheme` до первого окна — корректно, оставить |
| 2 | Конструктор | `MainWindow()` → `ApplyEarlyWindowPlacement()` (MainWindow.xaml.cs:538, 3517) | `AppWindow.Resize` + `AppWindow.Move` + `SetPresenter(Default)` + min-size через `OverlappedPresenter` |
| 3 | Конструктор | `ApplyStartupVisuals()` (567) | `ExtendsContentIntoTitleBar`, тема корня, **solid-Bootstrap overlay**, backdrop+transparent, `SyncLoadingChrome(true)`, иконка |
| 4 | Splash-activate | `ActivateAsSplashSurface()` (300) | `EnsureLoadingOverlaySolidBootstrap` → backdrop → **`DWMWA_CLOAK=1`** → `IsShownInSwitchers=false` → `Activate()` → «reapply» backdrop |
| 5 | Reveal | `ScheduleSplashFirstPaintReveal` → `FinishSplashFirstPaintReveal` (380, 429) | ждёт layout overlay+icon и **2 кадра `CompositionTarget.Rendering`**, затем overlay transparent, **`RootGrid.Opacity=0` → uncloak → fade 400 мс** (`WindowRevealFadeInMs`) |
| 6 | Cold start | `ScheduleColdStartAfterPaintFrames(8)` (340, 724) → `RunColdStartBootstrapCore` | навигация/страницы/Presence/`EnsureGlassRuntime` — **через 8 кадров после activate** |
| 7 | Dismiss | `TryDismissLoadingOverlay` (1085) + `DismissLoadingOverlayWhenReadyAsync` (2583) | минимум `LoadingMinimumMs=800`, `WindowBlurSourceHost.Opacity=0` → overlay Collapsed → **fade shell 500 мс** → `MarkReady`; hit-test off до конца fade |
| 8 | Fallback | `StartupDismissFallbackAsync` (466) | 8 с — страховочный dismiss |

**Вывод 1 — «за пределами экрана» в коде нет.** `SetWindowPos(−32000)`/park остались только в журнале (раздел «Solid bootstrap + DWM cloak → splash-first show», шаг 2). Единственный `AppWindow.Move` — в `ApplyEarlyWindowPlacement`, и он идёт на **целевые** координаты.

**Вывод 2 — видимый «старт не с того места» объясняется состоянием реестра, а не кодом.** `SavePlacement()` (3546) пишет `AppWindow.Position` как есть, `WindowPlacementStore.LoadOrDefault()` валидирует только `r>l && b>t`. Если хотя бы один запуск завершился в эпоху park (или на отключённом мониторе), в
`HKCU\Software\StanislavKa\GlassPlatform\GlassSettings\MainWindowPlacement`
лежит RECT с −32000 — и окно стартует «за экраном», а «возврат на место» делают уже шаги после показа. Это **гипотеза, которую проверяет Э0** (дамп значения ключа + координаты в трассе), но лечится она в любом случае одинаково: валидацией placement (Э4), а не cloak/park-механикой.

**Вывод 3 — активный костыль состоит из 5 механик:** cloak, `IsShownInSwitchers`, solid-bootstrap overlay, «reapply backdrop», два opacity-fade плюс счётчики кадров (2 и 8). Все они — обходные пути вокруг одной вещи: *backdrop/контент ещё не готовы в момент, когда окно стало видимым*.

---

## 2. Механика первого кадра в WinUI 3 (первоисточники)

Всё ниже — из `microsoft/microsoft-ui-xaml` (открытая часть движка, `dxaml/xcp/**`), актуальная ветка `main`.

| # | Факт | Где в источниках | Следствие для GlassSettings |
|---|------|------------------|------------------------------|
| F1 | HWND WinUI-окна создаётся **в конструкторе `Window`**, сразу: `RegisterDesktopWindowClass()` + `CreateDesktopWindow()`; класс регистрируется с `hbrBackground = COLOR_WINDOW+1`, окно создаётся с `WS_OVERLAPPEDWINDOW \| WS_VISIBLE`, но `nCmdShow = SW_HIDE` | `dxaml/xcp/dxaml/lib/DesktopWindowImpl.cpp` | «Скрыто до Activate» — штатное поведение фреймворка. Никакого park/Move-в-тень не нужно: окно и так не видно до `Activate()` |
| F2 | `ActivateImpl()` = применит placement фреймворка → `ShowWindow(SW_SHOW)` → `UpdateWindow` → `SetActiveWindow` | там же | `Activate()` — единственная точка, где окно становится видимым **и активным**. Всё, что должно быть в первом кадре, обязано быть готово до неё |
| F3 | `WM_ERASEBKGND` фреймворк обрабатывает сам: заливает клиент **непрозрачным** `GetHwndBackground(theme)` = `0xffffffff` (light) / `0xff000000` (dark), theme — из `ActualTheme` контента, иначе базовая тема приложения | `DesktopWindowImpl::OnMessage`, `FrameworkTheming::GetHwndBackground` | Сквозь эту заливку акрил не виден **в принципе**; она существует ровно до первого present острова. Пытаться «покрасить её в акрил» (прошлые #8/#10/#11) — путь в никуда; задача — чтобы к этому моменту был контент |
| F4 | Для desktop-окна остров по умолчанию **непрозрачен**: `SetHasTransparentBackground(false)`; в `true` переключает только `put_SystemBackdrop` → `SetXamlIslandRootBackground`, который одновременно вызывает `Theming_TryEnableHostBackdropBrush(hwnd)` («точка захвата host-backdrop для DesktopAcrylic») | `DesktopWindowImpl::OnCreate`, `DesktopWindowImpl::SetXamlIslandRootBackground` | Живой backdrop = (а) `Window.SystemBackdrop` присвоен, (б) остров прозрачный, (в) host-backdrop включён. Присвоение `null` (или смена объекта) возвращает непрозрачность → серая «пробка» |
| F5 | `put_SystemBackdrop` подключает цель **синхронно**, даже если `Content` ещё не задан: `XamlIsland::UpdateSystemBackdrop` → `SystemBackdrop.OnTargetConnected` (там же создаётся `DesktopAcrylicController`/`MicaController` и вешается `SystemBackdropConfiguration`) | `XamlIsland_Partial.cpp`, `controls/dev/Materials/DesktopAcrylicBackdrop/DesktopAcrylicBackdrop.cpp` | «Готовить backdrop к первому кадру» тривиально: присвоить один раз как можно раньше (в ctor / в XAML). **Повторное присваивание = disconnect+reconnect** — это и есть источник «миганий opaque→acrylic→opaque» из журналов (Runtime #3/#5) |
| F6 | Вид материала определяется политикой окружения: `SystemBackdropState ∈ {Active, Fallback, HighContrast}`, конфигурацию фреймворк держит по `Theme` / `IsInputActive` / `IsHighContrast` | `docs/api-specs/XamlBackdrop/xaml-backdrop-api.md` | **Неактивное/скрытое окно рисует Fallback — сплошной серый.** Т.е. cloak + `IsShownInSwitchers=false` + попытки `SW_SHOWNOACTIVATE`/`WS_EX_NOACTIVATE` сами гарантируют именно тот кадр, от которого они спасают (ср. Runtime #9/#10/#11) |
| F7 | У фреймворка есть собственное сохранение позиции (`Window.PlacementId`, контейнер `Microsoft.UI.Xaml.WindowPlacement`, применение внутри первого `Activate()`, запись по `WM_DESTROY`/`WM_ENDSESSION`); для приложений **без package identity** авторазмещение пропускается | `DesktopWindowImpl::ApplyInitialWindowPlacement`, `docs/design-notes/window-placement.md` | У GlassSettings identity нет → фреймворк placement не трогает; вся ответственность — на `WindowPlacementStore` (см. Вывод 2 и Э4) |

---

## 3. «Три разных серых»: что именно видно в первом кадре

Лечить надо не «серость вообще», а конкретный источник. Отличаются цветом и моментом.

| Кандидат | Цвет-ориентир | Кто рисует | Дискриминирующий признак | Чем снимается |
|----------|---------------|------------|---------------------------|---------------|
| H1. Заливка `WM_ERASEBKGND` | чистый `#FFFFFF` / `#000000` | WinUI (F3) | видна **до** первого present острова; пропадает, если контент построен до `Activate()` | порядок запуска (Э3), а не окно-трюки |
| H2. Акрил в состоянии Fallback | `#2C2C2C`/`#F3F3F3`-подобный, «матовое стекло» | DWM/backdrop-контроллер (F6) | совпадает с неактивным/клоакированным окном; исчезает при обычном `Activate()` | убрать cloak/`IsShownInSwitchers`/NOACTIVATE (Э2) |
| H3. Собственный solid-bootstrap overlay | `SolidBackgroundFillColorBaseBrush` `#202020` | код приложения (`EnsureLoadingOverlaySolidBootstrap`) | совпадает по цвету с фоном shell; виден ровно `LoadingMinimumMs` | удалить solid-слой, overlay сделать прозрачным (Э1/Э2) |
| H4. Прозрачный остров без backdrop | «дырка» в desktop | F4 при `SystemBackdrop=null` | desktop просвечивает там, где должен быть фон | не присваивать `null` после setup (Э1) |
| H5. Двойной Move/Resize | скачок геометрии, а не цвет | `AppWindow.Move/Resize` в ctor + `SetPresenter(Default)` | кадр верный, но окно «дёргается» | placement через `Window.Width/Height` и один источник координат (Э4) |

**Измерить, а не угадывать:** прежде чем менять хоть что-то, фиксируем baseline (Э0) — по кадрам и по меткам времени.

---

## 4. Как это устроено в WinUI Gallery (проверено по репозиторию)

| # | Факт из `microsoft/WinUI-Gallery` | Смысл |
|---|-----------------------------------|-------|
| G1 | `<Window.SystemBackdrop><MicaBackdrop /></Window.SystemBackdrop>` — в **XAML** корня `MainWindow.xaml` | backdrop присваивается один раз внутри `InitializeComponent()`, т.е. до любой отрисовки; никакого рантайм-`put` и никакого re-apply (F4/F5) |
| G2 | `RootGrid` — `Grid` **без `Background`** | прозрачность, сквозь которую и видно материал; «solid bootstrap» в репозитории отсутствует как класс |
| G3 | `MainWindow()` ctor: `InitializeComponent(); SetWindowProperties();`, где `SetWindowProperties` = `ExtendsContentIntoTitleBar`, `SetTitleBar`, `AppWindow.SetIcon`, `AppWindow.TitleBar.PreferredHeightOption = Tall`, `MinWidth/MinHeight` | chrome **до** Activate, включая `PreferredHeightOption` — т.е. пункт #15 журнала (`COMException 0x8007139F`) на текущем WASDK требует перепроверки, а не вечного обхода |
| G4 | Размер — через `Window.Width/Height` (читаются из `ApplicationData.LocalSettings` в ctor), позиция — на совести системы/фреймворка | нет `AppWindow.Move` в обход фреймворка, нет «сначала Resize, потом ещё раз Resize» |
| G5 | `App.OnLaunched`: `new MainWindow()` → `await ControlInfoDataSource…`/`IconsDataSource…` → `AddNavigationMenuItems()` → `ThemeHelper.Initialize()` → `Navigate(HomePage)` → **`MainWindow.Activate()`** | **один** `Activate()`, в самом конце, когда контент уже построен. Тяжёлые данные грузятся *до* показа окна |
| G6 | Тема — `ThemeHelper.Initialize()` (прокидывает `RequestedTheme` корня до Activate); caption-кнопки — в `RootGrid_Loaded` | разрешение темы происходит до первого кадра, а «мелочи, требующие Loaded» — после |
| G7 | Нет ни splash-окна, ни cloak, ни fade, ни «reapply backdrop», ни счётчиков кадров | весь наш костыль — следствие того, что порядок G5 не соблюдён, а не свойства WinUI |

**Ключевая переформулировка задачи:** цель не «покрасить первый кадр акрилом», а **не показывать кадр, в котором ещё нечего показывать**: backdrop — из XAML/ctor (G1), контент — до `Activate()` (G5), окно — активное сразу (F6). Тогда первый кадр = настоящий shell над живым акрилом, и все 5 механик становятся ненужными.

---

## 5. Что должно получиться (целевая картина)

```
App.OnLaunched
  ├─ тема приложения (как сейчас)                      [~0 мс]
  ├─ new MainWindow()
  │    ├─ InitializeComponent()      → backdrop из XAML, прозрачный RootGrid
  │    ├─ placement: Width/Height/Position (один раз)
  │    ├─ chrome: ExtendsContentIntoTitleBar, TitleBar, PreferredHeightOption, icon
  │    └─ BuildNavigation() + NavigateToRoot(HomePage)   [бюджет ≤ 150 мс]
  └─ window.Activate()                                  ← единственная точка показа
после первого present (Rendering ×1):
  ├─ EnsureGlassRuntime, Presence/watchers, preload иконок, обои, composition-blur
  └─ dismissal «загрузка» → снимается как обычный визуал контента
```

Никаких: cloak, `IsShownInSwitchers`, `RootGrid.Opacity`, solid-bootstrap, «подождать N кадров», повторного присвоения backdrop, fallback-таймеров на 8 с.

---

## 6. Этапы работ

Каждый этап — отдельный коммит + push в ветку сессии, с проверкой на машине пользователя (Windows 11 x64 Debug; по возможности также x86). Откат этапа = revert одного коммита.

### Э0 — Baseline и инструментирование (без изменения поведения)

| Что | Детали |
|-----|--------|
| `StartupPhaseTrace` | по образцу `ShutdownPhaseTrace`: метки `theme`, `hwnd-created`, `backdrop-set`, `content-ready`, `activate-in/out`, `first-rendering`, `shell-ready`, `dismiss`; `+ms` и дельта |
| Зонды | в метках дополнительно: `DwmGetWindowAttribute(DWMWA_CLOAKED)`, `AppWindow.IsVisible`, `WindowActivationState`, `Window.SystemBackdrop?.GetType().Name`, `IsShownInSwitchers` |
| Покадровый захват | `.claude/tmp/gs-startup-capture.ps1` (после задачи удалить, см. [repo-hygiene.md](../../contributing/repo-hygiene.md)): цикл BitBlt/`CopyFromScreen` ~60 fps, 2 с, PNG-серия + монтаж в одну полосу; позволяет различить H1–H5 по цвету кадра |
| Результат этапа | таблица «кадр → цвет → источник» на текущей сборке; фиксация длительностей (activate→первый present) |

Критерий: воспроизведённый журнал из ≥3 прогонов (holod/тёплый старт, x64/x86). Откат: не нужен.

### Э1 — Контракт backdrop: «присвоен один раз, меняется внутри объекта»

1. Объявить backdrop в `MainWindow.xaml` (`<Window.SystemBackdrop>`) — как G1; режим (Mica / MicaAlt / Acrylic / none) читается **до** `InitializeComponent()` (в `App.OnLaunched`) и кладётся в статический хелпер, либо — если нужна точность до байта — задаётся в ctor **первой строкой после** `InitializeComponent()` (это тот же момент, что и XAML: F5 подтверждает синхронный connect).
2. Новый `GlassWindowBackdrop : SystemBackdrop` (`GlassSettings/Services/`): владеет `MicaController`/`DesktopAcrylicController` (один экземпляр на экземпляр backdrop), создаёт их в `OnTargetConnected`, в `OnDefaultSystemBackdropConfigurationChanged` обновляет `Theme`, а **переключение Mica↔Acrylic делает сменой контроллера внутри уже подключённой цели** — без второго `put_SystemBackdrop`. Плюс явный `FallbackColor` = тон темы, чтобы даже Fallback не выглядел дефектом.
3. Удалить: `WindowBackdropService.ApplyFromRegistry/ApplyMode/ApplyMica/ApplyAcrylic/ApplyDesktopAcrylicFallback` в текущем виде (свести к «дать экземпляр backdrop для режима»), `MainWindow.ReapplyWindowBackdropIfChanged`, `ApplyMainWindowBackdropAndTransparent`, `ApplyTransparentSplashChrome`, `EnsureSplashBackdropAndTransparent`, `TryApplyBackdropAndTransparentWhenOverlayReady`, `OnLoadingOverlaySizeChangedForStartupChrome`, `_appliedWindowBackdropMode`, `_loadingOverlaySizeHooked`.
4. `StartupChromeService.ApplySolidBootstrapLayers` и весь solid-путь — удалить; `EnsureLoadingOverlaySolidBootstrap` — удалить; `AcrylicBackdropLayer` (всегда Collapsed) — удалить из XAML вместе с `WindowBackdropService.HideAcrylicLayer`.
5. **Запретить** `SystemBackdrop = null` в рантайме (H4) — в том числе при «нет Mica на Win10»: нет поддержки → тот же объект с `FallbackColor`, а не `null`.

Риск: in-app acrylic (`AcrylicInAppFillColorDefaultBrush` в pane/диалогах/зеркалах) при прозрачном острове смешивается «неправильно» (это прямо описано в комментарии F4). Already-known mitigation — явные кисти (`WindowBackdropService.GetNavigationOverlayPaneBrush`, `ApplyAcrylicInAppChromeBackground`) остаются и только усиливаются.
Критерий: в трассе ровно одно `backdrop-set`; при смене режима в приложении окно не мигает; `DWMWA_CLOAKED` не используется.

### Э2 — Демонтаж оконных трюков

Удалить за ненадобностью (после Э1):

| Объект | Действие |
|--------|----------|
| `Services/StartupWindowCloakService.cs` | удалить файл (единственный потребитель — `ActivateAsSplashSurface`) |
| `ActivateAsSplashSurface()` (300) | удалить; его содержимое распадуется: «иконка+overlay» → в ctor, «Activate» → в `App.OnLaunched` |
| `HideFromSwitchersDuringSplash` / `ShowInSwitchersAfterSplash` / `_startupHiddenFromSwitchers` | удалить (taskbar-кнопка появляется штатно вместе с окном) |
| `ScheduleSplashFirstPaintReveal` / `OnSplashFirstPaintRendering` / `SplashFirstPaintFallbackAsync` / `FinishSplashFirstPaintReveal` / `_splashFirstPaintReady` / `_splashWarmupFramesLeft` | удалить |
| `RootGrid.Opacity=0` + `BeginOpacityFadeIn(RootGrid, WindowRevealFadeInMs)` | удалить (400 мс «полупрозрачной дырки» — это часть артефакта, а не полировка) |
| `AllowActivateMessageFromLowerIl` | **оставить** — это про UIPI/WM_APP из ShellExt, к flash отношения не имеет |
| `StartupDismissFallbackAsync` (8 с) | оставить как чистый safety dismiss overlay, отвязав от reveal |

Отдельно: `Program.cs` существует только под `#if DEBUG` (в Release — генерируемый `Main`). Привести к одному входу на обеих конфигурациях (или явно задокументировать разницу), иначе «штатный старт» будет различаться Debug/Release и замеры Э0 поедут.

### Э3 — Порядок запуска как в Gallery (G5)

1. **До** `Activate()`: `BuildNavigation()`, `ApplyLocalizedStrings()`, `NavigateToRoot(typeof(HomePage))`, `SyncLoadingChrome` больше не нужен для сокрытия shell — shell виден с первого кадра.
2. **После** первого `CompositionTarget.Rendering` (один кадр, не восемь): `RunColdStartBootstrapCore` → `EnsureGlassRuntime()`, Presence/watchers, `ProcessIconResolver.PreloadAttachList*`, `DesktopWallpaperImageCache.BeginLoad`, `RecentWallpaperThumbCache.BeginLoad`, `RunDeferredStartupTasks()`.
3. Контракты, которые **сохраняем как есть** (они про данные и ввод, а не про flash):
   * `ShellContentReadyGate.MarkReady()` — после готовности Home, до стартового green-notice (см. [GlassSettings-notices.md](GlassSettings-notices.md));
   * порядок «Capture → RegWatch» в `RunDeferredStartupTasks` и `_ = ProductUpdateCoordinator.RunStartupAsync()`;
   * x86-ограничения (см. комментарии в `RunColdStartBootstrapCore`): PreNav → Navigate → PostNav, `StackReserveSize` уже 4 МБ;
   * `IsHitTestVisible`-гейт больше не нужен (окно не видно «неготовым»), но оставляем `_shellInputArmed` как no-op-флаг до подтверждения тестами.
4. LoadingOverlay: превращаем в обычный **прозрачный** индикатор внутри контента (иконка + лёгкий shimmer поперёк живого backdrop), с `LoadingMinimumMs`, сокращённым до 250–350 мс, и без управления видимостью shell. Если Э0 покажет, что shell строится < 150 мс, overlay вообще можно свести к «скелету» и снять шаг 3 полностью.
5. Убрать fade shell (`BeginShellContentFadeIn`, `ShellContentFadeInMs`), если проверка подтвердит отсутствие «сырых» кадров; при желании оставить как чистую дизайнерскую анимацию 150–200 мс — это уже не обходной путь.

Бюджет и приёмка: `activate-in → first-present` ≤ 100 мс на x64; окно появляется не позже, чем сейчас (сейчас — сразу, но «серым»; после Э3 — сразу и «готовым»).

### Э4 — Геометрия, chrome и «самолечение» placement

| Задача | Решение |
|--------|---------|
| Убрать двойную раскладку (H5) | размер — через `Window.Width/Height` (G4) при доступных new-windowing API; `AppWindow.Resize/Move` — только fallback для сборок без них (F7/`E_NOTIMPL`); `SetPresenter(Default)` из ctor удалить как no-op |
| Мин-размер | `Window.MinWidth/MinHeight` (как в Gallery) вместо `OverlappedPresenter.PreferredMinimum*`; `EnforceMinWindowSize` оставить как страховку на Win10 |
| Координаты вне мониторов | в `WindowPlacementStore.LoadOrDefault()` валидировать RECT по `MonitorFromRect`/work area всех экранов, при промахе — центр primary + clamp; `SavePlacement()` — не писать заведомо «заэкранный» RECT. Это снимает «старт за экраном» без всякого park и лечит уже накопленный мусор |
| `PreferredHeightOption` в ctor | эксперимент Э4 по G3: проверить на x64/x86, Win10/Win11. Если `0x8007139F` не воспроизводится — пункт #15 журнала объявить устаревшим и убрать `ApplyTitleBarPlacementOptions` из «после Activate»; если воспроизводится — оставить как есть (это не влияет на flash) |
| `TitleBarThemeHelper` | применять в ctor до Activate (там же, где иконка), с сохранением кэша `_lastDark`, чтобы не переприсваивать атрибуты |

### Э5 — Закрепление

* Обновить [GlassSettings-startup-white-flash.md](GlassSettings-startup-white-flash.md): раздел «Solid bootstrap + DWM cloak → splash-first show» пометить как *superseded*; пункты #9/#12/#13/#16 — «снято сменой порядка запуска»; добавить «Правило: первый кадр обеспечивается порядком `content → Activate`, а не состоянием окна».
* Новый раздел анти-паттернов в этом документе + ссылка из README GlassSettings и из [GlassSettings-development.md](GlassSettings-development.md).
* Метки `StartupPhaseTrace` оставить (они дороги в ~0 и нужны при регрессии).

---

## 7. Если после Э1–Э3 останется 1–2 кадра прокраски (Э6, по возрастанию цены)

| Вариант | Суть | Ожидаемый эффект | Риск |
|---------|------|------------------|------|
| E6-A | Принудить DWM-материал на HWND до показа: `DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE, DWMSBT_MAINWINDOW/TRANSIENTWINDOW)` (+ при необходимости `DwmExtendFrameIntoClientArea(-1)`), вызов — из `GlassSettings.Native.dll` сразу после создания HWND; WinUI-`SystemBackdrop` при этом — «пустой» подкласс (нужен лишь для прозрачности острова, F4) | DWM сам композитирует материал, независимо от готовности swap-chain → материал есть на кадре 0 | требует Win11 22000+ (у нас `TargetPlatformMinVersion 10.0.17763`) → обязательный downlevel-путь; конфликт с in-app acrylic |
| E6-B | `WS_EX_NOREDIRECTIONBITMAP` на HWND → GDI-заливка H1 становится невидимой | убирает именно «erase»-кадр | неподдерживаемый фреймворком сценарий (в WinUI есть ветки, которые явно смотрят на этот стиль); проверять на x86 и Win10 обязательно |
| E6-C | `RootGrid.UpdateLayout()` + `Window.Content` уже задан (XAML) до `Activate()`, чтобы первый present был осмысленным | уменьшает окно H1 до ≤1 кадра | лишняя синхронная layout-цена в ctor; измерить в Э0 |
| E6-D | Splash как отдельное окно | — | **не делать**: проходили (Runtime #1–#11), две swap-chain'ы, `COMException` на чужом DQ (H7), «swap» виден; к Gallery-модели отношения не имеет |
| E6-E | Разрешить платформенное стекло для собственного окна (SWCA из GlassCore) | «живой» акрил «как у всех» | **запрещено политикой**: `GlassSettings.exe` в `ProcessBlockList`, самоинжект GlassCompat не выполняется; `WinUIDesktopWin32WindowClass` в `RootBackdropPolicy::kHardList` — осознанно |

Порядок решений: сначала E6-C (безопасно), затем E6-A (нативно, с gate по OS), E6-B — только если и после этого кадр H1 ловится.

---

## 8. К чему не возвращаться (уже опровергнуто runtime-доказательствами)

`SW_HIDE → подготовка → SW_SHOW` (#12), `CompositionTarget.Rendering` на скрытом HWND (#13), полупрозрачный `ACCENT_ENABLE_ACRYLICBLURBEHIND` на host-HWND (#10), непрозрачный SWCA-gradient как «bootstrap» (#11), `WS_EX_TOOLWINDOW`/`ITaskbarList`, показ окна раньше готовности контента (#7), «догоняющее» переприсваивание `SystemBackdrop`. Общее объяснение провалов — F4/F5/F6: все они либо делают окно неактивным (Fallback), либо трогают подключённый backdrop (disconnect/reconnect).

---

## 9. Карта изменений по файлам (сводка этапов)

| Файл | Э1 | Э2 | Э3 | Э4 |
|------|----|----|----|----|
| `GlassSettings/MainWindow.xaml` | `Window.SystemBackdrop` (объявление), удалить `AcrylicBackdropLayer`; `RootGrid` без фона | — | `LoadingOverlay` → прозрачный индикатор | — |
| `GlassSettings/MainWindow.xaml.cs` | убрать `ReapplyWindowBackdropIfChanged`, `ApplyMainWindowBackdropAndTransparent`, `ApplyTransparentSplashChrome`, `EnsureSplashBackdropAndTransparent`, `TryApplyBackdropAndTransparentWhenOverlayReady`, `EnsureLoadingOverlaySolidBootstrap` | убрать `ActivateAsSplashSurface`, cloak/switchers-методы, `ScheduleSplashFirstPaintReveal`, `FinishSplashFirstPaintReveal`, `RootGrid`-fade | порядок cold start: nav + navigate до `Activate()`, остальное после 1-го `Rendering`; снять shell-гейты видимости | `ApplyEarlyWindowPlacement` → `Window.Width/Height`; `SetPresenter(Default)` из ctor убрать |
| `GlassSettings/App.xaml.cs` | — | `new MainWindow()` → один `Activate()` | тяжёлую подготовку — до `Activate()` | — |
| `GlassSettings/Services/StartupWindowCloakService.cs` | — | **удалить** | — | — |
| `GlassSettings/Services/StartupChromeService.cs` | solid-путь удалить | — | — | — |
| `GlassSettings/Services/WindowBackdropService.cs` | свести к «выдать экземпляр backdrop» | — | — | — |
| `GlassSettings/Services/GlassWindowBackdrop.cs` | **новый** (`SystemBackdrop`-подкласс: контроллеры Mica/Acrylic внутри, `FallbackColor`) | — | — | — |
| `GlassSettings/Services/StartupPhaseTrace.cs` | **новый** (Э0, по образцу `ShutdownPhaseTrace`) | — | — | — |
| `GlassSettings/Core/Placement/WindowPlacementStore.cs` | — | — | — | валидация RECT по монитору, миграция «отравленного» значения |
| `GlassSettings/Program.cs` | — | единый вход для Debug/Release | — | — |
| `GlassSettings/docs/GlassSettings-startup-white-flash.md` | — | — | — | пометка *superseded* (Э5) |

## 10. Критерии приёмки

1. В коде нет `StartupWindowCloakService`, `IsShownInSwitchers`, solid-bootstrap-слоя, счётчиков кадров reveal; `Activate()` вызывается ровно один раз на окно.
2. Первый различимый кадр (по серии снимков Э0): live-акрил + shell (pane + заголовок + контент или скелет), **не** `#FFFFFF`/`#000000`/`#202020`/матовый Fallback.
3. Нет скачка геометрии: `Move/Resize` — один раз, координаты валидны по монитору; «старт за экраном» отсутствует даже при намеренно испорченном `MainWindowPlacement` (negative-тест: записать −32000 → окно открывается в центре primary).
4. Смена Mica/Acrylic в настройках приложения — без мигания и без «дырки» в desktop.
5. Анимация открытия окна — системная (DWM), без собственного opacity-fade.
6. `activate-in → first-present` ≤ 100 мс (x64) и ≤ 250 мс (x86) по `StartupPhaseTrace`; dismiss overlay — без 8-секундных fallback-срабатываний в норме.
7. Регрессии нет: tooltips (acrylic), hosted-диалоги и штора, notices (green slide-in после `MarkReady`), live-reload из «Композиции»/«Стилизации», x86 cold start без AV.

---

## 11. Что нужно от пользователя, чтобы начать

1. **Splash-политика:** оставляем прозрачный in-content загрузчик (максимум близко к текущему UX, но без управления окном) или идём «в лоб» как Gallery — окно сразу с shell, всё догружается на месте?
2. **Бюджет до `Activate()`:** сколько мс допустимо «двойной клик по ярлыку → окно появилось» (моё предложение: ≤ 150 мс, и тогда часть холодно-стартовых вещей уходит до показа).
3. **Win10 (17763+) в приоритете?** От ответа зависит, идём ли в Э6-A (нативный `DWMWA_SYSTEMBACKDROP_TYPE`) и нужны ли `Window.Width/MinWidth` с `E_NOTIMPL`-ветками.
4. Готовность принять, что `AGENTS.md`-запреты (#10/#11, self-glass) остаются в силе — подтверждение, что «нативный DWM-путь» рассматриваем только как Э6, а не как основной.

---

## 12. Статус реализации (2026-11-25)

Состояние: **код изменён, не собран** — сборка и прогоны возможны только на Windows
(`msbuild GlassProject.sln /p:Configuration=Debug /p:Platform=x64`, артефакты `Build\x64\Debug\`).

### Что сделано (Э1–Э4 + Э0)

| Что | Файлы |
| --- | --- |
| `GlassWindowBackdrop : SystemBackdrop` — один объект на всё время жизни окна: `OnTargetConnected` создаёт контроллер и подключает цель; смена режима (`TryApplyMode`) пересоздаёт **только контроллер**; явный `FallbackColor` по теме; `SystemBackdrop` никогда не обнуляется, пока материал доступен | `Services/GlassWindowBackdrop.cs` (новый) |
| `AttachStartupBackdrop` (один раз, до `Activate()`) + `ApplyModeFromRegistry` (runtime, без переприсваивания) вместо `ApplyFromRegistry` / `ApplyMode` / `ApplyMica` / `ApplyAcrylic` / `ApplyDesktopAcrylicFallback` / `HideAcrylicLayer`; in-app chrome-хелперы сохранены без изменений | `Services/WindowBackdropService.cs` |
| Удалены: `StartupWindowCloakService.cs`, `StartupChromeService.cs`, `ActivateAsSplashSurface`, `EnsureLoadingOverlaySolidBootstrap`, весь «догоняющий» backdrop (`ReapplyWindowBackdropIfChanged`, `ApplyMainWindowBackdropAndTransparent`, `ApplyTransparentSplashChrome`, `EnsureSplashBackdropAndTransparent`, `TryApplyBackdropAndTransparentWhenOverlayReady`, `OnLoadingOverlaySizeChangedForStartupChrome`), reveal-машинерия (`ScheduleSplashFirstPaintReveal` + 3 обработчика + счётчики), switchers-toggle, `RootGrid`/`WindowBlurSourceHost` opacity-fade, `ScheduleColdStartAfterPaintFrames(8)`; `AcrylicBackdropLayer` убран из XAML | `MainWindow.xaml(.cs)`, `App.xaml.cs` |
| `LoadingOverlay` → прозрачный in-content индикатор: без фона, `LoadingIcon` + `ProgressRing`; при снятии — fade-out самого overlay (220 мс) и `MarkReady`; **в видимостью окна не связанные**; минимум видимости 800→300 мс; ожидание decode иконки из dismiss убрано (оно обслуживало reveal) | `MainWindow.xaml`, `MainWindow.xaml.cs` |
| Один `Activate()` (`ActivateAtStartup`) из `App.OnLaunched` (после `App.ShellWindow = window` вызывается `PrepareShellForStartup()` — HomePage/`NoticeViewportMirrorService` читают `App.ShellWindow` во время навигации); до него — тема, `ExtendsContentIntoTitleBar`/`SetTitleBar`, DWM dark, backdrop, placement, `AppWindow.SetIcon`, nav+`NavigateToRoot` (порядок Gallery); после него — `AppWindow.TitleBar.*`/`PreferredHeightOption`, Presence/watchers, Ensure, `Capture→RegWatch`; `ApplyPlacement` больше не двигает окно повторно | `App.xaml.cs`, `MainWindow.xaml.cs` |
| `WindowPlacementStore`: rect обязан пересекаться с виртуальным экраном (иначе — центр рабочего пространства primary), `Save` не закрепляет «заэкранный» RECT | `Core/Placement/WindowPlacementStore.cs` |
| Safety-dismiss: `StartupDismissFallbackAsync` (8 с) и `LoadingOverlayFallbackAsync` (8 с) слиты в один `LoadingOverlayFallbackAsync` — он же поднимает `_navigationReady`/`_shellInteractiveReady` и снимает индикатор, если pipeline завис | `MainWindow.xaml.cs` |
| `StartupPhaseTrace.Begin/Mark/MarkWindow` — трасса фаз + снимок `visible/switchers/cloaked/fg/backdrop/size/pos` в ctor:exit, activate:begin/end, first-present, cold-start-core, titlebar-chrome, indicator-dismiss | `Services/StartupPhaseTrace.cs` (новый) |

### Отклонения от плана

- `Window.SystemBackdrop` объявлен не в XAML, а в конструкторе (`ApplyStartupChrome` →
  `WindowBackdropService.AttachStartupBackdrop`): режим читается из реестра, поэтому тип
  материала нельзя зафиксировать в разметке. Смысл тот же: один объект, присвоен до показа.
- `WindowBlurSourceHost` оставлен как контейнер раскладки: после удаления reveal-fade и
  solid-слоёв код к нему больше не обращается (переименование/слияние с `RootGrid` — отдельная
  чистка, к старту отношения не имеет).
- `Window.Width/MinWidth` не используем: это клиентские DIP и `E_NOTIMPL` без new windowing (17763) —
  смешение единиц на масштабированном мониторе дороже выигрыша. Оставляем `AppWindow`-геометрию (physical px),
  как в Э4 «вариант А».
- `SetPresenter(Overlapped)` из пути старта убран полностью (был `SetPresenter(Default)` в `ApplyEarlyWindowPlacement`).
- Э2 по `Program.cs` выполнен как «явно задокументировать разницу»: Debug-вход (наш `Main`)
  и Release-вход (генерируемый `Main`) не унифицированы — порядок окна от этого не зависит
  (оба пути приходят в `App.OnLaunched`), а менять публичный вход Release без проверки на Windows нельзя.
- Гейт ввода оставлен **реальным**, а не no-op (п. 3 Э3): shell виден с первого кадра, поэтому
  `IsHitTestVisible` и `LoadingOverlay`-перехват держат клики до `MarkReady` — иначе клик по ещё
  не построенной Home даёт навигацию. Отвязка от него — отдельный шаг после тестов.
- Цвета `AppWindow.TitleBar.*` и `PreferredHeightOption` оставлены после `Activate()` (поздний эксперимент
  Э4 по #15 не проведён); в ctor перенесено то, что заведомо безопасно — `ExtendsContentIntoTitleBar`,
  `SetTitleBar`, DWM 19/20, `AppWindow.SetIcon`, геометрия.
- Двухфазная инициализация `AppThemeService` (Э5) **не** делалась — тема в ctor читается один раз, это не противоречит Э1–Э4.
- Э5 (тестовая кнопка перезапуска) и Э6-A (`DWMWA_SYSTEMBACKDROP_TYPE`) отложены; Э6-B (нативный
  `DwmDefWindowProc`-хук `WM_NCHITTEST`) и Э6-C (`Window.PlacementId` как optional-путь) не делались.

### Что нужно проверить на Windows (критерии приёмки из раздела 10)

1. Cold start x64 (Mica и Acrylic) + x86: нет grey frame, нет «пустой рамки», нет скачка геометрии;
   в первом кадре — shell + живой материал.
2. `StartupPhaseTrace`: `activate-in → first-present` ≤ 100 мс (x64) / ≤ 250 мс (x86); `cloaked=0`,
   `switchers=true`, `visible=true` на метках `activate:end` / `first-present` / `indicator-dismiss` совпадают
   (то есть окно не меняло состояние после показа).
3. Смена Mica ↔ Acrylic в панели настроек и извне (`GlassSettingsPreferencesWatcher`): без мигания
   и без «opaque»-кадра; после отказа материала (`transparency` off) фон окна остаётся читаемым (solid).
4. 10× старт: `App.log` без «opaque→acrylic→opaque»; ошибок `0x8007139F` в логе старта нет.
5. Win10 17763 (или `-ForceSolidBackdrop`-подобный прогон): окно непрозрачное, без «дырки», chrome корректный.
6. Регрессии: tooltips, hosted dialogs, notices (green/yellow после `MarkReady`), повторный activate из
   ShellExt (WM_APP, elevated GS), `Closing → Capture → Dispose RegWatch`, cold start x86 без AV.
7. Placement: окно с «заэкранным» RECT в реестре открывается по центру primary; перетаскивание на
   отключённый монитор не «запечатывает» координаты (см. `WindowPlacementStore.Save`).

---

## 13. Э7 — первый кадр и индикатор по факту прогонов (2026-11-25, после Э1–Э4)

Прогон на Windows (x64 Debug) показал: трюков с окном больше нет (`cloaked=0` и `switchers=True` на всех
метках, `visible=False → True` ровно один раз на `activate:end`), но старт выглядит так:
«пустое окно без материала» → «материал + контент» → «иконка и бублик поверх уже загруженного контента».

### Замеры пользователя (трасса `StartupPhaseTrace`)

```
+245ms chrome+backdrop        +411ms activate:end   (visible=True, cloaked=0, backdrop=GlassWindowBackdrop)
+260ms placement 1444x848     +467ms titlebar-chrome
+262ms ctor:exit              +469ms cold-start-core
+380ms shell-ready            +1499ms first-present  ← +1030 мс от activate:end
                              +3357ms indicator-dismiss ← +1858 мс от первого кадра
```

### Причины (все — гонка UI-потока, а не DWM/WinUI)

| # | Причина | Доказательство |
|---|---------|----------------|
| П1 | `Activated += OnFirstActivated` (страховка, оставленная с Э2) зовёт `TryCompleteColdStartBootstrap()` **сразу на активации** → весь cold start (preloads обоев/миниатюр, `AppWindow.TitleBar.*`) ложится между `ShowWindow` и первой композицией | `titlebar-chrome`/`cold-start-core` (+467/+469) идут **раньше** `first-present` (+1499) |
| П2 | PostNav-цепочка (`PlatformPresenceService.Attach`, watchers, `EnsureGlassRuntime`, `PublishColdStartIdleSnapshot`) стартует из `HomePage.OnLoaded` на следующем pump — тоже до первого кадра. `CompositionTarget.Rendering` поднимается на UI-потоке, поэтому любой блок потока сдвигает сам кадр:material и индикатор уезжают вместе с ним | разрыв +469 → +1499; рядом `[ReflectionConfigure] shared wallpaper decode: SetSourceAsync begin/complete` |
| П3 | `ApplyAppBranding()` переприсваивал `LoadingIcon.Source` (файловый `BitmapImage`) поверх XAML-ного `ms-appx:///Assets/app.ico` → второй decode, и иконка прилетает на кадр позже остального контента | «значок появляется поверх загруженного контента» |
| П4 | Снятие индикатора привязано к данным (`maxPreviewWaitMs=800`, `_shellInteractiveReady`-поллинг, `LoadingMinimumMs`), а не к кадру → бублик живёт ~1.9 с уже над готовым контентом | +1499 → +3357 |

### Что сделано (Э7.1 — ортогонально дизайну индикатора)

1. `OnFirstActivated` удалён вместе с подпиской: `RunColdStartBootstrapCore` запускается **только** из
   очереди первого кадра. Страховка «кадр так и не наступил» — `FirstPresentFallbackAsync` (1500 мс).
2. Новая инфраструктура `RunAfterFirstPresent(Action)` + `CompleteFirstPresent(...)` (очередь, дренируется
   ровно один раз на первом `Rendering` или на fallback-таймере; зарегистрированные после — выполняются сразу).
3. PostNav/Ensure/`MarkShellInteractiveReady` из `ScheduleColdStartPostNavAfterHomeLoaded` тоже идут через
   `RunAfterFirstPresent` → порядок PreNav → Navigate → PostNav (контракт x86) сохранён, сменился только
   момент: после первого кадра, на том же Normal-pump.
4. `ApplyAppBranding` больше не перезапускает decode: если XAML-источник на месте — он и используется,
   файловый источник подключается только как восстановление после `ImageFailed` (`ApplyLoadingIconFallbackSource`).

Ожидаемо после Э7.1: `activate-in → first-present` укладывается в ~≤100 мс (x64), материал, shell, иконка и
бублик — в одном первом кадре; остаётся вопрос П4 (сколько индикатор живёт после кадра) — он закрыт выбором
варианта ниже.

### Э7.2 — открытое решение: форма индикатора (А / Б)

| Вариант | Как выглядит | Плюсы | Минусы |
|---|---|---|---|
| **А — заставка-панель** | `LoadingOverlay` = панель in-app acrylic (`AcrylicInAppFillColorDefaultBrush`) поверх области контента (`Grid.Row=1`; pane и caption — живые сразу), в центре иконка, под ней бублик; при готовности — fade-out, под ним уже собранный контент | Иконка «на заставке», как просил пользователь; данные (`DesktopWallpaperImageCache`, превью, composition blur) успевают прогреться под veil — пустых слотов не видно; material окна живой с 1-го кадра, окном по-прежнему не управляем | На ~0.3–0.8 с область контента закрыта veil (это и есть заставка) |
| **Б — только бублик** | Как сейчас: прозрачный overlay, иконка + `ProgressRing`; жёсткий потолок жизни `LoadingMaximumMs` ≈ 600–900 мс от `Activate()`, `maxPreviewWaitMs` 800 → ~250 | Максимально «честно»: контент виден с первого кадра, ничего не перекрывается | Видно, как догружаются превью/notice-слоты (ради этого когда-то и вводились ожидания) |

Оба варианта не возвращают cloak/park/solid-подложку окна и не трогают `SystemBackdrop` после `Activate()`.

### Э7.2 — реализовано (выбран вариант А + отложенный показ)

> **Пересмотрено в Э8 (раздел 14):** форма панели (вариант А) осталась, но «отложенный показ» и
> «потолок видимости» оказались вредны — панель видима с первого кадра, а Home строится после него.

Форма: `LoadingOverlay` — панель `AcrylicInAppFillColorDefaultBrush` **внутри области страницы**
(`NavigationView` → `Grid`, сосед `PageContentHost`), а не `Grid.RowSpan="2"` над всем окном: caption и pane
живые с первого кадра, материал окна не перекрыт. Внутри — значок 108 и `ProgressRing` 28. `SystemBackdrop`
окна не меняется, индикатор окном по-прежнему не управляет.

Тайминг (`MainWindow.xaml.cs`, поля `LoadingIndicator*Ms`):

| Параметр | Значение | Смысл |
|---|---|---|
| `LoadingIndicatorShowDelayMs` | 150 | `ArmStartupLoadingIndicator()` ставит таймер от `Activate()`; если shell готов раньше — панель не показывается вовсе (тёплый старт без мигания) |
| момент показа | не раньше `CompleteFirstPresent` | `RequestShowLoadingIndicator()` при `_firstPresentSeen == false` только ставит `_indicatorPendingShow`: acrylic-панель в нулевом кадре удорожала бы сам первый кадр (тот же урок, что и с PostNav) |
| `LoadingIndicatorMaxMs` | 900 | таймер запускается в `ShowLoadingIndicatorNow()`, то есть ограничивает **видимость**, а не старт; по нему — `indicator-cap` + `TryDismissLoadingOverlay()`, даже если данные не отдались |
| `LoadingIndicatorMinVisibleMs` | 90 | показанную <90 мс панель гасим без 220-мс фейда (`HoldIndicatorBrieflyAsync` + `FinishIndicatorDismiss`): короткий фейд читается как мигание |
| `maxPreviewWaitMs` | 800 → 250 | ожидание `DesktopWallpaperImageCache` / composition blur под заставкой |
| `LoadingMinimumMs` | удалён | «не моргать» теперь обеспечивает задержка показа, а не искусственный минимум |
| ожидание dismiss | дедлайн 900 мс | `while (!_shellInteractiveReady …)` ограничен временем, а не привязан к `first-present`-поллингу |

Снятие: `TryDismissLoadingOverlay()` → `FinishIndicatorDismiss()` (панель в `Collapsed`,
`LoadingRing.IsActive = false`, `ArmShellInputAfterLoading()`, `ShellContentReadyGate.MarkReady()`);
если панель не показывалась — без анимации вообще.

Отказ от отдельного хука «Home отрендерилась / загрузка обоев упала» (`CompleteStartupIndicator`,
`NoteStartupPreviewFailure`): снятие по `Loaded` HomePage вернуло бы баг «пустой слот превью» (данные ещё не
отрисованы), а сценарий сбоя уже ограничен `LoadingIndicatorMaxMs` — панель живёт ≤ 900 мс и не зависает
поверх контента. `LoadingOverlayFallbackAsync` (8 с) остаётся страховкой конвейера.

Заметка по вводу: панель лежит внутри `NavigationView`, а `SetShellInputEnabled(false)` выключает hit-test
на всём его поддереве — во время старта `IsHitTestVisible` панели ни на что не влияет, клики не доходят ни до
неё, ни до shell. Раньше это делал overlay уровня `RootGrid` (он был вне `NavView` и проглатывал клики);
теперь защиту «недоигравшие клики не доезжают как навигация» обеспечивает hit-test-off subtree, а
`LoadingOverlay_OnPointerInput` остаются на случай повтора показа панели при уже включённом вводе.

Трасса после Э7.2 обязана показать: `activate:begin → first-present` ≤ ~500 мс (цель ≤100 на x64),
затем `indicator-shown` (на тёплом старте — его отсутствие), `indicator-dismiss − indicator-shown` ≤ ~900 мс,
`backdrop:mode=… <controller>` в первых строках.

---

## 14. Э8 — первый кадр без Home (правка по трассе после Э7.2, 2026-11-25)

### Что показал прогон

```
+215ms backdrop:target-connected acrylic     +410ms activate:end  (visible=True, cloaked=0)
+222ms chrome+backdrop                        +1395ms first-present   ← +985 мс от activate:end
+238ms placement                              +1463ms titlebar-chrome
+240ms ctor:exit                              +1464ms cold-start-core
+380ms shell-ready-for-first-frame             +2327ms indicator-shown ← +932 мс ПОСЛЕ кадра
+382ms activate:begin                          +2727ms indicator-dismiss
```

Между `cold-start-core` и `indicator-shown` в логе — `[ReflectionConfigure] shared wallpaper decode:
SetSourceAsync begin/complete`. То есть Э7.1 сработал (chrome/cold-start-core переехали за кадр), но:

| # | Дефект | Причина |
|---|--------|---------|
| О1 | `activate:end → first-present` = 985 мс: «секунда пустого окна без акрила» | Home навигируется **до** `Activate()` (принцип Э4 «content → Activate»). `HomePage.OnLoaded → BuildCards()/Refresh()` и первый measure/arrange огромного дерева попадают в тот же проход, что и нулевой кадр → кадр стоит столько, сколько стоит построение Home. Material появляется только вместе с кадром, поэтому «без акрила» |
| О2 | Заставка показалась через 932 мс **после** кадра, т.е. поверх готового контента | отложенный показ висит на `DispatcherQueueTimer`, а его Tick обрабатывается только когда UI-поток освобождается; весь pre-кадровый блок (О1) его и задержал. «Отложенный показ» в такой схеме принципиально non-функционален |
| О3 | Бублик жил ещё ~400 мс поверх контента | снятие ждёт `maxPreviewWaitMs`/`_attachIconPreload` — данные, а не факт готовности страницы |

### Решение (Э8)

1. **Порядок старта изменён**: до `Activate()` — только лёгкий shell (`FinishDeferredInitializationPreNav`
   → nav-элементы, search, layout, `SelectNavItem("home")`) и **видимая с XAML** панель заставки;
   `NavigateToRoot(typeof(HomePage))` перенесён в очередь первого кадра
   (`ActivateAtStartup` → `RunAfterFirstPresent(NavigateHomeAfterFirstFrame)` → `TryEnqueue(Normal)`).
   Метка `NavigateToRoot` до `Activate` была страховкой от «пустой рамки» — теперь пустоту закрывает
   заставка, а не контент, поэтому рамка не пустая, а кадр дешёвый.
2. **Заставка видима с первого кадра**: `LoadingOverlay Visibility="Visible"`,
   `LoadingRing IsActive="True"`; `LoadingIndicatorShowDelayMs`, `_indicatorPendingShow`,
   `ShowLoadingIndicatorNow()`, `LoadingIndicatorMinVisibleMs` и `HoldIndicatorBrieflyAsync` удалены
   (О2). `ArmStartupLoadingIndicator()` оставляет только аварийный потолок `LoadingIndicatorMaxMs` = 2500 мс.
3. **Снятие — по кадру, а не по данным**: `TryDismissLoadingOverlay()` → `FadeOutIndicatorOnNextFrame()`
   (один `CompositionTarget.Rendering`, затем 220 мс фейд) — панель гаснет на кадре, на котором Home
   уже отрисована; в метку снятия добавлена длительность: `indicator-dismiss (veil Nms)`.
4. **Диагностика О1/О3**: в `HomePage.OnLoaded` добавлены метки `home:onloaded:begin`,
   `home:build:end`, `home:refresh:end`; в `MainWindow` — `home-nav:begin`/`home-nav:end`.

Ожидаемая трасса после Э8: `activate:begin → first-present` ≤ ~100 мс (дерево = caption + pane +
панель заставки), `backdrop:mode=…` рядом с `first-present`, `home-nav:begin` сразу после кадра,
`home:build:end`/`home:refresh:end` показывают реальную цену построения Home (уже под заставкой),
`indicator-dismiss (veil …)` — сразу после готовности страницы, а не через секунду.

Риск, который надо проверить глазами: pane и caption в первом кадре живые, **область страницы пуста и
перекрыта acrylic-панелью со значком** — это и есть задуманная заставка; если захотим, чтобы значок был
по центру всего окна (в т.ч. над pane), панель возвращается на уровень `RootGrid` `Grid.Row="1"` —
одно свойство в XAML, без изменения очерёдности.
