# ExplorerCompat — XAML / WinUI «острова» (текущее состояние)

Документ описывает подсистему `ExplorerCompat` в `explorer.exe`, связанную с XAML
Diagnostics (TAP), WinUI «островами» (`DesktopWindowXamlSource`,
`DesktopChildSiteBridge`) и popup-островами (`PopupWindowSiteBridge`), а также
ограничения по восстановлению chrome после Stop.

> **Зона 1 (taskbar) больше не входит в ExplorerCompat.** Её UWP/WinUI-патчи
> вынесены в отдельную DLL `TaskbarCompat.dll` со своим жизненным циклом.

**Загрузка ExplorerCompat:** только **Win11**, лениво по аттачу `FileExplorerShellRoot`
(`CabinetWClass` / `ExploreWClass` → `ExplorerRoot::Attach` →
`GlassCompat_EnsureExplorerCompatLoaded`). Не на каждый `explorer.exe` в
`DoInitialize`. Нет DLL в каталоге — soft skip. Shell-only explorer без окна
папки модуль не грузит.

## Термины

- **Остров (island)**: WinUI-контент в `explorer.exe`, обычно через
  `Microsoft.UI.Xaml.Hosting.DesktopWindowXamlSource`.
- **Мост (bridge)**: HWND-обёртки `Microsoft.UI.Content.DesktopChildSiteBridge`
  и `Microsoft.UI.Content.PopupWindowSiteBridge`.
- **TAP / XAML Diagnostics**: подключение диагностики через
  `InitializeXamlDiagnosticsEx` и `IXamlDiagnostics` / `IVisualTreeService*`.
- **VTS3**: `IVisualTreeService3`, используется для
  `GetPropertyIndex` / `SetProperty` / `ClearProperty` / `CreateInstance`.

## Runtime-архитектура

```
ExplorerCompat_Initialize()
│
└── WinUI сессия (CLSID_GlassXamlTapSite)
    ├── InitializeXamlDiagnosticsEx (FrameworkUdk) → XamlTapSite
    ├── XamlVisualTreeWatcher (IVisualTreeServiceCallback2)
    └── ЗОНЫ 2, 3, 4: popup-остров, шапка проводника, FolderViewIsland
```

`ExplorerCompat.dll` поднимает **одну** WinUI IXDE-сессию через FrameworkUdk
(WinUI 3). Прежний UWP IXDE-контур и весь связанный с ним код (donor-order
gate, Phase A/B/C, runtime-gate `UnsupportedRuntime_WindowsUiXamlSystem32`)
полностью удалены и больше **не** используются — taskbar обслуживает
изолированный модуль `TaskbarCompat.dll`.

## Активные зоны патчинга

| Зона | Что патчится | TU |
|------|--------------|----|
| 3 | шапка проводника (host bars: navigation, address, tabs, command bar) | `xaml/Patcher/XamlHostBarPatcher.cpp` |
| 4 | FolderViewIsland (диагностический чёрный фон до загрузки контента) | `xaml/Patcher/XamlHostBarPatcher.cpp` |

Компактные WinUI-меню (бывшая зона 2) **удалены** из ExplorerCompat.
Win32-меню (`Menu\EffectType` и т.п.) — Popup hub / WndCompat, не этот гайд.

Общие хелперы (VTS3, parent/name walk, BFS) — в
`xaml/Patcher/XamlHostBarPatcherCommon.cpp` (namespace `XamlHostBarPatcher::Detail`).
DWXS-gate (explorer vs non-explorer island) — `XamlHostBarPatcherPopupDwxs.cpp`.

## Где в коде искать

| Файл | Роль |
|------|------|
| `ExplorerCompat/ExplorerCompat.cpp` | Initialize/Shutdown, маршрутизация WinEvent |
| `ExplorerCompat/xaml/XamlTapSite.cpp` | WinUI TAP-сайт, таймеры (Tray redraw, repatch) |
| `ExplorerCompat/xaml/XamlVisualTreeWatcher.cpp` | `OnVisualTreeChange` (Add/Remove) |
| `ExplorerCompat/xaml/XamlShutdownMarshal.cpp` | HWND_MESSAGE для shutdown |
| `ExplorerCompat/xaml/XamlClassFactory.cpp` | `IClassFactory` для WinUI TAP |
| `ExplorerCompat/xaml/DllGetClassObject.cpp` | роутинг WinUI CLSID |
| `ExplorerCompat/xaml/XamlGlassIds.hpp` | `CLSID_GlassXamlTapSite` (WinUI; UWP CLSID удалён) |
| `ExplorerCompat/xaml/Patcher/XamlHostBarPatcher.{h,cpp}` | фасад + ЗОНЫ 3, 4 |
| `ExplorerCompat/xaml/Patcher/XamlHostBarPatcherCommon.{h,cpp}` | общие VTS3 / parent / BFS-хелперы (`Detail`) |
| `ExplorerCompat/xaml/Patcher/XamlHostBarPatcherPopupDwxs.cpp` | DWXS-gate (`Detail::DwxsShouldSkipMarkingAsPopupRoot`) |
| `ExplorerCompat/xaml/Patcher/XamlHostBarPatcherState.{h,cpp}` | глобальное состояние chrome / FolderView Diag |
| `GlassShared/include/GlassMessages.h` | WM_APP для XAML shutdown |

## Что происходит на Stop (`RestoreExplorerChrome(Full)`)

`XamlVisualTreeWatcher::ShutdownVisualTreeWatcher` → `RestoreExplorerChrome(Full)`:

1. `RestoreHostBarPropertyPatches(diag)` — снимает host bar патчи (ЗОНА 3).
2. `RestoreDiagBackgrounds(diag)` — диагностические фоны FolderView (ЗОНА 4).
3. Восстановление `IShape::put_Fill` / `IBorder::put_Background` из локальных
   карт оригиналов.

## Live NoBackdrop OFF vs Stop (шапка проводника)

| | Stop / shutdown | Live `NoBackdrop=1` (toggle OFF) |
|---|-----------------|----------------------------------|
| TAP | `UnadviseVisualTreeChange` | остаётся активным |
| Шапка (ЗОНА 3+4) | `RestoreExplorerChrome(Full)` | `RestoreExplorerHostBarChrome` → `RestoreExplorerChrome(HostBarOnly)` |
| Полный `RestoreExplorerChrome(Full)` | да | **нет** |
| `g_disableHostBarPatching` | временно при restore | `true` при OFF; сброс в `ExplorerCompat_OnRootBackdropPolicyChanged(TRUE)` и `ApplyExplorerHostBarChrome` до FullReplay |

Entry: `ExplorerCompat_OnRootBackdropPolicyChanged(FALSE)` →
`XamlTapSite::OnRootBackdropDisabled` → **`RestoreExplorerHostBarChrome`** (restore ЗОНЫ 3+4, paint pump,
**без** GDI undo/redraw). Win32 redraw — позже через **`ExplorerCompat_PostLiveDisableChromeRedraw`**
(GDI undo + CHROME + SizeToggle). При ON: `OnRootBackdropEnabled` → **`ApplyExplorerHostBarChrome`**
(`ApplyExplorerChrome(FullReplay)`, prime, `CHROME_REDRAW`).

Подробнее: [RootBackdropDisableSequence.md](RootBackdropDisableSequence.md) § Explorer.

## Важные ограничения / наблюдения

### 1) Restore шапки (live OFF) — первый toggle OK, повторный OFF ломается

**Симптом:** после **первого** `NoBackdrop=OFF` в процессе шапка визуально
восстанавливается; после **повторного** toggle ON→OFF — снова частичный провал
(средний ряд, неактивные вкладки, фоны полос).

**Механизм (подтверждено логами `RestoreHostBar`):**

| Цикл | `restoredByOriginal` | Путь restore | Визуально |
|------|----------------------|--------------|-----------|
| 1-й OFF | > 0 | `SetProperty(original IInspectable)` + `ClearProperty` + sync UI `ClearValue` | OK |
| 2-й OFF | 0 | только `ClearProperty` + UI `ClearValue` | провал |

При **первом** патче `TrySetHostBarBrushProperty` сохраняет theme-кисть в
`g_hostBarOriginalInspectables` (если `GetProperty` вернул handle). При restore
COM-ссылки **освобождаются**. При **повторном** патче `GetProperty` на VTC-потоке
часто возвращает **0** (значение из Style/Default, не Local) → оригиналы не
захватываются → restore деградирует до ClearProperty, чего недостаточно для
полного theme re-apply.

`ClearProperty` / `DependencyObject.ClearValue` **не бесполезны** — они работают
в связке с `restoredByOriginal` на первом цикле. Ошибочный вывод «ClearProperty
никогда не восстанавливает» — артефакт одноразового capture.

**Код:** `XamlHostBarPatcherRestoreHostBar.cpp` (`RestoreHostBarPropertyPatches`),
`XamlHostBarPatcherPatch.cpp` (`TrySetHostBarBrushProperty`). Внутренний API:
`XamlHostBarPatcherPrivate.h`.

**Следующий шаг (не реализован):** pinned originals на lifetime процесса или
snapshot effective brush через `GetPropertyValuesChain` на UI-очереди при
`oldValue==0`.

### 2) Нельзя делать синхронные ожидания на UI-очереди при Stop

UI DispatcherQueue острова во время Stop дренируется нестабильно. Любые
дополнительные действия должны быть **батчированы** или **fire-and-forget** —
критично избегать многократных `WaitForSingleObject` на каждое свойство.

### 3) Нельзя делать синхронные вызовы на UI-очередь во время патча

Попытки «поймать оригинальные значения» через синхронный маршалинг на
UI-очередь в момент Add-событий ухудшают UX (заметные задержки/подвисания
при создании окна). На этапе патча допускаются только быстрые операции.

### 4) SWCA / DwmSetWindowAttribute(SystemBackdrop) на popup HWND

Host bar / chrome ExplorerCompat не вызывает `SetWindowCompositionAttribute` или
`DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)` на HWND
`PopupWindowSiteBridge` ради меню — отдельный WinUI menu-патчер удалён.

## Диагностика падений explorer.exe (faulting module / стек)

Для фиксации **модуля и смещения** при AV (в т.ч. меню шапки / WinUI):

1. **Без дампа — Журнал событий:** `eventvwr.msc` → Журналы Windows →
   **Приложение** — записи с источником *Application Error* / *Windows Error
   Reporting* по `explorer.exe`; в тексте события обычно указаны *Faulting
   application name*, *Faulting module name*, *Fault offset*.
2. **Полный дамп (WER LocalDumps):** из каталога репозитория, **от имени
   администратора**:
   `powershell -ExecutionPolicy Bypass -File scripts\ExplorerCrashDiagnostics.ps1 -Action Enable`  
   Дампы сохраняются в `%ProgramData%\GlassProject\CrashDumps\` (полный дамп,
   до 10 файлов). После воспроизведения откройте `.dmp` в Visual Studio
   (**Открыть → файл**) или WinDbg Preview — стек вызовов и загрузочный модуль
   будут в отчёте об исключении.
3. **Живой отладчик:** присоединиться к `explorer.exe` в Visual Studio,
   включить остановку при необработанных исключениях, воспроизвести сценарий —
   окно исключения покажет IP и стек (удобно для одноразового подтверждения).
4. **Отключить LocalDumps:** тот же скрипт с `-Action Disable`.
