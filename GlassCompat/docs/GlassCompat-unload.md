# GlassCompat — контракт выгрузки (зафиксировано)

**Эталон:** `.claude/backups/2026-07-15_1017-glasscore-residual` + H58 (HostShutdown **только** для BlockList).  
**Код:** `GlassCompat/src/GlassCompat.cpp`, `GlassCompat/src/EntryRundll.cpp`.  
**Не ломать** без явного запроса и без runtime-доказательства регрессии.

## Модель refcount

| Шаг | Ref | Кто |
|-----|-----|-----|
| INCONTEXT inject | 1 | OS pin (`SetWinEventHook`) |
| GUI / MinimalUnloadWatcher: `GetModuleHandleEx` | 2 | наш pin |
| Хост: `UnhookWinEvent` → deferred `FreeLibrary` | 1 | OS pin снят |
| `FreeLibraryAndExitThread(наш pin)` | 0 | выгрузка |

Нужны **оба** decrement’а. Один `FreeSelf` при живом OS-pin → DLL остаётся (Unlocker «легко снимает»).

## Два пути (разные правила)

### Event-based shutdown coordination (2026-09)

Порядок выгрузки **фундаментально изменён**: вместо `SendMessage`/`SendMessageTimeout`
(блокирующий, с таймаутами, потенциальные deadlock’и) — `PostMessage` (неблокирующий) +
именованные события (детерминированное ожидание `WaitForSingleObject(INFINITE)`).

**Ключевой принцип:** каждый поток снимает свои подклассы сам.

#### Flow (path=1)

```
GlassCompat::ExecuteShutdown()
  │
  ├─ TrySetShutdownPhase(ShutdownPhaseCoreDetaching) → shared memory
  ├─ GlassCore_Shutdown()
  │    ├─ AnimationWorker::Shutdown()
  │    │    ├─ Подсчёт PopupIn с валидным m_menuWindow
  │    │    ├─ PostMessage(WM_GLASS_ANIM_DETACH) → каждому m_menuWindow
  │    │    ├─ WaitForSingleObject(g_hAnimDetachDone, INFINITE)
  │    │    │    └─ GUI-thread → SubclassProc → Detach() → atomic counter → SetEvent
  │    │    └─ localStorage.clear()  ← безопасно: подклассы уже сняты
  │    ├─ Hooks::Uninstall()
  │    └─ return
  │
  ├─ TrySetShutdownPhase(ShutdownPhaseCoreDone) → shared memory
  ├─ UICompat_Shutdown()  ← ПЕРВЫМ до WndCompat (Unadvise XAML до XAML teardown)
  │    ├─ UICompat_StopXamlDiagnostics()
  │    │    ├─ UnhookSearchDeferIfAny()
  │    │    ├─ g_shutdownDone = true
  │    │    ├─ ShutdownActiveWatcher() → marshal WM_GLASS_UI_XAML_SHUTDOWN на UI-поток
  │    │    ├─ g_ixdeStaMayExit = true (отпускаем STA IXDE-поток)
  │    │    ├─ WaitForSingleObject(g_installThreadActive, 8s)
  │    │    ├─ WaitForSingleObject(g_objectCount==0 && g_serverLocks==0, 5s)  ← COM drain
  │    │    └─ return (безопасно: COM callbacks завершены)
  │    ├─ CoFreeUnusedLibraries()
  │    └─ return
  │
  ├─ TrySetShutdownPhase(ShutdownPhaseWndDetaching) → shared memory
  ├─ WndCompat_BeginShutdown()
  │    ├─ WinEventHook_Shutdown()
  │    │    ├─ g_hookInstalled = false  ← ПОСЛЕ DetachAll (prevent re-subclass race)
  │    │    ├─ RestoreTrackedControlsWin32Chrome(ShutdownDetach)
  │    │    │    └─ Каждый *_DetachAll() → PostMessage(WM_GLASS_DETACH_*) → каждое окно
  │    │    ├─ WaitForSingleObject(g_hDetachAllDone, INFINITE)
  │    │    │    └─ Каждый subclass proc → RemoveWindowSubclass → atomic counter → SetEvent
  │    │    ├─ Providers cleared, g_shutdownComplete = true
  │    │    └─ return
  │
  ├─ TrySetShutdownPhase(ShutdownPhaseWndDone) → shared memory
  └─ FreeLibrary sequence (безопасно: все подклассы и COM callbacks сняты)
```

#### Запрещено

- `SendMessage`/`SendMessageTimeout` в shutdown path — deadlock (GUI-thread внутри GlassCore callback)
- Таймауты ожидания событий — `INFINITE` детерминировано
- Снятие подклассов из чужого потока — каждый поток делает это сам
- `g_hookInstalled=false` **до** DetachAll — race: WinEvent subclass'ит новое окно после снятия snapshot → stale WndProc → CFG crash
- `FreeLibrary(UICompat)` **до** завершения COM callbacks — callback в unmapped UICompat → c0000005

#### ShutdownPhase enum

```cpp
enum ShutdownPhase : uint32_t {
    ShutdownPhaseNone             = 0,
    ShutdownPhaseCoreDetaching    = 1,  // GlassCore снимает подклассы
    ShutdownPhaseCoreDone         = 2,  // GlassCore безопасен для FreeLibrary
    ShutdownPhaseUiDetaching      = 3,  // UICompat останавливает COM callbacks
    ShutdownPhaseUiDone           = 4,  // UICompat безопасен для FreeLibrary
    ShutdownPhaseWndDetaching     = 5,  // WndCompat снимает подклассы
    ShutdownPhaseWndDone          = 6,  // WndCompat безопасен для FreeLibrary
};
```

Запись в shared memory через `TrySetShutdownPhase()` — для диагностики GlassSettings.

#### Связанные сообщения

- `WM_GLASS_ANIM_DETACH = WM_APP + 0xD00` — GlassCore Animation shutdown
- `WM_GLASS_DETACH_ROOT` — WndCompat Root subclass detach
- `WM_GLASS_DETACH_EXPLORERROOT` — WndCompat ExplorerRoot subclass detach
- `WM_GLASS_DETACH_TOOLTIP` — WndCompat Tooltip subclass detach
- ... (аналогично для каждого *_DetachAll)

### path=1 — Normal GUI (`GlassGuiThreadProc`)

1. При старте GUI-потока: `OpenEvent(HostShutdown)` **сразу** (как BlockList) — держать handle.
2. `ExecuteShutdown` (Core / Wnd / Explorer / Taskbar по порядку).
3. **H71:** `Wait(ранний HostShutdown)` до 30 с — иначе быстрые процессы (`ollama.exe`) делают FreeSelf
   **до** хостового `UnhookWinEvent` → OS-pin, нет `DLL_PROCESS_DETACH`.
   **Не** `OpenEvent` после COMPLETE (H70 → `WAIT_FAILED`: хост уже закрыл named event).
4. **H72:** `PostThreadMessage(WM_NULL)` всем потокам процесса + wake WinEvent-targets + `SleepEx`
   (без `DispatchMessage` — H68/H69). Иначе FreeSelf при живом OS-pin в idle-процессах
   (`ollama`/`igfxEM`/`NhNotif`) — HostShutdown уже OK, deferred FreeLibrary не доставлен.
   `conhost.exe` с 2026-08 в BlockList (path=2), не path=1.
5. `FreeSelfAndExitThread(hSelf, 1)` — **перед FLAET** ждёт `g_winEventCallbacksInFlight==0`
   (wake + `SleepEx`, без `DispatchMessage`). Иначе deferred `WinEventProc` после Unhook
   исполняется в unmapped образе (`<Unloaded_GlassCompat>`, ShellHost/Adobe 18.08).

**Запрещено на path=1:** `ReadHostPid()==0` как «успех» вместо Unhook (H55/H56); EnumWindows-шторм;
`DispatchMessage` после FreeLibrary Core/Wnd; поздний `OpenEvent(HostShutdown)` после COMPLETE;
`TerminateThread` GUI из `StartCwUnloadWatcher` (UAF в выгруженном GlassCompat). Watcher
форсит только `ExecuteShutdown` + `WM_QUIT`/wake; FLAET — из GUI после `shutdownComplete`,
либо из watcher **только** если `GlassCompat_GuiThreadId==0` и модуль ещё загружен.

**Не путать с H55:** ждать HostShutdown **после** ExecuteShutdown на **раннем** handle — ок; ждать вместо Unhook / с ложным PID — нет.

### path=2/3 — ProcessBlockList / Forbidden (`StartMinimalUnloadWatcher`)

Нет GUI / нет `DoInitialize`. Только pin + watcher.

1. `Wait(StopEvent)`.
2. **H58:** `Wait(HostShutdown)` (до 30 с) — иначе FreeSelf обгоняет Unhook → OS-pin в `svchost` / `msedgewebview2` / `FileCoAuth` / NVIDIA Overlay.
3. Короткий message pump.
4. `FreeSelfAndExitThreadWithProbe(hMod, 2|3)`.

**HostShutdown:** path=2/3 — обязателен; path=1 — **H71** (ранний Open + Wait после COMPLETE), не H55/H56.

INCONTEXT всё равно загружает `GlassCompat.dll` в процесс из BlockList (в т.ч. `taskhostw.exe`, `conhost.exe`, `msedgewebview2.exe`, `Adobe CEF Helper.exe` и любой exe с подстрокой `cef helper`). Полная инъекция (GlassCore / WndCompat / GUI) **не** выполняется: только pin + MinimalUnloadWatcher. `Initialize()` и `DoInitialize()` повторно проверяют BlockList — WinEvent static-классификация не единственный барьер. На path=2 перед FreeSelf — `DrainBeforeFreeSelf` (мин. 8 раундов: wake потоков + **синхронный** `SendMessageTimeout(WM_NULL)` по top-level окнам процесса + `SleepEx`, без `DispatchMessage`; Adobe: deferred Unhook должен снять `__ClientCallWinEventProc` до FLAET). CrashGuard (UEF/VEH) на path=2/3 **не** ставится — системные unhandled C++ EH (например AccountHealth toast) не перехватываются и не пишутся в Glass-дампы.

В Debug-сборке `DllMain` ATTACH глушит модальные abort()/assert-диалоги CRT (`_set_abort_behavior(0, _WRITE_ABORT_MSG)` + `_CrtSetReportMode(..., _CRTDBG_MODE_DEBUG)`), чтобы reboot не блокировался в BlockList-процессах. Сообщение остаётся в OutputDebugString. Встроенный BlockList — массив `kBuiltInNames` в `ProcessBlockList.h`; UI GlassSettings читает его через Native, второго списка нет.

## Хост (`EntryRundll::Main` → `done:`)

1. `UnhookWinEvent` (foreground + global).
2. `SendNotifyMessageW(HWND_BROADCAST, WM_NULL)` — пнуть deferred FreeLibrary.
3. `SignalHostShutdownComplete()` — **сразу после Unhook+broadcast** (path=1 / BlockList).
4. Cleanup presence / `ClearHostPid` / StopEvent.
5. **H71:** не `CloseHostShutdownEvent` на нормальном done — named event живёт до выхода процесса хоста.

Не переносить `SignalHostShutdown` обратно «после всего cleanup» без причины — watcher’ы будут ждать дольше и снова гоняться с FreeSelf.

## Что уже ломало выгрузку (не повторять)

| Ошибка | Симптом |
|--------|---------|
| `Engine::Initialize` вынести из `DllMain` + SEH (H53) | Полный init после AV → зависание `GlassCore_Shutdown` → нет FreeSelf → Compat в куче процессов |
| `HostShutdown` на path=1 **вместо** Unhook / `ReadHostPid()==0` как skip wait (H55/H56) | Ложный «успех», гонки |
| FreeSelf path=1 **до** HostShutdown на быстрых процессах (ollama.exe) | OS-pin, нет Compat DETACH (H70) |
| path=1 `OpenEvent(HostShutdown)` **после** COMPLETE (H70) | `WAIT_FAILED`, FreeSelf при OS-pin (conhost/сервисы) — нужен ранний Open (H71) |
| path=1 FreeSelf сразу после HostShutdown без wake чужих потоков | OS-pin жив в idle GUI (H71 OK, нет DETACH) — нужен H72 |
| path=1 FLAET без drain `g_winEventCallbacksInFlight` | AV `<Unloaded_GlassCompat>` (ShellHost / Adobe CEF Helper) |
| path=2 `DispatchMessage` в MinimalUnloadWatcher перед FreeSelf | user callback в unmapped GlassCompat (Adobe CEF Helper) |
| path=2 FreeSelf сразу при inFlight==0 без min wake rounds | KiUserCallbackDispatcher в unmapped GlassCompat (Adobe CEF Helper) |
| path=2 FLAET до deferred Unhook на UI-потоках | `__ClientCallWinEventProc` при CreateWindowEx после unload (Adobe 18.08) |
| WakeWinEventCallbackThreads между drain и FLAET | будит потоки, WinEvent после unmap (Adobe CEF Helper) |
| BlockList только в static WinEvent-классификации | `Initialize()`/`DoInitialize()` всё равно грузят GlassCore — нужна повторная проверка |
| `TerminateThread(GUI)` в `StartCwUnloadWatcher` | тот же UAF: watcher unmap'ит DLL под живым GUI/WinEvent |
| Откат H58 с path=2 | BlockList снова держит Compat после Stop |
| `g_hookInstalled=false` **до** DetachAll в WinEventHook_Shutdown | Очередной WinEvent subclass'ит новое окно после снятия snapshot → stale WndProc → `FAST_FAIL_GUARD_ICALL_CHECK_FAILURE` в CTray::_MessageLoop (Explorer.exe) |
| UICompat: нет COM drain перед FreeLibrary | COM callback ссылается на выгруженную UICompat.dll → `c0000005` Access Violation в ShellHost.exe |
| FreeLibrary Toolhelp-зомби GlassCore без полного teardown (H60/H61) | Core «уходит», затем AV `<unknown>` / hang HWiNFO |

## HWiNFO (связанный баг Core, не ломать Compat)

HWiNFO часто = **несколько процессов** `HWiNFO64.exe` (UI + sensors/helper). INCONTEXT грузит GlassCompat в **каждый**. Stop должен выгрузить оба независимо (у каждого свой GUI-поток / FreeSelf).

AV в `DllMain`/`Engine::Initialize` → `LoadLibrary(GlassCore)` NULL при живом Toolhelp-образе. **Нельзя** `FreeLibrary` по Toolhelp-base без доказанно безопасного teardown — см. H60/H61. Residual Core терпим до фикса AV; Compat unload не трогать.

## Loader subsystem (диагностика загрузки/выгрузки модулей)

Подсистема Loader отслеживает жизненный цикл загрузки и выгрузки peer-модулей
(GlassCore, WndCompat, ExplorerCompat, TaskbarCompat, UICompat) через shared memory.

### Диагностические биты

| Бит | Тип | Значение |
|-----|-----|----------|
| `DiagLoader` (1u << 8) | `DiagFlags` | Модули загружены (Running) |
| `DiagLifecycleLoader` (1u << 8) | `DiagLifecycle` | Фаза загрузки модулей (Starting) |

### Lifecycle-схема

```
DoInitialize() BEGIN
  └─ TrySetDiagLifecycle(DiagLifecycleLoader, true)   → Starting (жёлтый)
  └─ LoadLibrary(Core, Wnd, Explorer, Taskbar, UICompat)
DoInitialize() COMPLETE
  └─ TrySetDiagFlag(DiagLoader, true)                  → Running (зелёный)
  └─ TrySetDiagLifecycle(DiagLifecycleLoader, false)

ExecuteShutdown() BEGIN
  └─ TrySetDiagFlag(DiagLoader, false)                 → Stopping (серый через isProcessStopping)
  └─ FreeLibrary sequence
  └─ TryClearPid()                                     → отсутствие в списке
```

**Важно:** lifecycle-бит Loader'а устанавливается **только** при загрузке (`DoInitialize`).
При выгрузке (`ExecuteShutdown`) lifecycle-бит **не** устанавливается — UI определяет
серый цвет через `isProcessStopping` (state == Unloading), что исключает жёлтый кружок
в фазе выгрузки.

### UICompat COM lifecycle

UICompat использует COM-объекты (IXDE, TAP, visual tree watcher) с callback'ами в XAML-процессах
(ShellHost.exe, StartMenuExperienceHost). При выгрузке:

1. `UICompat_Shutdown()` → `UICompat_StopXamlDiagnostics()`
2. `ShutdownActiveWatcher()` — marshal shutdown на UI-поток через `SendMessageTimeoutW`
3. **COM drain**: ожидание `g_objectCount==0 && g_serverLocks==0` (до 5 с) — критично:
   иначе `FreeLibrary(UICompat.dll)` выгрузит DLL, пока COM callback ещё ссылается на её код
4. `CoFreeUnusedLibraries()` — COM выгружает DLL если `DllCanUnloadNow()==S_OK`

**Без COM drain:** `c0000005` Access Violation в ShellHost.exe (`<Unloaded_UICompat.dll>+0x11eae2`).

### Файлы-источники

| Файл | Функция |
|------|---------|
| `GlassShared/include/InjectProcessMap.h` | `DiagLoader`, `DiagLifecycleLoader` (бит 8) |
| `GlassCompat/src/GlassCompat.cpp` | `DoInitialize()`, `ExecuteShutdown()`, `RepublishDiagFlagsFromLoadedModules()`, `ClearDiagFlagsInMap()` |
| `UICompat/xaml/XamlDiagInit.cpp` | `UICompat_StopXamlDiagnostics()`, COM drain (`g_objectCount`/`g_serverLocks`) |
| `UICompat/xaml/UiTapSite.cpp` | `ShutdownActiveWatcher()`, marshal shutdown на UI-поток |
| `UICompat/xaml/DllGetClassObject.cpp` | `DllCanUnloadNow()`, `g_objectCount`/`g_serverLocks`/`g_shutdownDone` |
| `GlassSettings/Services/NativeHostInterop.InjectMap.cs` | `InjectDiagFlags.Loader`, `InjectDiagLifecycle.Loader` |
| `GlassSettings/Controls/InjectRuntimeModuleTable.xaml.cs` | Строка `("Loader", DiagLoader, Compat)` в `DiagMatrixRows` |

## Проверка после правок unload

- path=1: `path1 wait HostShutdown` `value1=0`/`value2=1`, затем `path1 wake deferred unload`, затем `FreeSelf` и `GlassCompat DLL_PROCESS_DETACH` (в т.ч. ollama/conhost/igfxEM/NhNotif).
- path=2: перед FreeSelf есть `BlockList wait HostShutdown`, затем DETACH; после Stop нет Compat в BlockList-процессах.

## Связанные объекты

- `GlassPlatform.StopEvent` — выход GUI / watcher.
- `GlassPlatform.HostShutdown` — «Unhook на хосте выполнен» (path=1 H71 + path=2/3).
- `GlassPlatform.HostAlive.<arch>` — **dead-man switch**. Мьютекс с DACL `WD+AC`,
  которым хост владеет всё время работы и **никогда** не освобождает: смерть потока
  хоста делает его abandoned. В инжектированном процессе любой успешный захват
  (`WAIT_OBJECT_0` или `WAIT_ABANDONED_0`) = хост мёртв; сразу после захвата —
  `ReleaseMutex`, чтобы сигнал получили остальные ожидающие процессы.
  Нужен там, где `StopEvent` не приходит никогда: `TerminateProcess` от Планировщика
  (переход на батарею, End task) и краш хоста.
  **Порядок выгрузки не меняет:** это только дополнительный, более ранний повод выйти
  из ожидания. path=1 по-прежнему ждёт `HostShutdown` после `ExecuteShutdown` (H71),
  path=2/3 — после `StopEvent` (H58); при доказанной смерти хоста ожидание
  заканчивается сразу вместо 30-секундного таймаута — конечное состояние то же.
- `Local\GlassHostPid` — не использовать как замену HostShutdown на path=1.
- `Local\GlassPlatform.InjectMap.x64` / `.x86` — **только диагностика** для GlassSettings (PID/kind/modules/diag/subclass). Не барьер выгрузки: не ждать карту в FreeSelf / path=1 и не менять порядок HostShutdown ради неё. Legacy `Local\GlassPlatform.InjectMap` — только fallback чтения.
- `Local\GlassCompat.UnloadRequest.<pid>` — per-PID команда из монитора. **Не** замена глобальному `StopEvent`. Семантика: Alive + хост жив (path=1) → `ExecuteShutdown` модулей, **без** `FreeSelf` Compat, пока жив INCONTEXT-хук; path=2/3 → FreeSelf как после Stop; residual / HostShutdown уже был → teardown + FreeSelf при живом GUI/watcher. Порядок path=1/2/3 при **глобальном** stop не менять.

## Диагностика

Аудит refcount и план инструментации (логи в файл) для разбора «модули выгружаются нестабильно,
GlassCompat остаётся загруженной»: [unload-instrumentation-plan.md](unload-instrumentation-plan.md).
