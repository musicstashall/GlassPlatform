# Архитектура платформы Glass

**Связанный код:** GlassCompat/, GlassCore/, WndCompat/, ExplorerCompat/, LogViewer/, GlassShared/, GlassSettings/

## Архитектура

```
Rundll32.exe "GlassCompat.dll",Main /start   — хост-процесс своей битности (на OS x64 — два хоста параллельно)
│
├── глобальный SetWinEventHook(WINEVENT_INCONTEXT)
│     └── ОС загружает GlassCompat.dll той же битности во GUI-процессы
│
├── StopEvent ("GlassPlatform.StopEvent.x64" / ".x86")
├── InstanceMutex ("GlassPlatform.Instance.Mutex.x64" / ".x86")
├── HostShutdown ("GlassPlatform.HostShutdown.x64" / ".x86")
├── GlassHostPid ("Local\GlassHostPid.x64" / ".x86")
└── GlassLogServer ──► LogViewer.exe (именованный канал \\.\pipe\GlassPlatform.Log) — общий

GlassCompat.dll в GUI-процессах (инжектированный режим)
│
├── GlassGuiThreadProc — GUI-поток: загружает peer-модули из корня своей DLL, ждёт StopEvent, выгружает
│     ├── GlassCore.dll    — движок хуков: IATRegistry, HookDispatcher, IAT-перехваты
│     ├── WndCompat.dll    — политика окон: SubclassProc, WndHandler, контролы
│     ├── ExplorerCompat.dll — lazy Win11: FileExplorerShellRoot (CabinetWClass/ExploreWClass)
│     └── TaskbarCompat.dll — lazy Win11: Tray-аттач (Shell_*TrayWnd) в этом PID
│         (нет DLL → soft skip через LoadOptionalModule)
│
└── GlassCompat_WinEventProc — глобальный WinEvent-коллбэк (вызывается ОС в потоке окна)
      └── WndCompat_HandleWinEvent → Root_Attach / Tray_Attach / ComboLBox_Attach / ...
            Tray → TaskbarHost_Attach → GlassCompat_EnsureTaskbarCompatLoaded
            FileExplorerShellRoot → ExplorerRoot::Attach → GlassCompat_EnsureExplorerCompatLoaded


LogViewer.exe              — UI логов: GlassLogServer + именованный канал;
                             START/STOP — только native-arch задача/хост (x64 exe → x64, x86 → x86);
                             при закрытии, если хост своей битности жив — диалог Да/Нет/Отмена

GlassShared (static lib)   — общий код: GlassPlatformTask, GlassModulePath, RegHelper, …
```

### Зависимости модулей (линковка)

Единственная допустимая статическая линковка между проектами solution для peer/host модулей — **GlassShared** (`ProjectReference` + `LinkLibraryDependencies` на static lib).

**Запрещено:**

- `ProjectReference` peer DLL друг на друга с `LinkLibraryDependencies=true`
- `AdditionalDependencies` / `#pragma comment(lib)` на `GlassCore.lib`, `WndCompat.lib`, `ExplorerCompat.lib`, `UICompat.lib`, `TaskbarCompat.lib`, `GlassCompat.lib` и аналоги

Межмодульные вызовы — только runtime: `LoadLibrary` / уже загруженный модуль + `GetProcAddress`, либо загрузка через GlassCompat (`LoadOptionalModule` / `Ensure*Loaded`).

**Допустимо:** `ProjectReference` без линковки (`ReferenceOutputAssembly=false`, без `LinkLibraryDependencies`) исключительно для build-order (пример: GlassShellExt → GlassShellHost).

Общие типы/константы/helpers — только в GlassShared. Не подключать private headers соседней DLL через `AdditionalIncludeDirectories` (legacy header-coupling у WndCompat / GlassSettings.Native к `GlassCore\include` — технический долг, не образец).

### Dual-arch (OS x64)

На OS x64 могут работать **два** хоста параллельно (раздельные mutex / StopEvent / HostPid / HostShutdown).
Задачи планировщика:

- `GlassPlatform.Win32 Autorun Task (x64)`
- `GlassPlatform.Win32 Autorun Task (x86)`

GlassSettings x64 показывает dual UI (две пары «Задача x86/x64», две строки Home). Сборка GlassSettings x86 — одна строка без меток разрядности.
Меню рабочего стола (GlassShellExt x64 на OS x64): два пункта «Включить/Остановить платформу x86|x64»; старт/стоп — только через задачу планировщика (`GlassPlatformTask::StartPlatform` / StopEvent). ShellExt x86 — один пункт без меток разрядности.
LogViewer стартует только свою битность. Reload IPC и log pipe пока **общие** (known limitation).

Установка в раздельные каталоги x86/x64 и реестр `InstallPath`: [GlassShared/docs/platform-installer.md](GlassShared/docs/platform-installer.md).

### Раскладка модулей на диске

Поиск через `GlassShared::GlassModulePath` / `GlassSettings.Core.Platform.GlassModulePath`. Плоский корень своей битности, **без** `Sys32`/`Sys64`.

| Процесс | Целевая битность | Install root |
|---------|------------------|--------------|
| GS x86 | x86 | корень GS |
| GS x64 | x64 | корень GS |
| GS x64 | x86 | `HKCU\Software\Wow6432Node\GlassPlatform\InstallPath`; нет ключа / нет `GlassCompat.dll` → модулей x86 нет |
| GlassCompat / инжект / LogViewer | native | каталог своего модуля |

Install root: каталог exe/DLL.

### Ключевые именованные объекты ядра

| Объект | Тип | Назначение |
|--------|-----|------------|
| `GlassPlatform.Instance.Mutex.x64` / `.x86` | Mutex | Синглтон хоста своей битности |
| `GlassPlatform.StopEvent.x64` / `.x86` | Event | Сигнал остановки |
| `GlassPlatform.HostShutdown.x64` / `.x86` | Event | Хост завершил shutdown |
| `Local\GlassHostPid.x64` / `.x86` | FileMapping | PID хоста |
| `Local\GlassPlatform.InjectMap.x64` | FileMapping | Live-карта инжектированных процессов (x64 DLL / host) |
| `Local\GlassPlatform.InjectMap.x86` | FileMapping | Live-карта инжектированных процессов (x86 DLL / host) |

Архитектура строки в мониторе берётся из **имени карты** (какую DLL открыла/создала), не через `IsWow64Process`. GlassSettings читает обе карты и мержит. Legacy `Local\GlassPlatform.InjectMap` при чтении — fallback как x64. В слоте: `moduleBits`, `diagFlags` (Hooks/PaintDispatcher/ModuleHandler/Explorer/Taskbar), статистика сабклассов WndCompat (`subclassWindowTotal` / `subclassKindMask`). Пишут владельцы на Startup/Shutdown / Attach·Detach — без опроса UI.

### Ключевые подсистемы GlassCore

- **IATRegistry** — реестр IAT-слотов с single-occupancy стеком (first-owner-wins). Решает конфликт нескольких диспетчеров на одном слоте без цепочек и трамплинов.
  - В слоте всегда стоит **ровно один** detour — первый активный регистрант (вложенные сессии слот не флипают, паритет BWM `hookRef`); `outOriginal` — всегда настоящий оригинал. Стыковку сценариев между разными `*Hooks.cpp` делают **только** узкими телами хуков с passthrough в настоящий `orig`, **а не** экспортом вспомогательных API из одного модуля в другой (иначе теряется смысл разнесения по файлам и IATRegistry).
- **HookDispatcher\<N,M\>** — шаблонный диспетчер IAT-хуков для одного модуля. Делегирует все записи в IAT через IATRegistry. Не пишет в память напрямую.
- **OverallHookMap** — 3D-диспетчер: модуль → индекс хука → слот. Единая точка для всех per-module IAT-хуков.
- **PaintDispatcher** — глобальный Detours-перехватчик BeginPaint/EndPaint/GetDC/ReleaseDC/CreateCompatibleDC/DeleteDC. Четыре независимых списка провайдеров: `g_paintProviders` (pre-BeginPaint / EndPaint), `g_postPaintProviders` (после успешного BeginPaint — hdc и PAINTSTRUCT; при регистрации — UiRibbon для NetUI / NetUiToolWindow), `g_getDCProviders` (GetDC/ReleaseDC), `g_compatDCProviders` (CreateCompatibleDC/DeleteDC). Button/Static регистрируются **только** через `RegisterPaintProvider` — DeleteDC от uxtheme::EndBufferedPaint до них не доходит. Стек `g_dcStack` (thread_local) хранит пары (HWND, HDC) для поддержки вложенных сессий.
- **Animation** — анимации popup-окон (DComp + WAM). `~PopupIn()` использует `SendMessageTimeoutW` для синхронного снятия SubclassProc перед уничтожением объекта.

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

### GlassCompat — двойной режим работы

**Хост-режим** (в Rundll32.exe):
- `EntryRundll.cpp::Main()` — создаёт `InstanceMutex`, `StopEvent`, `GlassHostPid`
- Устанавливает глобальный `SetWinEventHook(WINEVENT_INCONTEXT)`
- Запускает `GlassLogServer` (сервер логов)
- Ожидает `/stop` или закрытия через `AttachConsole`

**Инжектированный режим** (в GUI-процессах):
- `DllMain` → `GlassCompat_WinEventProc` регистрируется как WinEvent-коллбэк ОС
- `DoInitialize()` → `GlassGuiThreadProc` в отдельном потоке
- Заблокированные процессы (из `ProcessBlockList`) получают "unload watcher" поток: ждёт StopEvent → `FreeLibraryAndExitThread`
- Незаблокированные процессы: `GlassGuiThread` загружает GlassCore + WndCompat, ждёт StopEvent, выгружает
