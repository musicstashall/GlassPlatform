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
| 5 | **Ручная остановка задачи в Планировщике** | — | хост убивается планировщиком; `GlassStop`-вызова нет вообще | — |
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

## 3. Что не так с текущим состоянием

1. **Пять инициаторов — четыре механизма.** Поведение зависит от того, кто нажал «стоп»:
   из установщика процесс умирает жёстко, из меню — мягко.
2. **Установщик не посылает StopEvent.** Инжектированные процессы узнают о конце только по
   исчезновению `HostAlive`; при жёстком убийстве хоста не выполняется
   `UnhookWinEvent` + `host_broadcast` (см. лог: при мягком стопе он есть, при TaskStop — нет).
3. **Установщик умеет raw `ForceUnload`** (`/forceUnload` → `CreateRemoteThread(FreeLibrary)`),
   что противоречит политике R6/R7 и ровно этим был вызван AV в `Яндекс Музыка.exe`.
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

### Этап 5. Ручная остановка задачи

* Ничего не меняем на стороне инициатора (планировщик убивает хост), но координатор при
  следующем запуске/проверке распознаёт отсутствие `HostAlive` и пишет `stop_result
  reason=external`. Документируем как штатный путь: инжектированные процессы уходят по
  `shutdownByHostLiveness`, peer-модули выгружают себя сами.
* Критерий: прогон «End задачи» даёт ту же итоговую картину (`verify post8 hits=0`), что и
  мягкий стоп.

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

---

## 6. Риски и как их снимаем

| Риск | Снятие |
|---|---|
| Жёсткий TaskStop из установщика был выбран, чтобы не ждать 20 с | `Options.gracefulMs` задаёт вызывающий; для установщика 8 с, затем TaskStop |
| `LifecycleBusy` теперь берут все — возможен «залипший» busy | busy всегда снимается в конце `Request` (RAII-хранитель) |
| Инжектированный процесс может не просыпаться (DllHost в прогоне 11 — 2 мин) | ожидание остатков не блокирует установку: страница остатков + решение пользователя |
| Изменение поведения `CrashGuard` | `gracefulMs=0`, только `SetEvent` — эквивалентно текущему |
