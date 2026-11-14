# BWM, Acrylic и перенос в Glass

**Связанный код:** GlassCore/src/Hooks/, доноры вне репозитория

### Опорные проекты (доноры)

D:\VS_PROJECT\BlackWindowManager
Это решение BlackWindowManager, который нам служит в качестве примеров реализаций. В состав GlassProject не входит, не компилируется, редактированию не подлежит, только чтение!
BWM\DetoursHooks.cpp — это модуль для тестов, он не используется в реализациях отрисовки.
BWM\mmcHooks — модуль практически не используется, представляет собой Detours хуки.
BWM\Restorator... и WSB... — это специфичные модули для отрисовки особых окон.

### Acrylic / ExplorerCompat (XAML Diagnostics, панель задач)

Для **XAML Diagnostics** и сценариев «прозрачный слой под панелью задач» в **explorer.exe** ориентиры — **не** BWM `WinHandler.cpp` (там нет TAP/XAML), а проекты Acrylic:

- **`D:\VS_PROJECT\Acrylic\TranslucentTB-release\ExplorerTAP\`** — эталон: `tapsite.cpp` (`InitializeXamlDiagnosticsEx`), `visualtreewatcher.cpp` (привязка `DesktopWindowXamlSource` → контент, только `Rectangle` с именами `BackgroundFill` / `BackgroundStroke` под `TaskbarFrame`).
- **`D:\VS_PROJECT\Acrylic\ExplorerBlurMica-main\`** — заголовки/направление dual-stack UWP vs WinUI (`InitializeUWPXamlDiagnostics` / `InitializeWinUIXamlDiagnostics` и т.п. в духе `ExplorerBlurMica`).
- **`D:\VS_PROJECT\Acrylic\FileBrowsWindowPatch-master\`** — ориентир патча полос проводника (WinUI): `TAPSite`, `VisualTreeWatcher` с отдельным потоком для `AdviseVisualTreeChange`. **Контраст по скроллбару:** в FileBrows кастомизация GDI-полосы идёт через подмену `DrawThemeBackground` (`PaintScroll` / класс темы `ScrollBar`). В Glass — **другой путь**: IAT на **comctl32** (`DrawThemeBackground` + `BitBlt`), ленивая установка из `PaintDispatcher::HookGetDCEx` при caller **comctl32** и HWND в whitelist / `GlassIsComctlScrollHost` (маркер из `Root_Attach` в Full); политика темы — `ScrollBarDrawTheme`, узел — `ScrollBarThemeHooks.cpp` (без GDI+). Подробнее — `docs\ScrollbarRendering.md`.

**ExplorerCompat в explorer.exe — выгрузка:** **WndCompat** после `WndCompat_BeginShutdown` не держит долгоживущих COM-объектов вне процесса, поэтому `FreeLibrary` обычно снимает DLL. **ExplorerCompat** подключает TAP через `DllGetClassObject` / `InitializeXamlDiagnosticsEx`; WinUI может удерживать `IUnknown` на объектах диагностики после `Unadvise`, и тогда `DllCanUnloadNow` остаётся `S_FALSE` — модуль «застревает» так же, как раньше мог WndCompat при иных утечках, но причина другая (COM оболочки, не забытые субклассы). Принудительное снятие утилитами официально не поддерживается; после корректного Stop на практике часто безопасно, без гарантии на все сборки explorer. При неудаче `SendMessageTimeout` на `HWND_MESSAGE` — `DBG_CRITICAL`. После Stop — инвалидация островов WinUI и шкафов (`RedrawWindow` и т.п.); на `CabinetWClass`/`ExploreWClass` — дополнительно `RedrawWindow` + `SetWindowPos(..., SWP_FRAMECHANGED)` без принудительного DWM API (`DwmExtendFrameIntoClientArea` в ExplorerCompat не вызывается). `HWND_BROADCAST` / `WM_THEMECHANGED` в этом сценарии не используются.

**ExplorerCompat (XAML / WinUI острова) — текущее состояние:** описание архитектуры TAP/XAML Diagnostics, патчинга и ограничений восстановления chrome после Stop вынесено в [ExplorerCompat/docs/ExplorerXamlIslands.md](../../ExplorerCompat/docs/ExplorerXamlIslands.md). При доработках XAML‑части сначала сверяйся с этим документом.

**Бордюр панели задач:** два слоя (XAML top strip + DWM rounded frame), общая политика `NoBorderColor` / `BorderColor`. См. [TaskbarCompat/docs/TaskbarBorders.md](../../TaskbarCompat/docs/TaskbarBorders.md).

**GlassSettings.exe (окно настроек):** WinUI 3 + `NavigationView`, detail-страницы и hosted Win32-диалоги — см. [GlassSettings-WinUI.md](GlassSettings-WinUI.md), [GlassSettings-development.md](GlassSettings-development.md).

**BWM LightMode_* / DarkMode_* (GlassSettings, TaskbarCompat):** выбор имени ключа — **`GetSysColor(COLOR_WINDOW) < 0xFFFFFF`** (только `0xFFFFFF` → `LightMode_*`). API: `Shared::ThemeHelper::IsRegistryDarkModeFromColorWindow()`, запись — через реестр BWM в GlassSettings. **Не** `0x808080`, **не** `ShouldAppsUseDarkMode()` для ветки реестра (паритет `RootEvents` / `DropDownHooks`).

### BWM и Glass: куда класть логику перехвата (важно для ассистентов)

В **BWM** нет IATRegistry: один и тот же перехватчик (например `OverallHooks::MyBitBlt`, `MyRectangle`) часто обрабатывает **много разных** окон и контролов через длинные цепочки `if` по классу окна, модулю caller’а и контексту DC. Это «толстые» реализации.

В **GlassProject** наоборот: **каждая группа сценариев** живёт в **своём** файле `GlassCore\src\Hooks\*Hooks.cpp` (и связанных модулях), а несколько диспетчеров на одном IAT-слоте стыкуются через **IATRegistry** (single-occupancy стек: в слоте всегда один detour — первый активный, остальные ждут без переустановки). Тело хука должно быть **узким**: проверить только свои условия (свой класс / свой модуль / свой контекст) и иначе **сразу** вызвать сохранённый настоящий оригинал — без копирования чужих веток из BWM в этот файл.

**Как читать BWM `PaintHandler.cpp` при переносе:**

1. **Разделить путь:** внутри обработчика DC/paint это обычно ветки **`ctrl` vs `window`** («контрол» vs «окно»), а также отдельные функции вроде **`Dc`** vs **`CompatibleDc`** vs сценарий BeginPaint — условия и включаемые хуки **разные**. Нельзя слепо переносить кусок из одной ветки, ожидая, что он покроет другую.
2. **Сопоставить с Glass:** логика **root-окон** (то, что в BWM относится к «window» и вешается на корень/диалог без привязки к конкретному классу коммон-контроля) обычно идёт в **`RootWindowHooks.cpp`** (`ClassifyRootWindow`, `OnRootHooksAttach`). Логика **конкретного контроля** (`SysListView32`, `ToolbarWindow32`, ComboLBox, Button, …) — в **соответствующем** `*Hooks.cpp` и провайдерах PaintDispatcher, а не в Root.
3. **Один API — несколько файлов:** если в BWM один `MyXxx` обслуживает и ListView, и MFC toolbar, в Glass это могут быть **разные** хуки на том же экспорте (разные `HookDispatcher` / модули), связанные стеком IATRegistry (в слоте — первый активный). **Не** расширять чужой `*Hooks.cpp` «чуть-чуть» ради root-сценария, если политика проекта — держать root отдельно в `RootWindowHooks.cpp` (и наоборот — не складывать специфику ListView в Root).

4. **Не смешивать хуки контролов между файлами:** у каждого контрола — **своё** тело хука и **свой** `HookDispatcher` в **своём** `GlassCore\src\Hooks\*Hooks.cpp`. **Запрещено:** вызывать публичные API одного контрольного модуля из другого (например `ListViewControl::…` из `ToolbarHooks.cpp` или экспорт вспомогательных функций из `ScrollBar` в `ListViewHooks` ради порядка отрисовки), проверять в чужом файле чужой `thread_local`-стек сессии (`g_lvSessionStack`, `g_modStack` и т.д.). Если два контрола патчат **один и тот же** IAT одного модуля (частый случай — `sysmon.ocx`), добавляется **вторая независимая** реализация в **своём** `*Hooks.cpp` и своём `EnableHooksOnModule`; occupant'а слота задаёт **только IATRegistry** (first-owner-wins: первый активный регистрант; при «не мой сценарий» → настоящий `orig`).

Кратко: **из BWM переносится не вся функция целиком, а только та ветка, которая относится к целевой группе классов/контекста; место в коде Glass определяется модульностью проекта, а не структурой файла в BWM.**

См. [hooks-and-paint.md](hooks-and-paint.md).
