# GlassSettings — белая вспышка при старте (журнал попыток)

Контекст: в тёмной системной теме Windows 11 (в т.ч. 24H2) окно `GlassSettings.exe` при старте на ~1 с показывается белым, затем окрашивается по теме. WinUI-острова следуют `AppsUseLightTheme`; Win32/`COLOR_WINDOW` у части сценариев остаётся светлым — см. обсуждение платформенного бага (отдельная задача).

**Статус:** попытки устранения вспышки только в GlassSettings **откачены** (2026-06-08). Код возвращён к состоянию до экспериментов. Документ фиксирует матрицу, чтобы не повторять неудачные ветки.

См. также: [GlassSettings-development.md](GlassSettings-development.md), [GlassSettings-visual-styles.md](GlassSettings-visual-styles.md).

---

## Исходное поведение (до экспериментов)

| Элемент | Поведение |
|---------|-----------|
| `App.OnLaunched` | `ApplyApplicationThemeAtStartup()` → `new MainWindow()` → `Activate()` |
| `MainWindow` ctor | `ApplyAppPreferencesFromRegistry()` (тема + Mica), `ApplyChromeBackgrounds()` (прозрачные слои), loading overlay с темой в `ApplyLoadingOverlayTheme()` |
| `TitleBarThemeHelper` | DWM immersive dark по `ActualTheme` корня |
| GlassCore в host | `NativeHostInterop.EnsureGlassRuntime()` в ctor; пользовательский тест без GlassCore/WndCompat — вспышка **сохранялась** |

---

## Матрица опробованных вариантов

| # | Подход | Суть | Результат | Примечание |
|---|--------|------|-----------|------------|
| 1 | Solid bootstrap XAML | Непрозрачный `#202020` / `#F3F3F3` на `RootGrid`, `WindowBlurSourceHost`, `LoadingOverlay` до Mica | Вспышка осталась | Прозрачные слои в ctor всё равно включались позже |
| 2 | `TitleBarThemeHelper` по реестру | `AppThemeVisualResolver.ResolveIsDarkMode()` вместо `ActualTheme` | Вспышка осталась | DWM caption мог быть верным, клиент WinUI — нет |
| 3 | Явный `RequestedTheme` | `WindowThemeBootstrap`: не оставлять `ElementTheme.Default` при System | Вспышка осталась | |
| 4 | Отложить прозрачный chrome | `ApplyTransparentChrome` при снятии loading overlay, не в ctor | Вспышка осталась | |
| 5 | `ProcessUxThemeBootstrap` | `AllowDarkModeForApp` (133), `SetPreferredAppMode` (135), `RefreshImmersiveColorPolicy` (104) до HWND | Вспышка осталась | В `App()` и `OnLaunched` |
| 6 | DWM caption colors | `DWMWA_CAPTION_COLOR`, `DWMWA_TEXT_COLOR` + attr 19/20 | Вспышка осталась | |
| 7 | `PreShowWindowService` + `SW_SHOW` до `Activate` | Показ до активации | Ухудшение / вспышка | Убрано |
| 8 | `WindowEraseBkgndGuard` | Подкласс `WM_ERASEBKGND`, заливка HWND | Вспышка осталась | WinUI рисует поверх composition |
| 9 | Отложить Mica | `SystemBackdrop` не в ctor; после show / loading | Пустая рамка или вспышка | Зависит от комбинации |
| 10 | Полупрозрачный SWCA bootstrap | `ACCENT_ENABLE_ACRYLICBLURBEHIND` на скрытом HWND | **Пустая «дырка»** | Desktop просвечивает |
| 11 | Непрозрачный SWCA gradient | `ACCENT_ENABLE_GRADIENT`, `#FF202020` | **Костыльный фон**, не Mica/Acrylic | Пользователь: «не относится ни к Mica, ни Acrylic» |
| 12 | `SW_HIDE` → подготовка → `SW_SHOW` | Win32-паттерн инициализации до показа | Вспышка / пустая рамка | WinUI не композитит содержимое на скрытом HWND |
| 13 | `CompositionTarget.Rendering` на `SW_HIDE` | 2 кадра композиции до `SW_SHOW` | **Пустая рамка несколько секунд**, затем краш | Рендер на скрытом окне не готовит UI |
| 14 | `ActivateWhenReady` | Отложенный `Activate` после layout | Задержка, дырка, краши | |
| 15 | Разделение `ApplyPlacement` | Geometry в ctor, Chrome после `SW_SHOW` | **COMException 0x8007139F** | `PreferredHeightOption` до `Activate()` |
| 16 | Chrome после `Activate` | `ApplyPlacementChrome` в `OnFirstActivated` | Краш устранён, UX хуже | В сочетании с SWCA — пустое окно |
| 17 | Отложить `ExtendsContentIntoTitleBar` | Title bar extension после первого кадра | Не решило вспышку | |
| 18 | `WinUIDesktopWin32WindowClass` в exempt | `RootBackdropPolicy::kHardList` | Не влияло на вспышку | Откатано вместе с остальным |
| 19 | Исключить GlassCore/WndCompat | Host без native glass | **Вспышка сохранилась** | Причина не в GlassCore attach |

**Легенда результата:** «Вспышка осталась» — кратковременный белый кадр при старте в тёмной теме; «Пустая рамка» — видимая оболочка без содержимого/фона; «Краш» — `COMException 0x8007139F` на `AppWindow.TitleBar`.

---

## Выводы для следующих попыток

1. **Не полагаться на отрисовку WinUI на `SW_HIDE`** — `CompositionTarget.Rendering` на скрытом HWND не заменяет показ; даёт задержку и пустую рамку.
2. **Не использовать полупрозрачный SWCA** на host HWND как bootstrap — «дырка»; непрозрачный gradient — чужой визуал, не Mica.
3. **`AppWindow.TitleBar.*` только после `Activate()`** — иначе `0x8007139F`.
4. **GlassCore/WndCompat в host не корень вспышки** — искать в WinUI startup / `SystemBackdrop` / первом кадре compositor.
5. Перспективные направления (не пробовались до отката): splash screen в package manifest; кастомный `Window` subclass на нативной стороне; показ только после первого **видимого** `Rendering` с уже применённым Mica; исследование `Microsoft.UI.Xaml` host HWND child bridges.

---

## Solid bootstrap + DWM cloak → splash-first show (2026-07-20)

**Симптом (до правки):** цепочка «непрозрачное → Mica → заставка → снова Mica»; затем пустой кадр до заставки. Причины: `ApplyTransparentLayers` до `SystemBackdrop` при `LoadingOverlay` ActualSize `0×0`; показ окна до layout splash.

**Рабочий подход (не #9 / #12 / не TOOLWINDOW):**

| Шаг | Где | Что |
|-----|-----|-----|
| 1 | `ApplyStartupVisuals` | theme + `ApplySolidBootstrapLayers` + opaque splash + icon |
| 2 | `ApplyEarlyWindowPlacement` | размер + **park (−32000)** через `SetWindowPos` + `AppWindow.Move` |
| 3 | `ActivateCloakedUntilSplashReady` | cloak → `Activate` → repark + reassert |
| 4 | cold-start | TitleBar → backdrop+transparent **только** при overlay size &gt; 0 |
| 5 | reveal | splash ready → Move на место **под cloak** → 3× `Rendering` → uncloak |
| 6 | dismiss | **без смены порядка:** Capture → RegWatch → Collapsed → `MarkReady` |

Файлы: [`StartupChromeService.cs`](../../GlassSettings/Services/StartupChromeService.cs), [`MainWindow.xaml.cs`](../../GlassSettings/MainWindow.xaml.cs), [`App.xaml.cs`](../../GlassSettings/App.xaml.cs).
*(ранее: `StartupWindowCloakService.cs` — удалён вместе с two-window cold-start)*

**Отвергнуто runtime-доказательствами:** transparent/backdrop при overlay `0×0`; uncloak сразу после Move (пустой solid ~1 с до paint); `WS_EX_TOOLWINDOW` / `ITaskbarList`; полагаться только на `AppWindow.Position` без `GetWindowRect`/`SetWindowPos`.

Backup: `.claude/backups/2026-07-20_0906/`.

---

## StartupSplashWindow + hidden MainWindow (2026-07-20)

**Симптом:** кнопка taskbar при park (−32000); видимая заставка MainWindow коротко (~1 с), пока долго строится chrome.

**Подход (паттерн WinUI Gallery child `Window`):**

| Шаг | Где | Что |
|-----|-----|-----|
| 1 | `App.OnLaunched` | `StartupSplashWindow` Activate (**DesktopAcrylic**, transparent root, иконка) → 3× Rendering |
| 2 | затем | MainWindow ctor/Activate: `IsShownInSwitchers=false` + cloak + park |
| 3 | cold-start | chrome/nav/Ensure под splash; **без** раннего reveal |
| 4 | dismiss | Collapsed overlay (off-screen) → Move/uncloak → close splash → Low post-dismiss hooks |

**Runtime (session 8ae958):** `fallback-2s` вызывал reveal **до** dismiss → splash закрывался, MainWindow с overlay. Исправлено: fallback только `ScheduleDismiss` на 8 с; reveal только из dismiss.

**Runtime #2:** splash acrylic → через ~1 с opaque; Main «неготовый». Причина: `SystemBackdrop` на MainWindow @~650 ms гасит acrylic splash (H9). Исправлено: backdrop Main только в `FinishStartupUncloak`; `MarkReady`/WarmAttach до/после swap без «догоняющих» notices.

**Runtime #3:** splash мигает opaque→acrylic→opaque. (1) Activate до готовности acrylic; (2) `backdrop-applied`+`CloseQuietly` в один ms. Исправлено: splash cloak→acrylic→uncloak; Main backdrop **после** `CloseStartupSplash`.

**Runtime #4:** без мигания, но splash всегда opaque. Solid bootstrap под cloak + ранний uncloak — DesktopAcrylic не просвечивает. Исправлено: Transparent с ctor; cloak warmup 8 кадров; re-apply acrylic при uncloak; Main после 3 кадров settle.

**Runtime #5:** acrylic OK → opaque после Main.Activate; Main без backdrop ~1.3 с (`Close` до `backdrop-applied`). Исправлено: splash = AcrylicBrush без SystemBackdrop; Main backdrop в cold-start; EnsureBackdrop **до** Close splash.

**Runtime #6:** AcrylicBrush splash выглядит opaque (тема fill). Снова DesktopAcrylic; Reapply после Main.Activate; defer Main backdrop; swap = Hide (не sync Close).

**Runtime #7 (H15):** Gallery-splash на отдельном `DispatcherQueue` → **COMException**, splash не создаётся. WinUI `Window` только на потоке Application.

**Runtime #8 (H16):** Gallery-splash на потоке App; Main после 4× Rendering; defer Main backdrop; Reapply splash после Main.Activate.

**Runtime #9 (H20):** `DesktopAcrylicBackdrop` в логах есть, визуал solid gray/black — inactive window fallback после `Main.Activate`. `RestoreForeground`/`SetForegroundWindow` **не** возвращали фокус (user proof).

**Runtime #10 (H21):** Main с `WS_EX_NOACTIVATE`; всё равно `Window.Activate()` ставит fg=Main (лог).

**Runtime #11 (H22):** Main через `AppWindow.Show(false)` / `SW_SHOWNOACTIVATE` **без** `Activate`; bootstrap вручную. Splash — без caption buttons (`SetBorderAndTitleBar`).

Файлы: [`StartupSplashWindow.cs`](../../GlassSettings/Services/StartupSplashWindow.cs) (удалён), [`App.xaml.cs`](../../GlassSettings/App.xaml.cs), [`MainWindow.xaml.cs`](../../GlassSettings/MainWindow.xaml.cs).

Backup: `.claude/backups/2026-07-20_1015-splash-window/`.

---

## Single-window splash на Main (2026-07-20)

**Симптом:** заметный переход splash→Main (swap двух окон); Gallery-splash сам по себе без визуальных багов.

**Подход:** одно `MainWindow` — splash на том же HWND:

| Шаг | Где | Что |
|-----|-----|-----|
| 1 | `App.OnLaunched` | `new MainWindow()` → `ActivateAsSplashSurface()` |
| 2 | Activate | DWM cloak → `Activate` → solid overlay + icon → layout |
| 3 | first paint | transparent + uncloak + `IsShownInSwitchers` (кнопка taskbar с видимой заставкой) |
| 4 | cold-start | NavView `Opacity=0` (не Collapsed — иначе нет `HomePage.OnLoaded`); Ensure под splash |
| 5 | dismiss | prep под splash → overlay Collapsed → `SyncLoadingChrome(false)` → fade-in shell 500 ms → `MarkReady` |

**Контракт dismiss (не ломать):**

1. Под заставкой (`SyncLoadingChrome(true)`): Capture / RegWatch / WarmAttach / wallpaper preview.
2. `LoadingOverlay.Collapsed` — клики по overlay съедаются (`Handled`); визуал shell включается, **hit-test shell остаётся off** до конца fade.
3. Fade shell → `ArmShellInputAfterSplash` (hit-test on) → `ShellContentReadyGate.MarkReady` → стартовое green slide-in.
4. Не включать hit-test при Collapsed: иначе отложенные клики со splash доезжают как навигация с Главной (`GlassDrill`).

**DWM open animation:** для WinUI `AppWindow` системная анимация открытия **не** идёт штатным путём (`ShowWindow` → false). Вместо неё: WinUI fade `RootGrid` 0→1 при uncloak (`WindowRevealFadeInMs`); после dismiss — fade shell (`ShellContentFadeInMs`). Не Hide→Show / TOOLWINDOW.

Файлы: [`App.xaml.cs`](../../GlassSettings/App.xaml.cs), [`MainWindow.xaml.cs`](../../GlassSettings/MainWindow.xaml.cs), [`StartupChromeService.cs`](../../GlassSettings/Services/StartupChromeService.cs), [`StartupWindowCloakService.cs`](../../GlassSettings/Services/StartupWindowCloakService.cs).

Backup: `.claude/backups/2026-07-20_1127-single-window-splash/`.

---

## Cold start preload (2026-06-28)

Под loading overlay:

| Preload | Сервис | Overlay |
|---------|--------|---------|
| Иконки AttachList | `ProcessIconResolver.PreloadAttachListAsync` | ждёт до **600 ms** |
| Обои (все preview) | `DesktopWallpaperImageCache.BeginLoad` — **до** `NavigateTo`, не блокирует overlay | без ожидания |
| Blur «Композиция» (HKCU) | `CompositionWallpaperLayersCache.BeginPrewarm` — после dismiss, **только при** `OpenGlassUiPolicy.IsFeatureExposedInUi()` | без ожидания |

Минимальная длительность заставки — **800 ms** (`LoadingMinimumMs`). После dismiss — `WarmAttachListPageIfReady()` и composition blur prewarm (`Low`). Единый decode **340 px** + raw bytes: [`DesktopWallpaperImageCache`](../../GlassSettings/Services/DesktopWallpaperImageCache.cs) — Обзор, Визуальные стили, Композиция, Стилизация, **Текст и шрифты**; blur pipeline — [`CompositionWallpaperLayersCache`](../../GlassSettings/Services/CompositionWallpaperLayersCache.cs) (Vista+Win7, sync apply на cache hit). Crossfade при смене обоев: [GlassSettings-wallpaper-preview-crossfade.md](GlassSettings-wallpaper-preview-crossfade.md).

**Инвалидация blur cache:** смена обоев → invalidate + re-prewarm; accent / external DWM sync → invalidate без фонового prewarm.

---

## Откат (2026-06-08)

Восстановлено из `.claude/backups/2026-06-08_1521/` (и `App.xaml.cs` из `1525`):

- `GlassSettings/App.xaml.cs`
- `GlassSettings/MainWindow.xaml.cs`
- `GlassSettings/Services/TitleBarThemeHelper.cs`
- `GlassSettings/Services/WindowThemeBootstrap.cs`
- `GlassSettings/Services/AppThemeService.cs`
- `GlassShared/src/RootBackdropPolicy.cpp`

Удалены добавленные файлы:

- `StartupChromeService.cs`
- `ProcessUxThemeBootstrap.cs`
- `PreShowWindowService.cs`
- `WindowEraseBkgndGuard.cs`
- `WindowStartupShowService.cs`
