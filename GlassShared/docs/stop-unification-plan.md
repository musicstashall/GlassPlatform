# Унификация остановки платформы: единый центр `Shared::GlassStop`

Документ описывает, как остановка платформы устроена сейчас, кто и где выполняет
остаточную выгрузку, и пошаговый план сведения всех инициаторов к одному механизму.
Инструментация, на которой построены выводы, — `GlassCompat/docs/unload-instrumentation-plan.md`
(разделы R6–R10).

---

## 1. Кто сейчас останавливает платформу

| # | Инициатор | Точка входа | Что реально делает | Файлы |
|---|---|---|---|---|
| 1 | **GlassSettings** (кнопка «Остановить») | `GlassPlatformService.StopPlatform` / `StopPlatformAndWait` | `SetEvent(GlassPlatform.StopEvent.x64/x86)`; зомби-ветка — `IRegisteredTask::Stop` | `GlassSettings/Core/Platform/GlassPlatformService.cs:70,83`, `GlassPlatformService.Install.cs:69`, `Native/OpenGlassHostTask.cpp:372` |
| 2 | **GlassShellExt** (меню рабочего стола) | `DesktopShellActions` | `TryBeginLifecycleBusy` + `GS::SignalPlatformStop` + ожидание `!Running` + `StopOrphanAutorunTaskInstance` | `GlassShellExt/DesktopShellActions.cpp:510-525` |
| 3 | **GlassSetup** (установка/обновление) | `StopRunningOpenGlassTasks` | **не сигналит StopEvent**: перечисляет задачи планировщика и вызывает `IRegisteredTask::Stop(0)` (жёсткое завершение) | `GlassSetup/src/InstallSupport/GlassInstallUtil.cpp:817-860`, `GlassInstallHost.cpp` |
| 4 | **GlassUnins** (удаление) | тот же путь | то же + отдельный `TaskStop` перед снятием MSI | `GlassSetup/src/InstallSupport/GlassInstallRemoval.cpp:797-801` |
| 5 | **Ручная остановка задачи в Планировщике** (и любой TerminateProcess хоста) | — | хост убивается; инжектированные процессы узнают об этом сами — по abandoned-мьютексу `GlassPlatform.HostAlive` (см. §2а) | `GlassCompat/src/GlassCompat.cpp:3283` |
| 6 | **LogViewer** | `UiMainWindow_ImplPlatform.cpp` | `SignalPlatformStop` (свой, не через общий слой) | `LogViewer/Ui/MainWindow/UiMainWindow_ImplPlatform.cpp` |
| 7 | **GlassCompat: CrashGuard** | `SignalPlatformStopBothArch` | дублирует обе ветки arch вручную | `GlassCompat/src/CrashGuard.cpp:563` |
| 8 | **GlassCompat: HostShellWatchdog** | — | свой цикл `SetEvent` по обоим архам | `GlassCompat/src/HostShellWatchdog.cpp:48` |
| 9 | Хост платформы | `rundll32 GlassCompat.dll,Main /stop` | `SetEvent(kStopEventName)` — команда, которую используют трей/задача/скрипты | `GlassCompat/src/EntryRundll.cpp:896-905` |

Итог: **четыре разных механизма** вместо одного:

* `SetEvent(StopEvent)` — «мягкая» остановка (1, 2, 6, 7, 8, 9);
* `IRegisteredTask::Stop(0)` — жёсткое убийство задачи (3, 4, 5);
* детект «хост умер» по abandoned-мьютексу `GlassPlatform.HostAlive` внутри каждого
  инжектированного процесса (`shutdownByHostLiveness`);
* отдельный путь установщика: встроенный `GlassSettings.Native` + `GlassSettings_UnlockerW`
  `/scanGlassModulesSetup` и `/forceUnload` (`GlassSetup/src/InstallSupport/GlassInstallUnlock.cpp:107,283`).

---

## 2. Где выполняется остаточная («дожим») выгрузка

Ни один из этапов не создаёт новый процесс. Схема из четырёх уровней:

| Уровень | Кто выполняет | В каком процессе | Что делает | События в логе |
|---|---|---|---|---|
| 1 | GUI-поток GlassCompat | **в каждом инжектированном процессе** | `stop_trigger` → `ExecuteShutdown` (peer-модули) → `FreeLibraryAndExitThread` | `stop_trigger`, `shutdown_begin/end`, `module_free`, `freeself_*` |
| 2 | Minimal-watcher (`StartMinimalUnloadWatcher`) | в том же процессе, отдельный поток | для `path=2/3` (BlockList/Forbidden): ждёт StopEvent / UnloadRequest / HostAlive → FLAET | `watcher_wake`, `watcher_exit` |
| 3 | Хост (`ForceUnloadResiduals`) | **в процессе хоста** (тот же `rundll32` + GlassCompat, который был `/start`) | `TryRemoteRequestShutdown` — `CreateRemoteThread` **внутрь чужого процесса**, вызывает его же export `GlassCompat_RequestShutdown`; затем guarded sweep (`ack` + число окон, `mode=2` — оставить) | `cw_residual_*`, `cw_remote_shutdown`, `cw_sweep_guard`, `cw_sweep_pid`, `cw_sweep_end` |
| 4 | ОС | — | когда процесс-владелец INCONTEXT-хуков (хост) завершается, user32 отпускает per-process ссылки, и оставшиеся процессы доканчивают свой unload | `dll_detach` |

Проверка по прогону 11 (`debug-8e6259-11.log`):

```
13:52:31.664  stop_trigger  в 11 процессах (каждый — свой pid)
13:52:32.453  host_shutdown_signal  pid 35788 (rundll32-хост)
13:52:35.6    freeself_flaet  DllHost/Happ/Rainmeter/CV_64 …
13:52:43.417  cw_residual_end  left=1 (explorer)
13:52:45…54    cw_sweep_guard mode=2 ×5 → denied=5 (никого не добивали FreeLibrary)
13:52:54.988  dll_detach хоста (process_exit=1)
13:52:55.0    dll_detach CV_64, explorer, Rainmeter, Happ — уже после выхода хоста
13:55:10.597  dll_detach DllHost (простаивал 2 мин 15 с — очередь не просыпалась)
```

Отсюда правила, которые нельзя терять при рефакторинге:

* **raw `FreeLibrary` извне запрещён** (R6/R7): снять ссылку user32 может только сама ОС;
* «дожим» — это просьба к процессу (graceful) и ожидание, а не принудительная выгрузка;
* окончательный результат виден только **после** выхода хоста (R8/R9, `verify post2/post8`);
* инертный процесс может отдать модуль через минуты (DllHost выше) — это норма, не ошибка.

---

---

## 2б. `ForceUnloadResiduals` пошагово (что делает «дожим»)

Функция живёт **в процессе хоста** (`GlassCompat/src/CoreWindowHostInject.cpp:2033-2440`) и
вызывается из `EntryRundll.cpp` после `host_shutdown_signal`. Ни одного нового процесса она
не создаёт.

| Шаг | Что делает | События |
|---|---|---|
| 1 | `CollectCompatCandidates` — снимок процессов, где есть `GlassCompat` (до всех ожиданий, чтобы не зацепить свежие инжекты) | — |
| 2 | забирает `g_remoteLoadPids` (процессы, куда модули грузили через `LoadLibrary` удалённо: ShellHost/SEH/Start/Search) и очищает список | — |
| 3 | добавляет CoreWindow-хосты, `explorer.exe`, `Widgets*`, у которых реально загружен `GlassCompat` (Toolhelp) | `cw_residual_begin`, `cw_residual_pid` |
| 4 | UICompat-restore: для каждого (кроме explorer — там remote shutdown давал AV в `Windows.UI.Xaml`) `TrySehPlmWake` → проверка `IsRemoteProcessSchedulable` → `WakeRemoteCwProcess` → `PostWmNullToTrackedHwnds` → `TryRemoteUICompatShutdown` | — |
| 5 | мягкий сигнал: `SignalRemoteUnload` (флаг в WPM + named event + `WM_QUIT`), wake, `WM_NULL` | — |
| 6 | немедленный graceful: `TryRemoteRequestShutdown` — `CreateRemoteThread` **внутрь** процесса с его же export `GlassCompat_RequestShutdown` (внутри: `RequestShutdown` → GUI-цикл → `ExecuteShutdown` → `FreeLibraryAndExitThread`). Это **просьба**, а не выгрузка | `cw_remote_shutdown` |
| 7 | ожидание №1: до 20 тиков (40 для смеси процессов) × 50 мс, каждый тик дешёвый wake; полное перечисление потоков — каждый 10-й тик; ранний выход, если остался только OS-pin (все `GuiTid=0`) | — |
| 8 | повторный сигнал + ожидание №2: 10–20 тиков × 50 мс | — |
| 9 | ушедшим — `TrySehPlmRest(DisableDebugging)`; у кого модуль остался — `EnableDebugging` не снимаем | — |
| 10 | подсчёт остатка | `cw_residual_end` (`left`, `left_pids`, `ms`) |
| 11 | sweep: по каждому кандидату с `mask==1` (только `compat`) — `ack` (named mutex `GlassCompat_ShutdownDone_<pid>`), число видимых окон, graceful + «пинок» окон/потоков, ожидание 0.6 с; не ушёл → `mode=2` (оставить) | `cw_sweep_guard`, `cw_sweep_pid` |
| 12 | агрегат | `cw_sweep_end` (`candidates/tried/unloaded/graceful/denied/left/left_pids`) |

Чего функция **не** делает (по политике R6/R7): не вызывает `FreeLibrary` в чужих процессах
(только при `GLASS_ULTRACE_SWEEP_ALL=1`), не завершает процессы, не трогает explorer-UICompat.

Бюджет времени в прогоне 11: `cw_residual_begin` 13:52:32.860 → `cw_residual_end` 13:52:43.417
(10.9 с, из них ожидания 1–2 и UICompat), затем sweep 13:52:45.7…54.7 (~9 с — по ~2.2 с на
кандидата, потому что каждый graceful-вызов ждёт до 3 с). Основной резерв ускорения Stop —
параллельный обход кандидатов и отказ от graceful для процессов с `ack=1` и без окон.

---

## 2а. Стоп через завершение хоста — штатный путь, а не дефект

Требование к платформе: **остановка хоста = сигнал остановки всем процессам**. Оно
выполнено и должно сохраниться при унификации. Механика (работает уже сейчас):

| Шаг | Кто | Что происходит |
|---|---|---|
| 1 | хост | всё время держит мьютекс `Local\GlassPlatform.HostAlive.x64` (own) |
| 2 | каждый инжектированный процесс | в GUI-цикле `WaitForMultipleObjects` ждёт `StopEvent` / `UnloadRequest` / **`HostAlive`** (`GlassCompat.cpp:3226-3290`) |
| 3 | хост убит (TaskScheduler `End`, диспетчер, падение) | мьютекс становится **abandoned** → ожидание отдаёт `WAIT_ABANDONED_0` |
| 4 | процесс | `ReleaseMutex` (передать сигнал следующему ждущему) → `guiStopTrigger="hostAlive"` → `ExecuteShutdown()` → `FreeLibraryAndExitThread` |
| 5 | процессы `path=2/3` (BlockList/Forbidden) | то же в `StartMinimalUnloadWatcher`: `trigger=hostAlive` → FLAET (`GlassCompat.cpp:4270-4290`) |
| 6 | ОС | после смерти процесса-владельца INCONTEXT-хуков user32 отпускает per-process ссылки, и оставшиеся процессы доканчивают unload |

Что при этом **не** происходит (и это нормально): хост, убитый жёстко, не выполняет свою
часть работы — `UnhookWinEvent`, `host_broadcast`, `ForceUnloadResiduals`. Компенсируют это
пункты 4–6: каждый процесс уходит сам, а ссылки на нашу DLL снимает ОС по факту смерти
процесса-владельца хуков. Именно поэтому в логе прогона 11 `dll_detach` приходят **после**
`dll_detach` хоста.

Что в этом пути действительно надо доработать (пункты для этапа 5 плана):

| # | Проблема | Доработка |
|---|---|---|
| A1 | процесс, в который DLL попала INCONTEXT-инъекцией, но `WinEventProc` больше не вызывался, не имеет ни GUI-цикла, ни watcher'а → узнает о стопе только при следующем событии | при неудаче `StartGuiThreadIfNeeded` (и при `Initialize`, если GUI не поднялся) поднимать `StartMinimalUnloadWatcher(1)` — тогда liveness-ожидание есть у любого процесса с нашей DLL |
| A2 | «тихий» процесс может отдать модуль через минуты (в прогоне 11 `DllHost` 35892 — 2 мин 15 с после стопа) | различать «ещё не проснулся» и «не может»: watcher пишет `host_alive_seen` с задержкой; для инсталлятора это повод показать страницу остатков, а не жать |
| A3 | нет отдельной записи «стоп пришёл по смерти хоста» единого формата | `stop_trigger what="hostAlive"`, `stop_result reason=host_dead` в общем формате `GlassStop` |
| A4 | никто не проверяет, что путь «смерть хоста» реально доходит до всех | тест-сценарий U7: End задачи → у каждого процесса `stop_trigger what=hostAlive` → `verify post8 hits=0` |

**Вывод для унификации:** `StopEvent` остаётся предпочтительным путём (хост успевает снять
хуки и разослать `host_broadcast`), но путь «хост убит» — равноправный штатный режим, а не
ошибка. Поэтому в `GlassStop` он становится явной причиной `Reason::HostDead`, а не
«недостатком, который надо исключить»; установщику достаточно остановить задачу и
дождаться, пока процессы сами снимут модули (шаги 4–6), — принудительная выгрузка не нужна.

---

## 2в. Если хост убит планировщиком: `ForceUnloadResiduals` не выполняется — и что вместо него

**Короткий ответ: для самой выгрузки он и не нужен.** Функция целиком живёт в хосте, поэтому
при `TerminateProcess` её нет. Остаточную выгрузку в этом сценарии делают механизмы
**внутри каждого процесса**, которым хост не требуется:

| Механизм | Как срабатывает | Лог |
|---|---|---|
| abandoned-мьютекс `HostAlive` | ядро отдаёт `WAIT_ABANDONED_0` в `WaitForMultipleObjects` — не нужны ни очередь сообщений, ни окно | `stop_trigger what="hostAlive"` (`GlassCompat.cpp:3283`) |
| PID-poll хоста | `CheckHostAlive`: `ReadHostPid()` + `OpenProcess`/`WaitForSingleObject` на каждой итерации цикла (~1 с), debounce 2 тика | `stop_trigger what="hostLiveness"` (`GlassCompat.cpp:3641`) |
| minimal-watcher (`path=2/3`) | ждёт тот же мьютекс | `watcher_wake trigger=hostAlive` |

Дальше — как при мягком стопе: `ExecuteShutdown()` → `FreeLibraryAndExitThread`, а
оставшиеся ссылки снимает user32 по факту смерти процесса-владельца хуков.

**Чего при убитом хосте действительно нет** (это не выгрузка, а «разбудить и подстраховать»):

| Возможность хоста | Что теряется | Кто компенсирует |
|---|---|---|
| `host_broadcast` (HWND_BROADCAST WM_NULL) | общий пинок очередей | сам процесс: `DrainBeforeFreeSelf` пинает свои окна и callback-потоки |
| `PostWmNullAllThreads(pid)` — по ВСЕМ потокам чужого процесса | пробуждение потоков без окна (на них висит per-thread hook-ссылка) | никто: ссылка снимется, когда поток сам зайдёт в user32 |
| `TrySehPlmWake` | thaw PLM-frozen процесса | никто: процесс проснётся при активации |
| `TryRemoteRequestShutdown` | graceful в процесс, чей GUI-цикл не крутится (заморожен/застрял в XAML) | никто |
| `TryRemoteUICompatShutdown` | backup-restore для CoreWindow-хостов | сам процесс в `ExecuteShutdown` (в прогоне 11 `module_shutdown UICompat` у ShellHost 26352 отработал) |

Наблюдаемое следствие в прогоне 11: `DllHost` 35892 сделал `stop_trigger` в 13:52:31.664 и
`freeself_flaet` в 13:52:35.629 (`os_refs=1`), а `dll_detach` пришёл только в 13:55:10.597 —
через 2 мин 35 с. Это ровно тот класс остатка, который при живом хосте снимается пинком по
всем потокам.

Цена этого — **время, а не стабильность**: при убитом хосте ничего не выгружается
принудительно (policy R6/R7), поэтому худший случай — «модуль ещё лежит», а не AV.

### Варианты закрыть окно (выбрать один)

| Вариант | Что делаем | Плюсы | Минусы |
|---|---|---|---|
| **W1. Только in-process** | ничего нового: процесс сам пинает свои окна/потоки, остаток снимается при следующем событии | ноль риска, ноль новых процессов | «тихий» процесс может держать модуль минутами (DllHost в прогоне 11) |
| **W2. Побудитель по запросу** | после `TaskStop` вызвать `rundll32 GlassSettings.Native.dll,GlassSettings_UnlockerW /wakeResiduals`: `PostMessage(WM_NULL)` окнам и потокам процессов с `GlassCompat`, **без** `FreeLibrary` | покрывает замороженные/застрявшие GUI; носитель уже есть (`GlassSettings.Native`, в нём нет Glass-модулей) | не работает при «голом» End задачи — будить некому |
| **W3. Сторожевая задача** | при старте платформы ставится короткоживущая задача-«сторож»: если хост исчез, выполняет W2; при штатном стопе удаляется | покрывает ЛЮБОЙ способ смерти хоста, включая End задачи и падение | +1 задача → обязательны правки GlassSetup/GlassUnins (создание/удаление/перечисление), иначе «фантом» после удаления |

Рекомендация: **W2 сейчас, W3 — если «End задачи = полная остановка» должно быть гарантией**.
W1 остаётся базой в любом случае.

Правила для W2/W3 (те же, что для верификатора в R10): запуск только при отсутствии
`GlassSetup.exe`/`GlassUnins.exe`/`msiexec.exe`; никакого `LoadLibrary` наших модулей в чужие
процессы; никакого `FreeLibrary`; помощник выходит сам за 8–10 с.


## 3. Что не так с текущим состоянием

1. **Пять инициаторов — четыре механизма.** Поведение зависит от того, кто нажал «стоп»:
   из установщика процесс умирает жёстко, из меню — мягко.
2. **Установщик не посылает StopEvent, а сразу убивает задачу.** Процессы при этом всё равно
   получают стоп (abandoned `HostAlive`, §2а), но хост не успевает сделать свою часть:
   `UnhookWinEvent` + `host_broadcast` + дожим остатков. Это не «лишний путь», а менее
   полный: его надо приблизить к штатному (этап 5, A1–A4).
3. **Установщик умеет raw `ForceUnload`** (`/forceUnload` → `CreateRemoteThread(FreeLibrary)`),
   что противоречит политике R6/R7 и ровно этим был вызван AV в `Яндекс Музыка.exe`.
   При этом он и не нужен: после смерти хоста процессы снимают модуль сами (§2а).
4. **`StopEvent` не сбрасывается при старте хоста.** Событие manual-reset: `CreateEventW`
   на существующее имя вернёт уже сигнальное состояние, если кто-то ещё держит хэндл.
5. **`LifecycleBusy` берут не все** (GlassSetup/GlassUnins/LogViewer — нет), поэтому GS и
   ShellExt могут одновременно показать «останавливается» и запустить второй цикл.
6. **Нет единого результата**: кто ждёт `HostShutdown`, кто мьютекс, кто задачу; таймауты
   (3 с / 20 с / 30 с) разбросаны по местам вызова.
7. **Диагностика размазана**: `stop_trigger` пишет каждый процесс, но нет одной записи
   «кто, зачем и когда попросил стоп» и «чем это кончилось».

---

## 4. Целевая архитектура

Единый координатор в `GlassShared` (его линкуют и Setup, и Unins, и ShellExt, и GS-native):

```cpp
// GlassShared/include/GlassStop.h
namespace Shared::GlassStop
{
    enum class Reason : std::uint8_t
    {
        User = 0,        // меню / кнопка в GlassSettings / LogViewer
        Installer = 1,   // GlassSetup
        Uninstaller = 2, // GlassUnins
        Crash = 3,       // CrashGuard / HostShellWatchdog
        External = 4,    // задача убита планировщиком/диспетчером
    };

    struct Options
    {
        Reason reason = Reason::User;
        DWORD  gracefulMs = 20000;  // ожидание HostShutdown
        DWORD  hostGoneMs = 5000;   // ожидание исчезновения HostAlive
        bool   allowTaskStop = false;   // разрешить IRegisteredTask::Stop как fallback
        bool   allowForceUnload = false; // ЗАПРЕЩЕНО по умолчанию (политика R6/R7)
    };

    struct Result
    {
        bool hostGone = false;
        bool hostKilledByTask = false;
        unsigned residualProcesses = 0; // из InjectModuleScan, только чтение
        DWORD elapsedMs = 0;
        const wchar_t* failedPhase = L""; // "" | "busy" | "signal" | "host" | "residuals"
    };

    Result Request(PlatformArch arch, const Options& options) noexcept;
    Result Request(PlatformArch arch) noexcept; // = Options{ Reason::User }
}
```

Внутри `Request` — единственная последовательность фаз (все инициаторы получают её же):

| Фаза | Действие | Отказ |
|---|---|---|
| 0 | если `HostAlive` нет → уже остановлено: посчитать остатки и вернуть | — |
| 1 | `TryBeginLifecycleBusy`; при `false` → `failedPhase="busy"` | второй стоп не запускается |
| 2 | `SetEvent(StopEvent)` + лог `stop_request{reason,arch}` | `failedPhase="signal"` |
| 3 | ждать `HostShutdown` ≤ `gracefulMs`, затем исчезновения `HostAlive` ≤ `hostGoneMs` | — |
| 4 | если хост жив и `allowTaskStop` → `IRegisteredTask::Stop` / `schtasks /End`, пометить `hostKilledByTask` | `failedPhase="host"` |
| 5 | `InjectModuleScan::RefreshInjectResidualSticky` (**только чтение**) → `residualProcesses` | `failedPhase="residuals"` |
| 6 | `EndLifecycleBusy` + `PulsePresence` + лог `stop_result{...}` | — |

Инварианты:

* **никогда** не вызывать `FreeLibrary` в чужих процессах (R6/R7) — опция есть только чтобы
  явно запрещать; удаление raw-пути из установщика входит в этап 4;
* порядок фаз и семантика `path=1/2/3` не меняются (`GlassCompat/docs/GlassCompat-unload.md`);
* `StopEvent` остаётся тем же именем и тем же смыслом — совместимость с уже установленными
  версиями и с ручным `/stop` сохраняется.

Экспорт для управляемого кода (один, вместо `SignalPlatformStop*` + `StopOrphan*`):

```cpp
// GlassSettings/Native/OpenGlassHostTask.cpp
__declspec(dllexport) int WINAPI GlassSettings_RequestPlatformStop(int arch, int reason, int options);
```

---

## 5. План этапов

### Этап 1. Ядро `Shared::GlassStop` (без смены поведения)

* Новые файлы: `GlassShared/include/GlassStop.h`, `GlassShared/src/GlassStop.cpp`
  (+ строки в `GlassShared.vcxproj`, `GlassUnins.vcxproj` — он компилирует исходники GlassShared
  напрямую).
* Внутри — ровно текущая логика: `SignalPlatformStop` + ожидания, как в
  `GlassSettings/Core/Platform/GlassPlatformService.cs`.
* Новые события трассировки: `stop_request`, `stop_phase`, `stop_result`.
* Критерий: ни один вызов ещё не переключён, поведение не изменилось; сборка всех проектов.

### Этап 2. Сброс `StopEvent` на старте хоста

* `GlassCompat/src/EntryRundll.cpp`: после `CreateEventW(...)` и получения `HostAlive` —
  если `WaitForSingleObject(hStopEvent, 0) == WAIT_OBJECT_0`, сделать `ResetEvent` и записать
  `stop_event_stale_reset` в лог.
* Критерий: после Stop→Start повторный старт не завершается сам, в логе один `stop_event_stale_reset`.

### Этап 3. Переключение мягких инициаторов

* GlassSettings: `GlassPlatformService.StopPlatform/StopPlatformAndWait` →
  `NativeHostInterop.RequestPlatformStop(arch, Reason.User, gracefulMs)`; зомби-ветку
  `StopOrphanAutorunTaskInstance` убрать внутрь фазы 4 координатора.
* GlassShellExt: `DesktopShellActions` — вместо `TryBeginLifecycleBusy` + `SignalPlatformStop`
  + ожиданий один вызов `GlassStop::Request(User)`.
* LogViewer: то же.
* GlassCompat: `CrashGuard::SignalPlatformStopBothArch` и `HostShellWatchdog` →
  `GlassStop::Request(Crash)` (без ожиданий, `gracefulMs=0`).
* Критерий: `grep -rn "SetEvent" | grep -i stop` даёт **одно** место — `GlassStop.cpp`.

### Этап 4. Установщик и деинсталлятор

* `StopRunningOpenGlassTasks` (GlassSetup/GlassUnins) → `GlassStop::Request(Installer|Uninstaller,
  allowTaskStop=true)`; прямые `IRegisteredTask::Stop(0)` остаются только внутри координатора.
* `/forceUnload` из `GlassInstallUnlock.cpp` **по умолчанию не вызывается**: вместо него
  страница остатков (данные из `InjectModuleScan`) и штатное закрытие приложения
  пользователем. Принудительная выгрузка — только по явному подтверждению и только для
  процессов, где наш `ExecuteShutdown` уже отработал (ack-мьютекс R6).
* Критерий: при установке/удалении в логе есть `stop_request reason=installer`, а при
  отказе от закрытия процессов — ни одного `fu_begin` без `ack=1`.

### Этап 5. Паритет пути «хост завершён» (abort-parity)

Цель: **остановка хоста любым способом = полная остановка платформы**. Этот путь уже
работает (abandoned `HostAlive`, §2а), его надо довести до паритета с мягким стопом.

* 5.1 `Reason::HostDead` в `GlassStop`: координатор (и любой наблюдатель) при отсутствии
  `HostAlive` пишет `stop_result reason=host_dead` без попыток «дожать» чужие процессы.
* 5.2 Закрыть дыру «DLL есть, watcher'а нет» (A1): если `StartGuiThreadIfNeeded` не смог
  создать поток, вызывать `StartMinimalUnloadWatcher(1)` — liveness-ожидание появится у
  любого процесса с нашей DLL.
* 5.3 Наблюдаемость (A2/A3): `stop_trigger what="hostAlive"` + в записи watcher'а
  `host_alive_ms` (сколько процесс ждал от смерти хоста до FLAET); это отличает «проснулся
  поздно» от «не может проснуться».
* 5.4 Установщику не нужен `IRegisteredTask::Stop` как отдельный механизм выгрузки: он
  останавливает задачу (или посылает StopEvent) и **ждёт исчезновения модулей** по
  `InjectModuleScan` — процессы уходят сами по §2а, шаги 4–6.
* 5.5 Закрыть окно «некому пинать» (§2в): `GlassSettings_UnlockerW /wakeResiduals` —
  `WM_NULL` во все окна и потоки процессов с `GlassCompat`, без `FreeLibrary`; вызывается
  установщиком/деинсталлятором/GS после `TaskStop` (W2).
* 5.6 (опционально, по решению) сторожевая задача W3: срабатывает, если хост исчез, и
  выполняет 5.5; требует правок установщика/деинсталлятора (создание и удаление задачи).
* Критерий: четыре сценария (меню, GlassSettings, установщик, End задачи) дают одинаковый
  итог `verify post8 hits=0`; в логе каждого процесса при End задачи есть
  `stop_trigger what="hostAlive"`.

### Этап 6. Документация и приёмка

* Обновить `GlassCompat/docs/GlassCompat-unload.md` (раздел «Кто инициирует стоп»),
  `GlassSetup/README.md` (строки про `IRegisteredTask::Stop` и `/forceUnload`), этот документ.
* Матрица сборки: `GlassShared` → `GlassCompat` → `GlassSettings.Native` → `GlassShellExt` →
  `GlassSetup`/`GlassUnins` → `LogViewer`.

### Приёмка рефакторинга

| # | Требование |
|---|---|
| U1 | Все инициаторы вызывают `GlassStop::Request`; в коде одно место с `SetEvent(StopEvent)` |
| U2 | Один формат записи о стопе: `stop_request` + `stop_result` в логе хоста/инициатора |
| U3 | Установщик и деинсталлятор не делают `FreeLibrary` в чужих процессах без `ack=1` |
| U4 | Все четыре сценария (меню, GS, установщик, End задачи) дают одинаковый итог: `post8 hits=0` |
| U5 | `StopEvent` не «протекает» между циклами старт/стоп |
| U6 | Второй одновременный стоп не запускается (`failedPhase="busy"`) |
| U7 | «End задачи» = полная остановка: в каждом инжектированном процессе `stop_trigger what="hostAlive"`, peer-модули выгружены, `post8 hits=0`, крашей нет |
| U8 | Ни один сценарий остановки не требует `FreeLibrary` в чужом процессе (включая установщик) |

---

## 6. Риски и как их снимаем

| Риск | Снятие |
|---|---|
| Жёсткий TaskStop из установщика был выбран, чтобы не ждать 20 с | `Options.gracefulMs` задаёт вызывающий; для установщика 8 с, затем TaskStop |
| `LifecycleBusy` теперь берут все — возможен «залипший» busy | busy всегда снимается в конце `Request` (RAII-хранитель) |
| Инжектированный процесс может не просыпаться (DllHost в прогоне 11 — 2 мин) | ожидание остатков не блокирует установку: страница остатков + решение пользователя |
| Изменение поведения `CrashGuard` | `gracefulMs=0`, только `SetEvent` — эквивалентно текущему |
