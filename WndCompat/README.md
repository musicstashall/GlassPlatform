# WndCompat

**Связанный код:** `WndCompat/`

### Ключевые подсистемы WndCompat

- **WndHandler** — начальный скан существующих окон через `WH_GETMESSAGE` + `WM_GLASS_ATTACH_ROOT`; Attach/Detach по событиям из GlassCompat
- **WindowClassifier** — `ControlKind::WinForms_*` и `ControlKind::Atl_*` / `Wtl_*` отделяют альтернативные имена класса от нативных (в `WinEventHandler` — отдельные `case`, те же `*_Attach`); нестабильный суффикс `ATL:`+hex и прочий непрошитый шум ATL/WTL → `Ignored` (без контейнера 5.5 / `Root_Attach`). Сценарий MMC chart — по родителю `ChartGraphClass` и `wvc` в `RootWindowHooks`, не через произвольные `ATL:+hex`. Поиск `ChartGraphClass` вверх по родителям запоминается на HWND (`GlassPlatform.ChartMemoParent` / `ChartMemoResult`); смена непосредственного родителя сбрасывает память.
- **RootSubclassProc** — субкласс root-окон: DWM blur, WM_ERASEBKGND, WM_CTLCOLOR*
- **ComboLBoxSubclassProc** — субкласс ComboLBox: WM_PAINT, WM_MOUSEMOVE, WM_VSCROLL, WM_MOUSEWHEEL, WM_KEYDOWN
- **ComboBoxSubclassProc** — субкласс ComboBox: WM_LBUTTONDOWN → AnimateWindow hook
- **ControlTracker** — Attach SubclassProc для Button, Edit, Static, ListView, ToolBar, Rebar, DirectUI
- **UiDispatcher** — thread-local диспетчер, гарантирует выполнение в GUI-потоке

**Именование исходников WndCompat — явное правило (`*Host.cpp` vs `*Subclass.cpp`):**
- **`*Host.cpp`** — окно **без** `SetWindowSubclass`: только `GlassMarkWindow` / учёт HWND / вызовы GlassCore по событию (пример: `ScrollBarHost.cpp`, `IeServerHost.cpp`).
- **`*Subclass.cpp`** — есть субкласс и своя логика в `SubclassProc` / `DefSubclassProc`.
Старые файлы в репозитории могут ещё называться иначе; **новые** модули и постепенное выравнивание — строго по этому разделению.

### Кросс-поточное снятие субклассов

`RemoveWindowSubclass` **обязательно** вызывать из потока-владельца окна. Для этого определены специальные сообщения:

| Сообщение | Значение | Кто обрабатывает |
|-----------|----------|-----------------|
| `WM_GLASS_DETACH_ROOT` | `WM_APP + 0x202` | `RootSubclassProc` |
| `WM_GLASS_DETACH_COMBOLVOX` | `WM_APP + 0x203` | `ComboLBoxSubclassProc` |
| `WM_GLASS_DETACH_COMBOBOX` | `WM_APP + 0x204` | `ComboBoxSubclassProc` |

При shutdown: `*_DetachAll()` проверяет поток и для чужих окон вызывает `SendMessageTimeoutW(SMTO_ABORTIFHUNG, 1000ms)`.

> **Реестр сообщений:** `GlassShared\include\GlassMessages.h` — единственный файл, где объявляются `WM_APP`-сообщения платформы. Никогда не объявляй такие константы локально в других файлах.

### WndCompat — на что обращать внимание

- **Режим Full — шлюз attach контролов:** `WndCompat::GlassAttachGate::ShouldAttachGlassToHwnd` (`Tracker/GlassAttachGate.*`). Для `HWND` смотрится корень `GetAncestor(..., GA_ROOT)`: если класс корня в списке exempt backdrop (`Shared::RootBackdropPolicy`), поддерево помечается в `Shared::GlassHostRegistry` как без Glass (`MarkSubtreeGlassDisabled` / `IsSubtreeGlassDisabled`) и attach для **обычных** контролов под этим корнем блокируется. Режимы не Full — шлюз всегда пропускает (без реестра).
- **Исключение — popup списка комбобокса:** окно класса **`ComboLBox`** не наследует этот запрет: для него `ShouldAttachGlassToHwnd` сразу возвращает `true` (список дропдауна привязан к сценарию комбобокса, а не к классу top-level root вроде Rainmeter). Единственный запрет на уровне процесса — нет инжекции / `ProcessBlockList` в GlassCompat, а не шлюз WndCompat.
- **WinUI host HWND (ExplorerCompat-only):** `Shared::ExplorerCompatHostPolicy` — классы `DesktopChildSiteBridge`, `InputSiteWindowClass`, … обрабатываются WndCompat **только** в `explorer.exe`. **GlassSettings** WndCompat не грузит. См. [ExplorerXamlIslands.md](../ExplorerCompat/docs/ExplorerXamlIslands.md).
- **Shell LightDismiss (Notification Center):** `Shell_LightDismissMessageWindow` / `Shell_LightDismissOverlay` — в `RootBackdropPolicy::kHardList` (как `Shell_InputSwitch*`); не Root attach, не SWCA/DWM.
- `RemoveWindowSubclass` вызывать **только с потока-владельца окна** — для чужих потоков использовать `WM_GLASS_DETACH_*` через `SendMessageTimeoutW`
- Порядок DetachAll при shutdown: `ComboBox_DetachAll` → `ComboLBox_DetachAll` → `Root_DetachAll`
- Провайдеры (`GetDropDownEventProvider`, `GetRootEventProvider`) проверять на `nullptr` **и** на `IsShuttingDown()` перед вызовом
- `ControlTracker_AttachIfNeeded` субклассирует контролы без DetachAll — при shutdown сабклассы снимаются через `WM_NCDESTROY` каждого контрола
- **InternetExplorer_Server**: `IeServer_Attach` / `Detach` / `DetachAll` — только учёт HWND + `GlassMarkWindow` / `GlassUnmarkWindow` (субкласс и `WM_APP` для IE не используются); снятие маркера в `EVENT_OBJECT_DESTROY` и в `IeServer_DetachAll` при shutdown.

## Документация

| Задача | Документ |
|--------|----------|
| Flyout / PopupIn (GlassCore) | [GlassCore/docs/FlyoutBorderPaintOrder.md](../GlassCore/docs/FlyoutBorderPaintOrder.md) |
| Live OFF Root backdrop | [GlassSettings/docs/RootBackdropDisableSequence.md](../GlassSettings/docs/RootBackdropDisableSequence.md) |
| Explorer XAML hosts | [ExplorerCompat/docs/ExplorerXamlIslands.md](../ExplorerCompat/docs/ExplorerXamlIslands.md) |
| Выгрузка / detach | [GlassCompat/docs/GlassCompat-unload.md](../GlassCompat/docs/GlassCompat-unload.md) |
