# План инструментации выгрузки модулей (refcount-audit)

**Тип документа:** диагностический план для Cursor AI (локально на ПК).
Ничего в контракте выгрузки не меняет: [GlassCompat-unload.md](GlassCompat-unload.md) остаётся
источником истины по `path=1` / `path=2/3` / BlockList.

**Ветка работы:** `arena/627639b4-glassplatform` (опубликована в origin).
Локально: `git fetch origin && git checkout arena/627639b4-glassplatform`.

**Актуальность привязок `file:line`:** коммит `8a5a491`. При правках номера строк плывут —
ищите по имени функции, а не по номеру.

---

## 0. Промпт для Cursor (копировать целиком)

> Прочитай `GlassCompat/docs/unload-instrumentation-plan.md` и `GlassCompat/docs/GlassCompat-unload.md`.
> Реализуй этап **R1** из раздела 6: трассировщик `Shared::Diag::UnloadTrace` (раздел 5) и точки
> логирования из таблицы R1. Ничего не менять в порядке выгрузки, не добавлять `Sleep`/`Wait`,
> не трогать `path=1` vs BlockList. Вся инструментация — под `#if defined(GLASS_UNLOAD_TRACE)`,
> каждая вставка помечена комментарием `// GLASS-ULTRACE` (одна строка, чтобы потом удалить
> механически по `grep -rn "GLASS-ULTRACE"`). Сборка x64 Debug из корня solution.
> После сборки — отчёт: список вставок (файл:функция), путь лог-файлов, как включить/выключить.

---

## 1. Цель и наблюдаемая проблема

**Цель:** быстрая и стабильная выгрузка **всех** модулей (`GlassCompat`, `WndCompat`, `GlassCore`,
`ExplorerCompat`, `TaskbarCompat`, `UICompat`) из всех процессов, без крэшей и без «остатков».

**Наблюдение сейчас:** модули выгружаются нестабильно, `GlassCompat.dll` часто остаётся загруженной.
Подозрение пользователя: `GlassCompat` получает **один лишний refcount**, из-за чего `FreeLibrary` /
`FreeLibraryAndExitThread` не доводят счётчик до 0. При этом выгрузка из GlassSettings
(страница «Процессы» → Unload) выгружает модуль через `FreeLibrary` (Unlocker-стиль) «без краша»,
а прочие зависшие процессы просто жёстко завершаются.

Ниже — разбор всех этапов и **план измерений**, который либо подтвердит «лишний +1», либо покажет,
что остаток — это не наш пин, а **недоставленный deferred-free от ОС** (тогда и «нестабильность»,
и «то выгрузилось, то нет» объясняются одинаково хорошо).

> Важно: у нас **два разных остатка** и их нельзя путать.
> 1. **Наш пин** (`GetModuleHandleEx` / компенсирующий `LoadLibrary`) — детерминированный, считается кодом.
> 2. **Пин ОС** от `SetWinEventHook(..., WINEVENT_INCONTEXT)` — их столько, сколько установлено хуков
>    в этот процесс, и снимаются они **асинхронно** (`UnhookWinEvent` → deferred `FreeLibrary`
>    на потоке-получателе). Отсюда «то выгрузилось, то нет» без всякой утечки.

---

## 2. Карта этапов выгрузки (с точки зрения refcount)

### Этап 0 — загрузка и пины

| Механизм | Где | Что делает со счётчиком |
|---|---|---|
| Глобальный INCONTEXT-хук `CREATE..SHOW` | `EntryRundll.cpp:819` | +1 в **каждом** процессе, куда ОС его доставит |
| INCONTEXT-хук `EVENT_SYSTEM_FOREGROUND` | `EntryRundll.cpp:828` | +1 (те же процессы) |
| INCONTEXT-хук `EVENT_OBJECT_UNCLOAKED` | `EntryRundll.cpp:837` | +1 (те же процессы) |
| Per-PID INCONTEXT: `CREATE..SHOW`, `UNCLOAKED`, `LOCATIONCHANGE` | `CoreWindowHostInject.cpp:974-1035` | до **+3** в конкретном CW-процессе |
| `TryRemoteLoadLibrary` (`CreateRemoteThread(LoadLibraryW)`) | `CoreWindowHostInject.cpp:546-601` | **+1** в целевом процессе, владелец — **наш код в цели** |
| `TryRemoteBootstrap` → `GlassCompat_RemoteBootstrap` | `:604`, `GlassCompat.cpp:3432-3450` | ставит флаг `g_remoteLoadLibraryPin`, сам пин **не берёт** (`UNCHANGED_REFCOUNT`) |
| GUI-поток: `GetModuleHandleEx(FROM_ADDRESS)` | `GlassCompat.cpp:2133` | **+1** на поколение GUI |
| CW-unload-watcher: `GetModuleHandleEx` | `GlassCompat.cpp:2909` | **+1** в CW-процессах |
| Minimal-watcher (`path=2/3`): `GetModuleHandleEx` | `GlassCompat.cpp:3135` | **+1** в BlockList/Forbidden |
| `TryLoadAndInitUICompat` → `GetModuleHandleExW(0, L"UICompat.dll")` | `GlassCompat.cpp:1647` | **+1** к UICompat (не к Compat) |

Ключевой вывод: **число ссылок на `GlassCompat.dll` в процессе заранее неизвестно** — оно зависит от
того, сколько INCONTEXT-хуков ОС успела привязать к этому процессу, плюс наши пины. Поэтому
«счётчик 2 → FLAET → 0» — неверная модель для процесса, куда доставлено 3 хука.

### Этап 1 — триггеры остановки

| Триггер | Кто | Где |
|---|---|---|
| `StopEvent` | хост по `/stop` | `GlassCompat.cpp:2158` (ранний poll), `:2690` (post-drain) |
| `Local\GlassCompat.UnloadRequest.<pid>` (soft unload из «Процессов») | GlassSettings | `InjectProcessMap.h:174-190`, приём — только Minimal-watcher `GlassCompat.cpp:3200` |
| `GlassCompat_CwForceUnloadFlag` (WPM) + `Local\GlassCompat.CwForce.<pid>` | хост | `CoreWindowHostInject.cpp:331`, `:127`; poll — `GlassCompat.cpp:2295`, `:2935` |
| `WM_QUIT` в GUI-поток | хост / watcher | `GlassCompat.cpp:2233`, `:2966` |
| `HostShutdown` | хост после `UnhookWinEvent` | `EntryRundll.cpp:970` |
| `HostAlive` (abandoned mutex = хост мёртв) | инжект | `GlassCompat.cpp:2209`, `:3238` |
| `TerminateProcess` | Планировщик / пользователь | — (только `HostAlive`) |

Существенно: **soft unload (`UnloadRequest`) обрабатывает только Minimal-watcher** (path=2/3).
В «обычном» (path=1, Full) процессе событие `UnloadRequest` никто не слушает — значит
`GlassSettings_RequestProcessUnload` (`InjectProcessMapNative.cpp:624`) для живого path=1 процесса
просто выставит событие, никто не среагирует, вернётся `TRUE`, а потом всё равно придёт Force.
Это надо видеть в логах (событие `gs_request soft_ok=1`, а реакции в процессе нет).

### Этап 2 — `ExecuteShutdown` (модули)

`GlassCompat.cpp:882` → UICompat → (explorer) Taskbar → (explorer) Explorer → GlassCore → WndCompat →
FreeLibrary Core → FreeLibrary Explorer/Taskbar/UICompat/Wnd (под SRWLOCK) → `ReportUnloadResult`.

Тут же — «дожимы»: `CoFreeUnusedLibraries` + `TryDropIxdeResidualModule` (`GlassCompat.cpp:229-260`) —
**дополнительный `FreeLibrary` сверх парного к `LoadLibrary`**. Если это срабатывает дважды
(например, UICompat пинилась через `GetModuleHandleExW(0, ...)` в `:1647` и грузилась через
`LoadOptionalModule`), одна из ссылок «съедается» чужой.

### Этап 3 — `FreeSelfAndExitThread`

`GlassCompat.cpp:2088-2108`:

1. `TryClearPid` — **карта инжекта чистится ДО фактической выгрузки** (UI врёт «процесса нет», модуль ещё жив).
2. `g_remoteLoadLibraryPin.exchange(false)` → если `true`, делается **компенсирующий `FreeLibrary(hSelfRef)`**.
3. `DrainBeforeFreeSelf()` — 8–24 раунда `WakeAllProcessThreadsWmNull` + `WakeWinEventCallbackThreads` + `PumpAllProcessWindowsSyncWmNull` + `SleepEx(50)`.
4. `FreeLibraryAndExitThread(hSelfRef)` — снимает наш GUI-пин.

Леджер этого шага: `−1 (remote, если флаг) −1 (GUI pin)`, а всего было
`+1 remote + 1 GUI pin + N (OS INCONTEXT)`. Модуль уходит ⇔ к моменту шага 4 **все N OS-ссылок уже
сняты** deferred-вызовами. Это и есть точка нестабильности.

### Этап 4 — `done:` хоста

`EntryRundll.cpp:942-980`: `UnhookWinEvent` ×3 → `CoreWindowHostInject::Shutdown()` (снимает все per-PID хуки,
`:1574-1608`) → broadcast `WM_NULL` + `Sleep(300)` → `SignalHostShutdownComplete()` →
`ForceUnloadResiduals()` (`CoreWindowHostInject.cpp:1640`).

Заметьте порядок: **клиентам разрешают FreeSelf (`SignalHostShutdownComplete`) до того, как запущена
добивка остатков**. То есть гонка «наш FLAET vs remote FreeLibrary хоста» легальна и должна быть видна
в таймлайне.

### Этап 5 — выгрузка из GlassSettings («Процессы»)

- `InjectRuntimeMonitor.RequestProcessUnload` (`InjectRuntimeMonitor.cs:214-243`): Residual / платформа off → Force; иначе soft.
- Native `GlassSettings_RequestProcessUnload` (`InjectProcessMapNative.cpp:624`): `SignalUnloadRequest(pid)`, при неудаче → `UnloadPid` → remote `FreeLibrary`.
- `Shared::ForceUnloadGlassModules::ForceUnload` (`GlassShared/src/ForceUnloadGlassModules.cpp`): до **4 проходов**, порядок `WndCompat → GlassCore → GlassCompat`, `CreateRemoteThread(FreeLibrary)` с ожиданием **1500 мс**, без контроля «сколько ссылок осталось» и без сверки с состоянием хуков.

Именно поэтому «выгружает без краша»: (а) чаще всего он только **уменьшает** счётчик и оставляет модуль
(и GlassSettings показывает остаток), (б) если счётчик дошёл до 0 — deferred-фьючерсы ОС уже мертвы,
т.к. хост снял хуки раньше. Крэш появляется, когда force срабатывает **до** unhook или когда под
живым потоком снимается последний ref.

---

## 3. Сводный леджер: кто и что должен снимать

| Модуль | Кто загружает (+1) | Кто обязан снять (−1) | Известные «лишние» пути |
|---|---|---|---|
| `GlassCompat` | ОС (INCONTEXT ×N), remote `LoadLibrary`, GUI-пин, watcher-пины | ОС после `UnhookWinEvent`; `FreeLibrary` компенсации remote; FLAET GUI; watcher `FreeLibrary(hPin)` | см. H80, H82, H87, H89 |
| `GlassCore` | `DoInitialize` (`LoadOptionalModule`) | `ExecuteShutdown` (до 4 попыток) | HWiNFO-zombie (H60/H61) — скипается намеренно |
| `WndCompat` | `DoInitialize` | `ExecuteShutdown` | `FreeLibrary(savedWnd)` без проверки результата |
| `ExplorerCompat` | lazy `Ensure*` + IXDE `LoadLibrary` | `FreeLibrary` + `TryDropIxdeResidualModule` | H86, двойной владелец |
| `TaskbarCompat` | lazy `Ensure*`; **`GetModuleHandleW` без +1** (`GlassCompat.cpp:1404`) | `FreeLibrary(savedTaskbar)` | H86: ручка без +1, а `FreeLibrary` по ней — лишний −1 |
| `UICompat` | lazy `Ensure*`; remote inject; `GetModuleHandleExW(0, ...)` (+1) | `FreeLibrary(savedUICompat)` + `TryDrop` | H83 |

**Вывод для инструментации:** одного «мышления в терминах 0/1/2» недостаточно —
нужен **фактический `LoadCount`** каждого модуля в критические моменты и **счётчик установленных
INCONTEXT-хуков на процесс**. Оба измерения описаны в разделе 5.

---

## 4. Гипотезы, которые должна закрыть инструментация

Приоритет = что проверяем в первом прогоне.

| # | Гипотеза | Проверяемое следствие в логе |
|---|---|---|
| **H80** | `g_remoteLoadLibraryPin` — **bool**, а не счётчик, и ставится в `GlassCompat_RemoteBootstrap` (`GlassCompat.cpp:3435`) **безусловно**, даже если модуль в этот процесс грузился не через `CreateRemoteThread(LoadLibraryW)`. Тогда в `FreeSelfAndExitThread` (`:2101-2103`) уходит **лишний −1** (преждевременный unmap → «нестабильно»), либо, наоборот, при >1 remote-load компенсация только одна → **+1 остаток** | `ev=bootstrap remote_flag_before=0/1`, `ev=freeself_remote_comp free_ret=1`, `refcount_before/after` вокруг компенсации |
| **H81** | Пин ОС кратен числу INCONTEXT-хуков; часть хуков не снимается или снимается не в тот PID (3 глобальных + до 3 per-PID) | `ev=cw_hook_install/unhook` по pid: `n_installed` vs `n_unhooked`; `ev=host_hook_unhook` count |
| **H82** | Deferred `FreeLibrary` от ОС приходит позже, чем наш `FLAET`; в idle/AppContainer/без pump процесса — не приходит вовсе. Отсюда «модуль остался», а через минуты исчезает сам | `refcount` перед FLAET > `own pins`; `ev=ldr_notify reason=unloaded` с задержкой после `ev=host_hook_unhook` |
| **H83** | `GetModuleHandleExW(0, L"UICompat.dll")` (`GlassCompat.cpp:1647`) добавляет +1 к UICompat, которая могла быть загружена remote-инжектом; в shutdown снимается один `FreeLibrary` | `ev=pin_acq mod=UICompat src=uh_pin`, `ev=module_free mod=UICompat refcount_after=1` |
| **H84** | `TryRemoteDropShellHostResidualPin` (`CoreWindowHostInject.cpp:795-875`) делает **ровно один** remote `FreeLibrary`; при остатке >1 не снимает | `ev=cw_residual_drop loaded_before/after` одинаково `1` |
| **H85** | `ForceUnload` — слепые 4 прохода, таймаут 1500 мс, без проверки результата и без учёта незавершённых teardown | `ev=fu_try pass=.. mod=.. wait=timeout ret=0 loaded_after=1` |
| **H86** | Ручки вида `g_hTaskbar = GetModuleHandleW(...)` (`GlassCompat.cpp:1404`, `:1518`) не дают +1, но по ним позже делается `FreeLibrary` → **лишний −1** по чужой ссылке (симметрично H80) | `ev=module_free src=unowned refcount_before/after` |
| **H87** | Watcher-пины (`:2909`, `:3135`) освобождаются в разных ветках; `call_once` не перевзводится — второй шанс после раннего выхода не наступает | `ev=watcher_exit line=.. pin_rel=1/0`, и наличие `pin_acq` без парного `pin_rel` |
| **H88** | `SignalHostShutdownComplete` (`EntryRundll.cpp:970`) раньше `ForceUnloadResiduals` (`:979`) → гонка «наш FLAET vs remote FreeLibrary», порядок зависит от планировщика | таймлайн `host_shutdown_signal` / `freeself_flaet` / `fu_begin` |
| **H89** | Новая генерация GUI (`StartGuiThreadIfNeeded`, `:3303`) берёт новый пин; если предыдущая не сделала FLAET — утечка +1 на цикл | `ev=pin_acq src=gui_thread gen=2` и отсутствие `pin_rel`/`dll_detach` |
| **H90** | Soft unload (`UnloadRequest`) для path=1 процесса никем не слушается → GS считает «запрошено», реакции нет, дальше всё равно Force | `ev=gs_request soft_ok=1` без `ev=stop_trigger src=unloadRequest` в процессе |

---

## 5. Трассировщик `Shared::Diag::UnloadTrace`

### 5.1 Почему не `DebugLog`

`DebugLog` (`GlassShared/include/DebugLog.h`) в Debug-сборке уходит в именованный канал через
`GlassLogClient` — **поднимает воркер-поток** и требует CRT/pipe. Нельзя вызывать:
в `DllMain` (loader lock), в `LdrRegisterDllNotification`-колбэке, после `FreeSelfAndExitThread`-подготовки,
в AppContainer-процессах без доступа к pipe. Поэтому — отдельный минималистичный файловый трассер.

### 5.2 Размещение и API

- `GlassShared/include/UnloadTrace.h`, `GlassShared/src/UnloadTrace.cpp` (линкуется и в GlassCompat,
  и в GlassSettings.Native — оба уже используют `GlassShared`).
- Вся реализация под `#if defined(GLASS_UNLOAD_TRACE)`; иначе — пустые inline-функции.

```cpp
namespace Shared::Diag
{
    // Идемпотентно. Вызывать только из "безопасного" контекста (не DllMain, не loader lock).
    void TraceInit(HMODULE hSelf, const wchar_t* tag);

    // ev — короткий идентификатор (см. 5.5). Остальное — k=v пары.
    void TraceEvent(const wchar_t* ev, const wchar_t* fmt, ...);
    void TraceEventOnce(const wchar_t* ev, const wchar_t* fmt, ...);

    // Только WriteFile в уже открытый handle + OutputDebugStringW. Разрешено в DllMain/DETACH.
    void TraceDetachMarker(const wchar_t* what);

    // Реальный LoadCount модуля (0xFFFFFFFFFFFFFFFF = unknown). См. 5.3.
    unsigned long long ReadLoaderRefCount(HMODULE hModule);

    // Снимок: все Glass*.dll в текущем процессе: base, LoadCount, GetModuleHandle, Toolhelp.
    void TraceModuleState(const wchar_t* stage);

    // Наш собственный леджер пинов (см. 5.4).
    void PinAcquire(const wchar_t* module, const wchar_t* reason);
    void PinRelease(const wchar_t* module, const wchar_t* reason);
    void TracePins(const wchar_t* stage);
}
```

### 5.3 Измерения, которые закрывают вопрос «лишний refcount?»

**A. `ReadLoaderRefCount(HMODULE)`** — единственный способ увидеть правду:

1. `LdrFindEntryForAddress` (ntdll, экспортируется) → `PLDR_DATA_TABLE_ENTRY`.
2. `entry->DdagNode` (`LDR_DDAG_NODE`) → поле `LoadCount`.
3. Офсеты `DdagNode`/`LoadCount` — версионные. Поэтому **один раз** на процесс писать строку
   калибровки: hex-дамп первых `0x40` байт `LDR_DATA_TABLE_ENTRY` и `LDR_DDAG_NODE` + вычисленное значение.
   Санити-проверка: `LoadCount ∈ 0..64`; иначе отдавать `unknown` и писать `refcount_src=none`.
4. Всё под `__try/__except` (SEH), без аллокаций.

**B. `LdrRegisterDllNotification`** (ntdll) — точные события загрузки/выгрузки **наших** модулей:

```cpp
// ev=ldr_notify what=loaded|unloaded dll=GlassCompat.dll base=0x.. refcount=..
```

Это и есть доказательство «модуль действительно ушёл», а не «GetModuleHandle вернул NULL».
Регистрировать лениво в `TraceInit` (не в `DllMain`). В колбэке — только `TraceEvent`
(WriteFile + ODS, никаких `CreateFile`/`LoadLibrary`/аллокаций).

**C. Счётчик хуков** — в хосте: `g_hookCountGlobal` и `map<pid, int>` для per-PID;
все `SetWinEventHook`/`UnhookWinEvent` проходят через обёртки, которые логируют и инкрементируют.

**D. Наш леджер пинов** (`PinAcquire`/`PinRelease`) — чтобы можно было печатать
`own=3 (gui,cw_watcher,uh_ui)` и сравнивать с реальным `LoadCount`.
**Правило реализации:** ни один `LoadLibrary`/`GetModuleHandleEx`/`FreeLibrary`/`FLAET` для
`Glass*.dll` не остаётся без парного `Pin*`-вызова.

### 5.4 Формат строки

```
t=1234.567 utc=17:42:08.213 pid=8124 tid=9044 arch=x64 exe=ShellHost.exe ev=bootstrap remote_flag_before=0 refcount=3
```

- `t` — мс от старта процесса (`QueryPerformanceCounter`), 3 знака.
- `utc` — `GetSystemTimePreciseAsFileTime` (для склейки с логом хоста и с временем Stop).
- Одна строка ≤ ~400 символов, `\r\n`, UTF-8.

**Заголовок сессии** (первая строка каждого файла, один раз):

```
t=0.000 pid=8124 exe=ShellHost.exe arch=x64 build=26200 appcontainer=1 cwhost=1 mode=1 hostPid=3312 trace=file
```

### 5.5 Файл: путь, ротация, надёжность

Порядок попыток (первая успешная фиксируется в заголовке `trace=`):

1. `%LOCALAPPDATA%\GlassPlatform\diag\unload-<exe>-<pid>-<arch>.log`
2. `GetTempPathW()` → `...\GlassPlatform-diag\unload-...log`
3. только `OutputDebugStringW` (+ пометить `trace=ods`) — для AppContainer/SEH, куда не пишется никуда.

Правила:

- `FILE_SHARE_READ|FILE_SHARE_WRITE`, append, `FILE_FLAG_WRITE_THROUGH` не нужен.
- Открывать **лениво** при первом `TraceEvent` (не в `DllMain`, не в `call_once`-инициализаторе модуля).
- Ротация на старте: если файл с тем же именем > 4 МБ — переименовать в `.1` (перезаписав старый).
- Никаких `std::thread`, `std::string`-логики в горячем пути: фиксированный буфер `wchar_t[512]`,
  `swprintf_s`, конвертация в UTF-8 в стек, `WriteFile` под `SRWLOCK`.
- Фильтр по процессам (переменная окружения `GLASS_ULTRACE_EXE`, значения через `;`, по подстроке,
  default: `explorer;ShellHost;SearchHost;StartMenuExperienceHost;WidgetService;Widgets;taskhostw;msedgewebview2`),
  чтобы не писать 100 файлов с одного Stop.

### 5.6 Включение/выключение

- Препроцессор `GLASS_UNLOAD_TRACE` в `GlassCompat.vcxproj`, `GlassSettings.Native.vcxproj`, `GlassShared.vcxproj`
  (x64 Debug). Выключение = убрать define.
- Каждая вставка в чужом коде — строка-маркер `// GLASS-ULTRACE` (и `// GLASS-ULTRACE-BEGIN/END` для блоков),
  чтобы снятие было механическим (`grep -rn "GLASS-ULTRACE"`).

---

## 6. Точки инструментации

Три раунда: **R1** — минимум для проверки гипотезы (делаем сейчас), **R2** — детализация модулей,
**R3** — только если R1/R2 не закрыли вопрос.

### R1 — обязательное (первый прогон)

| # | Файл / функция | Событие | Поля |
|---|---|---|---|
| 1 | `GlassCompat/DllMain.cpp` `DllMain` ATTACH | `dll_attach` | `base`, `pid` |
| 2 | `DllMain.cpp` DETACH | `dll_detach` (только `TraceDetachMarker`) | `base`, `tid` |
| 3 | `GlassCompat.cpp:2133` `GlassGuiThreadProc` | `pin_acq` | `mod=GlassCompat src=gui_thread gen=`, `h`, `ok`, `gle`, `refcount` |
| 4 | `GlassCompat.cpp:3432` `GlassCompat_RemoteBootstrap` | `bootstrap` | `tid`, `remote_flag_before`, `refcount_before`, `exit(tid|0)`, `refcount_after` |
| 5 | `GlassCompat.cpp:2101` `FreeSelfAndExitThread` | `freeself_begin` | `path`, `h`, `remote_flag`, `refcount`, `inFlight` |
| 6 | там же, компенсация remote (`:2102-2103`) | `freeself_remote_comp` | `ret`, `refcount_before/after` |
| 7 | `GlassCompat.cpp:225` `DrainBeforeFreeSelf` | `drain_begin`, `drain_round` (только i=0,1,последний), `drain_end` | `rounds`, `seen`, `posted`, `sync`, `inFlight`, `elapsedMs`, `refcount` |
| 8 | `GlassCompat.cpp:2108` перед `FLAET` | `freeself_flaet` (последняя строка процесса) | `refcount`, `own_pins`, `h` |
| 9 | `GlassCompat.cpp:882` `ExecuteShutdown` | `shutdown_begin`, `shutdown_end` | `trigger` (см. п.10), `elapsedMs`, `mask`, `phase` |
| 10 | вызовы `ExecuteShutdown` (`:2861`, `:3041`) и точки `break` из GUI-цикла | `stop_trigger` | `src=stopEvent|hostLiveness|unloadRequest|cwForceFlag|wm_quit|hostAlive` |
| 11 | `ExecuteShutdown` по модулям (UICompat `:914`, Taskbar `:948`, Explorer `:978`, Core `:996`, Wnd `:1035`) | `module_shutdown` | `mod`, `ret`, `complete` (IsShutdownComplete), `waitedMs` |
| 12 | каждый `FreeLibrary` peer-модуля в `ExecuteShutdown` (`:1098`, `:1186`, `:1225`, `:1277`, `:1360`) | `module_free` | `mod`, `base`, `ret`, `refcount_before/after`, `gmh`, `toolhelp`, `src=owned|unowned` |
| 13 | `GlassCompat.cpp:229` `TryDropIxdeResidualModule` | `module_residual_drop` | `mod`, `comState`, `ret`, `refcount` |
| 14 | `GlassCompat.cpp:2909` `StartCwUnloadWatcher` | `watcher_start` | `kind=cw`, `pin`, `ok` |
| 15 | `GlassCompat.cpp:2940-3083` все `return`/`FLAET` в CW-watcher | `watcher_exit` | `line=<метка ветки>`, `shutdownStarted`, `complete`, `guiTid`, `gmh`, `refcount`, `pin_rel=1` |
| 16 | `GlassCompat.cpp:3135` `StartMinimalUnloadWatcher` | `watcher_start`, `watcher_wake` (trigger), `watcher_exit` | `kind=minimal`, `path`, `trigger`, `wr`, `h58`, `pin_rel` |
| 17 | `EntryRundll.cpp:819/828/837` | `host_hook_install` | `kind`, `h`, `ok` |
| 18 | `EntryRundll.cpp:947-961` | `host_hook_unhook` | `kind`, `h`, `ok` |
| 19 | `EntryRundll.cpp:966-970` | `host_broadcast`, `host_shutdown_signal` | `t` |
| 20 | `EntryRundll.cpp:979` → `CoreWindowHostInject.cpp:1640` | `cw_residual_begin`, `cw_residual_pid`, `cw_residual_end` | `pids`, `exe`, `compat`, `ui`, `elapsedMs`, `still` |
| 21 | `CoreWindowHostInject.cpp:546` `TryRemoteLoadLibrary` | `cw_remote_load` | `pid`, `ok`, `before_loaded`, `after_loaded` |
| 22 | `CoreWindowHostInject.cpp:604` `TryRemoteBootstrap` | `cw_bootstrap` | `pid`, `ok`, `exit`, `guiTid` |
| 23 | `CoreWindowHostInject.cpp:974` `InstallProcessHook` | `cw_hook_install` | `pid`, `h_create`, `h_uncloak`, `h_loc`, `n_for_pid` |
| 24 | `CoreWindowHostInject.cpp:1574` `Shutdown` | `cw_hook_unhook` | `pid`, `n`, `total_hooks`, `elapsedMs` |
| 25 | `CoreWindowHostInject.cpp:795` `TryRemoteDropShellHostResidualPin` | `cw_residual_drop` | `pid`, `stage=wake|free`, `loaded_before/after`, `guiTid` |
| 26 | `CoreWindowHostInject.cpp:878` `TryRemoteRequestShutdown` | `cw_remote_shutdown` | `pid`, `stage`, `guiTid`, `loaded` |
| 27 | `GlassShared/src/ForceUnloadGlassModules.cpp` `ForceUnload` | `fu_begin`, `fu_try`, `fu_end` | `pid`, `host`, `wow64`, `pass`, `mod`, `base`, `ret`, `wait`, `loaded_after`, `left`, `elapsedMs` |
| 28 | `GlassSettings/Native/InjectProcessMapNative.cpp:624` | `gs_request` | `pid`, `soft_ok`, `force_ret`, `modules_after` |
| 29 | Верификация извне (хост и GS native) | `verify` | `t=+0|+2000|+10000`, `pid`, `exe`, `mods=<битовая маска>`, `refcount` (если процесс — мы) |

### R2 — детализация модулей (второй прогон, если R1 не закрыл)

- `WndCompat_BeginShutdown` / `WndCompat_IsShutdownComplete` + счётчик снятых субклассов.
- `ExplorerCompat_Shutdown` / `UICompat_Shutdown` / `TaskbarCompat_Shutdown` + `*_DebugComState` **до и после** `CoFreeUnusedLibraries`.
- `AuditGlassMessageWindowsBeforeFree` (`GlassCompat.cpp:813`) — сколько окон наших классов ещё живо.
- `g_winEventCallbacksInFlight` — значение в каждой ветке `ExecuteShutdown`/drain.
- `PinAcquire/PinRelease` для peer-модулей (`g_hCore`, `g_hWndCompat`, `g_hExplorer`, `g_hTaskbar`, `g_hUICompat`).
- Хост: `InjectMap` upsert/clear по pid (совпадает ли карта с реальным Toolhelp).

### R3 — крайний случай

- `WndCompat`/`GlassCore`: внутренние пины (`GetModuleHandleEx` в хуках) — аудит всех мест.
- ETW/`Process Monitor` для loader-событий, если `LdrRegisterDllNotification` недоступен.
- Дамп (`crashdump`-скилл) при крэше + сопоставление с `freeself_flaet`/`dll_detach`.

---

## 7. Протокол тестирования

Для **каждого** прогона приложить: время Stop (UTC), список процессов (PID+exe) до Stop,
папку `diag` целиком, скриншот страницы «Процессы» сразу после Stop и через 30 с.

| # | Сценарий | Что проверяет |
|---|---|---|
| S1 | Штатный Stop платформы (GS → «Остановить») при ~10 процессах с модулями | базовый таймлайн, H81/H82 |
| S2 | Stop + сразу Force Unload остатков из «Процессов» | H85, H88 |
| S3 | Unload одной строки из «Процессов» при **живом** хосте | H90, soft-путь |
| S4 | Открыть Пуск/меню → Ensure/re-inject → снова Stop | H80, H89 |
| S5 | `TerminateProcess` хоста (rundll32) → потом Stop/GS | `HostAlive`-путь |
| S6 | Stop при `taskhostw`/`msedgewebview2` (BlockList) | path=2, H87 |
| S7 | Stop при CW-хосте, который ни разу не показывался (PLM freeze) | «GUI нет»: пины/recovery |
| S8 | **5 циклов** Start→Stop подряд без перезагрузки | накопление `+1` (главный тест гипотезы) |

**Критерий приёмки:** после S8 через 5 с — **ни одного** процесса с `Glass*.dll`; ни одного крэша/дампа;
Stop укладывается в бюджет (зафиксировать baseline по `elapsedMs` в первом прогоне, цель — ≤ 3 с на процесс,
хост `done:` ≤ 5 с).

---

## 8. Как читать логи: «признак → вывод → фикс»

| Признак | Вывод | Что делать |
|---|---|---|
| `refcount` перед `freeself_flaet` == `own_pins` | наших лишних пинов нет, остаток = OS/deferred | H82: усилить доставку deferred-free (pump/wake), не «искать утечку» |
| `refcount` > `own_pins` | есть **чужая/неучтённая** ссылка | R2: Pin-леджер, найти источник (H80/H83/H86/H89) |
| `freeself_remote_comp ret=1`, но `remote_flag` был `0` при загрузке | H80: ложная компенсация (лишний −1) | заменить bool на счётчик, ставить только при реальном remote-load |
| `pin_acq` без `pin_rel` на том же поколении | H87/H89: утечка пина | закрыть ветку выхода, счётчик поколений |
| `n_installed` > `n_unhooked` по pid | H81: не все хуки сняты | переписать реестр хуков на счётчик + сверка |
| `cw_residual_drop loaded_after=1` | H84: один `FreeLibrary` не хватает | цикл до исчезновения + отказ при живом GUI |
| `fu_try wait=timeout` / `ret=0` | H85: remote `FreeLibrary` не отработал | вернуть результат, повторы с проверкой, не больше N |
| `dll_detach` пришёл **до** `host_hook_unhook` | преждевременный unmap (крэш-риск) | разобрать порядок FLAET/компенсации |
| `dll_detach` через минуты после Stop | H82: «поздняя» доставка | нормализовать drain/wake |
| `gs_request soft_ok=1`, реакции нет | H90: path=1 не слушает `UnloadRequest` | добавить обработчик в GUI-цикл (после подтверждения) |

---

## 9. Ограничения и запреты при инструментации

- **Не менять** порядок фаз, таймауты, набор `UnhookWinEvent`, семантику `path=1` vs BlockList.
- **Не вызывать** `DebugLog`/`LoadLibrary`/`CreateThread`/`CreateFile` в `DllMain`, в loader-lock-колбэках
  (`LdrRegisterDllNotification`), после начала `FreeSelfAndExitThread`.
- Трассер не должен брать блокировку, под которой вызывается Win32 с возможным re-entry
  (`PostMessage`, `SetEvent`): писать после операции, не внутри.
- Не логировать каждый `GlassCompat_WinEventProc` — только первый/последний на (pid,tid) или счётчик каждые 1000.
- Не аллоцировать в трассере после `freeself_begin` (CRT/куча могут быть уже сломаны): только стек.
- Не использовать инструментацию как «фикс»: логи не должны менять тайминги настолько, чтобы скрывать гонку
  (никаких `Sleep`, никаких дополнительных `Wait`).
- Лог-файлы — вне репозитория (`%LOCALAPPDATA%`), в git не коммитить.

---

## 10. Снятие инструментации (чеклист для Cursor)

1. `grep -rn "GLASS-ULTRACE" --include=*.cpp --include=*.h .` → удалить все вставки.
2. Убрать `GLASS_UNLOAD_TRACE` из `GlassCompat.vcxproj`, `GlassSettings.Native.vcxproj`, `GlassShared.vcxproj`.
3. Удалить `GlassShared/include/UnloadTrace.h`, `GlassShared/src/UnloadTrace.cpp`, записи в `.vcxproj`/`.filters`.
4. **Оставить постоянно** (дешёвые и полезные в бою, вне макроса):
   - `host_hook_unhook` с предупреждением при `n_installed != n_unhooked`;
   - итог `verify` после Stop (список процессов с остатками) — на уровне `DBG_WARNING` в хост;
   - `fu_end left=...` — если после Force остались модули, это видимая ошибка.
5. Коммиты инструментации должны быть отдельными (`diag: R1 ...`), чтобы снятие = revert,
   а не ручная правка поверх фикса.
6. После снятия — повторить S8 и приложить чистый прогон (подтверждение, что фикс не зависел от логов).

---

## Приложение A. Замеченные особенности (без правок; кандидаты в фиксы после логов)

1. `FreeSelfAndExitThread` компенсирует remote-load **одним** `FreeLibrary` при флаге-bool
   (`GlassCompat.cpp:2101-2103`), хотя флаг ставится на каждом bootstrap (`:3435`) — учёт неверен в обе стороны (H80).
2. `TryClearPid` вызывается **до** выгрузки (`GlassCompat.cpp:2097`) → диагностическая карта «врёт»
   и GlassSettings может считать процесс выгруженным, пока DLL жива.
3. `ExecuteShutdown` освобождает `savedWnd` без проверки результата (`:1360`) — единственный модуль без `refcount`-контроля.
4. `g_hTaskbar`/`g_hExplorer` могут быть получены `GetModuleHandleW` (без +1), но освобождаться `FreeLibrary` (H86).
5. `ForceUnload` не различает «отработал» и «счётчик не дошёл до нуля»: `RemoteFreeLibrary` возвращает
   только факт завершения потока (`ForceUnloadGlassModules.cpp:70-90`), результат `FreeLibrary` (BOOL) теряется.
6. `StartCwUnloadWatcher`/`StartMinimalUnloadWatcher` — `call_once`: после раннего выхода watcher'а
   второго шанса на выгрузку нет (H87).
7. Порядок `SignalHostShutdownComplete` → `ForceUnloadResiduals` (`EntryRundll.cpp:970,979`) допускает
   гонку «наш FLAET vs remote FreeLibrary» (H88).
8. `UnloadRequest` (soft unload) слушает только Minimal-watcher → для path=1 процесс «не отвечает» (H90).

---

## Приложение B. Быстрые правки-кандидаты (только после подтверждения логами)

| Гипотеза | Правка-кандидат | Риск |
|---|---|---|
| H80 | `g_remoteLoadLibraryPin` → `std::atomic<int> g_remoteLoadPins`; `+1` только в `TryRemoteLoadLibrary`-ветке (через export `GlassCompat_NoteRemoteLoad`), `−1` в цикле компенсации | средний: меняет баланс — только с логами |
| H81 | реестр хуков: считать per-PID `n` и логировать/сверять; при `Shutdown` — снимать пока `n > 0` | низкий |
| H82 | после `SignalHostShutdownComplete` — не «слепой» `Sleep(300)`, а **верифицированный** drain: ждать `dll_detach`/исчезновения модуля с дедлайном, будя потоки | средний |
| H84 | `TryRemoteDropShellHostResidualPin`: цикл (до 3 remote `FreeLibrary`) с проверкой `ModuleLoadedInProcess` и `GuiTid==0` между ними | средний |
| H85 | `ForceUnload`: возвращать `FreeLibrary`-BOOL из remote-потока (через `GetExitCodeThread`), останавливать проходы при `ret == 0`, финальный `left=` | низкий |
| H86 | владение ручками: `g_hTaskbar/g_hExplorer` → всегда `LoadLibrary` (или флаг `owned`), `FreeLibrary` только при `owned` | низкий |
| H87 | watcher: `pin_rel` в каждом `return` + `call_once` заменить на флаг сброса при остановке | низкий |
| H90 | GUI-цикл path=1: слушать `UnloadRequest` (WaitForMultipleObjects / poll), вызывать `ExecuteShutdown` + `FreeSelf` | средний |
